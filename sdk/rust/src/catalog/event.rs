use std::sync::Arc;

use crate::{Code, Error, Fields, Result};

use super::model::{Kind, MAX_FIELDS, Path, array_field_index, canonical_u64, canonical_usize, validate_patch, validate_value};

#[derive(Clone, Copy)]
pub(super) enum EventKind {
    Replace,
    Patch,
    Delete,
}

pub(super) struct CatalogEvent {
    pub kind: EventKind,
    pub path: Path,
    pub revision: u64,
    pub base_revision: u64,
    pub value_kind: Option<Kind>,
    pub encoded_bytes: usize,
    pub fields: Arc<Fields>,
}

struct EventDecoder<'a> {
    payload: &'a [u8],
    offset: usize,
}

/// 解码并完整校验一条 Catalog Pub/Sub `payload`。
///
/// `expected` 用于拒绝错误频道或成员，`maximum` 同时约束通知和完整值大小。
/// 成功只返回通过协议版本、操作形状、revision、字段顺序和容量校验的拥有型事件。
pub(super) fn decode_event(payload: &[u8], expected: &Path, maximum: usize) -> Result<CatalogEvent> {
    // 在读取任意 MessagePack 容器前应用保守线长上限，避免异常负载消耗过多解析工作。
    if payload.len() > maximum_event_payload(maximum) {
        return Err(Error::field(Code::Capacity, "notification"));
    }
    let mut decoder = EventDecoder { payload, offset: 0 };
    let count = decoder.array_length().ok_or_else(|| Error::field(Code::Corrupt, "notification"))?;
    if count < 4 {
        return Err(Error::field(Code::Corrupt, "notification"));
    }
    if decoder.text("protocol")? != "v1" {
        return Err(Error::field(Code::Protocol, "protocol"));
    }
    let operation = decoder.text("operation")?;
    if !expected.matches_member(decoder.text("path")?) {
        return Err(Error::field(Code::Target, "path"));
    }

    // 公共头部确认后按固定操作 ABI 解码；每个分支都必须精确匹配数组元素数。
    let event = match operation {
        "replace" if count == 7 => {
            let revision = decode_event_revision(&mut decoder, "@revision")?;
            let kind = Kind::parse(decoder.text("@kind")?).ok_or_else(|| Error::field(Code::Corrupt, "@kind"))?;
            let encoded_bytes = decode_event_usize(&mut decoder, maximum, "@encoded_bytes")?;
            let fields = Arc::new(decode_event_fields(&mut decoder, kind == Kind::Array)?);
            if validate_value(kind, fields.as_ref(), maximum)? != encoded_bytes {
                return Err(Error::field(Code::Corrupt, "@encoded_bytes").with_revision(revision));
            }
            CatalogEvent {
                kind: EventKind::Replace,
                path: expected.clone(),
                revision,
                base_revision: 0,
                value_kind: Some(kind),
                encoded_bytes,
                fields,
            }
        }
        "patch" if count == 8 => {
            let base_revision = decode_event_revision(&mut decoder, "@base_revision")?;
            let revision = decode_event_revision(&mut decoder, "@revision")?;
            if revision <= base_revision {
                return Err(Error::field(Code::Corrupt, "@revision"));
            }
            let kind = Kind::parse(decoder.text("@kind")?)
                .filter(|kind| *kind != Kind::Value)
                .ok_or_else(|| Error::field(Code::Corrupt, "@kind"))?;
            let encoded_bytes = decode_event_usize(&mut decoder, maximum, "@encoded_bytes")?;
            let fields = Arc::new(decode_event_fields(&mut decoder, false)?);
            validate_patch(fields.as_ref(), maximum)?;
            CatalogEvent {
                kind: EventKind::Patch,
                path: expected.clone(),
                revision,
                base_revision,
                value_kind: Some(kind),
                encoded_bytes,
                fields,
            }
        }
        "delete" if count == 4 => CatalogEvent {
            kind: EventKind::Delete,
            path: expected.clone(),
            revision: decode_event_revision(&mut decoder, "@revision")?,
            base_revision: 0,
            value_kind: None,
            encoded_bytes: 0,
            fields: Arc::new(Fields::new()),
        },
        "replace" | "patch" | "delete" => {
            return Err(Error::field(Code::Corrupt, "notification"));
        }
        _ => return Err(Error::field(Code::Protocol, "operation")),
    };
    if !decoder.done() {
        return Err(Error::field(Code::Corrupt, "notification"));
    }
    Ok(event)
}

/// 计算 `maximum` 值上限对应的最坏通知线长。
///
/// 返回值额外容纳字段名称、MessagePack 长度头与协议元数据，并使用饱和算术防溢出。
fn maximum_event_payload(maximum: usize) -> usize {
    maximum.saturating_add(maximum.min(MAX_FIELDS).saturating_mul(10)).saturating_add(1024)
}

