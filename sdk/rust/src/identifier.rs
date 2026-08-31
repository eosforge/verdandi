/// 校验 `value` 是否为 1 至 32 字节、只含 ASCII 大小写字母的 Zone。
pub(crate) fn valid_zone(value: &str) -> bool {
    !value.is_empty() && value.len() <= 32 && value.bytes().all(|byte| byte.is_ascii_alphabetic())
}

/// 校验 `value` 是否为规范 Type：首字符是 ASCII 字母，后续可含字母数字、`_`、`.`、`-`，总长不超过 64 字节。
pub(crate) fn valid_type(value: &str) -> bool {
    let bytes = value.as_bytes();
    !bytes.is_empty()
        && bytes.len() <= 64
        && bytes[0].is_ascii_alphabetic()
        && bytes[1..].iter().all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'.' | b'-'))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn zone_and_type_grammar_are_stable() {
        assert!(valid_zone("Alpha"));
        assert!(!valid_zone("alpha-1"));
        assert!(!valid_zone(&"a".repeat(33)));
        assert!(valid_type("Proxy.v2-east_1"));
        assert!(!valid_type("2Proxy"));
        assert!(!valid_type("Proxy:bad"));
    }
}
