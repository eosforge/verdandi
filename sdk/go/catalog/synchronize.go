package catalog

import (
	"context"
	"errors"
	"math"
	"sort"
	"strconv"
	"strings"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

type zoneMetadata struct {
	revision uint64
	floor    uint64
}

type readCandidate struct {
	entry     *Entry
	base      *rawState
	candidate *rawState
}

// syncWorker 是 Subscriber 当前唯一的临时权威读取协程，连续处理已经合并的范围或 Path 批次。
// pending 清空后立即释放同步槽并退出；owner 结束时停止，且每批独立受 syncTimeout 限制。
func (subscriber *Subscriber) syncWorker(owner context.Context) {
	defer subscriber.finishWorker()
	failures := 0
	for {
		if owner.Err() != nil {
			subscriber.stopSyncWorker()
			return
		}
		batch, ok := subscriber.takeSyncBatch()
		if !ok {
			return
		}
		if batch.scope {
			subscriber.markScope(StatusSynchronizing)
		} else {
			for path := range batch.paths {
				subscriber.markEntry(
					subscriber.getOrCreate(path, StatusSynchronizing),
					StatusSynchronizing,
				)
			}
		}
		ctx, cancel := subscriber.client.syncContext(owner)
		var err error
		if batch.scope {
			err = subscriber.synchronizeScope(ctx, owner, batch.forceFull)
		} else {
			err = subscriber.synchronizeExact(ctx, owner, pathSetSlice(batch.paths))
		}
		cancel()
		if err != nil {
			if batch.scope || batch.align {
				subscriber.markScope(StatusUnavailable)
			} else {
				for path := range batch.paths {
					subscriber.markEntry(
						subscriber.getOrCreate(path, StatusUnavailable),
						StatusUnavailable,
					)
				}
			}
			subscriber.report(err)
			notifySyncWaiters(batch.waiters, err)
			delay := subscriber.client.config.recoveryDelay(failures)
			failures++
			if len(batch.waiters) == 0 && retryableSyncError(err) &&
				waitContext(owner, delay) == nil {
				if batch.scope || batch.align {
					subscriber.requestScope(owner, batch.forceFull, nil)
				} else {
					for path := range batch.paths {
						subscriber.requestPath(owner, path, nil)
					}
				}
			}
			continue
		}
		failures = 0
		if batch.align {
			subscriber.markAligned()
		}
		if subscriber.carryBatch(batch) {
			continue
		}
		notifySyncWaiters(batch.waiters, nil)
	}
}

// notifySyncWaiters 非阻塞地把同一个同步结果交付给所有 waiter。
func notifySyncWaiters(waiters []syncWaiter, err error) {
	for _, waiter := range waiters {
		select {
		case waiter <- err:
		default:
		}
	}
}

// pathSetSlice 把 Path 集合转换为切片；后续读取路径会自行排序以保证确定顺序。
func pathSetSlice(paths map[Path]struct{}) []Path {
	result := make([]Path, 0, len(paths))
	for path := range paths {
		result = append(result, path)
	}
	return result
}

// synchronizeScope 把整个订阅范围收敛到权威最新状态。
// forceFull 跳过增量索引，直接枚举完整 live/deleted 集合；成功前后都用元数据与 PING 栅栏验证顺序。
func (subscriber *Subscriber) synchronizeScope(ctx context.Context, owner context.Context, forceFull bool) error {
	// 精确 Path 订阅无需扫描全局索引，但仍以前后元数据和 PING 保证一致的同步点。
	if !subscriber.subscription.broad() {
		metadata, err := subscriber.readMetadata(ctx)
		if err != nil {
			return err
		}
		maximum, err := subscriber.synchronizePaths(
			ctx,
			owner,
			subscriber.subscription.exactPaths(),
		)
		if err != nil {
			return err
		}
		if err := subscriber.pingFence(ctx); err != nil {
			return err
		}
		after, err := subscriber.readMetadata(ctx)
		if err != nil {
			return err
		}
		if maximum > after.revision {
			return newError(verdandi.CodeCorrupt, "@revision", maximum, nil)
		}
		subscriber.cursor = metadata.revision
		subscriber.persistCursor(metadata.revision)
		return nil
	}
	for {
		metadata, err := subscriber.readMetadata(ctx)
		if err != nil {
			return err
		}
		full := forceFull || subscriber.cursor == 0 ||
			subscriber.cursor > metadata.revision || subscriber.cursor < metadata.floor
		var paths map[Path]struct{}
		if full {
			paths, err = subscriber.collectFullPaths(ctx)
		} else {
			paths, err = subscriber.collectChangedPaths(
				ctx,
				subscriber.cursor,
				metadata.revision,
			)
		}
		if err != nil {
			return err
		}
		for _, path := range subscriber.subscription.exactPaths() {
			paths[path] = struct{}{}
		}
		if full {
			for _, path := range subscriber.entryPaths() {
				if subscriber.subscription.covers(path) {
					paths[path] = struct{}{}
				}
			}
		}
		maximum, err := subscriber.synchronizePaths(ctx, owner, pathSetSlice(paths))
		if err != nil {
			return err
		}
		if err := subscriber.pingFence(ctx); err != nil {
			return err
		}
		after, err := subscriber.readMetadata(ctx)
		if err != nil {
			return err
		}
		if maximum > after.revision {
			return newError(verdandi.CodeCorrupt, "@revision", maximum, nil)
		}
		if metadata.revision < after.floor {
			forceFull = true
			continue
		}
		subscriber.cursor = metadata.revision
		subscriber.persistCursor(metadata.revision)
		return nil
	}
}

// synchronizeExact 权威读取指定 paths 并原子发布每条结果；空输入为空操作。
func (subscriber *Subscriber) synchronizeExact(ctx context.Context, owner context.Context, paths []Path) error {
	maximum, err := subscriber.synchronizePaths(ctx, owner, paths)
	if err != nil {
		return err
	}
	if err := subscriber.pingFence(ctx); err != nil {
		return err
	}
	metadata, err := subscriber.readMetadata(ctx)
	if err != nil {
		return err
	}
	if maximum > metadata.revision {
		return newError(verdandi.CodeCorrupt, "@revision", maximum, nil)
	}
	return nil
}

// synchronizePaths 分页批量读取 paths，并返回观察到的最大 revision。
// 路径会排序去重，避免 map/调用顺序改变 Redis 命令和结果安装顺序。
func (subscriber *Subscriber) synchronizePaths(ctx context.Context, owner context.Context, paths []Path) (uint64, error) {
	if len(paths) == 0 {
		return 0, nil
	}
	sort.Slice(paths, func(left int, right int) bool {
		return paths[left].member() < paths[right].member()
	})
	unique := paths[:0]
	for _, path := range paths {
		if len(unique) == 0 || unique[len(unique)-1] != path {
			unique = append(unique, path)
		}
	}
	var maximum uint64
	for first := 0; first < len(unique); first += subscriber.client.config.maxInflightReads {
		last := min(first+subscriber.client.config.maxInflightReads, len(unique))
		candidates, err := subscriber.readPathBatch(ctx, owner, unique[first:last])
		if err != nil {
			return 0, err
		}
		for _, result := range candidates {
			if result.candidate.revision > maximum {
				maximum = result.candidate.revision
			}
			installed, installErr := subscriber.installState(result.entry, result.base, result.candidate)
			if installErr != nil {
				return 0, installErr
			}
			if installed {
				subscriber.persistEntry(result.entry, result.candidate)
			}
		}
	}
	return maximum, nil
}

// readPathBatch 读取一页 Path，并在 NOSCRIPT 时只重载 read 脚本后重试一次。
// 返回候选状态先全部解析，调用方随后统一 CAS 安装，避免一页解析中途留下部分结果。
func (subscriber *Subscriber) readPathBatch(
	ctx context.Context,
	owner context.Context,
	paths []Path,
) ([]readCandidate, error) {
	results, err := subscriber.evalReadBatch(ctx, paths)
	if isNoScript(err) {
		if _, loadErr := subscriber.client.scripts.read.Load(ctx, subscriber.client.redis).Result(); loadErr != nil {
			return nil, wrapDriver(verdandi.CodeUnavailable, loadErr)
		}
		results, err = subscriber.evalReadBatch(ctx, paths)
	}
	if err != nil {
		return nil, wrapDriver(verdandi.CodeUnavailable, err)
	}
	candidates := make([]readCandidate, len(paths))
	for index, path := range paths {
		entry := subscriber.getOrCreate(path, StatusSynchronizing)
		base := entry.state.Load()
		// 发送给 Redis 的 revision 必须来自稍后用于重建 Patch 回复的同一份不可变基准。
		localRevision := uint64(0)
		if completePresent(base) {
			localRevision = base.revision
		}
		// evalReadBatch 已捕获自己的基准 revision；两次调用之间若状态变化，
		// 必须用新基准精确重试一次，不能混合两代状态重建结果。
		if results[index].baseRevision != localRevision {
			subscriber.requestPath(owner, path, nil)
			continue
		}
		candidate, parseErr := parseReadReply(
			results[index].value,
			base,
			subscriber.client.config.maxRecordBytes,
		)
		if parseErr != nil {
			return nil, parseErr
		}
		candidates[index] = readCandidate{entry: entry, base: base, candidate: candidate}
	}
	filtered := candidates[:0]
	for _, candidate := range candidates {
		if candidate.candidate != nil {
			filtered = append(filtered, candidate)
		}
	}
	return filtered, nil
}

type readCommandResult struct {
	baseRevision uint64
	value        any
}

// evalReadBatch 用 Pipeline 对一页 Path 执行只读 Lua，并解析相对于 base 的完整候选状态。
func (subscriber *Subscriber) evalReadBatch(
	ctx context.Context,
	paths []Path,
) ([]readCommandResult, error) {
	pipeline := subscriber.client.redis.Pipeline()
	commands := make([]*redis.Cmd, len(paths))
	bases := make([]uint64, len(paths))
	for index, path := range paths {
		entry := subscriber.getOrCreate(path, StatusSynchronizing)
		state := entry.state.Load()
		if completePresent(state) {
			bases[index] = state.revision
		}
		commands[index] = subscriber.client.scripts.read.EvalSha(
			ctx,
			pipeline,
			readKeys(subscriber.client.config.Zone, path),
			path.member(),
			formatRevision(bases[index]),
		)
	}
	_, err := pipeline.Exec(ctx)
	if err != nil {
		return nil, err
	}
	results := make([]readCommandResult, len(commands))
	for index, command := range commands {
		value, resultErr := command.Result()
		if resultErr != nil {
			return nil, resultErr
		}
		results[index] = readCommandResult{baseRevision: bases[index], value: value}
	}
	return results, nil
}

// isNoScript 判断 Redis 错误是否为脚本缓存丢失；只用于触发确定的单脚本重载。
func isNoScript(err error) bool {
	return err != nil && (errors.Is(err, redis.ErrNoScript) ||
		strings.HasPrefix(err.Error(), "NOSCRIPT"))
}

// readMetadata 读取 Zone 全局 revision 与 replay floor，并校验二者规范且 floor 不超过 revision。
func (subscriber *Subscriber) readMetadata(ctx context.Context) (zoneMetadata, error) {
	values, err := subscriber.client.redis.HGetAll(
		ctx,
		metaKey(subscriber.client.config.Zone),
	).Result()
	if err != nil {
		return zoneMetadata{}, wrapDriver(verdandi.CodeUnavailable, err)
	}
	if len(values) == 0 {
		return zoneMetadata{}, nil
	}
	if len(values) != 2 {
		return zoneMetadata{}, newError(verdandi.CodeCorrupt, "catalog_meta", 0, nil)
	}
	revision, err := parseRevision(values["@revision"], true)
	if err != nil {
		return zoneMetadata{}, err
	}
	floor, err := parseRevision(values["@floor_revision"], true)
	if err != nil || floor > revision {
		return zoneMetadata{}, newError(verdandi.CodeCorrupt, "@floor_revision", revision, err)
	}
	return zoneMetadata{revision: revision, floor: floor}, nil
}

// collectChangedPaths 从 live/deleted revision ZSET 收集 (after, through] 范围内、且被订阅覆盖的 Path。
// 结果受 maxChangedPaths 限制，超限时调用方必须退化为完整同步。
func (subscriber *Subscriber) collectChangedPaths(
	ctx context.Context,
	from uint64,
	through uint64,
) (map[Path]struct{}, error) {
	paths := make(map[Path]struct{})
	if through <= from {
		return paths, nil
	}
	for _, key := range []string{
		liveKey(subscriber.client.config.Zone),
		deletedKey(subscriber.client.config.Zone),
	} {
		last := from
		for {
			values, err := subscriber.client.redis.ZRangeByScoreWithScores(
				ctx,
				key,
				&redis.ZRangeBy{
					Min:    "(" + formatRevision(last),
					Max:    formatRevision(through),
					Offset: 0,
					Count:  int64(subscriber.client.config.scanPageSize),
				},
			).Result()
			if err != nil {
				return nil, wrapDriver(verdandi.CodeUnavailable, err)
			}
			if len(values) == 0 {
				break
			}
			for _, value := range values {
				revision, err := exactScore(value.Score)
				if err != nil || revision <= last || revision > through {
					return nil, newError(verdandi.CodeCorrupt, "catalog_index", revision, err)
				}
				member, ok := value.Member.(string)
				if !ok {
					return nil, newError(verdandi.CodeCorrupt, "catalog_index", revision, nil)
				}
				path, ok := pathFromMember(member)
				if !ok {
					return nil, newError(verdandi.CodeCorrupt, "catalog_index", revision, nil)
				}
				if subscriber.subscription.covers(path) {
					paths[path] = struct{}{}
				}
				last = revision
			}
			if len(values) < subscriber.client.config.scanPageSize {
				break
			}
		}
	}
	return paths, nil
}

// collectFullPaths 分页枚举当前 live 与 deleted 索引中的全部覆盖 Path，并返回去重集合。
func (subscriber *Subscriber) collectFullPaths(ctx context.Context) (map[Path]struct{}, error) {
	paths := make(map[Path]struct{})
	for _, key := range []string{
		liveKey(subscriber.client.config.Zone),
		deletedKey(subscriber.client.config.Zone),
	} {
		cursor := uint64(0)
		for {
			values, next, err := subscriber.client.redis.ZScan(
				ctx,
				key,
				cursor,
				"*",
				int64(subscriber.client.config.scanPageSize),
			).Result()
			if err != nil {
				return nil, wrapDriver(verdandi.CodeUnavailable, err)
			}
			if len(values)%2 != 0 {
				return nil, newError(verdandi.CodeCorrupt, "catalog_index", 0, nil)
			}
			for index := 0; index < len(values); index += 2 {
				revision, err := parseIndexScore(values[index+1])
				if err != nil {
					return nil, err
				}
				path, ok := pathFromMember(values[index])
				if !ok {
					return nil, newError(verdandi.CodeCorrupt, "catalog_index", revision, nil)
				}
				if subscriber.subscription.covers(path) {
					paths[path] = struct{}{}
				}
			}
			cursor = next
			if cursor == 0 {
				break
			}
		}
	}
	return paths, nil
}

// exactScore 把 Redis ZSET float64 score 转成精确安全整数；非整数或超范围返回 corrupt。
func exactScore(score float64) (uint64, error) {
	if math.IsNaN(score) || math.IsInf(score, 0) || score < 1 ||
		score > float64(maximumRevision) || math.Trunc(score) != score {
		return 0, newError(verdandi.CodeCorrupt, "catalog_index", 0, nil)
	}
	return uint64(score), nil
}

// parseIndexScore 解析 ZSCAN 返回的规范十进制正 revision。
func parseIndexScore(value string) (uint64, error) {
	score, err := strconv.ParseFloat(value, 64)
	if err != nil {
		return 0, newError(verdandi.CodeCorrupt, "catalog_index", 0, err)
	}
	return exactScore(score)
}

// entryPaths 返回 Subscriber 当前所有 Entry 的 Path 快照，不持有 entriesMu 供后续 I/O。
func (subscriber *Subscriber) entryPaths() []Path {
	subscriber.entriesMu.RLock()
	defer subscriber.entriesMu.RUnlock()
	paths := make([]Path, 0, len(subscriber.entries))
	for path := range subscriber.entries {
		paths = append(paths, path)
	}
	return paths
}

// markAligned 把范围和全部 Entry 从 synchronizing/unavailable 发布为已对齐的 present/absent 状态。
func (subscriber *Subscriber) markAligned() {
	subscriber.scopeStatus.Store(uint32(StatusPresent))
	subscriber.entriesMu.RLock()
	entries := make([]*Entry, 0, len(subscriber.entries))
	for _, entry := range subscriber.entries {
		entries = append(entries, entry)
	}
	subscriber.entriesMu.RUnlock()
	for _, entry := range entries {
		for {
			current := entry.state.Load()
			if current == nil || current.status == StatusClosed {
				break
			}
			status := StatusAbsent
			if current.kind != 0 {
				status = StatusPresent
			} else if current.revision != 0 {
				status = StatusDeleted
			}
			if current.status == status ||
				entry.state.CompareAndSwap(current, current.withStatus(status)) {
				break
			}
		}
	}
}
