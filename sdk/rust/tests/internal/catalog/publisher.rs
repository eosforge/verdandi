use super::*;

#[test]
fn patch_capacity_uses_final_state_for_both_container_shapes() {
    for (kind, first, second) in [("map", "a", "b"), ("array", "0", "1")] {
        let fields = Fields::from([(first.to_owned(), b"xxxxx".to_vec()), (second.to_owned(), b"y".to_vec())]);
        let reply = Value::Array(vec!["1".into(), kind.into(), "8".into(), "x".into(), "yyyyy".into()]);
        assert_eq!(project_patch_reply(reply, 1, &fields, 8), Ok(8));
        let overflow = Fields::from([(first.to_owned(), b"xxxxxx".to_vec()), (second.to_owned(), b"y".to_vec())]);
        let reply = Value::Array(vec!["1".into(), kind.into(), "8".into(), "x".into(), "yyyyy".into()]);
        assert_eq!(project_patch_reply(reply, 1, &overflow, 8).map_err(|error| error.code()), Err(Code::Capacity));
    }
}

#[test]
fn patch_distinguishes_missing_and_corrupt_headers() {
    let fields = Fields::from([("x".into(), Vec::new())]);
    for (values, code) in [
        (vec![Value::Null; 4], Code::Stale),
        (vec![Value::Null, Value::Null, Value::Null, "orphan".into()], Code::Corrupt),
        (vec!["1".into(), Value::Null, Value::Null, Value::Null], Code::Corrupt),
        (vec![Value::Null], Code::Corrupt),
        (vec!["1".into(), "unknown".into(), "1".into(), "".into()], Code::Corrupt),
        (vec!["1".into(), "value".into(), "1".into(), "".into()], Code::Transition),
    ] {
        assert_eq!(
            project_patch_reply(Value::Array(values), 1, &fields, 8).map_err(|error| error.code()),
            Err(code)
        );
    }
}
