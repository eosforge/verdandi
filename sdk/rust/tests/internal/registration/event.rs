use super::*;

fn encode(value: &MessageValue) -> Vec<u8> {
    let mut output = Vec::new();
    let result = rmpv::encode::write_value(&mut output, value);
    assert!(result.is_ok());
    output
}

#[test]
fn decodes_binary_register_and_rejects_duplicates() {
    let event = MessageValue::Array(vec![
        "&protocol".into(),
        "v1".into(),
        "&kind".into(),
        "register".into(),
        "@uuid".into(),
        "0123456789abcdef0123456789abcdef".into(),
        "@revision".into(),
        1_u64.into(),
        "@timestamp".into(),
        2_u64.into(),
        "@ttl".into(),
        3_u64.into(),
        "@version".into(),
        4_u64.into(),
        ".role".into(),
        MessageValue::Binary(vec![0, 255]),
        "load".into(),
        MessageValue::Binary(vec![1, 2]),
    ]);
    let decoded = decode_registration_event(&encode(&event), &ZoneConfig::default());
    assert!(matches!(decoded, Ok(value) if value.attr["role"] == [0, 255] && value.data["load"] == [1, 2]));

    let duplicate = MessageValue::Array(vec!["&protocol".into(), "v1".into(), "&protocol".into(), "v1".into()]);
    assert!(matches!(
        decode_registration_event(&encode(&duplicate), &ZoneConfig::default()),
        Err(error) if error.code() == Code::Contract
    ));

    for duplicate in [
        MessageValue::Array(vec!["&future".into(), 1_u64.into(), "&future".into(), 2_u64.into()]),
        MessageValue::Array(vec![".role".into(), "a".into(), ".role".into(), "b".into()]),
        MessageValue::Array(vec!["load".into(), "1".into(), "load".into(), "2".into()]),
    ] {
        assert!(matches!(
            decode_registration_event(&encode(&duplicate), &ZoneConfig::default()),
            Err(error) if error.code() == Code::Contract
        ));
    }
}

#[test]
fn malformed_kinds_trailing_bytes_and_oversized_events_are_rejected() {
    let cases = [
        MessageValue::Array(vec!["&protocol".into(), "v1".into(), "&kind".into()]),
        MessageValue::Array(vec![
            "&protocol".into(),
            "v1".into(),
            "&kind".into(),
            "renew".into(),
            "@uuid".into(),
            "0123456789abcdef0123456789abcdef".into(),
            "@revision".into(),
            1_u64.into(),
            "@timestamp".into(),
            2_u64.into(),
            "load".into(),
            MessageValue::Binary(vec![1]),
        ]),
        MessageValue::Array(vec![
            "&protocol".into(),
            "v1".into(),
            "&kind".into(),
            "unknown".into(),
            "@uuid".into(),
            "0123456789abcdef0123456789abcdef".into(),
        ]),
    ];
    for value in cases {
        assert!(decode_registration_event(&encode(&value), &ZoneConfig::default()).is_err());
    }
    let mut trailing = encode(&MessageValue::Array(vec![
        "&protocol".into(),
        "v1".into(),
        "&kind".into(),
        "unregister".into(),
        "@uuid".into(),
        "0123456789abcdef0123456789abcdef".into(),
    ]));
    trailing.push(0);
    assert!(matches!(
        decode_registration_event(&trailing, &ZoneConfig::default()),
        Err(error) if error.code() == Code::Corrupt
    ));
    assert!(matches!(
        decode_registration_event(&vec![0; MAX_EVENT_BYTES + 1], &ZoneConfig::default()),
        Err(error) if error.code() == Code::Capacity
    ));
}

#[test]
fn container_values_and_excessive_declared_elements_are_rejected_before_expansion() {
    let nested_map = b"\x96\xa9&protocol\xa2v1\xa500000\xdf0000000000\xa5@uuid\xd9 00000000000000'000otoc0000000000";
    assert!(matches!(
        decode_registration_event(nested_map, &ZoneConfig::default()),
        Err(error) if error.code() == Code::Corrupt
    ));

    let excessive_array = [0xdd, 0x00, 0x00, 0x02, 0x10];
    assert!(matches!(
        decode_registration_event(&excessive_array, &ZoneConfig::default()),
        Err(error) if error.code() == Code::Capacity
    ));

    let impossible_binary = [0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0];
    let mut cursor = Cursor::new(impossible_binary.as_slice());
    assert!(read_event_value(&mut cursor, is_string_marker).is_err());
    assert_eq!(cursor.position(), 0);
}

#[test]
fn every_marker_and_short_suffix_is_bounded() {
    for marker in 0_u8..=u8::MAX {
        for suffix in 0..=8 {
            let mut payload = vec![0x92, 0xa1, b'x', marker];
            payload.resize(payload.len() + suffix, 0);
            let _ = decode_registration_event(&payload, &ZoneConfig::default());
        }
    }
}

#[test]
fn unknown_scalar_controls_are_ignored() {
    let values = [
        MessageValue::Nil,
        MessageValue::Boolean(true),
        (-1_i64).into(),
        MessageValue::F64(1.5),
        "future".into(),
        MessageValue::Binary(vec![0, 255]),
    ];
    for value in values {
        let event = MessageValue::Array(vec![
            "&protocol".into(),
            "v1".into(),
            "&kind".into(),
            "unregister".into(),
            "@uuid".into(),
            "0123456789abcdef0123456789abcdef".into(),
            "&future".into(),
            value,
        ]);
        assert!(decode_registration_event(&encode(&event), &ZoneConfig::default()).is_ok());
    }
}

#[test]
fn stored_hash_preserves_binary_fields_and_requires_canonical_meta() {
    let uuid = "0123456789abcdef0123456789abcdef";
    let value = RedisValue::Array(vec![
        "@uuid".into(),
        uuid.into(),
        "@revision".into(),
        "7".into(),
        "@timestamp".into(),
        "100".into(),
        "@ttl".into(),
        "50".into(),
        "@version".into(),
        "3".into(),
        ".role".into(),
        RedisValue::Bytes(vec![0, 255].into()),
        "payload".into(),
        RedisValue::Bytes(vec![1, 2, 3].into()),
    ]);
    let parsed = parse_stored_record(uuid, value, &ZoneConfig::default());
    assert!(matches!(
        parsed,
        Ok(Some(record))
            if record.meta.revision == 7
                && record.deadline == 150
                && record.attr["role"] == [0, 255]
                && record.data["payload"] == [1, 2, 3]
    ));

    let noncanonical = RedisValue::Array(vec![
        "@uuid".into(),
        uuid.into(),
        "@revision".into(),
        "07".into(),
        "@timestamp".into(),
        "100".into(),
        "@ttl".into(),
        "50".into(),
        "@version".into(),
        "3".into(),
    ]);
    assert!(matches!(
        parse_stored_record(uuid, noncanonical, &ZoneConfig::default()),
        Err(error) if error.code() == Code::Corrupt
    ));
}
