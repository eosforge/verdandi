package catalog

import (
	verdandi "github.com/eosforge/verdandi/sdk/go"
)

// parseReadReply 解析只读 Lua 回复，并相对 base 复用或构造完整不可变状态。
// maximumBytes 限制完整值；失败不发布部分状态。
func parseReadReply(value any, base *rawState, maximumBytes int) (*rawState, error) {
	items, ok := value.([]any)
	if !ok || len(items) == 0 || len(items)%2 != 0 {
		return nil, newError(verdandi.CodeCorrupt, "read_reply", 0, nil)
	}
	fields := make(map[string]any, len(items)/2)
	for index := 0; index < len(items); index += 2 {
		name, nameOK := redisString(items[index])
		if !nameOK {
			return nil, newError(verdandi.CodeCorrupt, "read_reply", 0, nil)
		}
		if _, exists := fields[name]; exists {
			return nil, newError(verdandi.CodeCorrupt, "read_reply", 0, nil)
		}
		fields[name] = items[index+1]
	}
	result, ok := readString(fields, "&result")
	if !ok {
		return nil, newError(verdandi.CodeCorrupt, "&result", 0, nil)
	}
	if result == "error" {
		reply := scriptReply{result: result}
		reply.status, _ = readString(fields, "&status")
		reply.field, _ = readString(fields, "&field")
		if revisionText, exists := readString(fields, "@revision"); exists {
			var err error
			reply.revision, err = parseRevision(revisionText, true)
			if err != nil {
				return nil, err
			}
		}
		return nil, scriptStatusError(reply)
	}
	if result != "ok" {
		return nil, newError(verdandi.CodeCorrupt, "&result", 0, nil)
	}
	status, ok := readString(fields, "&status")
	if !ok {
		return nil, newError(verdandi.CodeCorrupt, "&status", 0, nil)
	}
	revisionText, ok := readString(fields, "@revision")
	if !ok {
		return nil, newError(verdandi.CodeCorrupt, "@revision", 0, nil)
	}
	revision, err := parseRevision(revisionText, true)
	if err != nil {
		return nil, err
	}
	switch status {
	case "absent":
		if len(fields) != 3 || revision != 0 {
			return nil, newError(verdandi.CodeCorrupt, "read_reply", revision, nil)
		}
		return deletedState(0), nil
	case "deleted":
		if len(fields) != 3 || revision == 0 {
			return nil, newError(verdandi.CodeCorrupt, "read_reply", revision, nil)
		}
		return deletedState(revision), nil
	case "present":
		return parsePresentRead(fields, base, revision, maximumBytes)
	default:
		return nil, newError(verdandi.CodeCorrupt, "&status", revision, nil)
	}
}

// parsePresentRead 解析 Present 值的 revision/kind/fields，并验证完整编码大小。
func parsePresentRead(
	values map[string]any,
	base *rawState,
	revision uint64,
	maximumBytes int,
) (*rawState, error) {
	if len(values) != 8 || revision == 0 {
		return nil, newError(verdandi.CodeCorrupt, "read_reply", revision, nil)
	}
	mode, modeOK := readString(values, "&mode")
	replaceText, replaceOK := readString(values, "@replace_revision")
	kindText, kindOK := readString(values, "@kind")
	bytesText, bytesOK := readString(values, "@encoded_bytes")
	encodedFields, fieldOK := values["&fields"]
	if !modeOK || !replaceOK || !kindOK || !bytesOK || !fieldOK {
		return nil, newError(verdandi.CodeCorrupt, "read_reply", revision, nil)
	}
	replaceRevision, err := parseRevision(replaceText, false)
	if err != nil || replaceRevision > revision {
		return nil, newError(verdandi.CodeCorrupt, "@replace_revision", revision, err)
	}
	kind, ok := parseKind(kindText)
	if !ok {
		return nil, newError(verdandi.CodeCorrupt, "@kind", revision, nil)
	}
	encodedBytes, err := parseInteger(bytesText, "@encoded_bytes", maximumBytes)
	if err != nil {
		return nil, err
	}
	delta, err := decodeReadFields(encodedFields)
	if err != nil {
		return nil, err
	}
	state := &rawState{
		revision:        revision,
		replaceRevision: replaceRevision,
		status:          StatusPresent,
		kind:            kind,
		encodedBytes:    encodedBytes,
	}
	switch mode {
	case "replace":
		state.fields = delta
	case "patch":
		if !completePresent(base) || len(delta) == 0 ||
			base.revision >= revision || base.revision < replaceRevision ||
			base.replaceRevision != replaceRevision || base.kind != kind {
			return nil, newError(verdandi.CodeCorrupt, "&mode", revision, nil)
		}
		state.fields = cloneFields(base.fields)
		for name, value := range delta {
			state.fields[name] = value
		}
	case "unchanged":
		if !completePresent(base) || len(delta) != 0 ||
			base.revision != revision || base.replaceRevision != replaceRevision ||
			base.kind != kind || base.encodedBytes != encodedBytes {
			return nil, newError(verdandi.CodeCorrupt, "&mode", revision, nil)
		}
		state.fields = base.fields
	default:
		return nil, newError(verdandi.CodeCorrupt, "&mode", revision, nil)
	}
	_, actualBytes, err := validateValue(kind, state.fields, maximumBytes)
	if err != nil || actualBytes != encodedBytes {
		return nil, newError(verdandi.CodeCorrupt, "@encoded_bytes", revision, err)
	}
	return state, nil
}

// decodeReadFields 把 Lua 回复中的交替字段名/值数组解码为拥有型 Fields。
func decodeReadFields(value any) (verdandi.Fields, error) {
	items, ok := value.([]any)
	if !ok || len(items)%2 != 0 || len(items)/2 > maximumFields {
		return nil, newError(verdandi.CodeCorrupt, "fields", 0, nil)
	}
	fields := make(verdandi.Fields, len(items)/2)
	for index := 0; index < len(items); index += 2 {
		name, nameOK := eventString(items[index])
		field, fieldOK := eventBytes(items[index+1])
		if !nameOK || !fieldOK || !validFieldName(name) {
			return nil, newError(verdandi.CodeCorrupt, "fields", 0, nil)
		}
		if _, exists := fields[name]; exists {
			return nil, newError(verdandi.CodeCorrupt, name, 0, nil)
		}
		fields[name] = field
	}
	return fields, nil
}

// readString 从回复字段表读取 name，并接受 string 或 []byte 表示。
func readString(values map[string]any, name string) (string, bool) {
	value, exists := values[name]
	if !exists {
		return "", false
	}
	return redisString(value)
}
