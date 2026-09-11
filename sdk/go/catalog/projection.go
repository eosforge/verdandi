package catalog

import (
	"slices"

	verdandi "github.com/eosforge/verdandi/sdk/go"
)

// projectPatchReply 验证 HMGET 的当前元数据和旧字段，以最终完整状态计算容量。
// 此函数无 I/O；发布端与边界回归共用同一份校验。
func projectPatchReply(values []any, baseRevision uint64, names []string, fields verdandi.Fields, maximumBytes int) (int, error) {
	if len(values) != 3+len(names) {
		return 0, newError(verdandi.CodeCorrupt, "catalog_header", baseRevision, nil)
	}
	if !slices.ContainsFunc(values, func(value any) bool { return value != nil }) {
		return 0, newError(verdandi.CodeStale, "@base_revision", 0, nil)
	}
	if values[0] == nil || values[1] == nil || values[2] == nil {
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
	projected, err := parseInteger(bytesText, "@encoded_bytes", maximumBytes)
	if err != nil {
		return 0, err
	}
	added, removed := 0, 0
	for index, name := range names {
		old := values[index+3]
		if kind == Array && old == nil {
			return 0, newError(verdandi.CodeTransition, name, baseRevision, nil)
		}
		if old == nil {
			added += len(name) + len(fields[name])
		} else {
			oldText, textOK := redisString(old)
			if !textOK {
				return 0, newError(verdandi.CodeCorrupt, name, baseRevision, nil)
			}
			removed += len(oldText)
			added += len(fields[name])
		}
		if removed > projected {
			return 0, newError(verdandi.CodeCorrupt, "@encoded_bytes", baseRevision, nil)
		}
	}
	// 只限制最终完整状态，字段先增后减的遍历顺序不影响合法性。
	projected = projected - removed + added
	if projected > maximumBytes {
		return 0, newError(verdandi.CodeCapacity, "value", baseRevision, nil)
	}
	return projected, nil
}
