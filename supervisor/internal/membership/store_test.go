package membership

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
	"sync"
	"testing"

	bolt "go.etcd.io/bbolt"
)

func TestLegacyMemberRoleMigrationAndPartialFieldsFailClosed(t *testing.T) {
	original := candidate(1)
	original.Epoch = 1
	encoded, err := json.Marshal(original)
	if err != nil {
		t.Fatal(err)
	}
	for _, test := range []struct {
		role, group bool
		valid       bool
	}{
		{false, false, true}, {true, true, true}, {true, false, false}, {false, true, false},
	} {
		var fields map[string]json.RawMessage
		if err := json.Unmarshal(encoded, &fields); err != nil {
			t.Fatal(err)
		}
		if !test.role {
			delete(fields, "role")
		}
		if !test.group {
			delete(fields, "group")
		}
		data, err := json.Marshal(fields)
		if err != nil {
			t.Fatal(err)
		}
		decoded, err := decode(data)
		if (err == nil) != test.valid {
			t.Fatalf("role=%v group=%v: %v", test.role, test.group, err)
		}
		if test.valid && (decoded.Role != Star || decoded.Group != "default" || decoded.Epoch != original.Epoch) {
			t.Fatal("legacy migration changed identity or epoch")
		}
	}
	for _, invalid := range []string{`""`, `null`, `"unknown"`} {
		var fields map[string]json.RawMessage
		if err := json.Unmarshal(encoded, &fields); err != nil {
			t.Fatal(err)
		}
		fields["role"], fields["group"] = json.RawMessage(invalid), json.RawMessage(invalid)
		data, err := json.Marshal(fields)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := decode(data); !errors.Is(err, ErrInvalid) {
			t.Fatalf("explicit invalid fields accepted: %s", invalid)
		}
	}
}

// 测试固定请求键便于重放, 生产客户端使用安全随机值且不由实例 ID 派生.
func startup(member Member) []byte { value := sha256.Sum256([]byte(member.ID)); return value[:] }

func candidate(index int) Member {
	return Member{
		Role: Star, Group: "default",
		ID:        fmt.Sprintf("%08x000040008000000000000000", index),
		Principal: fmt.Sprintf("%064x", index),
		Address:   fmt.Sprintf("127.0.0.1:%d", 10000+index),
	}
}

func TestRolesHaveIndependentCapacityAndRetryMetadataIsImmutable(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "members.db"), 1, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	if _, err := store.Register("alpha", candidate(1), startup(candidate(1))); err != nil {
		t.Fatal(err)
	}
	planet := candidate(2)
	planet.Role = Planet
	if _, err := store.Register("alpha", planet, startup(planet)); err != nil {
		t.Fatal("Star capacity blocked Planet:", err)
	}
	for _, changed := range []Member{candidate(2), func() Member { changed := planet; changed.Group = "other"; return changed }()} {
		if _, err := store.Register("alpha", changed, startup(changed)); !errors.Is(err, ErrConflict) {
			t.Fatal("retry changed role/group:", err)
		}
	}
	if _, err := store.Register("alpha", candidate(3), startup(candidate(3))); !errors.Is(err, ErrCapacity) {
		t.Fatal("Star capacity ignored")
	}
	other := candidate(4)
	other.Role = Planet
	if _, err := store.Register("alpha", other, startup(other)); !errors.Is(err, ErrCapacity) {
		t.Fatal("Planet capacity ignored")
	}
}

func TestConcurrentRegistrationReturnsOrderedCompleteSnapshots(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "members.db"), 32, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	const count = 24
	results := make(chan []Member, count)
	failures := make(chan error, count)
	var workers sync.WaitGroup
	for index := range count {
		workers.Go(func() {
			snapshot, err := store.Register("alpha", candidate(index+1), startup(candidate(index+1)))
			results <- snapshot
			failures <- err
		})
	}
	workers.Wait()
	close(results)
	close(failures)
	for err := range failures {
		if err != nil {
			t.Fatal(err)
		}
	}
	sizes := make(map[int]bool)
	for snapshot := range results {
		if sizes[len(snapshot)] {
			t.Fatalf("non-serial snapshots: duplicate length %d", len(snapshot))
		}
		sizes[len(snapshot)] = true
		for index := 1; index < len(snapshot); index++ {
			if snapshot[index-1].ID >= snapshot[index].ID {
				t.Fatal("snapshot not strictly sorted")
			}
		}
	}
	for size := 1; size <= count; size++ {
		if !sizes[size] {
			t.Fatalf("missing snapshot size %d", size)
		}
	}
}

