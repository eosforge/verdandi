use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use std::collections::BTreeMap;

use redb::{Database, ReadableTable, TableDefinition};

use super::model::{Kind, MAX_REVISION, Path as CatalogPath, RawState, Status, validate_value};

const RECORDS: TableDefinition<&[u8], &[u8]> = TableDefinition::new("verdandi_catalog_v2_records");

pub(super) struct Checkpoint {
    database: Database,
    disabled: AtomicBool,
}

impl Checkpoint {
    /// 打开或创建 `path` 指定的 redb Catalog 检查点。
    ///
    /// 缺失父目录会被创建；表初始化与提交失败返回最长 512 字节的存储错误文本。
    pub(super) fn open(path: &Path) -> std::result::Result<Self, String> {
        if let Some(parent) = path.parent().filter(|parent| !parent.as_os_str().is_empty()) {
            std::fs::create_dir_all(parent).map_err(store_error)?;
        }
        let database = Database::create(path).map_err(store_error)?;
        let transaction = database.begin_write().map_err(store_error)?;
        transaction.open_table(RECORDS).map_err(store_error)?;
        transaction.commit().map_err(store_error)?;
        Ok(Self {
            database,
            disabled: AtomicBool::new(false),
        })
    }

    /// 无锁报告检查点是否因一次存储错误而被永久停用。
    pub(super) fn disabled(&self) -> bool {
        self.disabled.load(Ordering::Acquire)
    }

    /// 永久停用当前检查点；后续读写退化为空成功，不影响 Redis 权威同步。
    pub(super) fn disable(&self) {
        self.disabled.store(true, Ordering::Release);
    }

    /// 加载 `zone`/`scope` 的单调 cursor 与全部 Path 状态。
    ///
    /// `maximum_bytes` 用于重新验证每个完整值。检查点停用时返回空状态；
    /// 任意键、编码或状态损坏使整个加载失败，调用方应停用检查点并从 Redis 重建。
    pub(super) fn load(&self, zone: &str, scope: &str, maximum_bytes: usize) -> std::result::Result<(u64, BTreeMap<CatalogPath, RawState>), String> {
        if self.disabled() {
            return Ok((0, BTreeMap::new()));
        }
        let transaction = self.database.begin_read().map_err(store_error)?;
        let table = transaction.open_table(RECORDS).map_err(store_error)?;
        let cursor_key = checkpoint_key(b'C', zone, scope, None);
        let cursor = match table.get(cursor_key.as_slice()).map_err(store_error)? {
            Some(value) => {
                let bytes = value.value();
                if bytes.len() != 8 {
                    return Err("invalid Catalog checkpoint cursor".to_owned());
                }
                u64::from_be_bytes(bytes.try_into().map_err(|_| "invalid Catalog checkpoint cursor".to_owned())?)
            }
            None => 0,
        };
        if cursor > MAX_REVISION {
            return Err("invalid Catalog checkpoint cursor".to_owned());
        }
        // redb 按字节排序；用同一前缀到追加 0xff 的半开区间枚举当前作用域全部 Entry。
        let prefix = checkpoint_key(b'E', zone, scope, None);
        let mut last = prefix.clone();
        last.push(0xff);
        let mut entries = BTreeMap::new();
        for result in table.range(prefix.as_slice()..last.as_slice()).map_err(store_error)? {
            let (key, value) = result.map_err(store_error)?;
            let member = key
                .value()
                .strip_prefix(prefix.as_slice())
                .and_then(|value| std::str::from_utf8(value).ok())
                .ok_or_else(|| "invalid Catalog checkpoint path".to_owned())?;
            let path = CatalogPath::from_member(member).ok_or_else(|| "invalid Catalog checkpoint path".to_owned())?;
            let state = decode_state(value.value(), maximum_bytes)?;
            entries.insert(path, state);
        }
        Ok((cursor, entries))
    }

