//! 有界退避和进程内会话编号, 不维护角色拓扑.
use std::{
    io,
    net::SocketAddr,
    sync::atomic::{AtomicU64, Ordering},
    time::Duration,
};
/// 分配当前进程内严格递增的 connection generation; 耗尽 u64 时停止接受新会话.
pub fn next_generation(generations: &AtomicU64) -> io::Result<u64> {
    // Relaxed 足够, 因为这里只要求该 atomic 自身的唯一递增值, 不用它发布其他内存状态.
    // `fetch_update` 通过 `checked_add` 拒绝回绕, 成功时返回更新前的值作为本次 generation.
    generations
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |current| current.checked_add(1))
        .map_err(|_| io::Error::other("connection generation exhausted"))
}

/// 根据目标地址和失败次数计算不超过上限的确定性抖动退避.
///
/// 确定性只用于避免引入随机依赖; 它不参与协议或安全决策.
pub fn retry_delay(seed: SocketAddr, failures: u32, minimum: Duration, maximum: Duration, salt: u64) -> Duration {
    // 第 1 次失败使用 2^0. shift 封顶 16, 防止位移溢出和极端失败次数扩大计算.
    let shift = failures.saturating_sub(1).min(16);
    let multiplier = 1_u32 << shift;

    // Duration 的 saturating 运算先防止算术溢出, 再把基础退避截断到配置上限.
    let base = minimum.saturating_mul(multiplier).min(maximum);
    let lower = base.saturating_sub(base / 4).max(minimum);
    let upper = base.saturating_add(base / 4).min(maximum);
    let jitter_window = upper.saturating_sub(lower);

    // 地址哈希只需要稳定分散, 不承担身份或抗碰撞安全职责.
    let address_hash = seed.ip().to_string().bytes().fold(u64::from(seed.port()), |state, byte| {
        state.wrapping_mul(1099511628211).wrapping_add(u64::from(byte))
    });
    // Duration 以 u128 返回毫秒数. 超出 u64 时取 u64 上限, 仍由最终 maximum 截断.
    let window_millis = u64::try_from(jitter_window.as_millis()).unwrap_or(u64::MAX);
    let jitter_millis = address_hash.wrapping_add(salt).wrapping_add(u64::from(failures)) % window_millis.saturating_add(1);
    // 先限定采样区间再选值. 到达 maximum 后仍能在其下方抖动, 不会全部被截成同一个值.
    lower.saturating_add(Duration::from_millis(jitter_millis)).min(maximum)
}

/// 从 boot ID 提取当前进程稳定, 不同启动通常不同的退避盐值.
pub fn jitter_salt(boot_id: &[u8]) -> u64 {
    // 混合完整启动 ID, 避免只取高字节导致同进程多个节点获得相同盐值.
    boot_id
        .iter()
        .fold(14695981039346656037_u64, |state, byte| (state ^ u64::from(*byte)).wrapping_mul(1099511628211))
}

#[cfg(test)]
#[path = "../tests/unit/retry.rs"]
mod tests;
