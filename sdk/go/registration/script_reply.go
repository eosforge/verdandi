package registration

import (
	"context"
	"strconv"
)

// registrationReply 是四种 Registration Lua 成功回复的最小公共投影。
type registrationReply struct {
	revision  uint64
	timestamp uint64
}

// callRegistration 执行 kind 指定的 Lua，并解析为稳定回复。
// ctx 控制调用；typeName、uuid 和 arguments 必须已由 SDK 校验，写入结果丢失按 ambiguous 返回。
func (client *clientRuntime) callRegistration(
	ctx context.Context,
	kind registrationScriptKind,
	typeName string,
	uuid string,
	arguments []any,
) (registrationReply, error) {
	if err := ctx.Err(); err != nil {
		return registrationReply{}, wrapContext(err)
	}
	script := protocolScripts.script(kind)
	if script == nil {
		return registrationReply{}, protocolError(codeInvalid, "&kind", 0)
	}
	// 固定两个键分别是单条 Registration 与同 Zone/Type 的 Registry membership/channel。
	keys := []string{
		"verdandi:registration:" + client.config.Zone + ":" + typeName + ":" + uuid,
		"verdandi:registry:" + client.config.Zone + ":" + typeName,
	}
	commandCtx, cancel := client.commandContext(ctx)
	raw, err := script.Run(commandCtx, client.redis, keys, arguments...).Result()
	cancel()
	if err != nil {
		return registrationReply{}, wrapDriver(codeAmbiguous, err)
	}
	return parseRegistrationReply(raw)
}

// parseRegistrationReply 解析 Lua 返回的交替名称/值数组。
// 成功返回 revision/timestamp；协议错误返回 Lua 提供的稳定状态、字段和权威 revision。
func parseRegistrationReply(raw any) (registrationReply, error) {
	values, ok := raw.([]any)
	if !ok || len(values) < 2 || len(values)%2 != 0 {
		return registrationReply{}, protocolError(codeCorrupt, "reply", 0)
	}
	// 回复字段数量很小；构建名称表能统一拒绝重复项并允许未来兼容的附加字段。
	fields := make(map[string]any, len(values)/2)
	for index := 0; index < len(values); index += 2 {
		name, ok := replyString(values[index])
		if !ok {
			return registrationReply{}, protocolError(codeCorrupt, "reply", 0)
		}
		if _, exists := fields[name]; exists {
			return registrationReply{}, protocolError(codeCorrupt, name, 0)
		}
		fields[name] = values[index+1]
	}
	result, ok := replyString(fields["&result"])
	if !ok {
		return registrationReply{}, protocolError(codeCorrupt, "&result", 0)
	}
	revision, _ := replyUint(fields["@revision"])
	if result == "ok" {
		timestamp, timestampOK := replyUint(fields["@timestamp"])
		if _, exists := fields["@timestamp"]; exists && !timestampOK {
			return registrationReply{}, protocolError(codeCorrupt, "@timestamp", 0)
		}
		return registrationReply{revision: revision, timestamp: timestamp}, nil
	}
	if result != "error" {
		return registrationReply{}, protocolError(codeCorrupt, "&result", 0)
	}
	status, ok := replyString(fields["&status"])
	if !ok {
		return registrationReply{}, protocolError(codeCorrupt, "&status", 0)
	}
	field, _ := replyString(fields["&field"])
	code := code(status)
	switch code {
	case codeInvalid, codeProtocol, codeContract, codeTarget, codeCapacity,
		codeMissing, codeStale, codeTransition, codeImmutable, codeCorrupt:
		return registrationReply{}, protocolError(code, field, revision)
	default:
		return registrationReply{}, protocolError(codeCorrupt, "&status", 0)
	}
}

// replyString 把 go-redis 可能返回的 string 或 []byte 统一转换为字符串。
func replyString(value any) (string, bool) {
	switch typed := value.(type) {
	case string:
		return typed, true
	case []byte:
		return string(typed), true
	default:
		return "", false
	}
}

// replyUint 解析正数且不超过 maxSafeInteger 的回复整数。
// 同时接受 RESP 整数、string 和 []byte，nil 或非规范范围返回 false。
func replyUint(value any) (uint64, bool) {
	switch typed := value.(type) {
	case int64:
		if typed <= 0 {
			return 0, false
		}
		return uint64(typed), uint64(typed) <= maxSafeInteger
	case string:
		parsed, err := strconv.ParseUint(typed, 10, 64)
		return parsed, err == nil && parsed > 0 && parsed <= maxSafeInteger
	case []byte:
		parsed, err := strconv.ParseUint(string(typed), 10, 64)
		return parsed, err == nil && parsed > 0 && parsed <= maxSafeInteger
	case nil:
		return 0, false
	default:
		return 0, false
	}
}
