use fred::prelude::*;
use fred::types::Value;

use crate::{Code, Error, FieldValue, Fields, Result};

use super::client::{ActiveGuard, Client};
use super::model::{Kind, MutationResult, Patch, Path, array_field_index, canonical_usize, catalog_key, encode, mutation_keys, validate_patch, validate_value};
use super::scripts::{ScriptKind, bytes_value, parse_revision, parse_script_reply, value_string};

/// 对多个 Path 执行原子 Replace、严格 Patch 与 Delete 的轻量写入视图。
///
/// Publisher 不拥有任务、锁或独立关闭状态；Catalog Client 生命周期控制所有操作。
pub struct Publisher {
    client: Client,
}

impl Publisher {
    /// 从 `client` 构造轻量 Publisher，不执行 Redis I/O 或启动任务。
    ///
    /// 构造时只验证 Catalog Client 仍可准入；返回对象无需单独关闭。
    pub fn new(client: &Client) -> Result<Self> {
        let guard = client.inner.admit()?;
        drop(guard);
        Ok(Self { client: client.clone() })
    }

    /// 以 Redis 执行顺序的最后写入获胜语义，原子替换 `path` 的完整值。
    ///
    /// `kind` 决定 Value/Array/Map 形状，`value` 通过 FieldValue 编码；所有校验在 Redis I/O 前完成。
    /// 成功返回 Redis 分配的正 revision，传输响应丢失返回 `Ambiguous`。
    pub async fn replace<T: FieldValue>(&self, path: &Path, kind: Kind, value: &T) -> Result<MutationResult> {
        let fields = encode(value)?;
        let encoded_bytes = validate_value(kind, &fields, self.maximum_bytes())?;
        let _guard = self.begin()?;
        let names = canonical_names(kind, &fields);
        let mut arguments = Vec::with_capacity(4 + fields.len() * 2);
        arguments.push(path.member().into());
        arguments.push(kind.as_str().into());
        arguments.push(encoded_bytes.to_string().into());
        arguments.push(fields.len().to_string().into());
        append_fields(&mut arguments, &fields, &names);
        self.mutate(ScriptKind::Replace, path, arguments)
            .await
            .map(|revision| MutationResult { revision })
    }

    /// 仅当 `patch.base_revision` 精确匹配当前值时，原子新增或覆盖字段。
    ///
    /// SDK 先读取必要字段计算完整容量；Lua 随后原子复核同一基准。
    /// 并发同 Path 写入只会使本次操作返回 `Stale`，不会提交基于旧状态的投影。
    pub async fn patch(&self, path: &Path, patch: Patch) -> Result<MutationResult> {
        super::client::ClientInner::validate_revision(patch.base_revision, "@base_revision")?;
        validate_patch(&patch.set, self.maximum_bytes())?;
        let _guard = self.begin()?;
        let projected = self.project_patch(path, patch.base_revision, &patch.set).await?;
        let names = patch.set.keys().map(String::as_str).collect::<Vec<_>>();
        let mut arguments = Vec::with_capacity(4 + patch.set.len() * 2);
        arguments.push(path.member().into());
        arguments.push(patch.base_revision.to_string().into());
        arguments.push(projected.to_string().into());
        arguments.push(patch.set.len().to_string().into());
        append_fields(&mut arguments, &patch.set, &names);
        self.mutate(ScriptKind::Patch, path, arguments)
            .await
            .map(|revision| MutationResult { revision })
    }

    /// 原子删除完整 `path` 并创建一个新的 tombstone revision。
    ///
    /// 即使目标已经缺失或删除，也会获得新的全局 revision；成功结果可用于观察收敛。
    pub async fn delete(&self, path: &Path) -> Result<MutationResult> {
        let _guard = self.begin()?;
        self.mutate(ScriptKind::Delete, path, vec![path.member().into()])
            .await
            .map(|revision| MutationResult { revision })
    }

    /// 返回当前 Publisher 继承的单值编码字节上限。
    fn maximum_bytes(&self) -> usize {
        self.client.inner.config.max_record_bytes
    }

    /// 在 Catalog Client 的关闭栅栏中接纳一次操作，并通过 Drop 自动释放计数。
    fn begin(&self) -> Result<ActiveGuard> {
        self.client.inner.admit()
    }

