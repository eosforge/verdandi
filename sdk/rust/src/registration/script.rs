use fred::prelude::*;
use fred::types::scripts::Script;

use super::client::ClientInner;
use crate::error::{Code, Error, Result};
use crate::fields::MAX_SAFE_INTEGER;

pub(crate) const REGISTRATION_REGISTER_LUA: &str = include_str!("register.lua");
pub(crate) const REGISTRATION_UPDATE_LUA: &str = include_str!("update.lua");
pub(crate) const REGISTRATION_RENEW_LUA: &str = include_str!("renew.lua");
pub(crate) const REGISTRATION_UNREGISTER_LUA: &str = include_str!("unregister.lua");

#[derive(Clone, Copy, Debug)]
pub(crate) enum RegistrationScriptKind {
    Register,
    Update,
    Renew,
    Unregister,
}

pub(crate) struct RegistrationScripts {
    register: Script,
    update: Script,
    renew: Script,
    unregister: Script,
}

impl RegistrationScripts {
    /// 从四份嵌入的规范 Lua 文本创建可并发复用的 Fred Script 对象。
    pub(crate) fn new() -> Self {
        Self {
            register: Script::from_lua(REGISTRATION_REGISTER_LUA),
            update: Script::from_lua(REGISTRATION_UPDATE_LUA),
            renew: Script::from_lua(REGISTRATION_RENEW_LUA),
            unregister: Script::from_lua(REGISTRATION_UNREGISTER_LUA),
        }
    }

    /// 返回 `kind` 对应的不可变脚本引用，不执行 Redis I/O。
    fn script(&self, kind: RegistrationScriptKind) -> &Script {
        match kind {
            RegistrationScriptKind::Register => &self.register,
            RegistrationScriptKind::Update => &self.update,
            RegistrationScriptKind::Renew => &self.renew,
            RegistrationScriptKind::Unregister => &self.unregister,
        }
    }

