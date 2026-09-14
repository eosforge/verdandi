//! 退避不越界、饱和后保留抖动和编号不回绕.
use super::*;
use std::collections::BTreeSet;

#[test]
fn backoff_stays_within_bounds_even_after_counter_saturation() {
    let address = SocketAddr::from(([127, 0, 0, 1], 7443));
    for (minimum, maximum) in [(1, 1), (1, 5), (100, 5000), (5000, 86400000)] {
        let (minimum, maximum) = (Duration::from_millis(minimum), Duration::from_millis(maximum));
        for failures in [0, 1, 2, 16, 32, u32::MAX] {
            for salt in [0, 1, u64::MAX] {
                let delay = retry_delay(address, failures, minimum, maximum, salt);
                assert!((minimum..=maximum).contains(&delay));
                assert_eq!(delay, retry_delay(address, failures, minimum, maximum, salt));
            }
        }
    }
}

#[test]
fn saturated_retries_still_spread_start_times() {
    let delays: BTreeSet<_> = (0..64)
        .map(|salt| {
            retry_delay(
                SocketAddr::from(([127, 0, 0, 1], 7443)),
                u32::MAX,
                Duration::from_millis(100),
                Duration::from_secs(5),
                salt,
            )
        })
        .collect();
    assert!(delays.len() > 32);
    assert_ne!(jitter_salt(b"process-a"), jitter_salt(b"process-b"));
}

#[test]
fn generation_exhaustion_does_not_wrap_or_modify_the_counter() -> io::Result<()> {
    let counter = AtomicU64::new(u64::MAX - 1);
    assert_eq!(next_generation(&counter)?, u64::MAX - 1);
    for _ in 0..3 {
        assert!(next_generation(&counter).is_err());
        assert_eq!(counter.load(Ordering::Relaxed), u64::MAX);
    }
    Ok(())
}
