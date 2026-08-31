use std::path::PathBuf;

use super::{DELETE_LUA, PATCH_LUA, READ_LUA, REPLACE_LUA};

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
