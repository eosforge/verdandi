mod hash;
mod key;
mod value;

use std::future::Future;
use std::time::Duration;

use fred::error::ErrorKind;
use fred::interfaces::{ClientLike, FredResult};

use crate::client::Client;
use crate::error::{Code, Error, Result};

pub use hash::{HashCommands, HashValue};
pub use key::{KeyCommands, TtlKeyCommands};
pub use value::{DecodeValue, EncodeValue};

const MAX_REDIS_KEY_BYTES: usize = 1024;
const MAX_REDIS_HASH_FIELDS: usize = 4096;
const MAX_REDIS_FIELD_NAME_BYTES: usize = 1024;
const MAX_REDIS_VALUE_BYTES: usize = 512 * 1024;
const MAX_REDIS_HASH_BYTES: usize = 512 * 1024;

#[derive(Clone, Copy)]
enum CommandKind {
    Read,
    Write,
}

impl Client {
    /// 用一条受操作超时约束的 PING 验证共享 Redis 传输当前可执行命令。
    pub async fn ping(&self) -> Result<()> {
        let _: String = command(self, self.driver().ping(None), CommandKind::Read).await?;
        Ok(())
    }

    /// 返回借用当前共享 Client 的整键/String 命令组；构造不执行 I/O 或分配连接。
    pub const fn key(&self) -> KeyCommands<'_> {
        KeyCommands { client: self }
    }

    /// 返回借用当前共享 Client 的 Redis Hash 命令组；构造不执行 I/O 或分配连接。
    pub const fn hash(&self) -> HashCommands<'_> {
        HashCommands { client: self }
    }
}

/// 在根传输关闭和普通操作超时共同约束下执行一条 Fred 命令。
///
/// `client` 提供传输生命周期，`future` 表示尚未等待的单次命令，`kind` 决定未知结果
/// 属于可用性失败还是不明确写入。成功返回驱动值，失败统一映射为稳定 Verdandi 错误。
async fn command<T, F>(client: &Client, future: F, kind: CommandKind) -> Result<T>
where
    F: Future<Output = FredResult<T>>,
{
    if client.is_closed() {
        return Err(Error::new(Code::Closed));
    }
    let _guard = client.begin_command().await;
    // 一个 select 同时承担关闭和超时等待；普通命令不创建子令牌或辅助任务。
    let timeout = tokio::time::timeout(client.timeout(), future);
    tokio::pin!(timeout);
    tokio::select! {
        _ = client.shutdown().cancelled() => Err(Error::new(match kind {
            CommandKind::Read => Code::Closed,
            CommandKind::Write => Code::Ambiguous,
        })),
        result = &mut timeout => match result {
            Ok(Ok(value)) => Ok(value),
            Ok(Err(error)) => Err(Error::driver(driver_code(kind, error.kind()), error)),
            Err(error) => Err(Error::driver(match kind {
                CommandKind::Read => Code::Deadline,
                CommandKind::Write => Code::Ambiguous,
            }, error)),
        },
    }
}

/// 根据命令 `kind` 和 Fred 的确定错误类别选择稳定 Verdandi 结果码。
///
/// 确定的认证/配置拒绝属于 Protocol，解析损坏属于 Corrupt；其余写入保守地标为 Ambiguous。
fn driver_code(kind: CommandKind, error: &ErrorKind) -> Code {
    match error {
        ErrorKind::Auth | ErrorKind::Config | ErrorKind::InvalidArgument | ErrorKind::InvalidCommand => Code::Protocol,
        ErrorKind::Parse | ErrorKind::Protocol => Code::Corrupt,
        _ => match kind {
            CommandKind::Read => Code::Unavailable,
            CommandKind::Write => Code::Ambiguous,
        },
    }
}

/// 校验完整 Redis `key` 非空且不超过根命令固定字节上限。
fn validate_key(key: &str) -> Result<()> {
    if key.is_empty() || key.len() > MAX_REDIS_KEY_BYTES {
        return Err(Error::field(Code::Invalid, "key"));
    }
    Ok(())
}

/// 校验 Redis Hash `field` 非空且不超过固定字段名字节上限。
fn validate_field(field: &str) -> Result<()> {
    if field.is_empty() || field.len() > MAX_REDIS_FIELD_NAME_BYTES {
        return Err(Error::field(Code::Invalid, "field"));
    }
    Ok(())
}

/// 把正数、整毫秒精度的 `ttl` 转换为 Redis 接受的 `i64` 毫秒。
///
/// 零值、亚毫秒精度或数值溢出返回 `Invalid`，防止隐式租期截断。
fn ttl_milliseconds(ttl: Duration) -> Result<i64> {
    if ttl.is_zero() || ttl.subsec_nanos() % 1_000_000 != 0 {
        return Err(Error::field(Code::Invalid, "ttl"));
    }
    i64::try_from(ttl.as_millis()).map_err(|_| Error::field(Code::Invalid, "ttl"))
}

/// 把 `size` 安全累加到 `total`，并实施完整 Hash 字节上限。
///
/// 加法溢出或超过上限时以 `field` 定位 `Capacity` 错误，且不允许继续发出 Redis I/O。
fn add_hash_size(total: &mut usize, size: usize, field: &str) -> Result<()> {
    *total = total.checked_add(size).ok_or_else(|| Error::field(Code::Capacity, field))?;
    if *total > MAX_REDIS_HASH_BYTES {
        return Err(Error::field(Code::Capacity, field));
    }
    Ok(())
}

/// 校验单个 Redis String 或 Hash 字段 `value` 不超过固定字节上限。
fn validate_value_size(value: &[u8], field: &str) -> Result<()> {
    if value.len() > MAX_REDIS_VALUE_BYTES {
        return Err(Error::field(Code::Capacity, field));
    }
    Ok(())
}

#[cfg(test)]
#[path = "../tests/internal/redis.rs"]
mod tests;
