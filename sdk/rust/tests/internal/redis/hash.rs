use super::*;

#[test]
fn static_field_names_are_unique_and_aggregate_bounded() {
    assert!(matches!(validate_hash_names(&[]), Err(error) if error.code() == Code::Contract));
    assert!(matches!(validate_hash_names(&["same", "same"]), Err(error) if error.code() == Code::Contract));

    let count = super::super::MAX_REDIS_HASH_BYTES / super::super::MAX_REDIS_FIELD_NAME_BYTES + 1;
    let names = (0..count)
        .map(|index| {
            let prefix = format!("{index:04}");
            let suffix_length = super::super::MAX_REDIS_FIELD_NAME_BYTES - prefix.len();
            prefix + &"x".repeat(suffix_length)
        })
        .collect::<Vec<_>>();
    let fields = names.iter().map(String::as_str).collect::<Vec<_>>();
    assert!(matches!(validate_hash_names(&fields), Err(error) if error.code() == Code::Capacity));
}
