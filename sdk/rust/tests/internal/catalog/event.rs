use super::*;

#[derive(serde::Deserialize)]
struct EventCorpus {
    events: Vec<EventVector>,
}

#[derive(serde::Deserialize)]
struct EventVector {
    name: String,
    payload_hex: String,
    path: EventPath,
    operation: String,
    revision: u64,
    base_revision: u64,
    kind: String,
    encoded_bytes: usize,
    fields: Vec<EventField>,
}

#[derive(serde::Deserialize)]
struct EventPath {
    part: String,
    id: String,
}

#[derive(serde::Deserialize)]
struct EventField {
    name: String,
    value_hex: String,
}

#[test]
fn decodes_shared_catalog_event_corpus() {
    let source = include_str!(concat!(env!("CARGO_MANIFEST_DIR"), "/../../testkit/conformance/v1/catalog_events.json"));
    let corpus: EventCorpus = serde_json::from_str(source).unwrap_or_else(|error| panic!("invalid conformance corpus: {error}"));
    for vector in corpus.events {
        let payload = hex::decode(&vector.payload_hex).unwrap_or_else(|error| panic!("{} has invalid payload: {error}", vector.name));
        let path = Path::new(&vector.path.part, &vector.path.id).unwrap_or_else(|error| panic!("{} has invalid path: {error}", vector.name));
        let event =
            decode_event(&payload, &path, super::super::model::MAX_RECORD_BYTES).unwrap_or_else(|error| panic!("{} was rejected: {error}", vector.name));
        let operation_matches = matches!(
            (vector.operation.as_str(), event.kind),
            ("replace", EventKind::Replace) | ("patch", EventKind::Patch) | ("delete", EventKind::Delete)
        );
        assert!(operation_matches, "{} operation mismatch", vector.name);
        assert_eq!(event.revision, vector.revision, "{} revision", vector.name);
        assert_eq!(event.base_revision, vector.base_revision, "{} base revision", vector.name);
        assert_eq!(event.encoded_bytes, vector.encoded_bytes, "{} encoded bytes", vector.name);
        if !vector.kind.is_empty() {
            assert_eq!(event.value_kind, Kind::parse(&vector.kind), "{} kind", vector.name);
        }
        assert_eq!(event.fields.len(), vector.fields.len(), "{} field count", vector.name);
        for field in vector.fields {
            let expected = hex::decode(&field.value_hex).unwrap_or_else(|error| panic!("{} field {}: {error}", vector.name, field.name));
            assert_eq!(event.fields.get(&field.name), Some(&expected), "{} field {}", vector.name, field.name);
        }
    }
}

#[test]
fn decoder_rejects_every_truncation_and_preserves_accepted_invariants() {
    let path = match Path::new("routing", "decoder") {
        Ok(path) => path,
        Err(error) => panic!("path construction failed: {error}"),
    };
    let events = [
        rmpv::Value::Array(vec![
            rmpv::Value::from("v1"),
            rmpv::Value::from("replace"),
            rmpv::Value::from(path.member()),
            rmpv::Value::from("1"),
            rmpv::Value::from("map"),
            rmpv::Value::from("4"),
            rmpv::Value::Array(vec![rmpv::Value::from("a"), rmpv::Value::Binary(b"one".to_vec())]),
        ]),
        rmpv::Value::Array(vec![
            rmpv::Value::from("v1"),
            rmpv::Value::from("patch"),
            rmpv::Value::from(path.member()),
            rmpv::Value::from("1"),
            rmpv::Value::from("2"),
            rmpv::Value::from("map"),
            rmpv::Value::from("4"),
            rmpv::Value::Array(vec![rmpv::Value::from("a"), rmpv::Value::Binary(b"two".to_vec())]),
        ]),
        rmpv::Value::Array(vec![
            rmpv::Value::from("v1"),
            rmpv::Value::from("delete"),
            rmpv::Value::from(path.member()),
            rmpv::Value::from("3"),
        ]),
    ];

    for value in events {
        let mut payload = Vec::new();
        if let Err(error) = rmpv::encode::write_value(&mut payload, &value) {
            panic!("notification encoding failed: {error}");
        }
        let event = match decode_event(&payload, &path, 128) {
            Ok(event) => event,
            Err(error) => panic!("valid notification was rejected: {error}"),
        };
        assert_event_invariants(&event, &path);

        for length in 0..payload.len() {
            assert!(decode_event(&payload[..length], &path, 128).is_err());
        }
        let mut trailing = payload.clone();
        trailing.push(0);
        assert!(decode_event(&trailing, &path, 128).is_err());

        for index in 0..payload.len() {
            let mut mutated = payload.clone();
            mutated[index] ^= 0xff;
            if let Ok(event) = decode_event(&mutated, &path, 128) {
                assert_event_invariants(&event, &path);
            }
        }
    }

    let oversized = vec![0; maximum_event_payload(128) + 1];
    assert!(decode_event(&oversized, &path, 128).is_err());
}

fn assert_event_invariants(event: &CatalogEvent, path: &Path) {
    assert_eq!(&event.path, path);
    assert!((1..=super::super::model::MAX_REVISION).contains(&event.revision));
    match event.kind {
        EventKind::Replace => {
            let Some(kind) = event.value_kind else {
                panic!("Replace is missing its value kind");
            };
            assert_eq!(validate_value(kind, event.fields.as_ref(), 128), Ok(event.encoded_bytes));
            assert_eq!(event.base_revision, 0);
        }
        EventKind::Patch => {
            assert!(event.base_revision > 0 && event.base_revision < event.revision);
            assert!(event.value_kind.is_some_and(|kind| kind != Kind::Value));
            assert!(validate_patch(event.fields.as_ref(), 128).is_ok());
        }
        EventKind::Delete => {
            assert_eq!(event.base_revision, 0);
            assert_eq!(event.value_kind, None);
            assert_eq!(event.encoded_bytes, 0);
            assert!(event.fields.is_empty());
        }
    }
}
