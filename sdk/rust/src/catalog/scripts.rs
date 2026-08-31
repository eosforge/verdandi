use std::collections::BTreeMap;
use std::collections::btree_map::Entry;

use fred::prelude::*;
use fred::types::Value;
use fred::types::scripts::Script;

use crate::{Code, Error, Result};

use super::model::canonical_u64;

const READ_LUA: &str = include_str!("protocol/read.lua");
const REPLACE_LUA: &str = include_str!("protocol/replace.lua");
const PATCH_LUA: &str = include_str!("protocol/patch.lua");
const DELETE_LUA: &str = include_str!("protocol/delete.lua");

#[derive(Clone, Copy)]
pub(super) enum ScriptKind {
    Read,
    Replace,
    Patch,
    Delete,
}

pub(super) struct Scripts {
    read: Script,
    replace: Script,
    patch: Script,
    delete: Script,
}

impl Scripts {
    /// 从四份嵌入的规范 Lua 文本创建可并发复用的 Fred Script 对象。
    pub(super) fn new() -> Self {
        Self {
            read: Script::from_lua(READ_LUA),
            replace: Script::from_lua(REPLACE_LUA),
            patch: Script::from_lua(PATCH_LUA),
            delete: Script::from_lua(DELETE_LUA),
        }
    }

    /// 返回 `kind` 对应的不可变脚本引用，不执行加载或 Redis I/O。
    pub(super) fn get(&self, kind: ScriptKind) -> &Script {
        match kind {
            ScriptKind::Read => &self.read,
            ScriptKind::Replace => &self.replace,
            ScriptKind::Patch => &self.patch,
            ScriptKind::Delete => &self.delete,
        }
    }

    /// 并发预加载全部 Catalog 脚本到 `client` 当前 Redis 主节点。
    ///
    /// 任一 SCRIPT LOAD 失败则返回 Fred 错误；脚本自身仍可在调用时用 EVALSHA 重载。
    pub(super) async fn load(&self, client: &fred::clients::Client) -> FredResult<()> {
        let read = Box::pin(self.read.load(client));
        let replace = Box::pin(self.replace.load(client));
        let patch = Box::pin(self.patch.load(client));
        let delete = Box::pin(self.delete.load(client));
        let _: ((), (), (), ()) = tokio::try_join!(read, replace, patch, delete)?;
        Ok(())
    }
}

pub(super) struct ScriptReply {
    pub result: String,
    pub revision: u64,
}

/// 解析 Catalog Lua 的交替名称/值 `value` 回复。
///
/// 成功返回稳定 result 与可选 revision；Lua `error` 被转换为带 field/revision 的 Verdandi Error，
/// 重复、未知或缺失字段统一视为 `Corrupt`。
pub(super) fn parse_script_reply(value: Value) -> Result<ScriptReply> {
    let mut fields = value_pairs(value, "script_reply")?;
    let result = take_string(&mut fields, "&result")?;
    // error 和 ok 两类回复分别验证允许字段，避免兼容路径静默接受协议漂移。
    if result == "error" {
        let status = take_string(&mut fields, "&status")?;
        let code = Code::from_status(&status).ok_or_else(|| Error::field(Code::Corrupt, "&status"))?;
        let field = fields.remove("&field").map(value_string).transpose()?.unwrap_or_default();
        let revision = fields.remove("@revision").map(|value| parse_revision(value, true)).transpose()?.unwrap_or(0);
        if !fields.is_empty() {
            return Err(Error::field(Code::Corrupt, "script_reply"));
        }
        let error = if field.is_empty() { Error::new(code) } else { Error::field(code, field) };
        return Err(if revision == 0 { error } else { error.with_revision(revision) });
    }
    if result != "ok" {
        return Err(Error::field(Code::Corrupt, "&result"));
    }
    let revision = fields.remove("@revision").map(|value| parse_revision(value, true)).transpose()?.unwrap_or(0);
    fields.remove("@floor_revision");
    fields.remove("@pruned");
    if !fields.is_empty() {
        return Err(Error::field(Code::Corrupt, "script_reply"));
    }
    Ok(ScriptReply { result, revision })
}

/// 把交替名称/值 Fred 数组转换为有序 map。
///
/// `field` 用于整体形状错误定位；奇数长度、非字符串名称或重复名称返回 `Corrupt`。
pub(super) fn value_pairs(value: Value, field: &str) -> Result<BTreeMap<String, Value>> {
    let Value::Array(values) = value else {
        return Err(Error::field(Code::Corrupt, field));
    };
    if values.is_empty() || values.len() % 2 != 0 {
        return Err(Error::field(Code::Corrupt, field));
    }
    let mut fields = BTreeMap::new();
    let mut values = values.into_iter();
    while let Some(name) = values.next() {
        let Some(value) = values.next() else {
            return Err(Error::field(Code::Corrupt, field));
        };
        let name = value_string(name)?;
        match fields.entry(name) {
            Entry::Vacant(entry) => {
                entry.insert(value);
            }
            Entry::Occupied(entry) => {
                return Err(Error::field(Code::Corrupt, entry.key()));
            }
        }
    }
    Ok(fields)
}

/// 从 `fields` 取出必需的 `name` 并转换为 UTF-8 字符串。
pub(super) fn take_string(fields: &mut BTreeMap<String, Value>, name: &str) -> Result<String> {
    value_string(fields.remove(name).ok_or_else(|| Error::field(Code::Corrupt, name))?)
}

/// 把一个 Fred Value 的拥有型字节转换为 UTF-8 String；其他类型或非法 UTF-8 返回 `Corrupt`。
pub(super) fn value_string(value: Value) -> Result<String> {
    let bytes = value.into_owned_bytes().ok_or_else(|| Error::field(Code::Corrupt, "value"))?;
    String::from_utf8(bytes).map_err(|_| Error::field(Code::Corrupt, "value"))
}

/// 把 Fred `value` 解析为规范 revision；`allow_zero` 决定零值是否合法。
pub(super) fn parse_revision(value: Value, allow_zero: bool) -> Result<u64> {
    let text = value_string(value)?;
    canonical_u64(&text, allow_zero, "@revision")
}

/// 把拥有型字节 `value` 零拷贝包装为 Fred Value，供二进制字段参数使用。
pub(super) fn bytes_value(value: Vec<u8>) -> Value {
    Value::Bytes(value.into())
}

#[cfg(test)]
#[path = "../../tests/internal/catalog/scripts.rs"]
mod tests;
