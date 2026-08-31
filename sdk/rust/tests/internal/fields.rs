use super::*;

const UUID: &str = "0123456789abcdef0123456789abcdef";

#[test]
fn independent_default_limits_accept_sixteen_attr_and_thirty_two_data_fields() {
    let limits = ZoneConfig::default();
    let attr = (0..16).map(|index| (format!("a{index}"), vec![b'a'; 128])).collect();
    let data = (0..32).map(|index| (format!("d{index}"), vec![b'd'; 128])).collect();
    assert!(validate_record(UUID, 1, 15_000, 1, &attr, &data, &limits).is_ok());

    let mut attr_overflow = attr.clone();
    attr_overflow.insert("overflow".to_owned(), vec![1]);
    assert!(matches!(
        validate_record(UUID, 1, 15_000, 1, &attr_overflow, &data, &limits),
        Err(error) if error.code() == Code::Capacity && error.field_name() == Some("attr")
    ));
    let mut data_value_overflow = data;
    data_value_overflow.insert("d0".to_owned(), vec![0; 129]);
    assert!(matches!(
        validate_record(UUID, 1, 15_000, 1, &attr, &data_value_overflow, &limits),
        Err(error) if error.code() == Code::Capacity && error.field_name() == Some("d0")
    ));
}

#[test]
fn reserved_names_safe_integer_bounds_and_uuid_are_rejected() {
    let limits = ZoneConfig::default();
    for name in ["", "@meta", "&control", ".stored"] {
        assert!(matches!(
            validate_field(name, b"value", 64, 128),
            Err(error) if error.code() == Code::Invalid
        ));
    }
    assert!(matches!(
        validate_record(UUID, 0, 1, 1, &Fields::new(), &Fields::new(), &limits),
        Err(error) if error.code() == Code::Invalid && error.field_name() == Some("@revision")
    ));
    assert!(matches!(
        validate_record(
            UUID,
            1,
            MAX_SAFE_INTEGER,
            MAX_SAFE_INTEGER,
            &Fields::new(),
            &Fields::new(),
            &limits
        ),
        Err(error) if error.code() == Code::Invalid && error.field_name() == Some("@ttl")
    ));
    assert!(validate_record(UUID, 1, MAX_HASH_FIELD_EXPIRE_AT_MS, MAX_SAFE_INTEGER, &Fields::new(), &Fields::new(), &limits).is_ok());
    assert!(valid_uuid(UUID));
    assert!(!valid_uuid("ABCDEF0123456789abcdef0123456789"));
    assert!(!valid_uuid("short"));
}

#[test]
fn decimal_digit_count_covers_safe_integer_boundaries() {
    assert_eq!(decimal_digits(0), 1);
    assert_eq!(decimal_digits(9), 1);
    assert_eq!(decimal_digits(10), 2);
    assert_eq!(decimal_digits(999), 3);
    assert_eq!(decimal_digits(MAX_SAFE_INTEGER), 16);
}
