use super::*;
use std::path::PathBuf;

#[test]
fn parses_success_and_protocol_error() {
    let reply = parse_registration_reply(Value::Array(vec![
        "&result".into(),
        "ok".into(),
        "@revision".into(),
        Value::Integer(4),
        "@timestamp".into(),
        Value::Integer(9),
    ]));
    assert_eq!(reply.map(|value| (value.revision, value.timestamp)), Ok((4, 9)));

    let error = parse_registration_reply(Value::Array(vec![
        "&result".into(),
        "error".into(),
        "&status".into(),
        "stale".into(),
        "@revision".into(),
        Value::Integer(7),
    ]));
    assert!(matches!(
        error,
        Err(error) if error.code() == Code::Stale && error.revision() == Some(7)
    ));
}

#[test]
fn arguments_are_deterministic_and_embedded_scripts_match_canonical_sources() {
    let uuid = "0123456789abcdef0123456789abcdef";
    let attr = crate::Fields::from([("z".to_owned(), b"last".to_vec()), ("a".to_owned(), b"first".to_vec())]);
    let data = crate::Fields::from([("load".to_owned(), b"1".to_vec()), ("address".to_owned(), b"127.0.0.1".to_vec())]);
    let arguments = register_arguments(uuid, 7, 15_000, 3, &attr, &data);
    assert_eq!(
        arguments,
        vec![
            uuid.into(),
            "7".into(),
            "15000".into(),
            "3".into(),
            ".a".into(),
            Value::Bytes(b"first".to_vec().into()),
            ".z".into(),
            Value::Bytes(b"last".to_vec().into()),
            "address".into(),
            Value::Bytes(b"127.0.0.1".to_vec().into()),
            "load".into(),
            Value::Bytes(b"1".to_vec().into()),
        ]
    );
    assert_eq!(
        update_arguments(uuid, 8, None, &crate::Fields::from([("load".into(), b"2".to_vec())]),),
        vec![uuid.into(), "8".into(), "".into(), "load".into(), Value::Bytes(b"2".to_vec().into()),]
    );
    assert_eq!(
        update_arguments(uuid, 9, Some(4), &crate::Fields::new()),
        vec![uuid.into(), "9".into(), "4".into()]
    );
    assert_eq!(renew_arguments(uuid, 9), vec![uuid.into(), "9".into()]);
    assert_eq!(unregister_arguments(uuid), vec![uuid.into()]);

    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..").join("..").join("lua").join("registration");
    let scripts = [
        ("register", REGISTRATION_REGISTER_LUA),
        ("update", REGISTRATION_UPDATE_LUA),
        ("renew", REGISTRATION_RENEW_LUA),
        ("unregister", REGISTRATION_UNREGISTER_LUA),
    ];
    if root.exists() {
        for (kind, embedded) in scripts {
            let canonical = std::fs::read_to_string(root.join(format!("{kind}.lua")));
            assert!(matches!(canonical, Ok(value) if value == embedded));
        }
    }
}

#[test]
fn duplicate_reply_fields_and_noncanonical_numbers_are_corrupt() {
    let duplicate = Value::Array(vec!["&result".into(), "ok".into(), "&result".into(), "ok".into()]);
    assert!(matches!(
        parse_registration_reply(duplicate),
        Err(error) if error.code() == Code::Corrupt
    ));
    assert_eq!(value_u64("01".into()), Some(1));
    assert_eq!(value_u64(Value::Integer(0)), None);
    assert_eq!(value_u64(Value::Integer(-1)), None);
}
