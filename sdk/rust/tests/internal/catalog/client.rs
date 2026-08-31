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

#[test]
fn catalog_config_requires_one_valid_zone() {
    let config = Config::new("Alpha");
    assert!(config.validate().is_ok());
    assert_eq!(config.sync_timeout, Duration::from_secs(30));
    assert_eq!(config.scan_page_size, 256);
    assert_eq!(config.max_inflight_reads, 32);
    assert_eq!(config.event_buffer_capacity, 256);
    assert_eq!(config.error_buffer_capacity, 64);
    assert_eq!(config.max_view_bytes, 0);
    assert_eq!(config.max_record_bytes, 512 * 1024);
    assert_eq!(config.recovery_initial_delay, Duration::from_millis(250));
    assert_eq!(config.recovery_max_delay, Duration::from_secs(5));
    assert_eq!(config.recovery_multiplier, 2);
    assert_eq!(config.recovery_jitter_percent, 10);
    assert_eq!(Config::new("alpha-1").validate(), Err(Error::field(Code::Invalid, "zone")));
}

#[test]
fn catalog_config_accepts_inclusive_boundaries() {
    let mut minimum = Config::new("Alpha");
    minimum.sync_timeout = Duration::from_millis(100);
    minimum.scan_page_size = 1;
    minimum.max_inflight_reads = 1;
    minimum.event_buffer_capacity = 1;
    minimum.error_buffer_capacity = 1;
    minimum.max_record_bytes = 1;
    minimum.recovery_initial_delay = Duration::from_millis(10);
    minimum.recovery_max_delay = Duration::from_millis(100);
    minimum.recovery_multiplier = 1;
    minimum.recovery_jitter_percent = 0;
    assert!(minimum.validate().is_ok());

    let mut maximum = Config::new("Alpha");
    maximum.sync_timeout = Duration::from_secs(3600);
    maximum.scan_page_size = 4096;
    maximum.max_inflight_reads = 256;
    maximum.event_buffer_capacity = 65_536;
    maximum.error_buffer_capacity = 4096;
    maximum.max_view_bytes = 64 * 1024 * 1024 * 1024;
    maximum.max_record_bytes = MAX_RECORD_BYTES;
    maximum.recovery_initial_delay = Duration::from_secs(5);
    maximum.recovery_max_delay = Duration::from_secs(30);
    maximum.recovery_multiplier = 8;
    maximum.recovery_jitter_percent = 50;
    assert!(maximum.validate().is_ok());
}

#[test]
fn catalog_config_reports_exact_invalid_field() {
    let cases: &[InvalidCase] = &[
        ("zone", "zone", |config| config.zone = "a:bad".into()),
        ("sync timeout", "catalog.sync_timeout", |config| config.sync_timeout = Duration::from_millis(99)),
        ("scan page", "catalog.scan_page_size", |config| config.scan_page_size = 4097),
        ("inflight reads", "catalog.max_inflight_reads", |config| config.max_inflight_reads = 257),
        ("event buffer", "catalog.event_buffer_capacity", |config| {
            config.event_buffer_capacity = 65_537;
        }),
        ("error buffer", "catalog.error_buffer_capacity", |config| config.error_buffer_capacity = 4097),
        ("view bytes", "catalog.max_view_bytes", |config| {
            config.max_view_bytes = 64 * 1024 * 1024 * 1024 + 1;
        }),
        ("record bytes", "catalog.max_record_bytes", |config| {
            config.max_record_bytes = MAX_RECORD_BYTES + 1
        }),
        ("recovery initial", "catalog.recovery.initial_delay", |config| {
            config.recovery_initial_delay = Duration::from_millis(9);
        }),
        ("recovery maximum", "catalog.recovery.max_delay", |config| {
            config.recovery_max_delay = Duration::from_millis(99);
        }),
        ("recovery multiplier", "catalog.recovery.multiplier", |config| config.recovery_multiplier = 9),
        ("recovery jitter", "catalog.recovery.jitter_percent", |config| {
            config.recovery_jitter_percent = 51;
        }),
        ("recovery relation", "catalog.recovery.initial_delay", |config| {
            config.recovery_initial_delay = Duration::from_secs(2);
            config.recovery_max_delay = Duration::from_secs(1);
        }),
        ("local store path", "catalog.local_store_path", |config| {
            config.local_store_path = Some(PathBuf::new());
        }),
    ];
    for (_name, field, mutate) in cases {
        let mut config = Config::new("Alpha");
        mutate(&mut config);
        assert_invalid_field(&config, field);
    }
}
