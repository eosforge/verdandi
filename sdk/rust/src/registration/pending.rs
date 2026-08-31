use std::collections::BTreeMap;

use super::event::RegistrationEvent;
use crate::error::{Code, Error, Result};

#[derive(Clone, Debug)]
pub(crate) struct PendingChange {
    pub event: RegistrationEvent,
    pub base_revision: u64,
    pub latest_revision: u64,
    pub repair: bool,
}

pub(crate) struct PendingChanges {
    entries: BTreeMap<String, PendingChange>,
    bytes: usize,
    max_entries: usize,
    max_bytes: usize,
}

impl PendingChanges {
    /// 创建同时受 `max_entries` 和 `max_bytes` 约束的空事件合并器。
    pub(crate) fn new(max_entries: usize, max_bytes: usize) -> Self {
        Self {
            entries: BTreeMap::new(),
            bytes: 0,
            max_entries,
            max_bytes,
        }
    }

    /// 接管并按 UUID 合并一条 `event`。
    ///
    /// 每个 UUID 最多保留一个逻辑变化；连续 Update/Renew 原地合并，间隙退化为定向 repair。
    /// 数量和字节拒绝是事务性的：失败保留此前已接纳状态。
    pub(crate) fn add(&mut self, event: RegistrationEvent) -> Result<()> {
        let Some(current) = self.entries.get(&event.uuid) else {
            if self.entries.len() >= self.max_entries {
                return Err(Error::field(Code::Capacity, "selector_event_entries"));
            }
            let next = initial_pending_change(event);
            let next_bytes = self.bytes.saturating_add(pending_change_size(&next));
            if next_bytes > self.max_bytes {
                return Err(Error::field(Code::Capacity, "selector_event_bytes"));
            }
            self.entries.insert(next.event.uuid.clone(), next);
            self.bytes = next_bytes;
            return Ok(());
        };

        let previous_bytes = pending_change_size(current);
        let can_mutate = self.bytes.saturating_add(registration_event_size(&event)) <= self.max_bytes;
        if can_mutate {
            let Some(current) = self.entries.get_mut(&event.uuid) else {
                return Err(Error::field(Code::Corrupt, "selector_pending"));
            };
            merge_pending_change(current, event)?;
            self.bytes = self.bytes.saturating_sub(previous_bytes).saturating_add(pending_change_size(current));
            return Ok(());
        }

        // 临近容量上限时先在克隆上合并，拒绝不会破坏此前已接纳条目。
        let mut next = current.clone();
        merge_pending_change(&mut next, event)?;
        let next_bytes = self.bytes.saturating_sub(previous_bytes).saturating_add(pending_change_size(&next));
        if next_bytes > self.max_bytes {
            return Err(Error::field(Code::Capacity, "selector_event_bytes"));
        }
        let Some(current) = self.entries.get_mut(&next.event.uuid) else {
            return Err(Error::field(Code::Corrupt, "selector_pending"));
        };
        *current = next;
        self.bytes = next_bytes;
        Ok(())
    }

    /// 按 UUID 确定顺序取出全部 PendingChange，并原地重置字节计数与 map。
    pub(crate) fn drain(&mut self) -> Vec<PendingChange> {
        self.bytes = 0;
        std::mem::take(&mut self.entries).into_values().collect()
    }
}

/// 从首条 `event` 推导合并基准、最新 revision 与正常状态。
///
/// Update 基准为 revision-1，Renew 基准为同 revision，Register/Unregister 不依赖本地基准。
fn initial_pending_change(event: RegistrationEvent) -> PendingChange {
    let base_revision = match event.kind.as_str() {
        "update" => event.revision.saturating_sub(1),
        "renew" => event.revision,
        _ => 0,
    };
    PendingChange {
        latest_revision: event.revision,
        event,
        base_revision,
        repair: false,
    }
}

/// 把 `incoming` 的逻辑效果合并到同 UUID 的 `current`。
///
/// 生命周期、同 revision 内容冲突或 revision 间隙会拒绝/转为 repair；Unregister 具有终止优先级。
fn merge_pending_change(current: &mut PendingChange, incoming: RegistrationEvent) -> Result<()> {
    if incoming.kind == "unregister" {
        *current = initial_pending_change(incoming);
        return Ok(());
    }
    if current.event.kind == "unregister" {
        return Err(Error::field(Code::Transition, "@uuid"));
    }
    // repair 状态只跟踪最新 revision/timestamp，直到一条足够新的完整 Register 重建基准。
    if current.repair {
        if incoming.kind == "register" && incoming.revision >= current.latest_revision {
            *current = initial_pending_change(incoming);
            return Ok(());
        }
        if incoming.revision > current.latest_revision {
            current.latest_revision = incoming.revision;
            current.event.revision = incoming.revision;
        }
        current.event.timestamp = current.event.timestamp.max(incoming.timestamp);
        return Ok(());
    }

    if incoming.kind == "register" {
        if incoming.revision < current.latest_revision {
            return Ok(());
        }
        let mut next = initial_pending_change(incoming);
        if next.latest_revision == current.latest_revision {
            if current.event.kind == "register" && !same_registration_content(&current.event, &next.event) {
                *current = repair_pending_change(current, &next.event);
                return Ok(());
            }
            next.event.timestamp = next.event.timestamp.max(current.event.timestamp);
        }
        *current = next;
        return Ok(());
    }

    match incoming.kind.as_str() {
        "update" => {
            merge_pending_update(current, incoming);
            Ok(())
        }
        "renew" => {
            merge_pending_renew(current, &incoming);
            Ok(())
        }
        _ => Err(Error::field(Code::Invalid, "&kind")),
    }
}