func TestRetryPersistenceAndRestartFencing(t *testing.T) {
	path := filepath.Join(t.TempDir(), "members.db")
	store, err := Open(path, 1, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	first := candidate(1)
	for range 3 {
		snapshot, err := store.Register("alpha", first, startup(first))
		if err != nil || len(snapshot) != 1 || snapshot[0].Epoch != 1 {
			t.Fatalf("retry changed registration: %v, %v", snapshot, err)
		}
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = Open(path, 1, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	if epoch, err := storedEpoch(store, "alpha", first.Principal); err != nil || epoch != 1 {
		t.Fatalf("lost registration: %d, %v", epoch, err)
	}
	if snapshot, err := store.Register("alpha", first, startup(first)); err != nil || len(snapshot) != 1 || snapshot[0].Epoch != 1 || snapshot[0].ID != first.ID {
		t.Fatalf("retry after reopening changed the issued identity: %v, %v", snapshot, err)
	}

	next := first
	next.ID = candidate(2).ID
	snapshot, err := store.Register("alpha", next, startup(next))
	if err != nil || len(snapshot) != 1 || snapshot[0].Epoch != 2 {
		t.Fatalf("restart leaked capacity: %v, %v", snapshot, err)
	}
	if _, err := store.Register("alpha", first, startup(first)); !errors.Is(err, ErrConflict) {
		t.Fatalf("stale retry replaced new process: %v", err)
	}
	if _, err := store.Register("alpha", candidate(3), startup(candidate(3))); !errors.Is(err, ErrCapacity) {
		t.Fatalf("capacity not enforced: %v", err)
	}
	if _, err := store.Register("beta", candidate(3), startup(candidate(3))); err != nil {
		t.Fatalf("cluster capacity leaked: %v", err)
	}
}

func TestConflictsDoNotMutateMembers(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "members.db"), 8, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	first := candidate(1)
	if _, err := store.Register("alpha", first, startup(first)); err != nil {
		t.Fatal(err)
	}
	tests := []struct {
		name   string
		change func(*Member)
	}{
		{"same-id-new-address", func(m *Member) { m.Address = "127.0.0.1:9000" }},
		{"same-id-new-group", func(m *Member) { m.Group = "other" }},
		{"other-principal-same-id", func(m *Member) { m.Principal = candidate(2).Principal }},
		{"other-principal-same-address", func(m *Member) { m.Principal = candidate(2).Principal; m.ID = candidate(2).ID }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			changed := first
			test.change(&changed)
			if _, err := store.Register("alpha", changed, startup(changed)); !errors.Is(err, ErrConflict) {
				t.Fatalf("accepted conflict: %v", err)
			}
			original, err := store.Register("alpha", first, startup(first))
			if err != nil || len(original) != 1 || original[0].Epoch != 1 {
				t.Fatalf("failure changed state: %v, %v", original, err)
			}
		})
	}
}

func TestCorruptMemberFailsClosedAndReleasesLock(t *testing.T) {
	path := filepath.Join(t.TempDir(), "members.db")
	store, err := Open(path, 8, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.db.Update(func(tx *bolt.Tx) error {
		bucket, err := tx.CreateBucketIfNotExists([]byte("alpha"))
		if err != nil {
			return err
		}
		return bucket.Put([]byte(candidate(1).Principal), []byte(`{"id":"broken"}`))
	}); err != nil {
		t.Fatal(err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	if reopened, err := Open(path, 8, DefaultMaximumStarts); !errors.Is(err, ErrInvalid) {
		if reopened != nil {
			_ = reopened.Close()
		}
		t.Fatalf("corrupt member accepted: %v", err)
	}
	db, err := bolt.Open(path, 0600, nil)
	if err != nil {
		t.Fatal("failed open leaked file lock:", err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
}

func TestValidationAndReturnedMemoryOwnership(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "members.db"), 8, DefaultMaximumStarts)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	for _, cluster := range []string{"", "a/b", "中文", string(bytes.Repeat([]byte{'a'}, 65))} {
		if _, err := store.Register(cluster, candidate(1), startup(candidate(1))); !errors.Is(err, ErrInvalid) {
			t.Fatalf("invalid cluster accepted: %v", err)
		}
	}
	for _, address := range []string{
		"0.0.0.0:1", "127.0.0.1:0", "224.0.0.1:2", "localhost:1", "[::]:1",
		"[::ffff:127.0.0.1]:1", "[fe80::1%2]:1", "[0:0:0:0:0:0:0:1]:1",
	} {
		member := candidate(1)
		member.Address = address
		if _, err := store.Register("alpha", member, startup(member)); !errors.Is(err, ErrInvalid) {
			t.Fatalf("invalid address accepted: %v", err)
		}
	}
	snapshot, err := store.Register("alpha", candidate(1), startup(candidate(1)))
	if err != nil {
		t.Fatal(err)
	}
	snapshot[0].ID = "changed"
	snapshot[0].Group = "changed"
	again, err := store.Register("alpha", candidate(1), startup(candidate(1)))
	if err != nil || again[0].ID != candidate(1).ID || again[0].Group != "default" {
		t.Fatalf("snapshot aliases persistence: %v, %v", again, err)
	}
}

// storedEpoch 只供白盒测试检查已持久成员, 不为客户端暴露基线 RPC 或生产读取 API.
func storedEpoch(store *Store, cluster, principal string) (uint64, error) {
	var epoch uint64
	err := store.db.View(func(tx *bolt.Tx) error {
		member, err := decode(tx.Bucket([]byte(cluster)).Get([]byte(principal)))
		epoch = member.Epoch
		return err
	})
	return epoch, err
}
