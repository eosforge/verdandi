use super::*;
use crate::Fields;

const UUID: &str = "0123456789abcdef0123456789abcdef";

fn fields(values: &[(&str, &[u8])]) -> Fields {
    values.iter().map(|(name, value)| ((*name).to_owned(), (*value).to_vec())).collect()
}

fn event(kind: &str, revision: u64, timestamp: u64, data: Fields) -> RegistrationEvent {
    RegistrationEvent {
        kind: kind.to_owned(),
        uuid: UUID.to_owned(),
        revision,
        timestamp,
        ttl: u64::from(kind == "register") * 100,
        version: u64::from(kind == "register"),
        has_version: kind == "register",
        attr: Fields::new(),
        data,
    }
}

#[test]
fn register_update_and_renew_collapse_to_complete_state() {
    let mut pending = PendingChanges::new(8, 1 << 20);
    let mut register = event("register", 1, 10, fields(&[("load", b"1")]));
    register.attr = fields(&[("role", b"edge")]);
    assert!(pending.add(register).is_ok());
    assert!(pending.add(event("update", 2, 20, fields(&[("load", b"2")]))).is_ok());
    assert!(pending.add(event("renew", 2, 30, Fields::new())).is_ok());
    let changes = pending.drain();
    assert_eq!(changes.len(), 1);
    assert_eq!(changes[0].event.kind, "register");
    assert_eq!(changes[0].event.revision, 2);
    assert_eq!(changes[0].event.timestamp, 30);
    assert_eq!(changes[0].event.data["load"], b"2");
    assert_eq!(pending.bytes, 0);
}

#[test]
fn contiguous_updates_merge_and_gap_requires_authoritative_state() {
    let mut pending = PendingChanges::new(8, 1 << 20);
    assert!(pending.add(event("update", 5, 50, fields(&[("a", b"first")]))).is_ok());
    assert!(pending.add(event("update", 6, 60, fields(&[("b", b"second")]))).is_ok());
    let change = pending.drain().remove(0);
    assert!(!change.repair);
    assert_eq!(change.base_revision, 4);
    assert_eq!(change.latest_revision, 6);
    assert_eq!(change.event.data["a"], b"first");
    assert_eq!(change.event.data["b"], b"second");

    assert!(pending.add(event("update", 7, 70, fields(&[("a", b"7")]))).is_ok());
    assert!(pending.add(event("update", 9, 90, fields(&[("a", b"9")]))).is_ok());
    let mut lower = event("register", 8, 80, fields(&[("a", b"8")]));
    lower.ttl = 100;
    lower.version = 1;
    lower.has_version = true;
    assert!(pending.add(lower).is_ok());
    let current = pending.entries.get(UUID);
    assert!(current.is_some_and(|change| change.repair && change.latest_revision == 9));
    let mut authoritative = event("register", 9, 100, fields(&[("a", b"9")]));
    authoritative.ttl = 100;
    authoritative.version = 1;
    authoritative.has_version = true;
    assert!(pending.add(authoritative).is_ok());
    assert!(pending.entries.get(UUID).is_some_and(|change| !change.repair));
}

#[test]
fn bounds_are_transactional_and_unregister_is_terminal() {
    let mut pending = PendingChanges::new(1, 1 << 20);
    assert!(pending.add(event("renew", 1, 1, Fields::new())).is_ok());
    let mut other = event("renew", 1, 1, Fields::new());
    other.uuid = "fedcba9876543210fedcba9876543210".to_owned();
    assert!(matches!(
        pending.add(other),
        Err(error) if error.code() == Code::Capacity
    ));

    let before = pending_change_size(pending.entries.get(UUID).unwrap_or_else(|| panic!("test fixture must retain its first entry")));
    pending.max_bytes = pending.bytes + 1;
    assert!(matches!(
        pending.add(event("update", 2, 2, fields(&[("payload", &[b'x'; 128])]))),
        Err(error) if error.code() == Code::Capacity
    ));
    assert_eq!(
        pending_change_size(
            pending
                .entries
                .get(UUID)
                .unwrap_or_else(|| { panic!("failed insertion must retain the prior entry") })
        ),
        before
    );

    pending.max_bytes = 1 << 20;
    assert!(pending.add(event("unregister", 0, 0, Fields::new())).is_ok());
    assert!(matches!(
        pending.add(event("renew", 1, 3, Fields::new())),
        Err(error) if error.code() == Code::Transition
    ));
}

#[test]
fn byte_accounting_tracks_in_place_growth_and_replacement() {
    let mut pending = PendingChanges::new(8, 1 << 20);
    let events = [
        event("update", 2, 20, fields(&[("a", b"1")])),
        event("update", 3, 30, fields(&[("a", &[b'x'; 128])])),
        event("update", 4, 40, fields(&[("b", b"2")])),
        event("update", 5, 50, fields(&[("a", b"3")])),
        event("renew", 5, 60, Fields::new()),
    ];
    for value in events {
        assert!(pending.add(value).is_ok());
        let accounted: usize = pending.entries.values().map(pending_change_size).sum();
        assert_eq!(pending.bytes, accounted);
    }
}

#[test]
fn large_single_registration_burst_remains_one_bounded_change() {
    let mut pending = PendingChanges::new(1, 512);
    for revision in 2..=10_001 {
        let value = [u8::try_from(revision & 0xff).unwrap_or_default()];
        assert!(pending.add(event("update", revision, revision, fields(&[("load", &value)]))).is_ok());
        assert_eq!(pending.entries.len(), 1);
        assert!(pending.bytes <= 512);
    }
    let change = pending.drain().remove(0);
    assert_eq!(change.base_revision, 1);
    assert_eq!(change.latest_revision, 10_001);
    assert_eq!(change.event.revision, 10_001);
}
