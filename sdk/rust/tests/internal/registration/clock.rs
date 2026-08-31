use super::*;

#[test]
fn parses_redis_time_and_rounds_uncertainty_up() {
    assert_eq!(parse_time(Value::Array(vec!["12".into(), "3456".into()])), Ok((12, 3456)));
    assert_eq!(ceil_milliseconds(Duration::from_nanos(1)), Ok(1));
    assert_eq!(ceil_milliseconds(Duration::from_millis(2)), Ok(2));
}
