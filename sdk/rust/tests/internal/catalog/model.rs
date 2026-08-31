use super::*;

#[test]
fn status_transition_shares_complete_fields() {
    let fields = Arc::new(Fields::from([("value".to_owned(), b"ready".to_vec())]));
    let state = RawState {
        revision: 3,
        replace_revision: 3,
        status: Status::Present,
        kind: Some(Kind::Value),
        encoded_bytes: 10,
        fields: Arc::clone(&fields),
    };
    let synchronizing = state.with_status(Status::Synchronizing);
    assert!(Arc::ptr_eq(&fields, &synchronizing.fields));
    assert_eq!(synchronizing.status, Status::Synchronizing);
}

#[test]
fn array_validation_is_canonical_without_index_allocation() {
    let fields = (0..512).map(|index| (index.to_string(), Vec::new())).collect::<Fields>();
    assert_eq!(validate_value(Kind::Array, &fields, 4096), Ok(1426));
    assert!(validate_value(Kind::Array, &Fields::from([("00".to_owned(), Vec::new())]), 4096,).is_err());
}
