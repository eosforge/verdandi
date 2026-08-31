use super::*;

#[test]
fn registration_config_bounds_pending_and_retained_bytes() {
    let mut config = Config::new("Alpha");
    assert_eq!(config.selector_event_bytes, 64 * 1024 * 1024);
    assert_eq!(config.selector_retained_bytes, None);
    config.selector_retained_bytes = Some(0);
    assert!(config.validate().is_ok());
    config.selector_event_bytes = 1024 * 1024 * 1024 + 1;
    assert!(matches!(config.validate(), Err(error) if error.code() == Code::Invalid));
    config.selector_event_bytes = 1;
    config.selector_retained_bytes = Some(1024 * 1024 * 1024 + 1);
    assert!(matches!(config.validate(), Err(error) if error.code() == Code::Invalid));
}

#[test]
fn hello_version_requires_one_complete_version_pair() {
    let response = Value::Array(vec!["server".into(), "redis".into(), "version".into(), "8.8.0".into()]);
    assert_eq!(hello_version(response), Ok("8.8.0".to_owned()));
    assert!(matches!(hello_version(Value::Array(vec!["server".into()])), Err(error) if error.code() == Code::Corrupt));
    assert!(matches!(
        hello_version(Value::Array(vec!["server".into(), "redis".into()])),
        Err(error) if error.code() == Code::Corrupt
    ));
}
