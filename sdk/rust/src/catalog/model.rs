use std::collections::BTreeSet;
use std::sync::Arc;

use arc_swap::ArcSwap;

use crate::{Code, Error, FieldValue, Fields, Result};

pub(super) const MAX_REVISION: u64 = (1_u64 << 53) - 1;
pub(super) const MAX_RECORD_BYTES: usize = 4 * 1024 * 1024;
pub(super) const MAX_FIELDS: usize = 65_536;

/// 解析不超过 Catalog 安全 revision 上限的规范无符号十进制文本。
///
/// `allow_zero` 决定零值是否有效，`field` 用于错误定位；前导零、非数字或超限返回 `Corrupt`。
pub(super) fn canonical_u64(value: &str, allow_zero: bool, field: &str) -> Result<u64> {
    let parsed = value.parse::<u64>().map_err(|_| Error::field(Code::Corrupt, field))?;
    if parsed > MAX_REVISION || !allow_zero && parsed == 0 || value != "0" && (value.starts_with('0') || !value.bytes().all(|byte| byte.is_ascii_digit())) {
        return Err(Error::field(Code::Corrupt, field));
    }
    Ok(parsed)
}

/// 解析不超过 `maximum` 的规范 `usize` 十进制文本。
///
/// `field` 标记损坏来源；前导零、非数字、平台宽度溢出或超过上限均返回 `Corrupt`。
pub(super) fn canonical_usize(value: &str, maximum: usize, field: &str) -> Result<usize> {
    let parsed = value.parse::<usize>().map_err(|_| Error::field(Code::Corrupt, field))?;
    if parsed > maximum || value != "0" && (value.starts_with('0') || !value.bytes().all(|byte| byte.is_ascii_digit())) {
        return Err(Error::field(Code::Corrupt, field));
    }
    Ok(parsed)
}

#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
/// 一个 Catalog Client Zone 内不可变的 part/id 身份。
pub struct Path {
    part: String,
    id: String,
}

impl Path {
    /// 校验并构造一个 Catalog Path。
    ///
    /// `part` 和 `id` 会转为拥有型字符串；两者必须符合协议字符集，字节上限分别为 64 与 128。
    pub fn new(part: impl Into<String>, id: impl Into<String>) -> Result<Self> {
        let part = part.into();
        let id = id.into();
        if !valid_segment(&part, 64) {
            return Err(Error::field(Code::Invalid, "part"));
        }
        if !valid_segment(&id, 128) {
            return Err(Error::field(Code::Invalid, "id"));
        }
        Ok(Self { part, id })
    }

    /// 返回不可变分区部分的借用。
    pub fn part(&self) -> &str {
        &self.part
    }

    /// 返回不可变标识部分的借用。
    pub fn id(&self) -> &str {
        &self.id
    }

    /// 构造索引使用的规范 `part:id` 成员文本。
    pub(super) fn member(&self) -> String {
        format!("{}:{}", self.part, self.id)
    }

    /// 无需分割或分配即可判断 `member` 是否精确表示当前 Path。
    pub(super) fn matches_member(&self, member: &str) -> bool {
        member.len() == self.part.len() + self.id.len() + 1
            && member.starts_with(&self.part)
            && member.as_bytes().get(self.part.len()) == Some(&b':')
            && member.ends_with(&self.id)
    }