    /// 执行 `kind` 指定的变更 Lua，并要求成功回复携带正 revision。
    ///
    /// `arguments` 必须已按固定 ABI 组装；写入响应丢失保守返回 `Ambiguous`。
    async fn mutate(&self, kind: ScriptKind, path: &Path, arguments: Vec<Value>) -> Result<u64> {
        let value = self
            .client
            .inner
            .call_script(kind, mutation_keys(&self.client.inner.config.zone, path), arguments, true)
            .await?;
        let reply = parse_script_reply(value)?;
        if reply.result != "ok" || reply.revision == 0 {
            return Err(Error::field(Code::Corrupt, "@revision"));
        }
        Ok(reply.revision)
    }

    /// 读取当前头部及被修改字段，验证基准与 Kind，并计算 Patch 后完整编码字节数。
    ///
    /// 此读取不持有分布式锁；最终 Lua 必须再次精确匹配 `base_revision`，
    /// 因而读取与提交之间的同 Path 竞态会安全地返回 `Stale`。
    async fn project_patch(&self, path: &Path, base_revision: u64, fields: &Fields) -> Result<usize> {
        let mut names = vec!["@revision".to_owned(), "@kind".to_owned(), "@encoded_bytes".to_owned()];
        names.extend(fields.keys().cloned());
        let value: Value = self
            .client
            .inner
            .command(
                self.client.inner.driver().hmget(catalog_key(&self.client.inner.config.zone, path), names),
                Code::Unavailable,
            )
            .await?;
        let Value::Array(values) = value else {
            return Err(Error::field(Code::Corrupt, "catalog_header"));
        };
        if values.len() != 3 + fields.len() || values[..3].iter().any(Value::is_null) {
            return Err(Error::field(Code::Corrupt, "catalog_header"));
        }
        let mut values = values.into_iter();
        let revision = parse_revision(values.next().ok_or_else(|| Error::field(Code::Corrupt, "catalog_header"))?, false)?;
        if revision != base_revision {
            return Err(Error::field(Code::Stale, "@base_revision").with_revision(revision));
        }
        let kind = Kind::parse(&value_string(values.next().ok_or_else(|| Error::field(Code::Corrupt, "catalog_header"))?)?)
            .ok_or_else(|| Error::field(Code::Corrupt, "@kind"))?;
        if kind == Kind::Value {
            return Err(Error::field(Code::Transition, "@kind").with_revision(revision));
        }
        let bytes_text = value_string(values.next().ok_or_else(|| Error::field(Code::Corrupt, "catalog_header"))?)?;
        // HMGET 的后三项与 BTreeMap 字段顺序一致，可单遍用旧值长度计算精确增量。
        let mut projected = canonical_usize(&bytes_text, self.maximum_bytes(), "@encoded_bytes")?;
        for ((name, replacement), previous) in fields.iter().zip(values) {
            if previous.is_null() {
                if kind == Kind::Array {
                    return Err(Error::field(Code::Transition, name).with_revision(revision));
                }
                projected = projected
                    .checked_add(name.len())
                    .and_then(|value| value.checked_add(replacement.len()))
                    .ok_or_else(|| Error::field(Code::Capacity, "value"))?;
            } else {
                let previous = previous.into_owned_bytes().ok_or_else(|| Error::field(Code::Corrupt, name))?;
                projected = projected
                    .checked_sub(previous.len())
                    .and_then(|value| value.checked_add(replacement.len()))
                    .ok_or_else(|| Error::field(Code::Capacity, "value"))?;
            }
            if projected > self.maximum_bytes() {
                return Err(Error::field(Code::Capacity, "value").with_revision(revision));
            }
        }
        Ok(projected)
    }
}

/// 按 `names` 的规范顺序把 `fields` 追加为交替名称/二进制值 Lua 参数。
fn append_fields(arguments: &mut Vec<Value>, fields: &Fields, names: &[&str]) {
    for name in names {
        if let Some(value) = fields.get(*name) {
            arguments.push((*name).into());
            arguments.push(bytes_value(value.clone()));
        }
    }
}

/// 返回 `fields` 的协议规范发布顺序。
///
/// Array 使用数值下标顺序；Value/Map 复用 BTreeMap 字节字典序。调用前必须已验证形状。
fn canonical_names(kind: Kind, fields: &Fields) -> Vec<&str> {
    if kind == Kind::Array {
        let mut names = vec![""; fields.len()];
        for name in fields.keys() {
            if let Some(index) = array_field_index(name, fields.len()) {
                names[index] = name;
            }
        }
        names
    } else {
        fields.keys().map(String::as_str).collect()
    }
}
