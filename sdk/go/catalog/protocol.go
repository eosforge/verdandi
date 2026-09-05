package catalog

import (
	"fmt"
	"strconv"

	verdandi "github.com/eosforge/verdandi/sdk/go"
	"github.com/eosforge/verdandi/sdk/go/internal/validate"
)

type scriptReply struct {
	result   string
	status   string
	field    string
	revision uint64
	floor    uint64
	pruned   uint64
}

// parseScriptReply 解析 Catalog Lua 的交替名称/值回复，并拒绝重复、缺失和非法字段。
func parseScriptReply(value any) (scriptReply, error) {
	items, ok := value.([]any)
	if !ok || len(items) == 0 || len(items)%2 != 0 {
		return scriptReply{}, newError(verdandi.CodeCorrupt, "script_reply", 0, nil)
	}
	fields := make(map[string]string, len(items)/2)
	for index := 0; index < len(items); index += 2 {
		name, nameOK := redisString(items[index])
		field, fieldOK := redisString(items[index+1])
		if !nameOK || !fieldOK {
			return scriptReply{}, newError(verdandi.CodeCorrupt, "script_reply", 0, nil)
		}
		if _, exists := fields[name]; exists {
			return scriptReply{}, newError(verdandi.CodeCorrupt, "script_reply", 0, nil)
		}
		fields[name] = field
	}
	reply := scriptReply{
		result: fields["&result"],
		status: fields["&status"],
		field:  fields["&field"],
	}
	var err error
	if text, exists := fields["@revision"]; exists {
		reply.revision, err = parseRevision(text, true)
		if err != nil {
			return scriptReply{}, err
		}
	}
	if text, exists := fields["@floor_revision"]; exists {
		reply.floor, err = parseRevision(text, true)
		if err != nil {
			return scriptReply{}, err
		}
	}
	if text, exists := fields["@pruned"]; exists {
		var valid bool
		reply.pruned, valid = validate.UintDecimal(text, (1<<31)-1, true)
		if !valid {
			_, err = strconv.ParseUint(text, 10, 31)
			return scriptReply{}, newError(verdandi.CodeCorrupt, "@pruned", 0, err)
		}
	}
	switch reply.result {
	case "ok":
		return reply, nil
	case "error":
		return reply, scriptStatusError(reply)
	default:
		return scriptReply{}, newError(verdandi.CodeCorrupt, "&result", 0, nil)
	}
}

// scriptStatusError 把 Lua error 回复中的稳定 status/field/revision 转换为 Verdandi Error。
func scriptStatusError(reply scriptReply) error {
	var code verdandi.Code
	switch reply.status {
	case "invalid":
		code = verdandi.CodeInvalid
	case "contract":
		code = verdandi.CodeContract
	case "capacity":
		code = verdandi.CodeCapacity
	case "stale":
		code = verdandi.CodeStale
	case "transition":
		code = verdandi.CodeTransition
	case "corrupt":
		code = verdandi.CodeCorrupt
	case "unavailable":
		code = verdandi.CodeUnavailable
	default:
		code = verdandi.CodeProtocol
	}
	return newError(code, reply.field, reply.revision, nil)
}

// redisString 把 go-redis 回复中的 string 或 []byte 统一转成字符串。
func redisString(value any) (string, bool) {
	switch value := value.(type) {
	case string:
		return value, true
	case []byte:
		return string(value), true
	default:
		return "", false
	}
}

// parseRevision 解析规范安全整数 revision；allowZero 决定零值是否有效。
func parseRevision(value string, allowZero bool) (uint64, error) {
	parsed, valid := validate.UintDecimal(value, maximumRevision, allowZero)
	if !valid {
		_, err := strconv.ParseUint(value, 10, 53)
		return 0, newError(verdandi.CodeCorrupt, "@revision", 0, err)
	}
	return parsed, nil
}

// parseInteger 解析 [0,maximum] 范围内的规范十进制 int，并把错误定位到 field。
func parseInteger(value string, field string, maximum int) (int, error) {
	if maximum < 0 {
		return 0, newError(verdandi.CodeCorrupt, field, 0, nil)
	}
	limit := uint64(maximum)
	if limit > (1<<31)-1 {
		limit = (1 << 31) - 1
	}
	parsed, valid := validate.UintDecimal(value, limit, true)
	if !valid {
		_, err := strconv.ParseUint(value, 10, 31)
		return 0, newError(verdandi.CodeCorrupt, field, 0, err)
	}
	return int(parsed), nil
}

// scriptArguments 把固定 prefix 与按 names 顺序排列的字段名/值拼成 Lua 参数。
func scriptArguments(prefix []any, names []string, fields verdandi.Fields) []any {
	result := make([]any, 0, len(prefix)+len(names)*2)
	result = append(result, prefix...)
	for _, name := range names {
		result = append(result, name, fields[name])
	}
	return result
}

// requireResultRevision 要求 reply 为 ok 且携带正 revision，否则返回对应稳定错误。
func requireResultRevision(reply scriptReply) (uint64, error) {
	if reply.result != "ok" || reply.revision == 0 {
		return 0, newError(
			verdandi.CodeCorrupt,
			"@revision",
			reply.revision,
			fmt.Errorf("catalog mutation returned no positive revision"),
		)
	}
	return reply.revision, nil
}
