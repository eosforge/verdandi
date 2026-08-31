/// 解析只读 Lua `value` 回复，并相对不可变 `base` 构造完整 RawState。
///
/// `maximum` 约束完整值；Absent/Deleted/Present 均严格检查允许字段，Lua error 转成稳定错误。
fn parse_read_reply(value: Value, base: &RawState, maximum: usize) -> Result<RawState> {
    let mut values = value_pairs(value, "read_reply")?;
    let result = take_string(&mut values, "&result")?;
    // 先分离 Lua error，再要求 ok/status/revision 组合精确匹配一个合法状态。
    if result == "error" {
        let status = take_string(&mut values, "&status")?;
        let code = Code::from_status(&status).ok_or_else(|| Error::field(Code::Corrupt, "&status"))?;
        let field = values.remove("&field").map(value_string).transpose()?.unwrap_or_default();
        let revision = values.remove("@revision").map(|value| parse_revision(value, true)).transpose()?.unwrap_or(0);
        if !values.is_empty() {
            return Err(Error::field(Code::Corrupt, "read_reply"));
        }
        let error = if field.is_empty() { Error::new(code) } else { Error::field(code, field) };
        return Err(if revision == 0 { error } else { error.with_revision(revision) });
    }
    if result != "ok" {
        return Err(Error::field(Code::Corrupt, "&result"));
    }
    let status = take_string(&mut values, "&status")?;
    let revision = parse_revision(values.remove("@revision").ok_or_else(|| Error::field(Code::Corrupt, "@revision"))?, true)?;
    match status.as_str() {
        "absent" => {
            if revision != 0 || !values.is_empty() {
                return Err(Error::field(Code::Corrupt, "read_reply"));
            }
            Ok(RawState::initial(Status::Absent))
        }
        "deleted" => {
            if revision == 0 || !values.is_empty() {
                return Err(Error::field(Code::Corrupt, "read_reply"));
            }
            Ok(RawState {
                revision,
                replace_revision: 0,
                status: Status::Deleted,
                kind: None,
                encoded_bytes: 0,
                fields: Arc::new(Fields::new()),
            })
        }
        "present" => parse_present_read(values, base, revision, maximum),
        _ => Err(Error::field(Code::Corrupt, "&status")),
    }
}

/// 解析 Present 只读回复的 replace revision、Kind、encoded bytes 和字段模式。
///
/// `base` 仅在 patch/unchanged 模式满足完整且连续条件时复用；`revision` 必须为正，
/// `maximum` 对合并后的完整值重新实施容量校验。
fn parse_present_read(mut values: BTreeMap<String, Value>, base: &RawState, revision: u64, maximum: usize) -> Result<RawState> {
    if revision == 0 {
        return Err(Error::field(Code::Corrupt, "@revision"));
    }
    let mode = take_string(&mut values, "&mode")?;
    let replace_revision = parse_revision(
        values
            .remove("@replace_revision")
            .ok_or_else(|| Error::field(Code::Corrupt, "@replace_revision"))?,
        false,
    )?;
    if replace_revision > revision {
        return Err(Error::field(Code::Corrupt, "@replace_revision"));
    }
    let kind = Kind::parse(&take_string(&mut values, "@kind")?).ok_or_else(|| Error::field(Code::Corrupt, "@kind"))?;
    let encoded_bytes = canonical_usize(&take_string(&mut values, "@encoded_bytes")?, maximum, "@encoded_bytes")?;
    let fields = decode_redis_fields(values.remove("&fields").ok_or_else(|| Error::field(Code::Corrupt, "&fields"))?)?;
    if !values.is_empty() {
        return Err(Error::field(Code::Corrupt, "read_reply"));
    }
    // replace 自带完整字段；patch 在同一 replace 世代上合并；unchanged 直接复用 Arc 字段存储。
    let fields = match mode.as_str() {
        "replace" => Arc::new(fields),
        "patch"
            if base.complete_present()
                && !fields.is_empty()
                && base.revision < revision
                && base.revision >= replace_revision
                && base.replace_revision == replace_revision
                && base.kind == Some(kind) =>
        {
            let mut merged = base.fields.as_ref().clone();
            merged.extend(fields);
            Arc::new(merged)
        }
        "unchanged"
            if base.complete_present()
                && fields.is_empty()
                && base.revision == revision
                && base.replace_revision == replace_revision
                && base.kind == Some(kind)
                && base.encoded_bytes == encoded_bytes =>
        {
            Arc::clone(&base.fields)
        }
        _ => return Err(Error::field(Code::Corrupt, "&mode")),
    };
    let actual = validate_value(kind, &fields, maximum)?;
    if actual != encoded_bytes {
        return Err(Error::field(Code::Corrupt, "@encoded_bytes").with_revision(revision));
    }
    Ok(RawState {
        revision,
        replace_revision,
        status: Status::Present,
        kind: Some(kind),
        encoded_bytes,
        fields,
    })
}

/// 把 Lua 交替字段名/值数组解码为拥有型有序 Fields。
///
/// `value` 必须是偶数长度数组且不超过字段上限；保留名、非字节值和重复字段均返回 Corrupt。
fn decode_redis_fields(value: Value) -> Result<Fields> {
    let Value::Array(values) = value else {
        return Err(Error::field(Code::Corrupt, "fields"));
    };
    if values.len() % 2 != 0 || values.len() / 2 > MAX_FIELDS {
        return Err(Error::field(Code::Corrupt, "fields"));
    }
    let mut fields = Fields::new();
    let mut values = values.into_iter();
    while let Some(name) = values.next() {
        let Some(value) = values.next() else {
            return Err(Error::field(Code::Corrupt, "fields"));
        };
        let name = value_string(name)?;
        if name.is_empty() || name.starts_with('@') {
            return Err(Error::field(Code::Corrupt, "fields"));
        }
        let value = value.into_owned_bytes().ok_or_else(|| Error::field(Code::Corrupt, "fields"))?;
        match fields.entry(name) {
            BTreeEntry::Vacant(entry) => {
                entry.insert(value);
            }
            BTreeEntry::Occupied(entry) => {
                return Err(Error::field(Code::Corrupt, entry.key()));
            }
        }
    }
    Ok(fields)
}
