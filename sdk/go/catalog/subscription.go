package catalog

import (
	"crypto/sha256"
	"sort"
	"strings"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

// Subscription 选择整个 Client Zone、若干 Part、若干精确 Path 或任意非空组合。
// 规范化时会删除被更宽范围覆盖的冗余项。
type Subscription struct {
	// Zone 订阅 Client Zone 中全部 Catalog Path。
	Zone bool
	// Parts 订阅每个命名分区下的全部 Path。
	Parts []string
	// Paths 订阅精确且已验证的 Path。
	Paths []Path
}

// normalizedSubscription 保存去重覆盖集合、确定顺序频道和模式。
type normalizedSubscription struct {
	zone     bool
	parts    map[string]struct{}
	paths    map[Path]struct{}
	channels []string
	patterns []string
}

// normalizeSubscription 校验并规范化 subscription，同时构造 Redis Pub/Sub 频道/模式。
func normalizeSubscription(zone string, subscription Subscription) (normalizedSubscription, error) {
	result := normalizedSubscription{
		zone:  subscription.Zone,
		parts: make(map[string]struct{}),
		paths: make(map[Path]struct{}),
	}
	for _, part := range subscription.Parts {
		if !validPathSegment(part, 64) {
			return normalizedSubscription{}, newError(verdandi.CodeInvalid, "part", 0, nil)
		}
		result.parts[part] = struct{}{}
	}
	for _, path := range subscription.Paths {
		if !path.valid() {
			return normalizedSubscription{}, newError(verdandi.CodeInvalid, "path", 0, nil)
		}
		result.paths[path] = struct{}{}
	}
	if !result.zone && len(result.parts) == 0 && len(result.paths) == 0 {
		return normalizedSubscription{}, newError(verdandi.CodeInvalid, "subscription", 0, nil)
	}
	prefix := zonePrefix(zone)
	if result.zone {
		clear(result.parts)
		clear(result.paths)
		result.patterns = []string{prefix + ":*"}
		return result, nil
	}
	parts := make([]string, 0, len(result.parts))
	for part := range result.parts {
		parts = append(parts, part)
	}
	sort.Strings(parts)
	for _, part := range parts {
		result.patterns = append(result.patterns, prefix+":"+part+":*")
	}
	paths := make([]Path, 0, len(result.paths))
	for path := range result.paths {
		if _, covered := result.parts[path.part]; covered {
			delete(result.paths, path)
			continue
		}
		paths = append(paths, path)
	}
	sort.Slice(paths, func(left int, right int) bool {
		return paths[left].member() < paths[right].member()
	})
	for _, path := range paths {
		result.channels = append(result.channels, catalogKey(zone, path))
	}
	return result, nil
}

// covers 判断一个有效 Path 是否落在规范化订阅覆盖内。
func (subscription normalizedSubscription) covers(path Path) bool {
	if !path.valid() {
		return false
	}
	if subscription.zone {
		return true
	}
	if _, covered := subscription.parts[path.part]; covered {
		return true
	}
	_, covered := subscription.paths[path]
	return covered
}

// broad 报告订阅是否需要扫描全 Zone/Part 索引，而非仅读取精确 Path。
func (subscription normalizedSubscription) broad() bool {
	return subscription.zone || len(subscription.parts) != 0
}

// exactPaths 返回按规范 member 排序的精确 Path 副本。
func (subscription normalizedSubscription) exactPaths() []Path {
	paths := make([]Path, 0, len(subscription.paths))
	for path := range subscription.paths {
		paths = append(paths, path)
	}
	sort.Slice(paths, func(left int, right int) bool {
		return paths[left].member() < paths[right].member()
	})
	return paths
}

// checkpointScope 对规范订阅覆盖生成稳定 SHA-256 二进制作用域，隔离本地检查点。
func (subscription normalizedSubscription) checkpointScope() string {
	var canonical strings.Builder
	if subscription.zone {
		canonical.WriteString("zone\n")
	} else {
		parts := make([]string, 0, len(subscription.parts))
		for part := range subscription.parts {
			parts = append(parts, part)
		}
		sort.Strings(parts)
		for _, part := range parts {
			canonical.WriteString("part\x00")
			canonical.WriteString(part)
			canonical.WriteByte('\n')
		}
		for _, path := range subscription.exactPaths() {
			canonical.WriteString("path\x00")
			canonical.WriteString(path.member())
			canonical.WriteByte('\n')
		}
	}
	sum := sha256.Sum256([]byte(canonical.String()))
	return string(sum[:])
}
