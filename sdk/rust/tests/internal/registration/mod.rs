use super::*;

#[test]
fn uuid_and_exact_milliseconds_are_canonical() {
    let uuid = new_uuid();
    assert!(matches!(uuid, Ok(value) if value.len() == 32 && value.bytes().all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())));
    assert_eq!(duration_milliseconds(Duration::from_millis(1)), Ok(1));
    assert_eq!(duration_milliseconds(Duration::from_nanos(1)).map_err(|error| error.code()), Err(Code::Invalid));
    assert_eq!(
        duration_milliseconds(Duration::from_millis(MAX_HASH_FIELD_EXPIRE_AT_MS)),
        Ok(MAX_HASH_FIELD_EXPIRE_AT_MS)
    );
    assert_eq!(
        duration_milliseconds(Duration::from_millis(MAX_HASH_FIELD_EXPIRE_AT_MS + 1)).map_err(|error| error.code()),
        Err(Code::Invalid)
    );
}

#[test]
fn corrupt_and_ambiguous_outcomes_require_full_recovery() {
    assert!(uncertain_registration_outcome(&Error::new(Code::Ambiguous)));
    assert!(uncertain_registration_outcome(&Error::new(Code::Corrupt)));
    assert!(!uncertain_registration_outcome(&Error::new(Code::Contract)));
}

#[test]
fn fields_mailbox_keeps_only_the_latest_value_per_field() {
    let mut mailbox = RegistrationMailbox::default();
    let admission = Arc::new(Semaphore::new(3));
    let Ok(first_permit) = Arc::clone(&admission).try_acquire_owned() else {
        panic!("first permit unavailable");
    };
    let (first, first_result) = oneshot::channel();
    mailbox.merge_update(
        Update {
            version: None,
            data: Fields::from([("power".to_owned(), b"2".to_vec()), ("zone".to_owned(), b"east".to_vec())]),
        },
        first,
        first_permit,
    );
    let Ok(second_permit) = Arc::clone(&admission).try_acquire_owned() else {
        panic!("second permit unavailable");
    };
    let (second, second_result) = oneshot::channel();
    mailbox.merge_update(
        Update {
            version: Some(3),
            data: Fields::from([("power".to_owned(), b"4".to_vec())]),
        },
        second,
        second_permit,
    );
    let Ok(renew_permit) = Arc::clone(&admission).try_acquire_owned() else {
        panic!("renew permit unavailable");
    };
    let (renew, renew_result) = oneshot::channel();
    mailbox.push_renew(renew, renew_permit);
    drop(first_result);
    drop(second_result);
    drop(renew_result);
    assert_eq!(admission.available_permits(), 0);

    let Some(batch) = mailbox.take() else {
        panic!("missing merged batch");
    };
    assert_eq!(batch.version, Some(3));
    assert_eq!(batch.data.get("power").map(Vec::as_slice), Some(b"4".as_slice()));
    assert_eq!(batch.data.get("zone").map(Vec::as_slice), Some(b"east".as_slice()));
    assert_eq!(batch.updates.len(), 2);
    assert_eq!(batch.renews.len(), 1);
    assert!(mailbox.take().is_none());
    drop(batch);
    assert_eq!(admission.available_permits(), 3);
}