/// 从 `decoder` 读取 `field` 对应的规范正 revision。
fn decode_event_revision(decoder: &mut EventDecoder<'_>, field: &str) -> Result<u64> {
    let text = decoder.text(field)?;
    canonical_u64(text, false, field)
}

/// 从 `decoder` 读取一个不超过 `maximum` 的规范 `usize`，并以 `field` 定位错误。
fn decode_event_usize(decoder: &mut EventDecoder<'_>, maximum: usize, field: &str) -> Result<usize> {
    let text = decoder.text(field)?;
    canonical_usize(text, maximum, field)
}

/// 解码按名称/值交替排列的字段数组。
///
/// `array_replace` 为 true 时要求名称严格等于连续数组下标，否则要求名称严格递增。
/// 返回 map 拥有全部字段值；重复、乱序或数量越界使整条事件失败。
fn decode_event_fields(decoder: &mut EventDecoder<'_>, array_replace: bool) -> Result<Fields> {
    let count = decoder
        .array_length()
        .filter(|count| count % 2 == 0 && count / 2 <= MAX_FIELDS as u64)
        .ok_or_else(|| Error::field(Code::Corrupt, "fields"))?;
    let field_count = usize::try_from(count / 2).map_err(|_| Error::field(Code::Corrupt, "fields"))?;
    let mut fields = Fields::new();
    let mut previous = None;
    for index in 0..field_count {
        let name = decoder.text("fields")?;
        let value = decoder.bytes("fields")?;
        let invalid_order = if array_replace {
            array_field_index(name, field_count) != Some(index)
        } else {
            previous.is_some_and(|previous| previous >= name)
        };
        if name.is_empty() || invalid_order {
            return Err(Error::field(Code::Corrupt, "fields"));
        }
        previous = Some(name);
        if fields.insert(name.to_owned(), value).is_some() {
            return Err(Error::field(Code::Corrupt, "fields"));
        }
    }
    Ok(fields)
}

impl<'a> EventDecoder<'a> {
    /// 报告 decoder 是否已精确消费整个 payload。
    fn done(&self) -> bool {
        self.offset == self.payload.len()
    }

    /// 读取 MessagePack 数组头并返回元素数量；类型错误或截断返回 `None`。
    fn array_length(&mut self) -> Option<u64> {
        let code = self.code()?;
        match code {
            0x90..=0x9f => Some(u64::from(code & 0x0f)),
            0xdc => {
                let value = self.take(2)?;
                Some(u64::from(u16::from_be_bytes([value[0], value[1]])))
            }
            0xdd => {
                let value = self.take(4)?;
                Some(u64::from(u32::from_be_bytes([value[0], value[1], value[2], value[3]])))
            }
            _ => None,
        }
    }

    /// 读取 `field` 对应的字符串/二进制值并验证 UTF-8。
    ///
    /// 返回切片直接借用原始 payload，生命周期由 `'a` 约束。
    fn text(&mut self, field: &str) -> Result<&'a str> {
        let value = self.raw_bytes().ok_or_else(|| Error::field(Code::Corrupt, field))?;
        std::str::from_utf8(value).map_err(|_| Error::field(Code::Corrupt, field))
    }

    /// 读取 `field` 对应的字符串/二进制值并复制为拥有型字节。
    fn bytes(&mut self, field: &str) -> Result<Vec<u8>> {
        self.raw_bytes().map(<[u8]>::to_vec).ok_or_else(|| Error::field(Code::Corrupt, field))
    }

    /// 读取一个 MessagePack str/bin 载荷并返回对原始 payload 的零拷贝切片。
    fn raw_bytes(&mut self) -> Option<&'a [u8]> {
        let code = self.code()?;
        let length = match code {
            0xa0..=0xbf => u64::from(code & 0x1f),
            0xc4 | 0xd9 => u64::from(self.take(1)?[0]),
            0xc5 | 0xda => {
                let value = self.take(2)?;
                u64::from(u16::from_be_bytes([value[0], value[1]]))
            }
            0xc6 | 0xdb => {
                let value = self.take(4)?;
                u64::from(u32::from_be_bytes([value[0], value[1], value[2], value[3]]))
            }
            _ => return None,
        };
        self.take(length)
    }

    /// 读取当前位置的一个 MessagePack 类型字节并推进 offset。
    fn code(&mut self) -> Option<u8> {
        let value = *self.payload.get(self.offset)?;
        self.offset += 1;
        Some(value)
    }

    /// 从当前位置借用 `length` 字节并推进 offset；平台转换、加法或边界失败返回 `None`。
    fn take(&mut self, length: u64) -> Option<&'a [u8]> {
        let length = usize::try_from(length).ok()?;
        let end = self.offset.checked_add(length)?;
        let value = self.payload.get(self.offset..end)?;
        self.offset = end;
        Some(value)
    }
}

#[cfg(test)]
#[path = "../../tests/internal/catalog/event.rs"]
mod tests;
