use std::path::Path;
use std::time::Duration;

use super::Config;
use crate::Code;

#[derive(serde::Deserialize)]
struct ConfigurationCorpus {
    cases: Vec<ConfigurationCase>,
    raw_cases: Vec<RawConfigurationCase>,
}

#[derive(serde::Deserialize)]
struct ConfigurationCase {
    name: String,
    valid: bool,
    #[serde(default)]
    field: String,
    #[serde(default)]
    code: String,
    document: serde_json::Value,
}

#[derive(serde::Deserialize)]
struct RawConfigurationCase {
    name: String,
    valid: bool,
    #[serde(default)]
    field: String,
    #[serde(default)]
    code: String,
    #[serde(default)]
    source: String,
    #[serde(default)]
    source_hex: String,
}

#[test]
fn shared_configuration_corpus_has_identical_results() {
    let source = include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../testkit/conformance/v1/configuration.json"));
    let corpus: ConfigurationCorpus = serde_json::from_str(source).unwrap_or_else(|error| panic!("invalid conformance corpus: {error}"));
    for case in corpus.cases {
        let document = serde_json::to_vec(&case.document).unwrap_or_else(|error| panic!("{} encoding failed: {error}", case.name));
        assert_conformance(&case.name, case.valid, &case.code, &case.field, &document);
    }
    for case in corpus.raw_cases {
        let source = if case.source_hex.is_empty() {
            case.source.into_bytes()
        } else {
            decode_hex(&case.source_hex).unwrap_or_else(|| panic!("{} has invalid source_hex", case.name))
        };
        assert_conformance(&case.name, case.valid, &case.code, &case.field, &source);
    }
}

fn assert_conformance(name: &str, valid: bool, code: &str, field: &str, source: &[u8]) {
    match Config::from_json(source) {
        Ok(_) if valid => {}
        Ok(_) => panic!("{name} unexpectedly succeeded"),
        Err(error) if !valid => {
            assert_eq!(error.code().as_str(), code, "{name} code");
            assert_eq!(error.field_name(), Some(field), "{name} field");
        }
        Err(error) => panic!("{name} unexpectedly failed: {error}"),
    }
}

fn decode_hex(source: &str) -> Option<Vec<u8>> {
    let mut output = Vec::with_capacity(source.len() / 2);
    let mut digits = source.as_bytes().chunks_exact(2);
    for pair in &mut digits {
        let high = (pair[0] as char).to_digit(16)?;
        let low = (pair[1] as char).to_digit(16)?;
        output.push(u8::try_from((high << 4) | low).ok()?);
    }
    digits.remainder().is_empty().then_some(output)
}

#[test]
fn loads_shared_example() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join("..").join("..").join("configuration.example.json");
    let config = Config::load_json(path).unwrap_or_else(|error| panic!("{error}"));
    let redis = config.redis_config().unwrap_or_else(|error| panic!("{error}"));
    assert!(redis.endpoint.starts_with("redis://127.0.0.1:6379"));
    assert_eq!(redis.timeout, Duration::from_secs(2));
    let registration = config
        .registration_config()
        .unwrap_or_else(|error| panic!("{error}"))
        .unwrap_or_else(|| panic!("missing Registration config"));
    assert_eq!(registration.zone, "Alpha");
    assert_eq!(registration.policy.data_max_fields, 32);
    let catalog = config
        .catalog_config()
        .unwrap_or_else(|error| panic!("{error}"))
        .unwrap_or_else(|| panic!("missing Catalog config"));
    assert_eq!(catalog.zone, "Alpha");
    assert_eq!(catalog.max_record_bytes, 512 * 1024);
}

#[test]
fn rejects_noncanonical_json() {
    let cases = [
        (
            "unknown",
            r#"{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"future":true}}"#,
            "json",
        ),
        (
            "duplicate",
            r#"{"version":"v1","version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"]}}"#,
            "json",
        ),
        (
            "trailing",
            r#"{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"]}} {}"#,
            "json",
        ),
        (
            "null",
            r#"{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"timeout_ms":null}}"#,
            "json",
        ),
        (
            "version",
            r#"{"version":"v2","redis":{"mode":"standalone","addresses":["localhost:6379"]}}"#,
            "version",
        ),
        (
            "explicit zero timeout",
            r#"{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"timeout_ms":0}}"#,
            "redis.timeout_ms",
        ),
        (
            "unbracketed IPv6",
            r#"{"version":"v1","redis":{"mode":"standalone","addresses":["::1:6379"]}}"#,
            "redis.addresses",
        ),
    ];
    for (name, source, field) in cases {
        let error = Config::from_json(source.as_bytes()).err().unwrap_or_else(|| panic!("{name} succeeded"));
        assert_eq!(error.code(), Code::Invalid, "{name}: {error}");
        assert_eq!(error.field_name(), Some(field), "{name}: {error}");
    }
}