    /// 单调保存 `path` 的完整 `state`。
    ///
    /// `zone`/`scope` 隔离不同客户端视图，`maximum_bytes` 在编码前复核容量。
    /// 磁盘中相同或更高 revision 会保留，防止迟到异步持久化回退检查点。
    pub(super) fn save_entry(&self, zone: &str, scope: &str, path: &CatalogPath, state: &RawState, maximum_bytes: usize) -> std::result::Result<(), String> {
        if self.disabled() {
            return Ok(());
        }
        let key = checkpoint_key(b'E', zone, scope, Some(&path.member()));
        let encoded = encode_state(state, maximum_bytes)?;
        let transaction = self.database.begin_write().map_err(store_error)?;
        {
            let mut table = transaction.open_table(RECORDS).map_err(store_error)?;
            let keep_existing = match table.get(key.as_slice()).map_err(store_error)? {
                Some(previous) => decode_state(previous.value(), maximum_bytes)?.revision >= state.revision,
                None => false,
            };
            if keep_existing {
                drop(table);
                transaction.commit().map_err(store_error)?;
                return Ok(());
            }
            table.insert(key.as_slice(), encoded.as_slice()).map_err(store_error)?;
        }
        transaction.commit().map_err(store_error)
    }

    /// 单调保存 `zone`/`scope` 已完成对齐的 `revision` cursor。
    ///
    /// 超过协议安全范围或已有字节损坏返回错误；较旧 revision 不覆盖较新值。
    pub(super) fn save_cursor(&self, zone: &str, scope: &str, revision: u64) -> std::result::Result<(), String> {
        if self.disabled() {
            return Ok(());
        }
        if revision > MAX_REVISION {
            return Err("invalid Catalog checkpoint cursor".to_owned());
        }
        let key = checkpoint_key(b'C', zone, scope, None);
        let encoded = revision.to_be_bytes();
        let transaction = self.database.begin_write().map_err(store_error)?;
        {
            let mut table = transaction.open_table(RECORDS).map_err(store_error)?;
            let keep_existing = match table.get(key.as_slice()).map_err(store_error)? {
                Some(previous) => {
                    let value = previous.value();
                    if value.len() != 8 {
                        return Err("invalid Catalog checkpoint cursor".to_owned());
                    }
                    u64::from_be_bytes(value.try_into().map_err(|_| "invalid Catalog checkpoint cursor".to_owned())?) >= revision
                }
                None => false,
            };
            if keep_existing {
                drop(table);
                transaction.commit().map_err(store_error)?;
                return Ok(());
            }
            table.insert(key.as_slice(), encoded.as_slice()).map_err(store_error)?;
        }
        transaction.commit().map_err(store_error)
    }
}

/// 构造无歧义二进制检查点键。
///
/// `kind` 区分 cursor/Entry，`zone` 和 `scope` 用 NUL 分隔；Entry 可追加规范 `member`。
fn checkpoint_key(kind: u8, zone: &str, scope: &str, member: Option<&str>) -> Vec<u8> {
    let mut key = Vec::with_capacity(3 + zone.len() + scope.len() + member.map_or(0, str::len));
    key.push(kind);
    key.push(0);
    key.extend_from_slice(zone.as_bytes());
    key.push(0);
    key.extend_from_slice(scope.as_bytes());
    key.push(0);
    if let Some(member) = member {
        key.extend_from_slice(member.as_bytes());
    }
    key
}

