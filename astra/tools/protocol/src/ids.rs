//! MessageID 清单的解析和分配规则, 供显式生成器及独立回归测试共同使用.

use std::{
    collections::{BTreeMap, BTreeSet},
    fs, io,
    path::Path,
};

/// 保留已有映射和已删除消息的编号, 按名称排序后为新消息追加非零 uint16 ID.
pub(crate) fn assign(previous: &str, messages: impl IntoIterator<Item = String>) -> io::Result<BTreeMap<u16, String>> {
    let mut ids = BTreeMap::new();
    let mut names = BTreeSet::new();
    // 清单损坏或分支合并造成冲突时直接失败, 不擅自重新编号已有消息.
    for line in previous.lines().filter(|line| !line.trim().is_empty() && !line.starts_with('#')) {
        let mut fields = line.split_whitespace();
        let id = fields
            .next()
            .and_then(|id| id.parse::<u16>().ok())
            .ok_or_else(|| invalid("invalid message ID"))?;
        let name = fields.next().ok_or_else(|| invalid("missing message name"))?;
        if id == 0 || fields.next().is_some() || ids.insert(id, name.to_owned()).is_some() || !names.insert(name.to_owned()) {
            return Err(invalid("duplicate or invalid entry in proto/message-ids.lock"));
        }
    }
    // BTreeSet 同时去重和排序, 保证输入顺序及平台变化不会影响新编号.
    let messages: BTreeSet<_> = messages.into_iter().collect();
    for name in messages.difference(&names) {
        let id = ids
            .last_key_value()
            .map_or(0_u16, |(id, _)| *id)
            .checked_add(1)
            .ok_or_else(|| invalid("uint16 MessageID space exhausted"))?;
        ids.insert(id, name.clone());
    }
    Ok(ids)
}

/// 默认只检查编号清单; 仅显式生成命令允许写入, 返回值表示文件是否发生变化.
pub(crate) fn store(path: &Path, previous: &str, current: &str, allow_update: bool) -> io::Result<bool> {
    if previous == current {
        return Ok(false);
    }
    if !allow_update {
        return Err(invalid("generated protocol is stale; run scripts/generate-proto; see proto/README.md"));
    }
    fs::write(path, current)?;
    Ok(true)
}

/// 构建输入错误不包含外部数据原文, 由清单路径定位需要修正的来源.
fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
