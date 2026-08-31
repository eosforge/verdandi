use super::*;

struct Oversized;

impl EncodeValue for Oversized {
    fn encode_value(&self, destination: &mut Vec<u8>) -> Result<()> {
        destination.resize(MAX_REDIS_VALUE_BYTES + 1, 0);
        Ok(())
    }
}

#[test]
fn typed_encoding_enforces_the_value_ceiling() {
    assert!(matches!(encode(&Oversized), Err(error) if error.code() == Code::Capacity));
}