/// 校验并编码一个完整 RawState 为版本化 `VCAT2` 二进制记录。
///
/// `maximum_bytes` 约束 Present 值；只有 Present、Absent、Deleted 可持久化。
fn encode_state(state: &RawState, maximum_bytes: usize) -> std::result::Result<Vec<u8>, String> {
    validate_state(state, maximum_bytes)?;
    let (status, kind) = match (state.status, state.kind) {
        (Status::Present, Some(kind)) => (1_u8, kind_byte(kind)),
        (Status::Absent, None) => (2, 0),
        (Status::Deleted, None) => (3, 0),
        _ => return Err("invalid Catalog checkpoint state".to_owned()),
    };
    // 固定 35 字节头部后按 BTreeMap 顺序写入长度前缀字段，保证重放确定且可边界检查。
    let mut encoded = Vec::with_capacity(35 + state.encoded_bytes);
    encoded.extend_from_slice(b"VCAT2");
    encoded.extend_from_slice(&state.revision.to_be_bytes());
    encoded.extend_from_slice(&state.replace_revision.to_be_bytes());
    encoded.push(status);
    encoded.push(kind);
    encoded.extend_from_slice(
        &u64::try_from(state.encoded_bytes)
            .map_err(|_| "Catalog checkpoint size overflow".to_owned())?
            .to_be_bytes(),
    );
    encoded.extend_from_slice(
        &u32::try_from(state.fields.len())
            .map_err(|_| "Catalog checkpoint field overflow".to_owned())?
            .to_be_bytes(),
    );
    for (name, value) in state.fields.iter() {
        encoded.extend_from_slice(
            &u32::try_from(name.len())
                .map_err(|_| "Catalog checkpoint name overflow".to_owned())?
                .to_be_bytes(),
        );
        encoded.extend_from_slice(name.as_bytes());
        encoded.extend_from_slice(
            &u32::try_from(value.len())
                .map_err(|_| "Catalog checkpoint value overflow".to_owned())?
                .to_be_bytes(),
        );
        encoded.extend_from_slice(value);
    }
    Ok(encoded)
}

/// 解码一条 `VCAT2` 检查点记录并重新执行全部状态与容量校验。
///
/// `value` 必须被精确消费；`maximum_bytes` 是当前客户端上限，旧文件不能绕过新配置。
fn decode_state(value: &[u8], maximum_bytes: usize) -> std::result::Result<RawState, String> {
    if value.len() < 35 || &value[..5] != b"VCAT2" {
        return Err("invalid Catalog checkpoint state".to_owned());
    }
    let revision = read_u64(value, 5)?;
    let replace_revision = read_u64(value, 13)?;
    let status = match value[21] {
        1 => Status::Present,
        2 => Status::Absent,
        3 => Status::Deleted,
        _ => return Err("invalid Catalog checkpoint status".to_owned()),
    };
    let kind = match value[22] {
        0 => None,
        1 => Some(Kind::Value),
        2 => Some(Kind::Array),
        3 => Some(Kind::Map),
        _ => return Err("invalid Catalog checkpoint kind".to_owned()),
    };
    let encoded_bytes = usize::try_from(read_u64(value, 23)?).map_err(|_| "Catalog checkpoint size overflow".to_owned())?;
    let count = usize::try_from(read_u32(value, 31)?).map_err(|_| "Catalog checkpoint field overflow".to_owned())?;
    // 所有长度和 offset 使用 checked 算术，截断、重复字段和尾随字节均拒绝整个记录。
    let mut offset = 35_usize;
    let mut fields = crate::Fields::new();
    for _ in 0..count {
        let name_length = usize::try_from(read_u32(value, offset)?).map_err(|_| "Catalog checkpoint name overflow".to_owned())?;
        offset = offset.checked_add(4).ok_or_else(|| "Catalog checkpoint overflow".to_owned())?;
        let name_end = offset.checked_add(name_length).ok_or_else(|| "Catalog checkpoint overflow".to_owned())?;
        let name = std::str::from_utf8(value.get(offset..name_end).ok_or_else(|| "truncated Catalog checkpoint".to_owned())?)
            .map_err(|_| "invalid Catalog checkpoint name".to_owned())?
            .to_owned();
        offset = name_end;
        let value_length = usize::try_from(read_u32(value, offset)?).map_err(|_| "Catalog checkpoint value overflow".to_owned())?;
        offset = offset.checked_add(4).ok_or_else(|| "Catalog checkpoint overflow".to_owned())?;
        let value_end = offset.checked_add(value_length).ok_or_else(|| "Catalog checkpoint overflow".to_owned())?;
        let field = value.get(offset..value_end).ok_or_else(|| "truncated Catalog checkpoint".to_owned())?.to_vec();
        offset = value_end;
        if fields.insert(name, field).is_some() {
            return Err("duplicate Catalog checkpoint field".to_owned());
        }
    }
    if offset != value.len() {
        return Err("trailing Catalog checkpoint bytes".to_owned());
    }
    let state = RawState {
        revision,
        replace_revision,
        status,
        kind,
        encoded_bytes,
        fields: Arc::new(fields),
    };
    validate_state(&state, maximum_bytes)?;
    Ok(state)
}

