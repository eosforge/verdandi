package verdandi

import (
	"bytes"
	"context"
	"reflect"
	"sort"
	"strings"
)

// Hash 提供带容量限制的 Redis Hash 命令。
// 它只是指向共享 Client 的廉价不可变视图，不持有独立连接或关闭责任。
type Hash struct {
	// client 是执行命令、缓存结构描述并管理连接生命周期的根客户端。
	client *Client
}

// redisHashField 记录一个 Go 结构字段在原结构中的索引及其 Redis 字段名。
type redisHashField struct {
	index int
	name  string
}

// redisHashDescriptor 是按具体结构体类型构建并缓存的不可变编解码计划。
type redisHashDescriptor struct {
	fields    []redisHashField
	names     []string
	decodeErr error
	encodeErr error
}

// Hash 返回根客户端的 Hash 命令组；调用本身不分配连接或执行 I/O。
func (client *Client) Hash() Hash {
	return Hash{client: client}
}

// Get 使用 HMGET 精确读取 T 描述的已导出顶层字段。
// 缺失字段保持 Go 零值，缺失整个 Hash 同样返回零值 T 和 nil。
func (command Hash) Get[T any](key string) (T, error) {
	return command.GetContext[T](context.Background(), key)
}

// GetContext 使用 ctx 和 HMGET 读取 T 描述的字段，并叠加根客户端操作超时。
// T 必须是非指针结构体；任何字段损坏都会返回 T 零值，不暴露部分解码结果。
func (command Hash) GetContext[T any](ctx context.Context, key string) (T, error) {
	var zero T
	if command.client == nil {
		return zero, protocolError(CodeClosed, "", 0)
	}
	if err := validateRedisKey(key); err != nil {
		return zero, err
	}
	descriptor := command.client.hashDescriptor(reflect.TypeFor[T]())
	if descriptor.decodeErr != nil {
		return zero, descriptor.decodeErr
	}

	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return zero, err
	}
	defer release()
	reply, err := command.client.redis.HMGet(commandCtx, key, descriptor.names...).Result()
	if err != nil {
		return zero, wrapRedisCommand(CodeUnavailable, err)
	}
	if len(reply) != len(descriptor.fields) {
		return zero, protocolError(CodeCorrupt, "hash", 0)
	}

	// 所有返回槽位和累计容量均验证后才写入结果；遇错时调用方只得到零值。
	var result T
	decoded := reflect.ValueOf(&result).Elem()
	total := 0
	for index, field := range descriptor.fields {
		if err := addRedisSize(&total, len(field.name), "hash"); err != nil {
			return zero, err
		}
		if reply[index] == nil {
			continue
		}
		value, err := redisHashReplyBytes(reply[index], field.name)
		if err != nil {
			return zero, err
		}
		if len(value) > maxRedisValueBytes {
			return zero, protocolError(CodeCapacity, field.name, 0)
		}
		if err := addRedisSize(&total, len(value), "hash"); err != nil {
			return zero, err
		}
		if err := decodeRedisReflectValue(decoded.Field(field.index), value, field.name); err != nil {
			return zero, err
		}
	}
	return result, nil
}

// Load 读取 Hash 的全部字段并返回脱离驱动缓冲区的 Fields。
// 缺失 key 返回非 nil 的空 map；该操作是 O(N) 的完整读取。
func (command Hash) Load(key string) (Fields, error) {
	return command.LoadContext(context.Background(), key)
}

// LoadContext 使用 ctx 完整读取 Hash；字段数量、名称、单值和总字节数均受固定上限约束。
func (command Hash) LoadContext(ctx context.Context, key string) (Fields, error) {
	if err := validateRedisKey(key); err != nil {
		return nil, err
	}
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return nil, err
	}
	defer release()
	reply, err := command.client.redis.HGetAll(commandCtx, key).Result()
	if err != nil {
		return nil, wrapRedisCommand(CodeUnavailable, err)
	}
	if len(reply) > maxRedisHashFields {
		return nil, protocolError(CodeCapacity, "hash", 0)
	}

	// go-redis 的字符串结果在转换为 []byte 时复制，返回 map 不与驱动或 SDK 状态共享。
	fields := make(Fields, len(reply))
	total := 0
	for name, value := range reply {
		if err := validateRedisField(name); err != nil {
			return nil, protocolError(CodeCorrupt, "field", 0)
		}
		if len(value) > maxRedisValueBytes {
			return nil, protocolError(CodeCapacity, name, 0)
		}
		if err := addRedisSize(&total, len(name)+len(value), "hash"); err != nil {
			return nil, err
		}
		fields[name] = []byte(value)
	}
	return fields, nil
}

