use std::time::{Duration, Instant};

use fred::prelude::*;
use fred::types::Value;

use super::client::ClientInner;
use crate::error::{Code, Error, Result};
use crate::fields::MAX_SAFE_INTEGER;

#[derive(Clone, Copy)]
pub(crate) struct RedisClock {
    anchor: Instant,
    upper: u64,
}

impl ClientInner {
    /// 用一次 Redis TIME 往返校准连接级保守上界时钟。
    ///
    /// 采样以本地 `Instant` 包围，Redis 毫秒加完整往返时间和 `clock_uncertainty`，
    /// 从而消除 SDK 与 Redis 墙钟偏差并保守判断租约过期。回复或安全整数溢出返回错误。
    pub(crate) async fn calibrate_clock(&self) -> Result<RedisClock> {
        let started = Instant::now();
        let value: Value = self
            .command(self.driver().custom(fred::cmd!("TIME"), Vec::<Value>::new()), Code::Unavailable)
            .await?;
        let finished = Instant::now();
        let (seconds, microseconds) = parse_time(value)?;
        if microseconds >= 1_000_000 {
            return Err(Error::field(Code::Corrupt, "redis_clock"));
        }
        let server_ms = seconds
            .checked_mul(1000)
            .and_then(|value| value.checked_add(microseconds / 1000))
            .filter(|value| *value > 0 && *value <= MAX_SAFE_INTEGER)
            .ok_or_else(|| Error::field(Code::Capacity, "redis_clock"))?;
        // 使用完整 RTT 而非一半 RTT，确保得到 Redis 当前时间的上界而非平均估计。
        let margin = finished
            .duration_since(started)
            .checked_add(self.config.clock_uncertainty)
            .ok_or_else(|| Error::field(Code::Capacity, "redis_clock"))?;
        let margin_ms = ceil_milliseconds(margin)?;
        let upper = server_ms
            .checked_add(margin_ms)
            .filter(|value| *value <= MAX_SAFE_INTEGER)
            .ok_or_else(|| Error::field(Code::Capacity, "redis_clock"))?;
        Ok(RedisClock { anchor: finished, upper })
    }
}

impl RedisClock {
    /// 把校准上界推进到当前 `Instant`，返回不超过跨语言安全整数的 Redis 毫秒估计。
    ///
    /// 本地 elapsed 向上取整；极端持续时间溢出时饱和到最大安全整数，使记录保守过期。
    pub(crate) fn upper_now(self) -> u64 {
        let elapsed = ceil_milliseconds(self.anchor.elapsed()).unwrap_or(MAX_SAFE_INTEGER);
        self.upper.saturating_add(elapsed).min(MAX_SAFE_INTEGER)
    }
}

/// 解析 Redis TIME 的 `[seconds, microseconds]` 回复。
///
/// 两项必须为非负整数；形状、类型或数值错误返回 `Corrupt`。
fn parse_time(value: Value) -> Result<(u64, u64)> {
    let Value::Array(values) = value else {
        return Err(Error::field(Code::Corrupt, "redis_clock"));
    };
    if values.len() != 2 {
        return Err(Error::field(Code::Corrupt, "redis_clock"));
    }
    let mut iterator = values.into_iter();
    let Some(seconds) = iterator.next().and_then(value_nonnegative_u64) else {
        return Err(Error::field(Code::Corrupt, "redis_clock"));
    };
    let Some(microseconds) = iterator.next().and_then(value_nonnegative_u64) else {
        return Err(Error::field(Code::Corrupt, "redis_clock"));
    };
    Ok((seconds, microseconds))
}

/// 把 Fred `value` 的非负整数、字符串或字节十进制形式转换为 `u64`。
fn value_nonnegative_u64(value: Value) -> Option<u64> {
    match value {
        Value::Integer(value) if value >= 0 => u64::try_from(value).ok(),
        Value::String(value) => value.parse().ok(),
        Value::Bytes(value) => std::str::from_utf8(&value).ok()?.parse().ok(),
        _ => None,
    }
}

/// 将 `value` 向上取整为毫秒；平台转换溢出返回 `Capacity`。
fn ceil_milliseconds(value: Duration) -> Result<u64> {
    let nanoseconds = value.as_nanos();
    let milliseconds = nanoseconds.div_ceil(1_000_000);
    u64::try_from(milliseconds).map_err(|_| Error::field(Code::Capacity, "redis_clock"))
}

#[cfg(test)]
#[path = "../../tests/internal/registration/clock.rs"]
mod tests;
