use super::*;

#[derive(Clone, Debug, Eq, PartialEq)]
struct Record {
    meta: Meta,
    attr: Fields,
    data: Fields,
}

fn detached_record(record: &SelectorRecord) -> Record {
    Record {
        meta: record.meta.clone(),
        attr: record.attr.as_ref().clone(),
        data: record.data.as_ref().clone(),
    }
}

#[test]
fn subscriber_pong_accepts_resp2_and_resp3_shapes_only() {
    let nonce = "0123456789abcdef";
    assert!(valid_pong(nonce.into(), nonce));
    assert!(valid_pong(Value::Array(vec!["pong".into(), nonce.into()]), nonce));
    assert!(valid_pong(Value::Array(vec!["PONG".into(), nonce.into()]), nonce));
    assert!(!valid_pong(Value::Array(vec!["pong".into(), "wrong".into()]), nonce));
    assert!(!valid_pong(Value::Array(vec![nonce.into()]), nonce));
}

#[test]
fn retry_delay_is_bounded() {
    for failures in 0..100 {
        let delay = retry_delay(failures, Duration::from_millis(100), Duration::from_secs(5), 2, 50);
        assert!(delay >= Duration::from_millis(50));
        assert!(delay <= Duration::from_secs(5));
    }
}

#[test]
fn natural_expiry_retains_for_one_additional_ttl() {
    let record = retained_test_record("11111111111111111111111111111111", 1_000, 100);
    let mut state = SelectorState::empty();
    state.bytes = selector_record_size(&record);
    state.deadlines.set(&record.meta.uuid, record.deadline);
    state.records.insert(record.meta.uuid.clone(), Arc::clone(&record));

    assert_eq!(state.expire(1_100, 1 << 20), 1);
    assert!(state.records.is_empty());
    assert_eq!(state.retained[&record.meta.uuid].until, 1_200);
    assert_eq!(state.expire(1_199, 1 << 20), 0);
    assert_eq!(state.expire(1_200, 1 << 20), 1);
    assert!(state.retained.is_empty());
}

#[test]
fn explicit_purge_removes_active_and_retained_records() {
    let record = retained_test_record("22222222222222222222222222222222", 1_000, 100);
    let mut state = SelectorState::empty();
    state.set_retained(Arc::clone(&record), 1_200, 1_000, 1 << 20);
    assert!(state.purge(&record.meta.uuid));
    assert!(state.records.is_empty());
    assert!(state.retained.is_empty());
    assert_eq!(state.retained_bytes, 0);
}

#[test]
fn retained_budget_evicts_the_earliest_deadline() {
    let first = retained_test_record("33333333333333333333333333333333", 1_000, 100);
    let second = retained_test_record("44444444444444444444444444444444", 1_100, 100);
    let limit = selector_record_size(&first);
    let mut state = SelectorState::empty();
    state.set_retained(Arc::clone(&second), 1_300, 1_000, limit);
    state.set_retained(Arc::clone(&first), 1_200, 1_000, limit);
    assert!(!state.retained.contains_key(&first.meta.uuid));
    assert!(state.retained.contains_key(&second.meta.uuid));
    assert!(state.retained_bytes <= limit);
}

#[test]
fn zero_budget_disables_retention() {
    let record = retained_test_record("55555555555555555555555555555555", 1_000, 100);
    let mut state = SelectorState::empty();
    state.set_retained(record, 1_200, 1_000, 0);
    assert!(state.retained.is_empty());
}

#[test]
fn detached_record_does_not_alias_internal_data() {
    let record = retained_test_record("66666666666666666666666666666666", 1_000, 100);
    let mut detached = detached_record(&record);
    let Some(load) = detached.data.get_mut("load") else {
        panic!("detached record lost load field");
    };
    load[0] = b'9';
    assert_eq!(record.data["load"], b"0");
}

#[test]
fn materialized_view_keeps_uuid_order_and_shared_records() {
    let mut state = SelectorState::empty();
    for uuid in [
        "33333333333333333333333333333333",
        "11111111111111111111111111111111",
        "22222222222222222222222222222222",
    ] {
        let record = retained_test_record(uuid, 1_000, 100);
        state.records.insert(uuid.to_owned(), record);
    }
    for uuid in ["55555555555555555555555555555555", "44444444444444444444444444444444"] {
        let record = retained_test_record(uuid, 1_000, 100);
        state.retained.insert(uuid.to_owned(), RetainedSelectorRecord { record, until: 1_200 });
    }

    let view = materialize_view(&state, 7, true);
    assert!(view.ordered_records.windows(2).all(|pair| pair[0].meta.uuid < pair[1].meta.uuid));
    assert!(
        view.ordered_records
            .iter()
            .all(|record| { view.records.get(&record.meta.uuid).is_some_and(|mapped| Arc::ptr_eq(mapped, record)) })
    );
    assert!(view.ordered_retained.windows(2).all(|pair| pair[0].record.meta.uuid < pair[1].record.meta.uuid));
}

#[test]
fn selection_token_wrap_clears_reusable_duplicate_marks() {
    let mut state = SelectionState::<Fields, Fields> {
        cache: HashMap::new(),
        overlays: HashMap::new(),
        view: None,
        selected: vec![1, 1],
        token: u64::MAX,
    };
    assert_eq!(state.advance_token(), 1);
    assert_eq!(state.selected, vec![0, 0]);
}

fn retained_test_record(uuid: &str, timestamp: u64, ttl: u64) -> Arc<SelectorRecord> {
    Arc::new(SelectorRecord {
        meta: Meta {
            uuid: uuid.to_owned(),
            revision: 1,
            timestamp,
            ttl,
            version: 1,
        },
        attr: Arc::new(Fields::from([("role".to_owned(), b"worker".to_vec())])),
        data: Arc::new(Fields::from([("load".to_owned(), b"0".to_vec())])),
        deadline: timestamp + ttl,
        size: 0,
    })
}
