package registration

import (
	"context"
	"math"
	"time"
)

// redisClock 保存一次 Redis TIME 校准得到的保守上界。
// anchor 使用本地单调时钟推进 upper，避免依赖可能跳变的本地墙上时钟。
type redisClock struct {
	anchor time.Time
	upper  uint64
}

// calibrateClock 用一次 Redis TIME 往返构造服务端时间上界。
// ctx 控制校准命令；上界加入完整往返时间和配置不确定度，因此不会把可能已过期的租约误判为有效。
func (client *clientRuntime) calibrateClock(ctx context.Context) (redisClock, error) {
	// 同一进程内的 time.Now 携带单调时钟，started/finished 差值不受墙上时钟校正影响。
	started := time.Now()
	commandCtx, cancel := client.commandContext(ctx)
	serverTime, err := client.redis.Time(commandCtx).Result()
	cancel()
	finished := time.Now()
	if err != nil {
		return redisClock{}, wrapDriver(codeUnavailable, err)
	}
	// 采用完整往返而非半程估算，以保守覆盖网络和服务端排队延迟。
	roundTrip := finished.Sub(started)
	margin := roundTrip + client.config.clockUncertainty
	serverMilliseconds := serverTime.UnixMilli()
	if serverMilliseconds <= 0 || margin < 0 {
		return redisClock{}, protocolError(codeCorrupt, "redis_clock", 0)
	}
	marginMilliseconds := uint64(math.Ceil(float64(margin) / float64(time.Millisecond)))
	upper := uint64(serverMilliseconds)
	if upper > maxSafeInteger-marginMilliseconds {
		return redisClock{}, protocolError(codeCapacity, "redis_clock", 0)
	}
	return redisClock{anchor: finished, upper: upper + marginMilliseconds}, nil
}

// upperNow 把校准上界按本地单调耗时推进到当前时刻。
// 未校准或数值将溢出时返回 maxSafeInteger，使所有租约安全地视为不可用。
func (clock redisClock) upperNow() uint64 {
	if clock.anchor.IsZero() {
		return maxSafeInteger
	}
	elapsed := time.Since(clock.anchor)
	if elapsed <= 0 {
		return clock.upper
	}
	milliseconds := uint64(math.Ceil(float64(elapsed) / float64(time.Millisecond)))
	if clock.upper > maxSafeInteger-milliseconds {
		return maxSafeInteger
	}
	return clock.upper + milliseconds
}
