use std::collections::{HashMap, HashSet};
use std::io::Cursor;

use fred::types::Value as RedisValue;
use rmpv::Value as MessageValue;

use super::config::{PROTOCOL_VERSION, ZoneConfig};
use super::selector::Meta;
use crate::Fields;
use crate::error::{Code, Error, Result};
use crate::fields::{MAX_HASH_FIELD_EXPIRE_AT_MS, MAX_SAFE_INTEGER, valid_uuid, validate_field, validate_record};

const MAX_EVENT_BYTES: usize = 128 * 1024;

#[derive(Clone, Debug)]
pub(crate) struct RegistrationEvent {
    pub kind: String,
    pub uuid: String,
    pub revision: u64,
    pub timestamp: u64,
    pub ttl: u64,
    pub version: u64,
    pub has_version: bool,
    pub attr: Fields,
    pub data: Fields,
}

pub(crate) struct StoredRecord {
    pub meta: Meta,
    pub attr: Fields,
    pub data: Fields,
    pub deadline: u64,
}

/// 解码并完整校验一条 Registration Pub/Sub `payload`。
///
/// `limits` 是当前 Zone 策略；输入受固定线长和协议字段数量约束。成功返回拥有型事件，
/// 任何容器、重复字段、保留字段、生命周期形状或容量错误都会拒绝整条消息。
pub(crate) fn decode_registration_event(payload: &[u8], limits: &ZoneConfig) -> Result<RegistrationEvent> {
    if payload.is_empty() || payload.len() > MAX_EVENT_BYTES {
        return Err(Error::field(Code::Capacity, "event"));
    }
    // 增量解码扁平 envelope，先验证声明容器大小，避免通用 MessagePack 解码器按攻击者长度分配。
    let mut cursor = Cursor::new(payload);
    let elements = rmp::decode::read_array_len(&mut cursor).map_err(|error| Error::driver(Code::Corrupt, error))?;
    if elements % 2 != 0 {
        return Err(Error::field(Code::Corrupt, "event"));
    }
    let ceiling = ZoneConfig::protocol_ceiling();
    let maximum_elements = (ceiling.attr_max_fields + ceiling.data_max_fields + 7) * 2;
    if usize::try_from(elements).unwrap_or(usize::MAX) > maximum_elements {
        return Err(Error::field(Code::Capacity, "event"));
    }

    // 在单遍扫描中按字段名选择允许的标量类型；未知控制字段仅允许有界标量并被忽略。
    let mut event = RegistrationEvent {
        kind: String::new(),
        uuid: String::new(),
        revision: 0,
        timestamp: 0,
        ttl: 0,
        version: 0,
        has_version: false,
        attr: Fields::new(),
        data: Fields::new(),
    };
    const SEEN_PROTOCOL: u8 = 1 << 0;
    const SEEN_KIND: u8 = 1 << 1;
    const SEEN_UUID: u8 = 1 << 2;
    const SEEN_REVISION: u8 = 1 << 3;
    const SEEN_TIMESTAMP: u8 = 1 << 4;
    const SEEN_TTL: u8 = 1 << 5;
    const SEEN_VERSION: u8 = 1 << 6;

    let mut seen = 0_u8;
    let mut unknown_controls: Option<HashSet<String>> = None;
    for _ in 0..elements / 2 {
        let name = read_event_value(&mut cursor, is_string_marker)?;
        let name = message_owned_string(name).ok_or_else(|| Error::field(Code::Corrupt, "event"))?;
        if name.is_empty() {
            return Err(Error::field(Code::Corrupt, "event"));
        }
        let marker = match name.as_str() {
            "&protocol" => SEEN_PROTOCOL,
            "&kind" => SEEN_KIND,
            "@uuid" => SEEN_UUID,
            "@revision" => SEEN_REVISION,
            "@timestamp" => SEEN_TIMESTAMP,
            "@ttl" => SEEN_TTL,
            "@version" => SEEN_VERSION,
            _ => 0,
        };
        if marker != 0 {
            if seen & marker != 0 {
                return Err(Error::field(Code::Contract, name));
            }
            seen |= marker;
        } else if (name.starts_with('&') || name.starts_with('@')) && unknown_controls.as_ref().is_some_and(|controls| controls.contains(&name)) {
            return Err(Error::field(Code::Contract, name));
        }
        let value = match name.as_str() {
            "&protocol" | "&kind" | "@uuid" => read_event_value(&mut cursor, is_string_marker)?,
            "@revision" | "@timestamp" | "@ttl" | "@version" => read_event_value(&mut cursor, is_integer_marker)?,
            _ if name.starts_with('&') || name.starts_with('@') => read_event_value(&mut cursor, is_scalar_marker)?,
            _ => read_event_value(&mut cursor, is_string_marker)?,
        };
        match name.as_str() {
            "&protocol" => {
                if !message_string_equals(&value, PROTOCOL_VERSION) {
                    return Err(Error::field(Code::Protocol, name));
                }
            }
            "&kind" => {
                event.kind = message_owned_string(value).ok_or_else(|| Error::field(Code::Corrupt, name))?;
            }
            "@uuid" => {
                event.uuid = message_owned_string(value).ok_or_else(|| Error::field(Code::Corrupt, &name))?;
                if !valid_uuid(&event.uuid) {
                    return Err(Error::field(Code::Invalid, name));
                }
            }
            "@revision" => {
                event.revision = message_positive_u64(&value).ok_or_else(|| Error::field(Code::Invalid, name))?;
            }
            "@timestamp" => {
                event.timestamp = message_positive_u64(&value).ok_or_else(|| Error::field(Code::Invalid, name))?;
            }
            "@ttl" => {
                event.ttl = message_positive_u64(&value).ok_or_else(|| Error::field(Code::Invalid, name))?;
            }
            "@version" => {
                event.version = message_positive_u64(&value).ok_or_else(|| Error::field(Code::Invalid, name))?;
                event.has_version = true;
            }
            _ if name.starts_with('&') || name.starts_with('@') => {
                unknown_controls.get_or_insert_with(HashSet::new).insert(name);
            }
            _ => {
                let value = message_bytes(value).ok_or_else(|| Error::field(Code::Corrupt, &name))?;
                if let Some(field) = name.strip_prefix('.') {
                    validate_field(field, &value, limits.field_name_max_bytes, limits.attr_value_max_bytes)?;
                    if event.attr.insert(field.to_owned(), value).is_some() {
                        return Err(Error::field(Code::Contract, name));
                    }
                } else {
                    validate_field(&name, &value, limits.field_name_max_bytes, limits.data_value_max_bytes)?;
                    if event.data.contains_key(&name) {
                        return Err(Error::field(Code::Contract, name));
                    }
                    event.data.insert(name, value);
                }
            }
        }
    }
    if cursor.position() != u64::try_from(payload.len()).unwrap_or(u64::MAX) {
        return Err(Error::field(Code::Corrupt, "event"));
    }

    // 完成字节消费后再验证每种生命周期消息的必需/禁止字段组合。
    for (marker, required) in [(SEEN_PROTOCOL, "&protocol"), (SEEN_KIND, "&kind"), (SEEN_UUID, "@uuid")] {
        if seen & marker == 0 {
            return Err(Error::field(Code::Corrupt, required));
        }
    }
    match event.kind.as_str() {
        "register" => {
            if event.revision == 0 || event.timestamp == 0 || event.ttl == 0 || !event.has_version {
                return Err(Error::field(Code::Contract, "register"));
            }
            validate_record(&event.uuid, event.revision, event.ttl, event.version, &event.attr, &event.data, limits)?;
        }
        "update" => {
            if event.revision == 0 || event.timestamp == 0 || !event.attr.is_empty() || (!event.has_version && event.data.is_empty()) {
                return Err(Error::field(Code::Contract, "update"));
            }
        }
        "renew" => {
            if event.revision == 0 || event.timestamp == 0 || event.ttl != 0 || event.has_version || !event.attr.is_empty() || !event.data.is_empty() {
                return Err(Error::field(Code::Contract, "renew"));
            }
        }
        "unregister" => {
            if event.revision != 0 || event.timestamp != 0 || event.ttl != 0 || event.has_version || !event.attr.is_empty() || !event.data.is_empty() {
                return Err(Error::field(Code::Contract, "unregister"));
            }
        }
        _ => return Err(Error::field(Code::Invalid, "&kind")),
    }
    Ok(event)
}