#[test]
fn preserves_explicit_zero() {
    let config = Config::from_json(
        br#"{
            "version":"v1",
            "redis":{"mode":"standalone","addresses":["localhost:6379"],"reconnect":{"delay_ms":10}},
            "registration":{"zone":"Alpha","selector":{"view_publish_interval_ms":0,"max_retained_bytes":0,"clock_uncertainty_ms":0}}
        }"#,
    )
    .unwrap_or_else(|error| panic!("{error}"));
    let redis = config.redis_config().unwrap_or_else(|error| panic!("{error}"));
    assert_eq!(redis.reconnect.delay, Duration::from_millis(10));
    let registration = config
        .registration_config()
        .unwrap_or_else(|error| panic!("{error}"))
        .unwrap_or_else(|| panic!("missing Registration config"));
    assert_eq!(registration.selector_publish_interval, Duration::ZERO);
    assert_eq!(registration.selector_retained_bytes, Some(0));
    assert_eq!(registration.clock_uncertainty, Duration::ZERO);
}

#[test]
fn converts_sentinel_topology() {
    let config = Config::from_json(
        br#"{
            "version":"v1",
            "redis":{
                "mode":"sentinel",
                "addresses":["sentinel-a:26379","[::1]:26379"],
                "master_name":"primary",
                "auth":{"username":"data","password":"secret"},
                "sentinel_auth":{"username":"sentinel","password":"secret"}
            }
        }"#,
    )
    .unwrap_or_else(|error| panic!("{error}"));
    let redis = config.redis_config().unwrap_or_else(|error| panic!("{error}"));
    assert!(redis.endpoint.starts_with("redis-sentinel://data:secret@sentinel-a:26379"));
    assert!(redis.endpoint.contains("sentinelServiceName=primary"));
    assert!(redis.endpoint.contains("node=%5B%3A%3A1%5D%3A26379"));
}

#[test]
fn rejects_oversized_json() {
    let source = vec![b' '; 1024 * 1024 + 1];
    let error = Config::from_json(&source).err().unwrap_or_else(|| panic!("oversized JSON succeeded"));
    assert_eq!(error.code(), Code::Capacity);
    assert_eq!(error.field_name(), Some("json"));
}

#[test]
fn converts_tls_object_without_reading_files() {
    let config = Config::from_json(
        br#"{
            "version":"v1",
            "redis":{
                "mode":"standalone",
                "addresses":["127.0.0.1:6379"],
                "tls":{
                    "enabled":true,
                    "system_roots":false,
                    "server_name":"redis.test",
                    "ca_file":"missing-ca.pem",
                    "cert_file":"missing-client.pem",
                    "key_file":"missing-key.pem"
                }
            }
        }"#,
    )
    .unwrap_or_else(|error| panic!("JSON parsing performed certificate I/O: {error}"));
    let redis = config.redis_config().unwrap_or_else(|error| panic!("{error}"));
    let tls = redis.tls.unwrap_or_else(|| panic!("missing TLS config"));
    assert!(!tls.system_roots);
    assert_eq!(tls.server_name.as_deref(), Some("redis.test"));
    assert_eq!(tls.ca_file.as_deref(), Some(Path::new("missing-ca.pem")));
}

#[test]
fn rejects_invalid_tls_relationships() {
    let cases = [
        (
            "legacy boolean",
            r#"{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"tls":true}}"#,
            "json",
        ),
        (
            "disabled custom field",
            r#"{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"tls":{"server_name":"redis.test"}}}"#,
            "redis.tls",
        ),
        (
            "empty trust set",
            r#"{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"tls":{"enabled":true,"system_roots":false}}}"#,
            "redis.tls.ca_file",
        ),
        (
            "certificate without key",
            r#"{"version":"v1","redis":{"mode":"standalone","addresses":["localhost:6379"],"tls":{"enabled":true,"cert_file":"client.pem"}}}"#,
            "redis.tls.cert_file",
        ),
        (
            "sentinel missing fixed server name",
            r#"{"version":"v1","redis":{"mode":"sentinel","addresses":["localhost:26379"],"master_name":"primary","tls":{"enabled":true}}}"#,
            "redis.tls.server_name",
        ),
    ];
    for (name, source, field) in cases {
        let error = Config::from_json(source.as_bytes()).err().unwrap_or_else(|| panic!("{name} succeeded"));
        assert_eq!(error.code(), Code::Invalid, "{name}: {error}");
        assert_eq!(error.field_name(), Some(field), "{name}: {error}");
    }
}
