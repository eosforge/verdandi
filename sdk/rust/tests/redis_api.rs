use verdandi::{Code, DecodeValue, EncodeValue, Error, Fields, HashValue, Result};

#[derive(Debug, Default, Eq, PartialEq, HashValue)]
struct DerivedHash {
    #[redis(name = "@revision")]
    revision: u64,
    name: String,
    enabled: bool,
    #[redis(skip)]
    local_only: String,
}

#[derive(Debug, Default, Eq, PartialEq)]
struct ManualHash {
    value: String,
}

impl HashValue for ManualHash {
    const FIELDS: &'static [&'static str] = &["value"];

    fn decode_hash(values: &[Option<Vec<u8>>]) -> Result<Self> {
        if values.len() != 1 {
            return Err(Error::field(Code::Corrupt, "hash"));
        }
        Ok(Self {
            value: values[0].as_deref().map(String::decode_value).transpose()?.unwrap_or_default(),
        })
    }

    fn encode_hash(&self, destination: &mut Fields) -> Result<()> {
        let mut encoded = Vec::new();
        self.value.encode_value(&mut encoded)?;
        let _ = destination.insert("value".to_owned(), encoded);
        Ok(())
    }
}

#[test]
fn derive_uses_exact_fields_and_defaults_missing_values() -> Result<()> {
    assert_eq!(DerivedHash::FIELDS, &["@revision", "name", "enabled"]);
    let decoded = DerivedHash::decode_hash(&[Some(b"7".to_vec()), Some(b"north".to_vec()), None])?;
    assert_eq!(
        decoded,
        DerivedHash {
            revision: 7,
            name: "north".to_owned(),
            enabled: false,
            local_only: String::new(),
        }
    );

    let mut fields = Fields::new();
    decoded.encode_hash(&mut fields)?;
    assert_eq!(fields.get("@revision").map(Vec::as_slice), Some(b"7".as_slice()));
    assert_eq!(fields.get("name").map(Vec::as_slice), Some(b"north".as_slice()));
    assert_eq!(fields.get("enabled").map(Vec::as_slice), Some(b"0".as_slice()));
    assert!(!fields.contains_key("local_only"));
    Ok(())
}

#[test]
fn manual_hash_value_remains_a_first_class_option() -> Result<()> {
    let value = ManualHash::decode_hash(&[Some(b"manual".to_vec())])?;
    assert_eq!(value.value, "manual");
    let mut fields = Fields::new();
    value.encode_hash(&mut fields)?;
    assert_eq!(fields.get("value").map(Vec::as_slice), Some(b"manual".as_slice()));
    Ok(())
}