    /// 从规范 `part:id` 索引成员解析 Path；分隔或段校验失败返回 `None`。
    pub(super) fn from_member(member: &str) -> Option<Self> {
        let (part, id) = member.split_once(':')?;
        Self::new(part, id).ok()
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
/// Catalog 值的规范顶层表示。
pub enum Kind {
    /// 恰好包含一个名为 `value` 的应用字段。
    Value,
    /// 包含规范、连续十进制字段 `0..n-1`。
    Array,
    /// 包含由应用命名的字段。
    Map,
}

impl Kind {
    /// 返回写入 Redis 和 Pub/Sub 协议的稳定小写名称。
    pub(super) const fn as_str(self) -> &'static str {
        match self {
            Self::Value => "value",
            Self::Array => "array",
            Self::Map => "map",
        }
    }

    /// 把稳定协议 `value` 解析为 Kind；未知名称返回 `None`。
    pub(super) fn parse(value: &str) -> Option<Self> {
        match value {
            "value" => Some(Self::Value),
            "array" => Some(Self::Array),
            "map" => Some(Self::Map),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
/// 一次已接纳变更的 Redis 所有结果。
pub struct MutationResult {
    /// 范围为 `1..=2^53-1` 的正 revision。
    pub revision: u64,
}

#[derive(Clone, Debug)]
/// 对 Array 或 Map 执行的严格新增与覆盖操作。
pub struct Patch {
    /// 本次 Patch 要求精确匹配的当前 Catalog revision。
    pub base_revision: u64,
    /// 完整字段增量；首版不支持字段级删除。
    pub set: Fields,
}

#[derive(Clone, Debug, Default)]
/// 一个 Subscriber 的 Zone、分区与精确 Path 覆盖范围。
pub struct Subscription {
    /// 是否订阅 Client Zone 中的全部 Path。
    pub zone: bool,
    /// 订阅这些分区下的全部 Path。
    pub parts: Vec<String>,
    /// 订阅这些精确 Path。
    pub paths: Vec<Path>,
}

#[derive(Clone)]
pub(super) struct NormalizedSubscription {
    pub zone: bool,
    pub parts: BTreeSet<String>,
    pub paths: BTreeSet<Path>,
    pub channels: Vec<String>,
    pub patterns: Vec<String>,
}

impl NormalizedSubscription {
    /// 校验并规范化 `subscription`，同时构造 `zone` 下确定顺序的频道与模式集合。
    ///
    /// 空覆盖或非法 Part 返回 `Invalid`；Zone 覆盖会消除更窄的 Part/Path，
    /// Part 覆盖会消除其下重复的精确 Path。
    pub(super) fn new(zone: &str, subscription: Subscription) -> Result<Self> {
        let mut parts = BTreeSet::new();
        for part in subscription.parts {
            if !valid_segment(&part, 64) {
                return Err(Error::field(Code::Invalid, "part"));
            }
            parts.insert(part);
        }
        let mut paths = subscription.paths.into_iter().collect::<BTreeSet<_>>();
        if !subscription.zone && parts.is_empty() && paths.is_empty() {
            return Err(Error::field(Code::Invalid, "subscription"));
        }
        let prefix = zone_prefix(zone);
        // Zone 模式覆盖全部更窄项，直接返回一条模式避免无意义的频道列表。
        if subscription.zone {
            return Ok(Self {
                zone: true,
                parts: BTreeSet::new(),
                paths: BTreeSet::new(),
                channels: Vec::new(),
                patterns: vec![format!("{prefix}:*")],
            });
        }
        // 删除已被 Part 模式覆盖的精确 Path，降低订阅和检查点状态规模。
        paths.retain(|path| !parts.contains(path.part()));
        let patterns = parts.iter().map(|part| format!("{prefix}:{part}:*")).collect();
        let channels = paths.iter().map(|path| catalog_key(zone, path)).collect();
        Ok(Self {
            zone: false,
            parts,
            paths,
            channels,
            patterns,
        })
    }

    /// 判断有效 `path` 是否落在规范化覆盖范围内。
    pub(super) fn covers(&self, path: &Path) -> bool {
        self.zone || self.parts.contains(path.part()) || self.paths.contains(path)
    }

    /// 报告当前范围是否包含 Zone/Part 模式，因而需要扫描全局 revision 索引。
    pub(super) fn broad(&self) -> bool {
        self.zone || !self.parts.is_empty()
    }

    /// 为本地检查点构造稳定、无歧义且顺序确定的订阅作用域文本。
    pub(super) fn checkpoint_scope(&self) -> String {
        if self.zone {
            return "zone\n".to_owned();
        }
        let mut scope = String::new();
        for part in &self.parts {
            scope.push_str("part\0");
            scope.push_str(part);
            scope.push('\n');
        }
        for path in &self.paths {
            scope.push_str("path\0");
            scope.push_str(&path.member());
            scope.push('\n');
        }
        scope
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
/// 一个稳定 Entry 的本地同步状态。
pub enum Status {
    /// 修复正在运行；最后一份完整值可以继续保留，但不可声明为当前。
    Synchronizing,
    /// 已同步的完整值。
    Present,
    /// 已同步缺失，且没有保留 tombstone revision。
    Absent,
    /// 已同步删除，并保留删除 revision。
    Deleted,
    /// 修复当前无法推进；最后完整值可以保留但不可选择为当前。
    Unavailable,
    /// Subscriber 已关闭的终止状态。
    Closed,
}

#[derive(Clone, Debug)]
pub(super) struct RawState {
    pub revision: u64,
    pub replace_revision: u64,
    pub status: Status,
    pub kind: Option<Kind>,
    pub encoded_bytes: usize,
    pub fields: Arc<Fields>,
}

impl RawState {
    /// 构造 revision 为零、无值且处于 `status` 的初始不可变状态。
    pub(super) fn initial(status: Status) -> Self {
        Self {
            revision: 0,
            replace_revision: 0,
            status,
            kind: None,
            encoded_bytes: 0,
            fields: Arc::new(Fields::new()),
        }
    }

    /// 浅克隆当前完整状态并只替换 `status`；不可变字段 `Arc` 继续共享。
    pub(super) fn with_status(&self, status: Status) -> Self {
        let mut state = self.clone();
        state.status = status;
        state
    }

    /// 报告状态是否持有可作为 Patch 基准的完整 Present 内容。
    pub(super) fn complete_present(&self) -> bool {
        self.revision != 0 && self.replace_revision != 0 && self.kind.is_some()
    }
}

struct EntryInner {
    path: Path,
    state: ArcSwap<RawState>,
}

#[derive(Clone)]
/// 在更新、删除和重建期间始终绑定同一 Path 的稳定句柄。
pub struct Entry(Arc<EntryInner>);

impl Entry {
    /// 创建绑定 `path`、以 `status` 初始化的稳定 Entry。
    pub(super) fn new(path: Path, status: Status) -> Self {
        Self(Arc::new(EntryInner {
            path,
            state: ArcSwap::from_pointee(RawState::initial(status)),
        }))
    }

    /// 返回此 Entry 的不可变 Path。
    pub fn path(&self) -> &Path {
        &self.0.path
    }

    /// 无锁返回当前本地同步状态。
    pub fn status(&self) -> Status {
        self.0.state.load().status
    }

    /// 无锁返回此 Path 最后已知的完整 revision；尚无完整状态时为零。
    pub fn revision(&self) -> u64 {
        self.0.state.load().revision
    }

    /// 报告状态是否为已权威对齐的 Present、Absent 或 Deleted。
    pub fn synchronized(&self) -> bool {
        synchronized_status(self.status())
    }

    /// 不执行 Redis 或磁盘 I/O，解码一份与内部状态脱离的强类型投影。
    ///
    /// `T` 从当前完整字段解码；失败附加当前 revision 且不修改 Entry。同步中或不可用状态
    /// 仍可返回最后完整值，但调用方必须结合 `Snapshot::synchronized` 判断是否可用。
    pub fn load<T: FieldValue>(&self) -> Result<Snapshot<T>> {
        let state = self.0.state.load_full();
        let value = if state.kind.is_some() {
            Some(T::decode_fields(state.fields.as_ref()).map_err(|error| error.with_field_if_empty("value").with_revision(state.revision))?)
        } else {
            None
        };
        Ok(Snapshot {
            revision: state.revision,
            status: state.status,
            synchronized: synchronized_status(state.status),
            value,
        })
    }

    /// 原子加载并克隆当前不可变 RawState 的 `Arc`。
    pub(super) fn state(&self) -> Arc<RawState> {
        self.0.state.load_full()
    }

    /// 仅当当前状态仍与 `current` 为同一 `Arc` 时原子发布 `next`。
    ///
    /// 成功返回新状态 `Arc`；竞争失败返回 `None`，调用方必须重新读取并决定是否重试。
    pub(super) fn compare_and_swap(&self, current: &Arc<RawState>, next: RawState) -> Option<Arc<RawState>> {
        let next = Arc::new(next);
        let previous = self.0.state.compare_and_swap(current, Arc::clone(&next));
        Arc::ptr_eq(&previous, current).then_some(next)
    }

    /// 无条件原子发布完整 `state`，并返回已发布状态的共享引用。
    pub(super) fn store(&self, state: RawState) -> Arc<RawState> {
        let state = Arc::new(state);
        self.0.state.store(Arc::clone(&state));
        state
    }
}

#[derive(Clone, Debug)]
/// 一个 Entry revision 的独立强类型投影。
pub struct Snapshot<T> {
    /// 此快照表示的最后完整 revision。
    pub revision: u64,
    /// 同步与删除状态。
    pub status: Status,
    /// 仅在 Present、Absent 和 Deleted 时为 true。
    pub synchronized: bool,
    /// 保留完整原始字段时解码出的值。
    pub value: Option<T>,
}

/// 判断 `status` 是否代表已经权威同步的 Present、Absent 或 Deleted。
pub(super) fn synchronized_status(status: Status) -> bool {
    matches!(status, Status::Present | Status::Absent | Status::Deleted)
}

/// 调用应用 `value` 的 FieldValue 编码器，返回一份新建完整字段 map。
///
/// 编码错误在缺少字段上下文时补为 `value`；此函数不执行形状或容量校验。
pub(super) fn encode<T: FieldValue>(value: &T) -> Result<Fields> {
    let mut fields = Fields::new();
    value.encode_fields(&mut fields).map_err(|error| error.with_field_if_empty("value"))?;
    Ok(fields)
}

/// 按 `kind` 校验完整 `fields` 的形状、名称、连续数组索引和 `maximum` 字节上限。
///
/// 成功返回字段名与值的精确总字节数；失败不修改输入。
pub(super) fn validate_value(kind: Kind, fields: &Fields, maximum: usize) -> Result<usize> {
    if fields.len() > MAX_FIELDS {
        return Err(Error::field(Code::Capacity, "fields"));
    }
    // 先验证顶层形状，再统一核算字段名称和值，避免不同 Kind 的容量语义分叉。
    match kind {
        Kind::Value if fields.len() != 1 || !fields.contains_key("value") => {
            return Err(Error::field(Code::Contract, "value"));
        }
        Kind::Array => {
            for name in fields.keys() {
                if array_field_index(name, fields.len()).is_none() {
                    return Err(Error::field(Code::Contract, "array"));
                }
            }
        }
        Kind::Value | Kind::Map => {}
    }
    let mut bytes = 0_usize;
    for (name, value) in fields {
        validate_field_name(name)?;
        bytes = bytes
            .checked_add(name.len())
            .and_then(|bytes| bytes.checked_add(value.len()))
            .ok_or_else(|| Error::field(Code::Capacity, "value"))?;
        if bytes > maximum {
            return Err(Error::field(Code::Capacity, "value"));
        }
    }
    Ok(bytes)
}

/// 把 `name` 解析为 `[0,count)` 内无前导零的规范数组下标。
///
/// 空数组、非法字符、算术溢出、越界或非规范表示返回 `None`，过程不分配字符串。
pub(super) fn array_field_index(name: &str, count: usize) -> Option<usize> {
    if count == 0 || name.is_empty() || name.starts_with('0') && name != "0" {
        return None;
    }
    let mut value = 0_usize;
    for byte in name.bytes() {
        let digit = byte.checked_sub(b'0').filter(|digit| *digit <= 9)? as usize;
        value = value.checked_mul(10)?.checked_add(digit)?;
        if value >= count {
            return None;
        }
    }
    Some(value)
}

/// 校验非空 Patch `fields` 的字段数量、名称与 `maximum` 增量字节上限。
///
/// 此函数不验证目标 Kind 或基准 revision，它们由 Publisher 的权威读取路径负责。
pub(super) fn validate_patch(fields: &Fields, maximum: usize) -> Result<()> {
    if fields.is_empty() {
        return Err(Error::field(Code::Invalid, "patch"));
    }
    if fields.len() > MAX_FIELDS {
        return Err(Error::field(Code::Capacity, "fields"));
    }
    let mut bytes = 0_usize;
    for (name, value) in fields {
        validate_field_name(name)?;
        bytes = bytes
            .checked_add(name.len())
            .and_then(|bytes| bytes.checked_add(value.len()))
            .ok_or_else(|| Error::field(Code::Capacity, "patch"))?;
        if bytes > maximum {
            return Err(Error::field(Code::Capacity, "patch"));
        }
    }
    Ok(())
}

/// 校验应用字段 `name` 非空且不使用 `@` 协议保留前缀。
pub(super) fn validate_field_name(name: &str) -> Result<()> {
    if name.is_empty() || name.starts_with('@') {
        return Err(Error::field(Code::Invalid, name));
    }
    Ok(())
}

/// 校验 Path 段 `value` 的字节上限和 ASCII 字符集。
fn valid_segment(value: &str, maximum: usize) -> bool {
    !value.is_empty() && value.len() <= maximum && value.bytes().all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
}

/// 返回指定 `zone` 的 Catalog Redis 键前缀。
pub(super) fn zone_prefix(zone: &str) -> String {
    format!("verdandi:catalog:{zone}")
}

/// 返回 `zone` 全局 revision/floor 元数据 Hash 键。
pub(super) fn meta_key(zone: &str) -> String {
    format!("verdandi:catalog:{zone}:@meta")
}

/// 返回 `zone` 当前存在 Path 的 revision ZSET 键。
pub(super) fn live_key(zone: &str) -> String {
    format!("verdandi:catalog:{zone}:@live")
}

/// 返回 `zone` tombstone Path 的 revision ZSET 键。
pub(super) fn deleted_key(zone: &str) -> String {
    format!("verdandi:catalog:{zone}:@deleted")
}

/// 返回 `zone` tombstone 写入时间 ZSET 键。
pub(super) fn deleted_time_key(zone: &str) -> String {
    format!("verdandi:catalog:{zone}:@deleted_time")
}

/// 返回 `zone` 中 `path` 的完整 Catalog Hash 键，同时也是其 Pub/Sub 频道。
pub(super) fn catalog_key(zone: &str, path: &Path) -> String {
    format!("verdandi:catalog:{zone}:{}:{}", path.part, path.id)
}

/// 返回 `zone` 中 `path` 各字段 revision 的 Hash 键。
pub(super) fn field_revisions_key(zone: &str, path: &Path) -> String {
    format!("verdandi:catalog:{zone}:{}:{}:@field_revisions", path.part, path.id)
}

/// 按变更 Lua 固定 ABI 返回 `path` 所需的完整 Redis 键列表。
pub(super) fn mutation_keys(zone: &str, path: &Path) -> Vec<String> {
    vec![
        meta_key(zone),
        live_key(zone),
        deleted_key(zone),
        deleted_time_key(zone),
        catalog_key(zone, path),
        field_revisions_key(zone, path),
    ]
}

/// 按只读 Lua 固定 ABI 返回 `path` 所需的 Redis 键列表。
pub(super) fn read_keys(zone: &str, path: &Path) -> Vec<String> {
    vec![
        live_key(zone),
        deleted_key(zone),
        deleted_time_key(zone),
        catalog_key(zone, path),
        field_revisions_key(zone, path),
    ]
}

#[cfg(test)]
#[path = "../../tests/internal/catalog/model.rs"]
mod tests;
