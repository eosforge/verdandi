use super::*;

type InvalidCase = (&'static str, &'static str, fn(&mut Config));

fn assert_invalid_field(config: &Config, field: &str) {
    match config.validate() {
        Err(error) => {
            assert_eq!(error.code(), Code::Invalid);
            assert_eq!(error.field_name(), Some(field));
        }
        Ok(()) => panic!("configuration unexpectedly accepted; wanted invalid field {field}"),
    }
}

fn defaults() -> Value {
    Value::Array(ZoneConfig::default().values().into_iter().map(Value::from).collect())
}

#[test]
fn defaults_and_zone_are_stable() {
    let parsed = parse_zone_config(defaults());
    assert_eq!(parsed, Ok(ZoneConfig::default()));
    let config = Config::new("Alpha");
    assert!(config.validate().is_ok());
    assert_eq!(config.registration_buffer_capacity, 8);
    assert_eq!(config.registration_error_buffer_capacity, 16);
    assert_eq!(config.minimum_renew_interval, Duration::from_millis(100));
    assert_eq!(config.renew_jitter_percent, 10);
    assert_eq!(config.policy_refresh_jitter_percent, 10);
    assert_eq!(config.policy, RegistrationLimits::default());
    assert_eq!(config.selector_page_size, 256);
    assert_eq!(config.selector_event_buffer, 4096);
    assert_eq!(config.selector_event_bytes, 64 * 1024 * 1024);
    assert_eq!(config.selector_publish_interval, Duration::from_millis(10));
    assert_eq!(config.selector_sync_timeout, Duration::from_secs(30));
    assert_eq!(config.selector_max_bytes, 256 * 1024 * 1024);
    assert_eq!(config.selector_retained_bytes, None);
    assert_eq!(config.clock_refresh, Duration::from_secs(30));
    assert_eq!(config.clock_uncertainty, Duration::from_millis(1));
    assert_eq!(config.selector_error_buffer_capacity, 16);
    assert_eq!(config.selector_recovery_initial_delay, Duration::from_millis(100));
    assert_eq!(config.selector_recovery_max_delay, Duration::from_secs(5));
    assert_eq!(config.selector_recovery_multiplier, 2);
    assert_eq!(config.selector_recovery_jitter_percent, 50);
    assert!(matches!(Config::new("alpha-1").validate(), Err(error) if error.code() == Code::Invalid));
}

#[test]
fn registration_config_accepts_explicit_zero_options() {
    let mut config = Config::new("Alpha");
    config.renew_jitter_percent = 0;
    config.policy_refresh_jitter_percent = 0;
    config.selector_publish_interval = Duration::ZERO;
    config.selector_retained_bytes = Some(0);
    config.clock_uncertainty = Duration::ZERO;
    config.selector_recovery_jitter_percent = 0;
    assert!(config.validate().is_ok());
}

#[test]
fn registration_config_reports_exact_invalid_field() {
    let cases: &[InvalidCase] = &[
        ("zone", "zone", |config| config.zone = "a:bad".into()),
        ("registration buffer", "registration.buffer_capacity", |config| {
            config.registration_buffer_capacity = 257;
        }),
        ("registration error buffer", "registration.error_buffer_capacity", |config| {
            config.registration_error_buffer_capacity = 1025;
        }),
        ("minimum renew interval", "registration.min_renew_interval", |config| {
            config.minimum_renew_interval = Duration::from_millis(9);
        }),
        ("renew jitter", "registration.renew_jitter_percent", |config| {
            config.renew_jitter_percent = 51;
        }),
        ("policy refresh jitter", "registration.policy_refresh_jitter_percent", |config| {
            config.policy_refresh_jitter_percent = 51;
        }),
        ("policy", "registration.policy", |config| config.policy.attr_max_fields = 129),
        ("selector page", "selector.scan_page_size", |config| config.selector_page_size = 1025),
        ("selector pending entries", "selector.max_pending_entries", |config| {
            config.selector_event_buffer = 65_537;
        }),
        ("selector pending bytes", "selector.max_pending_bytes", |config| {
            config.selector_event_bytes = 1024 * 1024 * 1024 + 1;
        }),
        ("selector publish interval", "selector.view_publish_interval", |config| {
            config.selector_publish_interval = Duration::from_millis(1001);
        }),
        ("selector publish precision", "selector.view_publish_interval", |config| {
            config.selector_publish_interval = Duration::from_nanos(1);
        }),
        ("selector sync timeout", "selector.sync_timeout", |config| {
            config.selector_sync_timeout = Duration::from_millis(99);
        }),
        ("selector active bytes", "selector.max_active_bytes", |config| config.selector_max_bytes = 0),
        ("selector retained bytes", "selector.max_retained_bytes", |config| {
            config.selector_retained_bytes = Some(1024 * 1024 * 1024 + 1);
        }),
        ("clock refresh", "selector.clock_refresh_interval", |config| {
            config.clock_refresh = Duration::from_millis(999);
        }),
        ("clock uncertainty", "selector.clock_uncertainty", |config| {
            config.clock_uncertainty = Duration::from_millis(1001);
        }),
        ("selector error buffer", "selector.error_buffer_capacity", |config| {
            config.selector_error_buffer_capacity = 1025;
        }),
        ("selector recovery initial", "selector.recovery.initial_delay", |config| {
            config.selector_recovery_initial_delay = Duration::from_millis(9);
        }),
        ("selector recovery maximum", "selector.recovery.max_delay", |config| {
            config.selector_recovery_max_delay = Duration::from_millis(99);
        }),
        ("selector recovery multiplier", "selector.recovery.multiplier", |config| {
            config.selector_recovery_multiplier = 9;
        }),
        ("selector recovery jitter", "selector.recovery.jitter_percent", |config| {
            config.selector_recovery_jitter_percent = 51;
        }),
        ("selector recovery relation", "selector.recovery.initial_delay", |config| {
            config.selector_recovery_initial_delay = Duration::from_secs(2);
            config.selector_recovery_max_delay = Duration::from_secs(1);
        }),
    ];
    for (_name, field, mutate) in cases {
        let mut config = Config::new("Alpha");
        mutate(&mut config);
        assert_invalid_field(&config, field);
    }
}

#[test]
fn zone_configuration_rejects_missing_noncanonical_and_above_ceiling() {
    let cases = [
        (1, Value::Null, Code::Missing),
        (1, "016".into(), Code::Invalid),
        (1, "0".into(), Code::Invalid),
        (0, "v2".into(), Code::Protocol),
        (4, "16385".into(), Code::Capacity),
        (5, "16385".into(), Code::Capacity),
    ];
    for (index, replacement, expected) in cases {
        let Value::Array(mut values) = defaults() else {
            panic!("test fixture must be an array");
        };
        values[index] = replacement;
        assert!(matches!(
            parse_zone_config(Value::Array(values)),
            Err(error) if error.code() == expected
        ));
    }
}
