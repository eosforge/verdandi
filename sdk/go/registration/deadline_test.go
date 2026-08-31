package registration

import "testing"

func TestDeadlineQueue(t *testing.T) {
	t.Parallel()
	queue := newDeadlineQueue(3)
	queue.set("b", 20)
	queue.set("a", 20)
	queue.set("c", 30)
	if uuid, ok := queue.expire(19); ok || uuid != "" {
		t.Fatalf("expire(19) = %q, %v", uuid, ok)
	}
	if uuid, ok := queue.expire(20); !ok || uuid != "a" {
		t.Fatalf("first expire(20) = %q, %v", uuid, ok)
	}
	queue.set("c", 10)
	if uuid, ok := queue.expire(20); !ok || uuid != "c" {
		t.Fatalf("second expire(20) = %q, %v", uuid, ok)
	}
	if !queue.remove("b") || queue.remove("missing") {
		t.Fatal("remove did not remain indexed and idempotent")
	}
	queue.set("later", 40)
	queue.set("first", 30)
	if uuid, ok := queue.pop(); !ok || uuid != "first" {
		t.Fatalf("pop() = %q, %v", uuid, ok)
	}
}
