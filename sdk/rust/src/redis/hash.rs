use std::collections::HashSet;

use fred::prelude::*;
use fred::types::MultipleKeys;

use super::{CommandKind, MAX_REDIS_HASH_FIELDS, MAX_REDIS_VALUE_BYTES, add_hash_size, command, validate_field, validate_key, validate_value_size};
use crate::client::Client;
use crate::error::{Code, Error, Result};
use crate::fields::Fields;

/// 由应用类型拥有的静态 Redis Hash 投影契约。
pub trait HashValue: Sized {
    /// HMGET 与 HSET 使用的有序、精确 Redis 字段名。
    const FIELDS: &'static [&'static str];

    /// 按 `FIELDS` 顺序解码值；缺失位置以 `None` 传入，失败不得返回部分对象。
    fn decode_hash(values: &[Option<Vec<u8>>]) -> Result<Self>;

    /// 为 `FIELDS` 中每个名称恰好编码一个值到初始为空的 `destination`。
    fn encode_hash(&self, destination: &mut Fields) -> Result<()>;
}

/// 从一个 Client 借用的、有容量上限的 Redis Hash 命令组。
pub struct HashCommands<'client> {
    pub(super) client: &'client Client,
}

impl HashCommands<'_> {
    /// 用 HMGET 精确读取 `T::FIELDS` 并解码一个完整 `T`。
    ///
    /// `key` 是完整 Redis 键；缺失字段行为由 `T` 定义，内置 derive 使用 `Default`。
    /// 字段描述或现有值损坏时返回错误，不发布部分对象。
    pub async fn get<T: HashValue>(&self, key: &str) -> Result<T> {
        validate_key(key)?;
        validate_hash_names(T::FIELDS)?;
        let fields = T::FIELDS.iter().copied().collect::<MultipleKeys>();
        let values: Vec<Option<Vec<u8>>> = command(self.client, self.client.driver().hmget(key, fields), CommandKind::Read).await?;
        if values.len() != T::FIELDS.len() {
            return Err(Error::field(Code::Corrupt, "hash"));
        }
        let mut total = 0;
        for (field, value) in T::FIELDS.iter().zip(&values) {
            add_hash_size(&mut total, field.len(), "hash")?;
            if let Some(value) = value {
                validate_value_size(value, field)?;
                add_hash_size(&mut total, value.len(), "hash")?;
            }
        }
        T::decode_hash(&values)
    }

    /// 把 `key` 中全部 Hash 字段读取为独占原始字节 map。
    ///
    /// 缺失键返回空 map；字段数量、名称、单值和完整字节数均受固定上限约束。
    pub async fn load(&self, key: &str) -> Result<Fields> {
        validate_key(key)?;
        let fields: Fields = command(self.client, self.client.driver().hgetall(key), CommandKind::Read).await?;
        validate_fields(&fields)?;
        Ok(fields)
    }

    /// 用 HSET 写入 `value` 按 `T::FIELDS` 描述的全部字段。
    ///
    /// 未出现在 `T` 中的已有字段不会被删除；编码输出必须与静态字段集合精确匹配。
    pub async fn set<T: HashValue>(&self, key: &str, value: &T) -> Result<()> {
        validate_key(key)?;
        validate_hash_names(T::FIELDS)?;
        let mut fields = Fields::new();
        value.encode_hash(&mut fields)?;
        validate_encoded_hash(T::FIELDS, &fields)?;
        self.hset(key, fields).await
    }

    /// 用 HSET 写入 `fields` 提供的全部原始字段。
    ///
    /// 这是局部写入而非完整替换；输入会复制为本次命令的拥有型数据并实施容量校验。
    pub async fn store(&self, key: &str, fields: &Fields) -> Result<()> {
        validate_key(key)?;
        if fields.is_empty() {
            return Err(Error::field(Code::Invalid, "fields"));
        }
        validate_fields(fields)?;
        self.hset(key, fields.clone()).await
    }

    /// 从 `key` 删除 `fields` 指定的多个 Hash 字段，并返回实际存在的字段数。
    ///
    /// 空列表或任何非法字段名在 I/O 前失败；响应丢失按 `Ambiguous` 返回。
    pub async fn delete(&self, key: &str, fields: &[&str]) -> Result<u64> {
        validate_key(key)?;
        if fields.is_empty() {
            return Err(Error::field(Code::Invalid, "fields"));
        }
        if fields.len() > MAX_REDIS_HASH_FIELDS {
            return Err(Error::field(Code::Capacity, "fields"));
        }
        let mut total = 0;
        for field in fields {
            validate_field(field)?;
            add_hash_size(&mut total, field.len(), "fields")?;
        }
        let fields = fields.iter().copied().collect::<MultipleKeys>();
        command(self.client, self.client.driver().hdel(key, fields), CommandKind::Write).await
    }

    /// 查询 `field` 当前是否存在于 `key` 的 Hash 中，不读取字段值。
    pub async fn contains_field(&self, key: &str, field: &str) -> Result<bool> {
        validate_key(key)?;
        validate_field(field)?;
        command(self.client, self.client.driver().hexists(key, field), CommandKind::Read).await
    }

    /// 返回 `key` 当前包含的 Hash 字段数量；缺失键返回零。
    #[allow(clippy::len_without_is_empty)]
    pub async fn len(&self, key: &str) -> Result<u64> {
        validate_key(key)?;
        command(self.client, self.client.driver().hlen(key), CommandKind::Read).await
    }

    /// 执行已经规范化为拥有型 `fields` 的统一 HSET 写路径。
    ///
    /// `key` 与完整 Hash 容量已在上层校验；未知写入结果返回 `Ambiguous`。
    async fn hset(&self, key: &str, fields: Fields) -> Result<()> {
        let _: u64 = command(self.client, self.client.driver().hset(key, fields), CommandKind::Write).await?;
        Ok(())
    }
}

