use std::collections::BTreeMap;

use crate::error::{Code, Error, Result};
use crate::registration::config::ZoneConfig;

pub(crate) const MAX_SAFE_INTEGER: u64 = (1_u64 << 53) - 1;
pub(crate) const MAX_HASH_FIELD_EXPIRE_AT_MS: u64 = (1_u64 << 46) - 1;

/// 一个编码为不透明字段值的固定顶层 Attr 或 Data 结构。
/// 零长度字节是合法值，不表示删除操作。
pub type Fields = BTreeMap<String, Vec<u8>>;

/// 应用负责实现的固定顶层结构与 Verdandi 独立字段之间的转换契约。
pub trait FieldValue: Sized {
    /// 把全部顶层字段编码进初始为空的 `dst`；失败不得留下可发布的部分结构。
    fn encode_fields(&self, dst: &mut Fields) -> Result<()>;

    /// 从完整且与 SDK 状态脱离的 `src` 解码一个新值。
    fn decode_fields(src: &Fields) -> Result<Self>;
}

impl FieldValue for Fields {
    /// 深拷贝当前 map 的字段名和值到 `dst`，使调用方后续修改不产生别名。
    fn encode_fields(&self, dst: &mut Fields) -> Result<()> {
        dst.extend(self.iter().map(|(name, value)| (name.clone(), value.clone())));
        Ok(())
    }

    /// 深拷贝完整 `src`，返回调用方独占的原始字段结构。
    fn decode_fields(src: &Fields) -> Result<Self> {
        Ok(src.clone())
    }
}

/// 调用 `value` 的 `FieldValue` 编码器并为错误补充 `field` 上下文。
///
/// 返回一份新建的完整字段 map；应用编码器失败时不发布部分结果。
pub(crate) fn encode_value<T: FieldValue>(value: &T, field: &str) -> Result<Fields> {
    let mut encoded = Fields::new();
    value.encode_fields(&mut encoded).map_err(|error| error.with_field_if_empty(field))?;
    Ok(encoded)
}

/// 通过规范字段表示克隆一个应用 `value`。
///
/// `field` 用于编码或解码失败时的稳定错误定位；返回值不与输入共享可变字段存储。
pub(crate) fn clone_value<T: FieldValue>(value: &T, field: &str) -> Result<T> {
    T::decode_fields(&encode_value(value, field)?).map_err(|error| error.with_field_if_empty(field))
}

/// 判断 `left` 与 `right` 是否包含完全相同、顺序相同的顶层字段名，不比较字段值。
pub(crate) fn same_field_structure(left: &Fields, right: &Fields) -> bool {
    left.len() == right.len() && left.keys().eq(right.keys())
}

/// 按 `limits` 校验完整 `attr` 和 `data` 的字段数量、名称和单值大小。
///
/// 任一字段失败立即返回稳定错误；完整记录总大小由 `validate_record` 另行校验。
pub(crate) fn validate_fields(attr: &Fields, data: &Fields, limits: &ZoneConfig) -> Result<()> {
    if attr.len() > limits.attr_max_fields {
        return Err(Error::field(Code::Capacity, "attr"));
    }
    if data.len() > limits.data_max_fields {
        return Err(Error::field(Code::Capacity, "data"));
    }
    for (name, value) in attr {
        validate_field(name, value, limits.field_name_max_bytes, limits.attr_value_max_bytes)?;
    }
    for (name, value) in data {
        validate_field(name, value, limits.field_name_max_bytes, limits.data_value_max_bytes)?;
    }
    Ok(())
}

/// 校验一个应用字段的 `name`、`value` 及各自上限。
///
/// 名称不能为空、不能使用协议保留前缀；名称或值超过上限分别返回 `Invalid` 或 `Capacity`。
pub(crate) fn validate_field(name: &str, value: &[u8], name_limit: usize, value_limit: usize) -> Result<()> {
    if name.is_empty() || name.len() > name_limit || name.starts_with('&') || name.starts_with('@') || name.starts_with('.') {
        return Err(Error::field(Code::Invalid, name));
    }
    if value.len() > value_limit {
        return Err(Error::field(Code::Capacity, name));
    }
    Ok(())
}

/// 校验一份可写入 Redis 的完整 Registration。
///
/// `uuid` 参与总字节核算；`revision`、`ttl_ms`、`version` 必须处于跨语言安全范围；
/// `attr`、`data` 必须满足 `limits`，且完整 Hash 估算不得超过记录上限。
pub(crate) fn validate_record(uuid: &str, revision: u64, ttl_ms: u64, version: u64, attr: &Fields, data: &Fields, limits: &ZoneConfig) -> Result<()> {
    if revision == 0 || revision > MAX_SAFE_INTEGER {
        return Err(Error::field(Code::Invalid, "@revision"));
    }
    if ttl_ms == 0 || ttl_ms > MAX_HASH_FIELD_EXPIRE_AT_MS {
        return Err(Error::field(Code::Invalid, "@ttl"));
    }
    if version == 0 || version > MAX_SAFE_INTEGER {
        return Err(Error::field(Code::Invalid, "@version"));
    }
    validate_fields(attr, data, limits)?;
    if registration_size(uuid, revision, ttl_ms, version, attr, data) > limits.record_max_bytes {
        return Err(Error::field(Code::Capacity, "registration"));
    }
    Ok(())
}

/// 精确估算 Registration Hash 的字段名和值总字节数。
///
/// `@timestamp` 按协议最大十进制宽度预留；其余标量使用实际十进制位数，
/// `attr` 名称额外包含 `.` 前缀。返回值只用于已受协议上限约束的输入。
pub(crate) fn registration_size(uuid: &str, revision: u64, ttl_ms: u64, version: u64, attr: &Fields, data: &Fields) -> usize {
    let mut size = "@uuid".len() + uuid.len();
    size += "@revision".len() + decimal_digits(revision);
    size += "@timestamp".len() + 16;
    size += "@ttl".len() + decimal_digits(ttl_ms);
    size += "@version".len() + decimal_digits(version);
    size += attr.iter().map(|(name, value)| 1 + name.len() + value.len()).sum::<usize>();
    size += data.iter().map(|(name, value)| name.len() + value.len()).sum::<usize>();
    size
}

/// 返回无符号十进制 `value` 的字符数，包括零值的一位。
const fn decimal_digits(mut value: u64) -> usize {
    let mut digits = 1;
    while value >= 10 {
        value /= 10;
        digits += 1;
    }
    digits
}

/// 判断 `value` 是否恰好为 32 个小写十六进制字符的 Registration UUID。
pub(crate) fn valid_uuid(value: &str) -> bool {
    value.len() == 32 && value.bytes().all(|byte| byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte))
}

#[cfg(test)]
#[path = "../tests/internal/fields.rs"]
mod tests;