/// 合并一条 Update。
///
/// 连续 revision 才可应用；完整 Register 基准原地覆盖已知 Data，Update/Renew 基准压缩为一个增量。
fn merge_pending_update(current: &mut PendingChange, incoming: RegistrationEvent) {
    if incoming.revision <= current.latest_revision {
        if incoming.revision == current.latest_revision
            && current.event.kind == "update"
            && (!same_optional_version(&current.event, &incoming) || current.event.data != incoming.data)
        {
            *current = repair_pending_change(current, &incoming);
        }
        return;
    }
    if incoming.revision != current.latest_revision.saturating_add(1) {
        *current = repair_pending_change(current, &incoming);
        return;
    }

    // 完整 Register 已包含所有固定 Data 字段；出现新字段说明本地结构不可信，必须 repair。
    if current.event.kind == "register" {
        if incoming.data.keys().any(|name| !current.event.data.contains_key(name)) {
            *current = repair_pending_change(current, &incoming);
            return;
        }
        for (name, value) in incoming.data {
            if let Some(field) = current.event.data.get_mut(&name) {
                *field = value;
            }
        }
        current.event.revision = incoming.revision;
        current.event.timestamp = current.event.timestamp.max(incoming.timestamp);
        if incoming.has_version {
            current.event.version = incoming.version;
            current.event.has_version = true;
        }
        current.latest_revision = incoming.revision;
        return;
    }

    let previous_timestamp = current.event.timestamp;
    if current.event.kind == "renew" {
        current.event = incoming;
    } else {
        current.event.data.extend(incoming.data);
        if incoming.has_version {
            current.event.version = incoming.version;
            current.event.has_version = true;
        }
        current.event.revision = incoming.revision;
    }
    current.event.timestamp = previous_timestamp.max(current.event.timestamp);
    current.latest_revision = current.event.revision;
}

/// 合并一条同 revision Renew，只提升 timestamp；跨 revision Renew 转为 repair。
fn merge_pending_renew(current: &mut PendingChange, incoming: &RegistrationEvent) {
    if incoming.revision < current.latest_revision {
        return;
    }
    if incoming.revision > current.latest_revision {
        *current = repair_pending_change(current, incoming);
        return;
    }
    current.event.timestamp = current.event.timestamp.max(incoming.timestamp);
}

/// 把无法安全合并的 `current`/`incoming` 压缩为一个有界定向权威读取标记。
fn repair_pending_change(current: &PendingChange, incoming: &RegistrationEvent) -> PendingChange {
    let latest_revision = current.latest_revision.max(incoming.revision);
    PendingChange {
        event: RegistrationEvent {
            kind: "repair".to_owned(),
            uuid: incoming.uuid.clone(),
            revision: latest_revision,
            timestamp: current.event.timestamp.max(incoming.timestamp),
            ttl: 0,
            version: 0,
            has_version: false,
            attr: Default::default(),
            data: Default::default(),
        },
        base_revision: 0,
        latest_revision,
        repair: true,
    }
}

/// 比较两条完整 Register 的 TTL、Version、Attr 和 Data，不比较时间戳与 revision。
fn same_registration_content(left: &RegistrationEvent, right: &RegistrationEvent) -> bool {
    left.ttl == right.ttl && left.version == right.version && left.attr == right.attr && left.data == right.data
}

/// 比较两条 Update 是否都省略 Version，或都携带相同 Version。
fn same_optional_version(left: &RegistrationEvent, right: &RegistrationEvent) -> bool {
    left.has_version == right.has_version && (!left.has_version || left.version == right.version)
}

/// 返回 `change` 当前事件的保守内存字节估算。
fn pending_change_size(change: &PendingChange) -> usize {
    registration_event_size(&change.event)
}

/// 估算 `event` 的 UUID、kind、字段名/值和 map 条目管理开销。
///
/// 常量 bookkeeping 是保守预算而非 Rust 分配器精确统计，用于稳定地限制合并缓存。
fn registration_event_size(event: &RegistrationEvent) -> usize {
    const BOOKKEEPING: usize = 128;
    BOOKKEEPING
        + event.uuid.len()
        + event.kind.len()
        + event.attr.iter().map(|(name, value)| 16 + name.len() + value.len()).sum::<usize>()
        + event.data.iter().map(|(name, value)| 16 + name.len() + value.len()).sum::<usize>()
}

#[cfg(test)]
#[path = "../../tests/internal/registration/pending.rs"]
mod tests;
