use super::*;

#[test]
fn update_remove_and_expire_keep_one_indexed_entry() {
    let mut queue = DeadlineQueue::with_capacity(3);
    queue.set("b", 20);
    queue.set("a", 20);
    queue.set("c", 30);
    queue.set("c", 10);
    assert_eq!(queue.next(), Some(10));
    assert_eq!(queue.expire(9), None);
    assert_eq!(queue.expire(10).as_deref(), Some("c"));
    assert_eq!(queue.expire(20).as_deref(), Some("a"));
    assert!(queue.remove("b"));
    assert_eq!(queue.next(), None);
    assert!(!queue.remove("missing"));
    queue.set("later", 40);
    queue.set("first", 30);
    assert_eq!(queue.pop().as_deref(), Some("first"));
}