/// 从 `cursor` 读取一个 MessagePack 标量，并用 `accepts` 在通用解码前筛选 marker。
///
/// 字符串/bin 声明长度会额外验证；返回 MessageValue 拥有其必要内容，错误统一映射为 Corrupt。
fn read_event_value(cursor: &mut Cursor<&[u8]>, accepts: fn(u8) -> bool) -> Result<MessageValue> {
    let position = usize::try_from(cursor.position()).map_err(|_| Error::field(Code::Capacity, "event"))?;
    let marker = cursor.get_ref().get(position).copied().ok_or_else(|| Error::field(Code::Corrupt, "event"))?;
    if !accepts(marker) {
        return Err(Error::field(Code::Corrupt, "event"));
    }
    validate_declared_string_length(cursor, marker)?;
    rmpv::decode::read_value(cursor).map_err(|error| Error::driver(Code::Corrupt, error))
}

/// 在不分配载荷的情况下验证当前位置 `marker` 声明的 str/bin 长度。
///
/// `cursor` 不推进；超过事件上限、头部截断、平台转换失败或正文不足均返回错误。
fn validate_declared_string_length(cursor: &Cursor<&[u8]>, marker: u8) -> Result<()> {
    let position = usize::try_from(cursor.position()).map_err(|_| Error::field(Code::Capacity, "event"))?;
    let input = cursor.get_ref();
    let remaining = input.len().checked_sub(position).ok_or_else(|| Error::field(Code::Corrupt, "event"))?;
    let (header, length): (usize, usize) = match marker {
        0xa0..=0xbf => (1, usize::from(marker & 0x1f)),
        0xc4 | 0xd9 => (2, usize::from(*input.get(position + 1).ok_or_else(|| Error::field(Code::Corrupt, "event"))?)),
        0xc5 | 0xda => {
            let bytes = input.get(position + 1..position + 3).ok_or_else(|| Error::field(Code::Corrupt, "event"))?;
            (3, usize::from(u16::from_be_bytes([bytes[0], bytes[1]])))
        }
        0xc6 | 0xdb => {
            let bytes = input.get(position + 1..position + 5).ok_or_else(|| Error::field(Code::Corrupt, "event"))?;
            let length = usize::try_from(u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]])).map_err(|_| Error::field(Code::Capacity, "event"))?;
            (5, length)
        }
        _ => return Ok(()),
    };
    if length > MAX_EVENT_BYTES || header.saturating_add(length) > remaining {
        return Err(Error::field(Code::Corrupt, "event"));
    }
    Ok(())
}