/// 验证 `state` 是否为可持久化的完整 Present、Absent 或 Deleted 状态。
///
/// `maximum_bytes` 约束字段内容；revision、replace revision、Kind、编码大小和空状态必须相互一致。
fn validate_state(state: &RawState, maximum_bytes: usize) -> std::result::Result<(), String> {
    if state.revision > MAX_REVISION || state.replace_revision > MAX_REVISION {
        return Err("invalid Catalog checkpoint revision".to_owned());
    }
    match state.status {
        Status::Present => {
            let kind = state.kind.ok_or_else(|| "missing Catalog checkpoint kind".to_owned())?;
            let size = validate_value(kind, &state.fields, maximum_bytes).map_err(|error| error.to_string())?;
            if state.revision == 0 || state.replace_revision == 0 || state.replace_revision > state.revision || size != state.encoded_bytes {
                return Err("invalid Catalog checkpoint header".to_owned());
            }
        }
        Status::Absent
            if state.revision != 0 || state.replace_revision != 0 || state.kind.is_some() || state.encoded_bytes != 0 || !state.fields.is_empty() =>
        {
            return Err("invalid Catalog checkpoint absence".to_owned());
        }
        Status::Deleted
            if state.revision == 0 || state.replace_revision != 0 || state.kind.is_some() || state.encoded_bytes != 0 || !state.fields.is_empty() =>
        {
            return Err("invalid Catalog checkpoint deletion".to_owned());
        }
        Status::Absent | Status::Deleted => {}
        _ => return Err("invalid Catalog checkpoint status".to_owned()),
    }
    Ok(())
}

/// 把 Catalog `kind` 转成检查点格式的稳定单字节标记。
fn kind_byte(kind: Kind) -> u8 {
    match kind {
        Kind::Value => 1,
        Kind::Array => 2,
        Kind::Map => 3,
    }
}

/// 从 `value` 的 `offset` 读取一个大端 `u64`；溢出或截断返回存储损坏错误。
fn read_u64(value: &[u8], offset: usize) -> std::result::Result<u64, String> {
    let end = offset.checked_add(8).ok_or_else(|| "Catalog checkpoint overflow".to_owned())?;
    Ok(u64::from_be_bytes(
        value
            .get(offset..end)
            .ok_or_else(|| "truncated Catalog checkpoint".to_owned())?
            .try_into()
            .map_err(|_| "invalid Catalog checkpoint integer".to_owned())?,
    ))
}

/// 从 `value` 的 `offset` 读取一个大端 `u32`；溢出或截断返回存储损坏错误。
fn read_u32(value: &[u8], offset: usize) -> std::result::Result<u32, String> {
    let end = offset.checked_add(4).ok_or_else(|| "Catalog checkpoint overflow".to_owned())?;
    Ok(u32::from_be_bytes(
        value
            .get(offset..end)
            .ok_or_else(|| "truncated Catalog checkpoint".to_owned())?
            .try_into()
            .map_err(|_| "invalid Catalog checkpoint integer".to_owned())?,
    ))
}

/// 把任意存储 `error` 转成最长 512 字节的安全诊断文本。
pub(super) fn store_error(error: impl std::fmt::Display) -> String {
    let mut detail = error.to_string();
    detail.truncate(512);
    detail
}

#[cfg(test)]
#[path = "../../tests/internal/catalog/checkpoint.rs"]
mod tests;
