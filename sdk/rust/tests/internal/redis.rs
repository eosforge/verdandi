use super::*;

#[test]
fn ttl_requires_positive_exact_milliseconds() {
    assert_eq!(ttl_milliseconds(Duration::from_millis(1)), Ok(1));
    assert!(matches!(ttl_milliseconds(Duration::ZERO), Err(error) if error.code() == Code::Invalid));
    assert!(matches!(ttl_milliseconds(Duration::from_nanos(1)), Err(error) if error.code() == Code::Invalid));
    assert!(matches!(ttl_milliseconds(Duration::MAX), Err(error) if error.code() == Code::Invalid));
}

#[test]
fn deterministic_driver_failures_are_not_ambiguous_writes() {
    assert_eq!(driver_code(CommandKind::Write, &ErrorKind::InvalidArgument), Code::Protocol);
    assert_eq!(driver_code(CommandKind::Write, &ErrorKind::IO), Code::Ambiguous);
    assert_eq!(driver_code(CommandKind::Read, &ErrorKind::IO), Code::Unavailable);
}