/// 校验静态 `fields` 非空、数量受限、名称合法且互不重复。
///
/// 这是 `HashValue::FIELDS` 的契约检查，不访问 Redis。
fn validate_hash_names(fields: &[&str]) -> Result<()> {
    if fields.is_empty() {
        return Err(Error::field(Code::Contract, "fields"));
    }
    if fields.len() > MAX_REDIS_HASH_FIELDS {
        return Err(Error::field(Code::Capacity, "fields"));
    }
    let mut unique = HashSet::with_capacity(fields.len());
    let mut total = 0;
    for field in fields {
        validate_field(field).map_err(|_| Error::field(Code::Contract, "field"))?;
        if !unique.insert(*field) {
            return Err(Error::field(Code::Contract, "fields"));
        }
        add_hash_size(&mut total, field.len(), "fields")?;
    }
    Ok(())
}

/// 要求编码后的 `fields` 与 `expected` 名称集合精确一致，并实施完整容量校验。
fn validate_encoded_hash(expected: &[&str], fields: &Fields) -> Result<()> {
    if fields.len() != expected.len() || expected.iter().any(|field| !fields.contains_key(*field)) {
        return Err(Error::field(Code::Contract, "fields"));
    }
    validate_fields(fields)
}

/// 校验原始 `fields` 的数量、名称、单值和完整 Hash 字节大小。
fn validate_fields(fields: &Fields) -> Result<()> {
    if fields.len() > MAX_REDIS_HASH_FIELDS {
        return Err(Error::field(Code::Capacity, "fields"));
    }
    let mut total = 0;
    for (field, value) in fields {
        validate_field(field)?;
        if value.len() > MAX_REDIS_VALUE_BYTES {
            return Err(Error::field(Code::Capacity, field));
        }
        add_hash_size(&mut total, field.len(), "hash")?;
        add_hash_size(&mut total, value.len(), "hash")?;
    }
    Ok(())
}

#[cfg(test)]
#[path = "../../tests/internal/redis/hash.rs"]
mod tests;
