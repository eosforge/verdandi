package verdandi

import (
	"context"
	"time"
)

const (
	maxRedisKeyBytes       = 1024
	maxRedisHashFields     = 4096
	maxRedisFieldNameBytes = 1024
	maxRedisValueBytes     = 512 * 1024
	maxRedisHashBytes      = 512 * 1024
)

// commandContext 校验根客户端与 parent，并为一条命令叠加固定操作超时。
// 返回的 release 必须由调用方在所有路径调用；失败时返回的 Context 和 release 均为 nil。
func (client *Client) commandContext(parent context.Context) (context.Context, func(), error) {
	if client == nil {
		return nil, nil, protocolError(CodeClosed, "", 0)
	}
	if parent == nil {
		return nil, nil, protocolError(CodeInvalid, "context", 0)
	}
	if err := parent.Err(); err != nil {
		return nil, nil, wrapContext(err)
	}
	if client.closed.Load() || client.redis == nil {
		return nil, nil, protocolError(CodeClosed, "", 0)
	}
	ctx, cancel := context.WithTimeout(parent, client.timeout)
	return ctx, cancel, nil
}

// validateRedisKey 校验 key 是否满足根命令层的非空和字节长度上限。
func validateRedisKey(key string) error {
	if len(key) == 0 || len(key) > maxRedisKeyBytes {
		return protocolError(CodeInvalid, "key", 0)
	}
	return nil
}

// validateRedisField 校验 field 是否满足 Hash 字段名的非空和字节长度上限。
func validateRedisField(field string) error {
	if len(field) == 0 || len(field) > maxRedisFieldNameBytes {
		return protocolError(CodeInvalid, "field", 0)
	}
	return nil
}

// validateRedisTTL 要求 ttl 为正数且能精确表示为整毫秒，避免隐式截断租期。
func validateRedisTTL(ttl time.Duration) error {
	if ttl <= 0 || ttl%time.Millisecond != 0 {
		return protocolError(CodeInvalid, "ttl", 0)
	}
	return nil
}

// addRedisSize 把 size 安全累加到 total，并在负数、单项超限或加法溢出前返回 capacity。
// field 用于指出容量错误所属的协议输入。
func addRedisSize(total *int, size int, field string) error {
	if size < 0 || size > maxRedisHashBytes || *total > maxRedisHashBytes-size {
		return protocolError(CodeCapacity, field, 0)
	}
	*total += size
	return nil
}

// Ping 使用后台 Context 验证共享 Redis 传输能否执行一条受超时限制的命令。
func (client *Client) Ping() error {
	return client.PingContext(context.Background())
}

// PingContext 使用 ctx 的取消、截止时间和值验证共享 Redis 传输。
// 根客户端的 Timeout 仍会叠加，二者中更早的截止时间生效。
func (client *Client) PingContext(ctx context.Context) error {
	commandCtx, release, err := client.commandContext(ctx)
	if err != nil {
		return err
	}
	defer release()
	return wrapRedisCommand(CodeUnavailable, client.redis.Ping(commandCtx).Err())
}