// Set 使用 HSET 写入 T 选择的每个顶层字段，包括零值字段。
// 它是局部更新，不删除 T 未描述或被忽略的 Hash 字段。
func (command Hash) Set[T any](key string, value T) error {
	return command.SetContext(context.Background(), key, value)
}

// SetContext 使用 ctx 编码并写入 value 的所有选定字段。
// 编码、名称和容量错误发生在 Redis I/O 前；响应丢失按 ambiguous 返回。
func (command Hash) SetContext[T any](ctx context.Context, key string, value T) error {
	if command.client == nil {
		return protocolError(CodeClosed, "", 0)
	}
	if err := validateRedisKey(key); err != nil {
		return err
	}
	descriptor := command.client.hashDescriptor(reflect.TypeFor[T]())
	if descriptor.encodeErr != nil {
		return descriptor.encodeErr
	}

	// 描述符顺序固定，因此参数顺序稳定，并只分配一次完整 HSET 参数切片。
	source := reflect.ValueOf(&value).Elem()
	arguments := make([]any, 0, len(descriptor.fields)*2)
	total := 0
	for _, field := range descriptor.fields {
		encoded, err := encodeRedisReflectValue(source.Field(field.index), field.name)
		if err != nil {
			return err
		}
		if err := addRedisSize(&total, len(field.name)+len(encoded), "hash"); err != nil {
			return err
		}
		arguments = append(arguments, field.name, encoded)
	}
	return command.hsetContext(ctx, key, arguments)
}

// Store 深拷贝 fields 后用 HSET 写入所有原始字段。
// 它是局部更新，不删除输入中没有出现的字段。
func (command Hash) Store(key string, fields Fields) error {
	return command.StoreContext(context.Background(), key, fields)
}

// StoreContext 使用 ctx 校验、排序、深拷贝并写入 fields。
// 排序保证动态 map 输入生成稳定参数顺序，调用方返回后可安全修改原缓冲区。
func (command Hash) StoreContext(ctx context.Context, key string, fields Fields) error {
	if err := validateRedisKey(key); err != nil {
		return err
	}
	if len(fields) == 0 {
		return protocolError(CodeInvalid, "fields", 0)
	}
	if len(fields) > maxRedisHashFields {
		return protocolError(CodeCapacity, "fields", 0)
	}

	// Go map 迭代无序；先排序名称可稳定测试、抓包和跨次调用行为。
	names := make([]string, 0, len(fields))
	for name := range fields {
		names = append(names, name)
	}
	sort.Strings(names)
	arguments := make([]any, 0, len(fields)*2)
	total := 0
	for _, name := range names {
		if err := validateRedisField(name); err != nil {
			return err
		}
		value := fields[name]
		if len(value) > maxRedisValueBytes {
			return protocolError(CodeCapacity, name, 0)
		}
		if err := addRedisSize(&total, len(name)+len(value), "hash"); err != nil {
			return err
		}
		arguments = append(arguments, name, bytes.Clone(value))
	}
	return command.hsetContext(ctx, key, arguments)
}

// hsetContext 执行已校验的 HSET 参数；调用方必须保证 arguments 为交替字段名和值。
func (command Hash) hsetContext(ctx context.Context, key string, arguments []any) error {
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return err
	}
	defer release()
	return wrapRedisCommand(CodeAmbiguous, command.client.redis.HSet(commandCtx, key, arguments...).Err())
}

// Delete 删除指定 Hash 字段，并返回执行时实际存在的字段数量。
func (command Hash) Delete(key string, fields ...string) (int64, error) {
	return command.DeleteContext(context.Background(), key, fields...)
}

// DeleteContext 使用 ctx 删除字段；空字段列表或任何非法字段名都会在 I/O 前失败。
// HDEL 响应丢失时返回 ambiguous，计数不可作为已确认结果使用。
func (command Hash) DeleteContext(ctx context.Context, key string, fields ...string) (int64, error) {
	if err := validateRedisKey(key); err != nil {
		return 0, err
	}
	if len(fields) == 0 {
		return 0, protocolError(CodeInvalid, "fields", 0)
	}
	if len(fields) > maxRedisHashFields {
		return 0, protocolError(CodeCapacity, "fields", 0)
	}
	total := 0
	for _, field := range fields {
		if err := validateRedisField(field); err != nil {
			return 0, err
		}
		if err := addRedisSize(&total, len(field), "fields"); err != nil {
			return 0, err
		}
	}
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return 0, err
	}
	defer release()
	count, err := command.client.redis.HDel(commandCtx, key, fields...).Result()
	if err != nil {
		return 0, wrapRedisCommand(CodeAmbiguous, err)
	}
	return count, nil
}

// Exists 查询 field 当前是否存在于指定 Hash，不读取字段值。
func (command Hash) Exists(key string, field string) (bool, error) {
	return command.ExistsContext(context.Background(), key, field)
}

