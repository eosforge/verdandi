use super::*;

#[test]
fn fixed_scalars_use_canonical_text() {
    for source in [b"".as_slice(), b"+1", b"01", b"-0", b" 1"] {
        assert!(matches!(i64::decode_value(source), Err(error) if error.code() == Code::Corrupt));
    }
    assert_eq!(i64::decode_value(b"-9223372036854775808"), Ok(i64::MIN));
    assert_eq!(u64::decode_value(b"18446744073709551615"), Ok(u64::MAX));
    assert_eq!(bool::decode_value(b"0"), Ok(false));
    assert_eq!(bool::decode_value(b"1"), Ok(true));
    assert!(matches!(bool::decode_value(b"true"), Err(error) if error.code() == Code::Corrupt));

    let mut encoded = Vec::new();
    assert!(i64::MIN.encode_value(&mut encoded).is_ok());
    assert_eq!(encoded, b"-9223372036854775808");
}

#[test]
fn bytes_and_strings_are_owned() {
    let mut source = b"value".to_vec();
    let decoded = Vec::<u8>::decode_value(&source).unwrap_or_default();
    source[0] = b'X';
    assert_eq!(decoded, b"value");
    assert_eq!(String::decode_value(b"text"), Ok("text".to_owned()));
    assert!(matches!(String::decode_value(&[0xff]), Err(error) if error.code() == Code::Corrupt));
}
