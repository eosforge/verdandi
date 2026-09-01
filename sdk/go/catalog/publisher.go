package catalog

import (
	"context"
	"strconv"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	redis "github.com/redis/go-redis/v9"
)

// Result 标识一次已接纳 Catalog 变更获得的 Redis 全局 revision。
type Result struct {
	// Revision 为正，且不超过跨语言安全整数范围。
	Revision uint64
}

// Patch 是一次严格基于版本的 Array/Map 局部更新。
// Set 只包含新增或覆盖字段；首版明确不支持字段级删除。
type Patch struct {
	// BaseRevision 必须精确等于目标 Path 当前 revision。
	BaseRevision uint64
	// Set 包含完整字段增量，值缓冲区在写入前脱离调用方。
	Set verdandi.Fields
}

// Publisher 是绑定 Catalog Client 的轻量写入视图。
// 它不拥有任务、锁或独立关闭状态；调用 Context 与 Catalog Client 共同控制每次操作。
type Publisher struct {
	client *Client
}

// Publisher 构造一个轻量多 Path 写入器，不执行 Redis I/O 或启动任务。
// Catalog Client 关闭后拒绝构造；返回对象无需单独关闭。
func (client *Client) Publisher() (*Publisher, error) {
	if client == nil {
		return nil, newError(verdandi.CodeInvalid, "client", 0, nil)
	}
	if client.closed() {
		return nil, newError(verdandi.CodeClosed, "", 0, nil)
	}
	return &Publisher{client: client}, nil
}

// Replace 原子发布一个完整 Value、Array 或 Map。
// path/kind/value 在 Redis I/O 前校验；并发 Replace/Delete 由 Redis 执行顺序决定最终状态。
func (publisher *Publisher) Replace(
	ctx context.Context,
	path Path,
	kind Kind,
	value verdandi.Encoder,
) (Result, error) {
	if !path.valid() {
		return Result{}, newError(verdandi.CodeInvalid, "path", 0, nil)
	}
	fields, err := encodeValue(value)
	if err != nil {
		return Result{}, err
	}
	names, size, err := validateValue(kind, fields, publisher.maximumBytes())
	if err != nil {
		return Result{}, err
	}
	operationCtx, done, err := publisher.begin(ctx)
	if err != nil {
		return Result{}, err
	}
	defer done()

	arguments := scriptArguments(
		[]any{path.member(), kind.text(), strconv.Itoa(size), strconv.Itoa(len(names))},
		names,
		fields,
	)
	revision, err := publisher.mutate(
		operationCtx,
		publisher.client.scripts.replace,
		mutationKeys(publisher.client.config.Zone, path),
		arguments,
	)
	return Result{Revision: revision}, err
}

// Patch 仅在 BaseRevision 当前时，原子新增/覆盖 Map 字段或覆盖现有 Array 索引。
// SDK 先读取必要字段计算容量；Lua 随后原子复核同一基准，竞态只会返回 stale，不会提交过期投影。
func (publisher *Publisher) Patch(
	ctx context.Context,
	path Path,
	patch Patch,
) (Result, error) {
	if !path.valid() {
		return Result{}, newError(verdandi.CodeInvalid, "path", 0, nil)
	}
	if patch.BaseRevision == 0 || patch.BaseRevision > maximumRevision {
		return Result{}, newError(verdandi.CodeInvalid, "@base_revision", patch.BaseRevision, nil)
	}
	fields := cloneFields(patch.Set)
	names, err := validatePatchFields(fields, publisher.maximumBytes())
	if err != nil {
		return Result{}, err
	}
	operationCtx, done, err := publisher.begin(ctx)
	if err != nil {
		return Result{}, err
	}
	defer done()

	projected, err := publisher.projectPatch(operationCtx, path, patch.BaseRevision, names, fields)
	if err != nil {
		return Result{}, err
	}
	arguments := scriptArguments(
		[]any{
			path.member(),
			formatRevision(patch.BaseRevision),
			strconv.Itoa(projected),
			strconv.Itoa(len(names)),
		},
		names,
		fields,
	)
	revision, err := publisher.mutate(
		operationCtx,
		publisher.client.scripts.patch,
		mutationKeys(publisher.client.config.Zone, path),
		arguments,
	)
	return Result{Revision: revision}, err
}

// Delete 原子删除完整 path，并总是创建一个新 tombstone revision，即使目标已缺失或已删除。
func (publisher *Publisher) Delete(ctx context.Context, path Path) (Result, error) {
	if !path.valid() {
		return Result{}, newError(verdandi.CodeInvalid, "path", 0, nil)
	}
	operationCtx, done, err := publisher.begin(ctx)
	if err != nil {
		return Result{}, err
	}
	defer done()
	revision, err := publisher.mutate(
		operationCtx,
		publisher.client.scripts.delete,
		mutationKeys(publisher.client.config.Zone, path),
		[]any{path.member()},
	)
	return Result{Revision: revision}, err
}

