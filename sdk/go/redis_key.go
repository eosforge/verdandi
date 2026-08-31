package verdandi

import (
	"bytes"
	"context"
	"errors"
	"reflect"
	"time"

	redis "github.com/redis/go-redis/v9"
)

// Key 提供带容量限制的 Redis String 与整键命令。
// 它只是指向共享 Client 的廉价不可变视图，不持有独立连接或关闭责任。
type Key struct {
	// client 是执行命令、提供超时并管理连接生命周期的根客户端。
	client *Client
}

// Key 返回根客户端的整键/String 命令组；调用本身不分配连接或执行 I/O。
func (client *Client) Key() Key {
	return Key{client: client}
}

// Get 使用根客户端超时读取并解码 key 对应的 Redis String。
// 返回值依次为解码结果、键是否存在和错误；缺失键返回 T 零值、false、nil。
func (command Key) Get[T any](key string) (T, bool, error) {
	return command.GetContext[T](context.Background(), key)
}

// GetContext 使用 ctx 的取消与截止时间读取并解码 key，同时叠加根客户端操作超时。
// T 必须满足稳定标量或标准编解码接口契约；不支持的 T 在 Redis I/O 前返回 contract。
func (command Key) GetContext[T any](ctx context.Context, key string) (T, bool, error) {
	var zero T
	if !supportsRedisDecode(reflect.TypeFor[T]()) {
		return zero, false, protocolError(CodeContract, "value", 0)
	}
	value, found, err := command.loadContext(ctx, key)
	if err != nil || !found {
		return zero, found, err
	}
	decoded, err := decodeRedisValue[T](value, "value")
	if err != nil {
		return zero, false, err
	}
	return decoded, true, nil
}

// Load 使用根客户端超时把一个 Redis String 读取为脱离驱动缓冲区的字节。
// 存在的空值返回非 nil 零长度切片、true、nil。
func (command Key) Load(key string) ([]byte, bool, error) {
	return command.LoadContext(context.Background(), key)
}

// LoadContext 使用 ctx 读取一个 Redis String，并返回调用方独占的字节切片。
func (command Key) LoadContext(ctx context.Context, key string) ([]byte, bool, error) {
	return command.loadContext(ctx, key)
}

// loadContext 实现 Load 与 Get 共用的原始读取路径。
// key 在命令准入前校验；Redis Nil 被转换为正常的缺失结果。
func (command Key) loadContext(ctx context.Context, key string) ([]byte, bool, error) {
	if err := validateRedisKey(key); err != nil {
		return nil, false, err
	}
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return nil, false, err
	}
	defer release()
	value, err := command.client.redis.Get(commandCtx, key).Bytes()
	if errors.Is(err, redis.Nil) {
		return nil, false, nil
	}
	if err != nil {
		return nil, false, wrapRedisCommand(CodeUnavailable, err)
	}
	if len(value) > maxRedisValueBytes {
		return nil, false, protocolError(CodeCapacity, "value", 0)
	}
	if len(value) == 0 {
		return []byte{}, true, nil
	}
	return value, true, nil
}

// Set 持久写入一个类型化 Redis String；若键原有 TTL，普通 SET 语义会清除它。
func (command Key) Set[T any](key string, value T) error {
	return command.SetContext(context.Background(), key, value)
}

// SetContext 使用 ctx 持久写入 value；编码或容量失败发生在 Redis I/O 前。
func (command Key) SetContext[T any](ctx context.Context, key string, value T) error {
	return command.setContext(ctx, key, value, 0)
}

// Store 深拷贝 value 后持久写入原始 Redis String，调用方可在返回后立即复用输入缓冲区。
func (command Key) Store(key string, value []byte) error {
	return command.StoreContext(context.Background(), key, value)
}

// StoreContext 使用 ctx 深拷贝并持久写入原始 value。
func (command Key) StoreContext(ctx context.Context, key string, value []byte) error {
	return command.storeContext(ctx, key, value, 0)
}

// SetWithTTL 写入类型化 Redis String，并设置正数、整毫秒精度的 ttl。
func (command Key) SetWithTTL[T any](key string, value T, ttl time.Duration) error {
	return command.SetWithTTLContext(context.Background(), key, value, ttl)
}

