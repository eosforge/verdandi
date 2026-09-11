use std::path::PathBuf;

use super::{DELETE_LUA, PATCH_LUA, READ_LUA, REPLACE_LUA};
use super::{parse_script_reply, value_string};
use crate::Code;
use fred::types::Value;

const SCRIPT_SOURCES: [(&str, &str); 4] = [("read", READ_LUA), ("replace", REPLACE_LUA), ("patch", PATCH_LUA), ("delete", DELETE_LUA)];

#[test]
fn embedded_scripts_match_generated_catalog_sources() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..").join("lua/catalog");
    for (name, embedded) in SCRIPT_SOURCES {
        let path = root.join(format!("{name}.lua"));
        let canonical = match std::fs::read_to_string(&path) {
            Ok(canonical) => canonical,
            Err(error) => panic!("failed to read {}: {error}", path.display()),
        };
        assert_eq!(embedded, canonical, "{} differs", path.display());
    }
}

#[test]
fn catalog_protocol_rejects_coerced_text_and_local_statuses() {
    for value in [
        Value::Integer(1),
        Value::Queued,
        Value::Array(vec!["ok".into()]),
        Value::Null,
        Value::Bytes(vec![0xff].into()),
    ] {
        assert_eq!(value_string(value).map_err(|error| error.code()), Err(Code::Corrupt));
    }
    for status in ["closed", "deadline", "ambiguous", "target", "missing", "immutable", "unknown"] {
        let reply = parse_script_reply(Value::Array(vec!["&result".into(), "error".into(), "&status".into(), status.into()]));
        assert!(matches!(reply, Err(error) if error.code() == Code::Protocol), "{status}");
    }
    let reply = parse_script_reply(Value::Array(vec!["&result".into(), "error".into(), "&status".into(), "unavailable".into()]));
    assert!(matches!(reply, Err(error) if error.code() == Code::Unavailable));
}