// maximumBytes 返回 Publisher 当前单值编码上限；无效接收者返回零。
func (publisher *Publisher) maximumBytes() int {
	if publisher == nil || publisher.client == nil {
		return 0
	}
	return publisher.client.config.maxRecordBytes
}

// begin 在 Catalog Client 的关闭栅栏中接纳一次操作。
// parent 控制调用；返回 Context 也响应 Client 关闭，释放函数必须执行。
func (publisher *Publisher) begin(parent context.Context) (context.Context, func(), error) {
	if publisher == nil || publisher.client == nil {
		return nil, nil, newError(verdandi.CodeClosed, "client", 0, nil)
	}
	return publisher.client.admit(parent)
}

// projectPatch 读取当前头部及被修改字段，验证 baseRevision/kind，并计算 Patch 后完整编码字节数。
// 此读取不是写锁：最终 Lua 必须再次精确匹配 baseRevision，任何同 Path 竞态都会使写入失败为 stale。
func (publisher *Publisher) projectPatch(
	ctx context.Context,
	path Path,
	baseRevision uint64,
	names []string,
	fields verdandi.Fields,
) (int, error) {
	lookup := make([]string, 0, 3+len(names))
	lookup = append(lookup, "@revision", "@kind", "@encoded_bytes")
	lookup = append(lookup, names...)
	commandCtx, cancel := publisher.client.commandContext(ctx)
	values, err := publisher.client.redis.HMGet(
		commandCtx,
		catalogKey(publisher.client.config.Zone, path),
		lookup...,
	).Result()
	cancel()
	if err != nil {
		return 0, wrapDriver(verdandi.CodeUnavailable, err)
	}
	if len(values) != len(lookup) || values[0] == nil || values[1] == nil || values[2] == nil {
		return 0, newError(verdandi.CodeCorrupt, "catalog_header", baseRevision, nil)
	}
	revisionText, ok := redisString(values[0])
	if !ok {
		return 0, newError(verdandi.CodeCorrupt, "@revision", baseRevision, nil)
	}
	revision, err := parseRevision(revisionText, false)
	if err != nil {
		return 0, err
	}
	if revision != baseRevision {
		return 0, newError(verdandi.CodeStale, "@base_revision", revision, nil)
	}
	kindText, ok := redisString(values[1])
	if !ok {
		return 0, newError(verdandi.CodeCorrupt, "@kind", baseRevision, nil)
	}
	kind, ok := parseKind(kindText)
	if !ok {
		return 0, newError(verdandi.CodeCorrupt, "@kind", baseRevision, nil)
	}
	if kind == Value {
		return 0, newError(verdandi.CodeTransition, "@kind", baseRevision, nil)
	}
	bytesText, ok := redisString(values[2])
	if !ok {
		return 0, newError(verdandi.CodeCorrupt, "@encoded_bytes", baseRevision, nil)
	}
	projected, err := parseInteger(bytesText, "@encoded_bytes", publisher.maximumBytes())
	if err != nil {
		return 0, err
	}
	for index, name := range names {
		old := values[index+3]
		if kind == Array && old == nil {
			return 0, newError(verdandi.CodeTransition, name, baseRevision, nil)
		}
		if old == nil {
			projected += len(name) + len(fields[name])
		} else {
			oldText, textOK := redisString(old)
			if !textOK {
				return 0, newError(verdandi.CodeCorrupt, name, baseRevision, nil)
			}
			projected += len(fields[name]) - len(oldText)
		}
		if projected < 0 || projected > publisher.maximumBytes() {
			return 0, newError(verdandi.CodeCapacity, "value", baseRevision, nil)
		}
	}
	return projected, nil
}

// mutate 执行一个已选择的 Catalog Lua，解析并返回 Redis 分配的 revision。
// keys/arguments 已由上层构造；响应丢失按 ambiguous 返回。
func (publisher *Publisher) mutate(
	ctx context.Context,
	script *redis.Script,
	keys []string,
	arguments []any,
) (uint64, error) {
	commandCtx, cancel := publisher.client.commandContext(ctx)
	value, err := script.Run(
		commandCtx,
		publisher.client.redis,
		keys,
		arguments...,
	).Result()
	cancel()
	if err != nil {
		return 0, wrapDriver(verdandi.CodeAmbiguous, err)
	}
	reply, err := parseScriptReply(value)
	if err != nil {
		return 0, err
	}
	return requireResultRevision(reply)
}