/// 判断 MessagePack `marker` 是否是 SDK 允许的字符串或二进制类型。
fn is_string_marker(marker: u8) -> bool {
    (0xa0..=0xbf).contains(&marker) || (0xc4..=0xc6).contains(&marker) || (0xd9..=0xdb).contains(&marker)
}

/// 判断 MessagePack `marker` 是否是正/负 fixint 或标准整数类型。
fn is_integer_marker(marker: u8) -> bool {
    marker <= 0x7f || marker >= 0xe0 || (0xcc..=0xd3).contains(&marker)
}

/// 判断未知控制字段是否使用安全的标量 marker，而非可递归或大容器。
fn is_scalar_marker(marker: u8) -> bool {
    is_string_marker(marker) || is_integer_marker(marker) || matches!(marker, 0xc0 | 0xc2 | 0xc3 | 0xca | 0xcb)
}

/// 解析单个 Registration Hash 的 `value` 并验证其完整协议状态。
///
/// `uuid` 必须与 `@uuid` 精确匹配，`limits` 约束字段与记录大小。缺失 Hash 返回 `Ok(None)`；
/// 成功返回 Meta/Attr/Data 以及 `timestamp+ttl` 的绝对 deadline。
pub(crate) fn parse_stored_record(uuid: &str, value: RedisValue, limits: &ZoneConfig) -> Result<Option<StoredRecord>> {
    let entries = redis_map_entries(value)?;
    if entries.is_empty() {
        return Ok(None);
    }
    let mut values = HashMap::with_capacity(entries.len());
    for (name, value) in entries {
        if values.insert(name.clone(), value).is_some() {
            return Err(Error::field(Code::Corrupt, name));
        }
    }
    for required in ["@uuid", "@revision", "@timestamp", "@ttl", "@version"] {
        if !values.contains_key(required) {
            return Err(Error::field(Code::Corrupt, required));
        }
    }
    let stored_uuid = redis_string(values.remove("@uuid").ok_or_else(|| Error::field(Code::Corrupt, "@uuid"))?)?;
    if stored_uuid != uuid || !valid_uuid(uuid) {
        return Err(Error::field(Code::Target, "@uuid"));
    }
    let revision = redis_canonical_u64(values.remove("@revision").ok_or_else(|| Error::field(Code::Corrupt, "@revision"))?)?;
    let timestamp = redis_canonical_u64(values.remove("@timestamp").ok_or_else(|| Error::field(Code::Corrupt, "@timestamp"))?)?;
    let ttl = redis_canonical_u64(values.remove("@ttl").ok_or_else(|| Error::field(Code::Corrupt, "@ttl"))?)?;
    let version = redis_canonical_u64(values.remove("@version").ok_or_else(|| Error::field(Code::Corrupt, "@version"))?)?;

    // Redis 管理的未知 @ 字段向前兼容地忽略；& 字段不属于持久 Hash，出现即损坏。
    let mut attr = Fields::new();
    let mut data = Fields::new();
    for (name, value) in values {
        if name.starts_with('@') {
            continue;
        }
        if name.starts_with('&') {
            return Err(Error::field(Code::Corrupt, name));
        }
        let value = redis_bytes(value)?;
        if let Some(field) = name.strip_prefix('.') {
            attr.insert(field.to_owned(), value);
        } else {
            data.insert(name, value);
        }
    }
    validate_record(uuid, revision, ttl, version, &attr, &data, limits)?;
    let deadline = timestamp
        .checked_add(ttl)
        .filter(|value| *value <= MAX_HASH_FIELD_EXPIRE_AT_MS)
        .ok_or_else(|| Error::field(Code::Corrupt, "@timestamp"))?;
    Ok(Some(StoredRecord {
        meta: Meta {
            uuid: uuid.to_owned(),
            revision,
            timestamp,
            ttl,
            version,
        },
        attr,
        data,
        deadline,
    }))
}