// SetWithTTLContext 使用 ctx 写入 value 并设置 ttl；不能精确表示为毫秒的 ttl 被拒绝。
func (command Key) SetWithTTLContext[T any](ctx context.Context, key string, value T, ttl time.Duration) error {
	if err := validateRedisTTL(ttl); err != nil {
		return err
	}
	return command.setContext(ctx, key, value, ttl)
}

// StoreWithTTL 深拷贝原始 value 后写入，并设置正数、整毫秒精度的 ttl。
func (command Key) StoreWithTTL(key string, value []byte, ttl time.Duration) error {
	return command.StoreWithTTLContext(context.Background(), key, value, ttl)
}

// StoreWithTTLContext 使用 ctx 深拷贝并写入 value，同时设置 ttl。
func (command Key) StoreWithTTLContext(ctx context.Context, key string, value []byte, ttl time.Duration) error {
	if err := validateRedisTTL(ttl); err != nil {
		return err
	}
	return command.storeContext(ctx, key, value, ttl)
}

// setContext 编码类型化 value，并把结果交给统一写路径；ttl 为零表示持久写入。
func (command Key) setContext[T any](ctx context.Context, key string, value T, ttl time.Duration) error {
	if err := validateRedisKey(key); err != nil {
		return err
	}
	encoded, err := encodeRedisValue(value, "value")
	if err != nil {
		return err
	}
	return command.writeContext(ctx, key, encoded, ttl)
}

// storeContext 校验并复制原始 value，再交给统一写路径；ttl 为零表示持久写入。
func (command Key) storeContext(ctx context.Context, key string, value []byte, ttl time.Duration) error {
	if err := validateRedisKey(key); err != nil {
		return err
	}
	if len(value) > maxRedisValueBytes {
		return protocolError(CodeCapacity, "value", 0)
	}
	return command.writeContext(ctx, key, bytes.Clone(value), ttl)
}

// writeContext 执行最终 SET；写入已发出但响应丢失时返回 ambiguous。
// key、value 与 ttl 必须已由上层路径完成校验。
func (command Key) writeContext(ctx context.Context, key string, value []byte, ttl time.Duration) error {
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return err
	}
	defer release()
	return wrapRedisCommand(CodeAmbiguous, command.client.redis.Set(commandCtx, key, value, ttl).Err())
}

// Delete 同步删除一个完整 key，并报告执行时该键是否存在。
func (command Key) Delete(key string) (bool, error) {
	return command.DeleteContext(context.Background(), key)
}

// DeleteContext 使用 ctx 同步删除一个完整 key；不确定写结果按 ambiguous 返回。
func (command Key) DeleteContext(ctx context.Context, key string) (bool, error) {
	if err := validateRedisKey(key); err != nil {
		return false, err
	}
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return false, err
	}
	defer release()
	count, err := command.client.redis.Del(commandCtx, key).Result()
	if err != nil {
		return false, wrapRedisCommand(CodeAmbiguous, err)
	}
	return count == 1, nil
}

// Exists 查询一个完整 key 当前是否存在，不读取其值。
func (command Key) Exists(key string) (bool, error) {
	return command.ExistsContext(context.Background(), key)
}

// ExistsContext 使用 ctx 查询一个完整 key 当前是否存在。
func (command Key) ExistsContext(ctx context.Context, key string) (bool, error) {
	if err := validateRedisKey(key); err != nil {
		return false, err
	}
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return false, err
	}
	defer release()
	count, err := command.client.redis.Exists(commandCtx, key).Result()
	if err != nil {
		return false, wrapRedisCommand(CodeUnavailable, err)
	}
	return count == 1, nil
}

// Expire 为已存在的 key 设置正数、整毫秒精度的 ttl，并报告是否实际应用。
func (command Key) Expire(key string, ttl time.Duration) (bool, error) {
	return command.ExpireContext(context.Background(), key, ttl)
}

// ExpireContext 使用 ctx 为已存在的 key 设置 ttl；响应丢失时返回 ambiguous。
func (command Key) ExpireContext(ctx context.Context, key string, ttl time.Duration) (bool, error) {
	if err := validateRedisKey(key); err != nil {
		return false, err
	}
	if err := validateRedisTTL(ttl); err != nil {
		return false, err
	}
	commandCtx, release, err := command.client.commandContext(ctx)
	if err != nil {
		return false, err
	}
	defer release()
	applied, err := command.client.redis.PExpire(commandCtx, key, ttl).Result()
	if err != nil {
		return false, wrapRedisCommand(CodeAmbiguous, err)
	}
	return applied, nil
}
