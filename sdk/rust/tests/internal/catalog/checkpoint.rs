use super::*;

#[test]
fn checkpoint_state_round_trips_and_rejects_unsafe_revision() {
    let state = RawState {
        revision: 7,
        replace_revision: 3,
        status: Status::Present,
        kind: Some(Kind::Map),
        encoded_bytes: 5,
        fields: Arc::new(crate::Fields::from([("x".to_owned(), b"data".to_vec())])),
    };
    let encoded = match encode_state(&state, 64) {
        Ok(encoded) => encoded,
        Err(error) => panic!("state encoding failed: {error}"),
    };
    let decoded = match decode_state(&encoded, 64) {
        Ok(decoded) => decoded,
        Err(error) => panic!("state decoding failed: {error}"),
    };
    assert_eq!(decoded.revision, 7);
    assert_eq!(decoded.replace_revision, 3);
    assert_eq!(decoded.kind, Some(Kind::Map));
    assert_eq!(decoded.fields.get("x"), Some(&b"data".to_vec()));

    let invalid = RawState {
        revision: MAX_REVISION + 1,
        ..state
    };
    assert!(encode_state(&invalid, 64).is_err());
}

#[test]
fn checkpoint_entry_and_cursor_are_monotonic() {
    let path = std::env::temp_dir().join(format!("verdandi-catalog-monotonic-{}-{}.redb", std::process::id(), fastrand::u64(..)));
    let checkpoint = match Checkpoint::open(&path) {
        Ok(checkpoint) => checkpoint,
        Err(error) => panic!("checkpoint open failed: {error}"),
    };
    let catalog_path = match CatalogPath::new("routing", "monotonic") {
        Ok(path) => path,
        Err(error) => panic!("Catalog path failed: {error}"),
    };
    let newer = RawState {
        revision: 9,
        replace_revision: 7,
        status: Status::Present,
        kind: Some(Kind::Map),
        encoded_bytes: 5,
        fields: Arc::new(crate::Fields::from([("x".to_owned(), b"data".to_vec())])),
    };
    let mut older = newer.clone();
    older.revision = 8;
    assert!(checkpoint.save_entry("Prod", "scope", &catalog_path, &newer, 64).is_ok());
    assert!(checkpoint.save_entry("Prod", "scope", &catalog_path, &older, 64).is_ok());
    assert!(checkpoint.save_cursor("Prod", "scope", 9).is_ok());
    assert!(checkpoint.save_cursor("Prod", "scope", 8).is_ok());
    let (cursor, entries) = match checkpoint.load("Prod", "scope", 64) {
        Ok(value) => value,
        Err(error) => panic!("checkpoint load failed: {error}"),
    };
    assert_eq!(cursor, 9);
    assert_eq!(entries.get(&catalog_path).map(|state| state.revision), Some(9));
    drop(checkpoint);
    if let Err(error) = std::fs::remove_file(&path) {
        panic!("checkpoint cleanup failed: {error}");
    }
}