/// 把 Fred `value` 转换为协议安全范围内的正整数。
///
/// 接受整数、字符串或字节十进制表示；零、负数、类型错误和超上限返回 `None`。
pub(crate) fn redis_positive_u64(value: RedisValue) -> Option<u64> {
    let parsed = match value {
        RedisValue::Integer(value) if value > 0 => u64::try_from(value).ok()?,
        RedisValue::String(value) => value.parse().ok()?,
        RedisValue::Bytes(value) => std::str::from_utf8(&value).ok()?.parse().ok()?,
        _ => return None,
    };
    (parsed <= MAX_SAFE_INTEGER).then_some(parsed)
}

/// 把 Fred 可能返回的 Map、交替 Array 或 Null 统一为拥有型字段条目列表。
///
/// 键必须是字符串；奇数数组或其他回复形状返回 Corrupt。
fn redis_map_entries(value: RedisValue) -> Result<Vec<(String, RedisValue)>> {
    match value {
        RedisValue::Map(values) => values
            .inner()
            .into_iter()
            .map(|(key, value)| {
                let key = key.into_string().ok_or_else(|| Error::field(Code::Corrupt, "registration"))?;
                Ok((key, value))
            })
            .collect(),
        RedisValue::Array(values) => {
            if values.len() % 2 != 0 {
                return Err(Error::field(Code::Corrupt, "registration"));
            }
            let mut result = Vec::with_capacity(values.len() / 2);
            let mut iterator = values.into_iter();
            while let Some(key) = iterator.next() {
                let Some(value) = iterator.next() else {
                    return Err(Error::field(Code::Corrupt, "registration"));
                };
                result.push((redis_string(key)?, value));
            }
            Ok(result)
        }
        RedisValue::Null => Ok(Vec::new()),
        _ => Err(Error::field(Code::Corrupt, "registration")),
    }
}

