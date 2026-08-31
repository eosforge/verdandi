use crate::error::{Code, Error, Result};

/// 从精确存储字节解码一个完整 Redis String 或 Hash 字段。
pub trait DecodeValue: Sized {
    /// 解码一个确定存在的完整 `source`；缺失键或字段不会调用此方法。
    fn decode_value(source: &[u8]) -> Result<Self>;
}

/// 把一个完整 Redis String 或 Hash 字段编码进初始为空的目标缓冲区。
pub trait EncodeValue {
    /// 向 `destination` 追加精确存储表示，且不得保留该缓冲区。
    fn encode_value(&self, destination: &mut Vec<u8>) -> Result<()>;
}

impl DecodeValue for Vec<u8> {
    /// 深拷贝 `source`，返回调用方独占的原始字节。
    fn decode_value(source: &[u8]) -> Result<Self> {
        Ok(source.to_vec())
    }
}

impl EncodeValue for Vec<u8> {
    /// 把当前字节完整追加到 `destination`。
    fn encode_value(&self, destination: &mut Vec<u8>) -> Result<()> {
        self.as_slice().encode_value(destination)
    }
}

impl EncodeValue for [u8] {
    /// 把当前字节切片完整追加到 `destination`。
    fn encode_value(&self, destination: &mut Vec<u8>) -> Result<()> {
        destination.extend_from_slice(self);
        Ok(())
    }
}

impl DecodeValue for String {
    /// 要求 `source` 为有效 UTF-8，并构造拥有型字符串。
    fn decode_value(source: &[u8]) -> Result<Self> {
        String::from_utf8(source.to_vec()).map_err(|_| Error::field(Code::Corrupt, "value"))
    }
}

impl EncodeValue for String {
    /// 把字符串的 UTF-8 字节追加到 `destination`。
    fn encode_value(&self, destination: &mut Vec<u8>) -> Result<()> {
        self.as_str().encode_value(destination)
    }
}

impl EncodeValue for str {
    /// 把字符串切片的 UTF-8 字节追加到 `destination`。
    fn encode_value(&self, destination: &mut Vec<u8>) -> Result<()> {
        destination.extend_from_slice(self.as_bytes());
        Ok(())
    }
}

impl DecodeValue for bool {
    /// 只接受规范单字节 `0` 或 `1`，其他表示返回 `Corrupt`。
    fn decode_value(source: &[u8]) -> Result<Self> {
        match source {
            b"0" => Ok(false),
            b"1" => Ok(true),
            _ => Err(Error::field(Code::Corrupt, "value")),
        }
    }
}

impl EncodeValue for bool {
    /// 把布尔值编码为规范单字节 `0` 或 `1`。
    fn encode_value(&self, destination: &mut Vec<u8>) -> Result<()> {
        destination.push(if *self { b'1' } else { b'0' });
        Ok(())
    }
}

macro_rules! integer_value {
    ($signed:literal; $($integer:ty),+ $(,)?) => {
        $(
            impl DecodeValue for $integer {
                /// 从规范十进制字节解码固定宽度整数，并拒绝符号或前导零歧义。
                fn decode_value(source: &[u8]) -> Result<Self> {
                    if !canonical_integer(source, $signed) {
                        return Err(Error::field(Code::Corrupt, "value"));
                    }
                    std::str::from_utf8(source)
                        .map_err(|_| Error::field(Code::Corrupt, "value"))?
                        .parse::<$integer>()
                        .map_err(|_| Error::field(Code::Corrupt, "value"))
                }
            }

            impl EncodeValue for $integer {
                /// 把固定宽度整数编码为规范十进制字节。
                fn encode_value(&self, destination: &mut Vec<u8>) -> Result<()> {
                    let mut buffer = itoa::Buffer::new();
                    destination.extend_from_slice(buffer.format(*self).as_bytes());
                    Ok(())
                }
            }
        )+
    };
}

integer_value!(true; i8, i16, i32, i64);
integer_value!(false; u8, u16, u32, u64);

/// 判断 `source` 是否为带符号或无符号整数允许的规范十进制形式。
///
/// `signed` 为 false 时拒绝负号；所有形式都拒绝空值、多余前导零和单独负号。
fn canonical_integer(source: &[u8], signed: bool) -> bool {
    if source == b"0" {
        return true;
    }
    let digits = if signed && source.first() == Some(&b'-') { &source[1..] } else { source };
    !digits.is_empty() && matches!(digits[0], b'1'..=b'9') && digits[1..].iter().all(u8::is_ascii_digit)
}

#[cfg(test)]
#[path = "../../tests/internal/redis/value.rs"]
mod tests;