// ExistsContext 使用 ctx 查询 field 是否存在，并叠加根客户端操作超时。
func (command Hash) ExistsContext(ctx context.Context, key string, field string) (bool, error) {
	if err := validateRedisKey(key); err != nil {
		return false, err
	}
	if err := validateRedisField(field); err != nil {
		return false, err
	}
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return false, err
	}
	defer release()
	exists, err := command.client.redis.HExists(commandCtx, key, field).Result()
	if err != nil {
		return false, wrapRedisCommand(CodeUnavailable, err)
	}
	return exists, nil
}

// Length 返回指定 Hash 当前包含的字段数量；缺失 key 返回零。
func (command Hash) Length(key string) (int64, error) {
	return command.LengthContext(context.Background(), key)
}

// LengthContext 使用 ctx 返回 Hash 字段数量，并叠加根客户端操作超时。
func (command Hash) LengthContext(ctx context.Context, key string) (int64, error) {
	if err := validateRedisKey(key); err != nil {
		return 0, err
	}
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return 0, err
	}
	defer release()
	length, err := command.client.redis.HLen(commandCtx, key).Result()
	if err != nil {
		return 0, wrapRedisCommand(CodeUnavailable, err)
	}
	return length, nil
}

// hashDescriptor 返回 valueType 的进程内不可变描述符，并保证并发首次构建只发布一个实例。
func (client *Client) hashDescriptor(valueType reflect.Type) *redisHashDescriptor {
	if cached, ok := client.hashDescriptors.Load(valueType); ok {
		return cached.(*redisHashDescriptor)
	}
	descriptor := buildRedisHashDescriptor(valueType)
	actual, _ := client.hashDescriptors.LoadOrStore(valueType, descriptor)
	return actual.(*redisHashDescriptor)
}

// buildRedisHashDescriptor 校验一个结构体的 Redis 字段映射和编解码能力。
// 返回值始终非 nil；契约错误保存在 descriptor 中，便于缓存并避免重复反射校验。
func buildRedisHashDescriptor(valueType reflect.Type) *redisHashDescriptor {
	descriptor := &redisHashDescriptor{}
	if valueType == nil || valueType.Kind() != reflect.Struct {
		err := protocolError(CodeContract, "value", 0)
		descriptor.decodeErr = err
		descriptor.encodeErr = err
		return descriptor
	}

	// 只选择已导出的直接字段；标签覆盖名称，"-" 明确排除字段，不展开嵌入结构。
	names := make(map[string]struct{}, valueType.NumField())
	nameBytes := 0
	for index := range valueType.NumField() {
		field := valueType.Field(index)
		if field.PkgPath != "" {
			continue
		}
		name := field.Name
		if tagged, ok := field.Tag.Lookup("redis"); ok {
			if tagged == "-" {
				continue
			}
			name = tagged
		}
		if validateRedisField(name) != nil || strings.ContainsRune(name, ',') {
			err := protocolError(CodeContract, field.Name, 0)
			descriptor.decodeErr = err
			descriptor.encodeErr = err
			return descriptor
		}
		if _, duplicate := names[name]; duplicate {
			err := protocolError(CodeContract, name, 0)
			descriptor.decodeErr = err
			descriptor.encodeErr = err
			return descriptor
		}
		if err := addRedisSize(&nameBytes, len(name), "fields"); err != nil {
			descriptor.decodeErr = err
			descriptor.encodeErr = err
			return descriptor
		}
		names[name] = struct{}{}
		descriptor.fields = append(descriptor.fields, redisHashField{index: index, name: name})
		descriptor.names = append(descriptor.names, name)
		if descriptor.decodeErr == nil && !supportsRedisDecode(field.Type) {
			descriptor.decodeErr = protocolError(CodeContract, name, 0)
		}
		if descriptor.encodeErr == nil && !supportsRedisEncode(field.Type) {
			descriptor.encodeErr = protocolError(CodeContract, name, 0)
		}
	}
	if len(descriptor.fields) == 0 {
		err := protocolError(CodeContract, "value", 0)
		descriptor.decodeErr = err
		descriptor.encodeErr = err
	} else if len(descriptor.fields) > maxRedisHashFields {
		err := protocolError(CodeCapacity, "fields", 0)
		descriptor.decodeErr = err
		descriptor.encodeErr = err
	}
	return descriptor
}

// redisHashReplyBytes 把 go-redis HMGET 的单个非 nil 槽位转换为字节。
// 仅接受 string 与 []byte；其他动态类型表示驱动回复损坏。
func redisHashReplyBytes(value any, field string) ([]byte, error) {
	switch value := value.(type) {
	case string:
		return []byte(value), nil
	case []byte:
		return bytes.Clone(value), nil
	default:
		return nil, protocolError(CodeCorrupt, field, 0)
	}
}