    /// 并发预加载全部 Registration 脚本到 `client` 当前 Redis 主节点。
    ///
    /// 任一 SCRIPT LOAD 失败返回 Fred 错误；调用路径仍可在 NOSCRIPT 时按脚本重载。
    pub(crate) async fn load(&self, client: &fred::clients::Client) -> std::result::Result<(), fred::error::Error> {
        // Script::load Future 保留嵌入源码；装箱后并发加载不会放大调用者栈帧。
        let register = Box::pin(self.register.load(client));
        let update = Box::pin(self.update.load(client));
        let renew = Box::pin(self.renew.load(client));
        let unregister = Box::pin(self.unregister.load(client));
        let _: ((), (), (), ()) = tokio::try_join!(register, update, renew, unregister,)?;
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct RegistrationReply {
    pub revision: u64,
    pub timestamp: u64,
}

impl ClientInner {
    /// 调用一项 Registration 生命周期 Lua 并解析稳定回复。
    ///
    /// `kind` 选择脚本；`type_name`/`uuid` 构造精确 Registration 与 Registry 键；
    /// `arguments` 必须符合固定位置 ABI；`ambiguous` 表示命令发出后结果丢失必须返回 Ambiguous。
    /// 调用受普通操作超时约束，但不直接观察领域取消，使显式领域关闭仍可执行有界 Unregister 清理。
    pub(crate) async fn call_registration(
        &self,
        kind: RegistrationScriptKind,
        type_name: &str,
        uuid: &str,
        arguments: Vec<Value>,
        ambiguous: bool,
    ) -> Result<RegistrationReply> {
        let keys = vec![
            format!("verdandi:registration:{}:{type_name}:{uuid}", self.config.zone),
            format!("verdandi:registry:{}:{type_name}", self.config.zone),
        ];
        // 生命周期 worker 串行拥有写入；即使领域关闭已发出，当前操作和随后有界 Unregister
        // 仍允许在普通超时内完成，以保留优雅清理语义。
        let result = tokio::time::timeout(
            self.config.timeout,
            self.scripts.script(kind).evalsha_with_reload::<Value, _, _>(&self.driver(), keys, arguments),
        )
        .await;
        let value = match result {
            Ok(Ok(value)) => value,
            Ok(Err(error)) => {
                return Err(Error::driver(if ambiguous { Code::Ambiguous } else { Code::Unavailable }, error));
            }
            Err(error) => {
                return Err(Error::driver(if ambiguous { Code::Ambiguous } else { Code::Deadline }, error));
            }
        };
        parse_registration_reply(value)
    }
}

/// 按 Register 固定 ABI 构造参数。
///
/// `uuid`、`revision`、`ttl_ms`、`version` 位于前四项；`attr` 名称加 `.` 前缀，
/// `data` 保持原名，二者按 BTreeMap 顺序交替追加名称和值。
pub(crate) fn register_arguments(uuid: &str, revision: u64, ttl_ms: u64, version: u64, attr: &crate::Fields, data: &crate::Fields) -> Vec<Value> {
    let mut arguments = Vec::with_capacity(4 + (attr.len() + data.len()) * 2);
    arguments.push(uuid.into());
    arguments.push(revision.to_string().into());
    arguments.push(ttl_ms.to_string().into());
    arguments.push(version.to_string().into());
    for (name, value) in attr {
        append_bytes(&mut arguments, format!(".{name}"), value.clone());
    }
    for (name, value) in data {
        append_bytes(&mut arguments, name.clone(), value.clone());
    }
    arguments
}

/// 按 Update 固定 ABI 构造参数。
///
/// `version=None` 编码为空字符串表示不修改 Version；`data` 只包含本次局部变化。
pub(crate) fn update_arguments(uuid: &str, revision: u64, version: Option<u64>, data: &crate::Fields) -> Vec<Value> {
    let mut arguments = Vec::with_capacity(3 + data.len() * 2);
    arguments.push(uuid.into());
    arguments.push(revision.to_string().into());
    arguments.push(version.map_or_else(String::new, |value| value.to_string()).into());
    for (name, value) in data {
        append_bytes(&mut arguments, name.clone(), value.clone());
    }
    arguments
}

/// 按 Renew 固定 ABI 构造 `uuid` 与当前 `revision` 两项参数。
pub(crate) fn renew_arguments(uuid: &str, revision: u64) -> Vec<Value> {
    vec![uuid.into(), revision.to_string().into()]
}

/// 按 Unregister 固定 ABI 构造只含 `uuid` 的参数。
pub(crate) fn unregister_arguments(uuid: &str) -> Vec<Value> {
    vec![uuid.into()]
}

/// 向 `arguments` 追加一个拥有型 `field` 和二进制 `value` 对。
fn append_bytes(arguments: &mut Vec<Value>, field: String, value: Vec<u8>) {
    arguments.push(field.into());
    arguments.push(Value::Bytes(value.into()));
}

/// 解析 Registration Lua 的交替名称/值回复。
///
/// 成功返回可选 revision/timestamp；Lua error 转为带 field/revision 的稳定错误。
/// 重复、缺失或未知结果类型返回 Corrupt；未知附加字段向前兼容地忽略。
fn parse_registration_reply(value: Value) -> Result<RegistrationReply> {
    let Value::Array(values) = value else {
        return Err(Error::field(Code::Corrupt, "reply"));
    };
    if values.len() < 2 || values.len() % 2 != 0 {
        return Err(Error::field(Code::Corrupt, "reply"));
    }
    // 单遍收集已知字段并用 seen 拒绝所有重复名称，避免后值覆盖前值产生歧义。
    let mut result = None;
    let mut status = None;
    let mut field = None;
    let mut revision = None;
    let mut timestamp = None;
    let mut seen = std::collections::BTreeSet::new();
    let mut iterator = values.into_iter();
    while let Some(key) = iterator.next() {
        let Some(value) = iterator.next() else {
            return Err(Error::field(Code::Corrupt, "reply"));
        };
        let key = value_string(key).ok_or_else(|| Error::field(Code::Corrupt, "reply"))?;
        if !seen.insert(key.clone()) {
            return Err(Error::field(Code::Corrupt, key));
        }
        match key.as_str() {
            "&result" => result = value_string(value),
            "&status" => status = value_string(value),
            "&field" => field = value_string(value),
            "@revision" => revision = value_u64(value),
            "@timestamp" => timestamp = value_u64(value),
            _ => {}
        }
    }
    match result.as_deref() {
        Some("ok") => Ok(RegistrationReply {
            revision: revision.unwrap_or(0),
            timestamp: timestamp.unwrap_or(0),
        }),
        Some("error") => {
            let status = status.ok_or_else(|| Error::field(Code::Corrupt, "&status"))?;
            let code = Code::from_status(&status).ok_or_else(|| Error::field(Code::Corrupt, "&status"))?;
            let mut error = field.map_or_else(|| Error::new(code), |field| Error::field(code, field));
            if let Some(revision) = revision {
                error = error.with_revision(revision);
            }
            Err(error)
        }
        _ => Err(Error::field(Code::Corrupt, "&result")),
    }
}

/// 把 Fred `value` 的拥有型字节转换为 UTF-8 String；类型或编码错误返回 `None`。
pub(crate) fn value_string(value: Value) -> Option<String> {
    String::from_utf8(value.into_owned_bytes()?).ok()
}

/// 把 Fred `value` 转换为协议安全范围内的正 `u64`。
///
/// 接受整数、字符串或字节十进制表示；零、负数、类型错误和超上限返回 `None`。
pub(crate) fn value_u64(value: Value) -> Option<u64> {
    let value = match value {
        Value::Integer(value) if value > 0 => u64::try_from(value).ok()?,
        Value::String(value) => value.parse().ok()?,
        Value::Bytes(value) => std::str::from_utf8(&value).ok()?.parse().ok()?,
        _ => return None,
    };
    (value <= MAX_SAFE_INTEGER).then_some(value)
}

#[cfg(test)]
#[path = "../../tests/internal/registration/script.rs"]
mod tests;
