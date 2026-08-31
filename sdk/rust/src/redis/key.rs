use std::time::Duration;

use fred::prelude::*;
use fred::types::Expiration;

use super::{CommandKind, DecodeValue, EncodeValue, MAX_REDIS_VALUE_BYTES, command, ttl_milliseconds, validate_key, validate_value_size};
use crate::client::Client;
use crate::error::{Code, Error, Result};

/// 从一个 Client 借用的、有容量上限的 Redis String 与整键命令组。
pub struct KeyCommands<'client> {
    pub(super) client: &'client Client,
}

/// 一次性带 TTL 写入模式；构造本身不执行 Redis I/O。
#[must_use = "a TTL write mode does nothing until set or store is awaited"]
pub struct TtlKeyCommands<'client> {
    client: &'client Client,
    ttl: Duration,
}

impl<'client> KeyCommands<'client> {
    /// 读取并解码 `key` 对应的 Redis String。
    ///
    /// `T` 必须实现稳定 `DecodeValue` 契约；缺失键返回 `None` 且不调用解码器，
    /// 损坏的现有值返回错误而不暴露部分结果。
    pub async fn get<T: DecodeValue>(&self, key: &str) -> Result<Option<T>> {
        let Some(value) = self.load(key).await? else {
            return Ok(None);
        };
        T::decode_value(&value).map(Some)
    }

    /// 把 `key` 对应的 Redis String 读取为调用方独占原始字节。
    ///
    /// 缺失键返回 `None`；键和返回值均受根命令容量上限约束。
    pub async fn load(&self, key: &str) -> Result<Option<Vec<u8>>> {
        validate_key(key)?;
        let value: Option<Vec<u8>> = command(self.client, self.client.driver().get(key), CommandKind::Read).await?;
        if let Some(value) = &value {
            validate_value_size(value, "value")?;
        }
        Ok(value)
    }

    /// 把类型化 `value` 持久写入 `key`；普通 SET 语义会清除已有 TTL。
    ///
    /// 编码和容量校验先于 I/O；发送后响应丢失返回 `Ambiguous`。
    pub async fn set<T: EncodeValue + ?Sized>(&self, key: &str, value: &T) -> Result<()> {
        validate_key(key)?;
        let encoded = encode(value)?;
        write(self.client, key, encoded, None).await
    }

    /// 在脱离调用方 `value` 字节后持久写入 `key`。
    ///
    /// 调用方可在返回后立即复用输入缓冲区；普通 SET 会清除已有 TTL。
    pub async fn store(&self, key: &str, value: &[u8]) -> Result<()> {
        validate_key(key)?;
        validate_value_size(value, "value")?;
        write(self.client, key, value.to_vec(), None).await
    }

    /// 为下一次写入选择正数、整毫秒精度的 `ttl`。
    ///
    /// 返回模式会消费当前轻量命令句柄；直到调用并等待 `set` 或 `store` 才执行 I/O。
    pub const fn with_ttl(self, ttl: Duration) -> TtlKeyCommands<'client> {
        TtlKeyCommands { client: self.client, ttl }
    }

    /// 同步删除完整 `key`，并返回执行时该键是否存在。
    pub async fn delete(&self, key: &str) -> Result<bool> {
        validate_key(key)?;
        let deleted: u64 = command(self.client, self.client.driver().del(key), CommandKind::Write).await?;
        Ok(deleted == 1)
    }

    /// 查询完整 `key` 当前是否存在，不读取其值。
    pub async fn exists(&self, key: &str) -> Result<bool> {
        validate_key(key)?;
        let count: u64 = command(self.client, self.client.driver().exists(key), CommandKind::Read).await?;
        Ok(count == 1)
    }

    /// 为现有 `key` 设置正数、整毫秒精度的 `ttl`，并报告是否实际应用。
    pub async fn expire(&self, key: &str, ttl: Duration) -> Result<bool> {
        validate_key(key)?;
        let milliseconds = ttl_milliseconds(ttl)?;
        command(self.client, self.client.driver().pexpire(key, milliseconds, None), CommandKind::Write).await
    }
}

impl TtlKeyCommands<'_> {
    /// 把类型化 `value` 写入 `key` 并应用构造命令组时选定的 TTL。
    ///
    /// `self` 被消费，避免同一临时 TTL 模式被意外重复使用。
    pub async fn set<T: EncodeValue + ?Sized>(self, key: &str, value: &T) -> Result<()> {
        validate_key(key)?;
        let milliseconds = ttl_milliseconds(self.ttl)?;
        let encoded = encode(value)?;
        write(self.client, key, encoded, Some(Expiration::PX(milliseconds))).await
    }

    /// 深拷贝原始 `value` 后写入 `key`，并应用构造命令组时选定的 TTL。
    pub async fn store(self, key: &str, value: &[u8]) -> Result<()> {
        validate_key(key)?;
        let milliseconds = ttl_milliseconds(self.ttl)?;
        validate_value_size(value, "value")?;
        write(self.client, key, value.to_vec(), Some(Expiration::PX(milliseconds))).await
    }
}

/// 调用 `value` 的 `EncodeValue` 实现并实施单值容量上限。
///
/// 返回新建且可转移给 Fred 的字节缓冲区；编码失败不执行 Redis I/O。
fn encode<T: EncodeValue + ?Sized>(value: &T) -> Result<Vec<u8>> {
    let mut encoded = Vec::new();
    value.encode_value(&mut encoded)?;
    if encoded.len() > MAX_REDIS_VALUE_BYTES {
        return Err(Error::field(Code::Capacity, "value"));
    }
    Ok(encoded)
}

/// 执行统一的 SET 写入路径。
///
/// `key` 与 `value` 在此已由上层校验；`expiration` 为 `None` 时持久写入，
/// 否则使用已经精确转换的 TTL。未知写入结果返回 `Ambiguous`。
async fn write(client: &Client, key: &str, value: Vec<u8>, expiration: Option<Expiration>) -> Result<()> {
    let _: () = command(client, client.driver().set(key, value, expiration, None, false), CommandKind::Write).await?;
    Ok(())
}

#[cfg(test)]
#[path = "../../tests/internal/redis/key.rs"]
mod tests;
