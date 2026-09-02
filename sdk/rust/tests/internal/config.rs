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
fn reconnect_delay_uses_exact_milliseconds() {
    let mut config = Config::new("redis://127.0.0.1:6379/0");
    assert!(config.validate().is_ok());
    config.reconnect.delay = Duration::from_nanos(1);
    assert!(matches!(
        config.validate(),
        Err(error) if error.code() == Code::Invalid
    ));
    config.reconnect.delay = Duration::from_secs(31);
    assert!(matches!(
        config.validate(),
        Err(error) if error.code() == Code::Invalid
    ));

    let defaults = Config::new("redis://127.0.0.1:6379");
    assert_eq!(defaults.database, 0);
    assert_eq!(defaults.timeout, Duration::from_secs(2));
    assert_eq!(defaults.connect_timeout, Duration::from_secs(5));
    assert_eq!(defaults.pool.min_connections, 1);
    assert_eq!(defaults.pool.max_connections, 4);
    assert_eq!(defaults.pool.idle_timeout, Duration::from_secs(10));
    assert_eq!(defaults.reconnect.delay, Duration::from_millis(100));
}

#[test]
fn tls_builds_private_roots_client_certificate_and_sni() {
    let fixture = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("..")
        .join("testkit")
        .join("tls");
    let certificate = fixture.join("certificate.pem");
    let mut config = Config::new("redis://127.0.0.1:6379");
    config.tls = Some(TlsConfig {
        system_roots: false,
        server_name: Some("redis.test".to_owned()),
        ca_file: Some(certificate.clone()),
        cert_file: Some(certificate),
        key_file: Some(fixture.join("private-key.pem")),
    });
    config.validate().unwrap_or_else(|error| panic!("{error}"));
    let native = config.fred_config().unwrap_or_else(|error| panic!("{error}"));
    assert!(native.tls.is_some());
    let hosts = native.server.hosts();
    assert_eq!(hosts.len(), 1);
    assert_eq!(hosts[0].tls_server_name.as_deref(), Some("redis.test"));
}

#[test]
fn tls_rejects_invalid_native_relationships_and_requires_sentinel_identity() {
    let mut config = Config::new("redis://127.0.0.1:6379");
    config.tls = Some(TlsConfig {
        system_roots: false,
        ..TlsConfig::default()
    });
    assert_invalid_field(&config, "tls.ca_file");

    let mut sentinel = Config::new("redis-sentinel://127.0.0.1:26379?sentinelServiceName=primary");
    sentinel.tls = Some(TlsConfig::default());
    assert_invalid_field(&sentinel, "tls.server_name");

    sentinel.tls = Some(TlsConfig {
        server_name: Some("redis.test".to_owned()),
        ..TlsConfig::default()
    });
    sentinel.validate().unwrap_or_else(|error| panic!("Sentinel fixed identity failed: {error}"));
    let native = sentinel
        .fred_config()
        .unwrap_or_else(|error| panic!("Sentinel TLS construction failed: {error}"));
    assert!(native.tls.is_some());
    assert!(
        native
            .server
            .hosts()
            .iter()
            .all(|server| server.tls_server_name.as_deref() == Some("redis.test"))
    );

    let mut whitespace = Config::new("redis://127.0.0.1:6379");
    whitespace.tls = Some(TlsConfig {
        server_name: Some("redis test".to_owned()),
        ..TlsConfig::default()
    });
    assert_invalid_field(&whitespace, "tls.server_name");
}

#[cfg(unix)]
#[test]
fn tls_rejects_non_utf8_native_paths() {
    use std::ffi::OsString;
    use std::os::unix::ffi::OsStringExt;

    let mut config = Config::new("redis://127.0.0.1:6379");
    config.tls = Some(TlsConfig {
        ca_file: Some(PathBuf::from(OsString::from_vec(vec![0xff]))),
        ..TlsConfig::default()
    });
    assert_invalid_field(&config, "tls.ca_file");
}

#[test]
fn tls_file_read_is_bounded() {
    let path = std::env::temp_dir().join(format!("verdandi-tls-{}-{}.pem", std::process::id(), fastrand::u64(..)));
    let size = usize::try_from(MAXIMUM_TLS_FILE_BYTES + 1).unwrap_or_else(|_| panic!("TLS test size does not fit usize"));
    std::fs::write(&path, vec![b'x'; size]).unwrap_or_else(|error| panic!("{error}"));
    let mut config = Config::new("redis://127.0.0.1:6379");
    config.tls = Some(TlsConfig {
        system_roots: false,
        ca_file: Some(path.clone()),
        ..TlsConfig::default()
    });
    let error = config.fred_config().err().unwrap_or_else(|| panic!("oversized TLS file succeeded"));
    let _ = std::fs::remove_file(path);
    assert_eq!(error.code(), Code::Capacity);
    assert_eq!(error.field_name(), Some("tls.ca_file"));
}

#[test]
fn transport_config_accepts_inclusive_boundaries() {
    let mut minimum = Config::new("redis://127.0.0.1:6379");
    minimum.timeout = Duration::from_millis(10);
    minimum.connect_timeout = Duration::from_millis(20);
    minimum.pool.min_connections = 1;
    minimum.pool.max_connections = 1;
    minimum.pool.idle_timeout = Duration::from_secs(1);
    minimum.reconnect.delay = Duration::from_millis(10);
    assert!(minimum.validate().is_ok());

    let mut maximum = Config::new("redis://127.0.0.1:6379");
    maximum.database = u8::MAX;
    maximum.timeout = Duration::from_secs(15);
    maximum.connect_timeout = Duration::from_secs(30);
    maximum.pool.min_connections = 1024;
    maximum.pool.max_connections = 1024;
    maximum.pool.idle_timeout = Duration::from_secs(3600);
    maximum.reconnect.delay = Duration::from_secs(30);
    assert!(maximum.validate().is_ok());
}

#[test]
fn transport_config_reports_exact_invalid_field() {
    let cases: [InvalidCase; 10] = [
        ("endpoint", "endpoint", |config| config.endpoint = " ".into()),
        ("timeout below range", "timeout", |config| config.timeout = Duration::from_millis(9)),
        ("timeout precision", "timeout", |config| config.timeout = Duration::from_nanos(10_000_001)),
        ("connect timeout", "connect_timeout", |config| {
            config.connect_timeout = Duration::from_millis(19)
        }),
        ("pool idle timeout", "pool.idle_timeout", |config| {
            config.pool.idle_timeout = Duration::from_millis(999)
        }),
        ("pool minimum", "pool.min_connections", |config| config.pool.min_connections = 0),
        ("pool maximum", "pool.max_connections", |config| config.pool.max_connections = 1025),
        ("pool relation", "pool.min_connections", |config| {
            config.pool.min_connections = 5;
            config.pool.max_connections = 4;
        }),
        ("reconnect delay", "reconnect.delay", |config| {
            config.reconnect.delay = Duration::from_millis(9);
        }),
        ("reconnect precision", "reconnect.delay", |config| {
            config.reconnect.delay = Duration::from_nanos(10_000_001);
        }),
    ];
    for (_name, field, mutate) in cases {
        let mut config = Config::new("redis://127.0.0.1:6379");
        mutate(&mut config);
        assert_invalid_field(&config, field);
    }
}