/// 从 Fred `value` 解析无前导零、正数且不超过安全上限的规范十进制整数。
fn redis_canonical_u64(value: RedisValue) -> Result<u64> {
    let text = redis_string(value)?;
    let parsed = text
        .parse::<u64>()
        .ok()
        .filter(|value| *value > 0 && *value <= MAX_SAFE_INTEGER && value.to_string() == text)
        .ok_or_else(|| Error::field(Code::Corrupt, "integer"))?;
    Ok(parsed)
}

/// 把 Fred `value` 的拥有型字节转换为 UTF-8 String；类型或编码错误返回 Corrupt。
fn redis_string(value: RedisValue) -> Result<String> {
    let bytes = value.into_owned_bytes().ok_or_else(|| Error::field(Code::Corrupt, "string"))?;
    String::from_utf8(bytes).map_err(|_| Error::field(Code::Corrupt, "string"))
}

/// 从 Fred `value` 取得拥有型字节；非字符串/二进制值返回 Corrupt。
fn redis_bytes(value: RedisValue) -> Result<Vec<u8>> {
    value.into_owned_bytes().ok_or_else(|| Error::field(Code::Corrupt, "value"))
}

/// 比较 MessagePack String/Binary 与规范 ASCII 文本，不创建临时 String。
fn message_string_equals(value: &MessageValue, expected: &str) -> bool {
    match value {
        MessageValue::String(value) => value.as_str() == Some(expected),
        MessageValue::Binary(value) => value.as_slice() == expected.as_bytes(),
        _ => false,
    }
}

/// 消费 MessagePack String/Binary 并复用其已有缓冲区生成 UTF-8 String。
fn message_owned_string(value: MessageValue) -> Option<String> {
    match value {
        MessageValue::String(value) => value.into_str(),
        MessageValue::Binary(value) => String::from_utf8(value).ok(),
        _ => None,
    }
}

/// 把 MessagePack String/Binary `value` 转为拥有型字节；其他类型返回 `None`。
fn message_bytes(value: MessageValue) -> Option<Vec<u8>> {
    match value {
        MessageValue::String(value) => Some(value.as_bytes().to_vec()),
        MessageValue::Binary(value) => Some(value),
        _ => None,
    }
}

/// 从 MessagePack `value` 读取安全范围内的正整数；其他值返回 `None`。
fn message_positive_u64(value: &MessageValue) -> Option<u64> {
    let value = value.as_u64()?;
    (value > 0 && value <= MAX_SAFE_INTEGER).then_some(value)
}

#[cfg(test)]
#[path = "../../tests/internal/registration/event.rs"]
mod tests;
