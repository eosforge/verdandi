package membership

import (
	"encoding/json"
	"errors"
	"math"
	"path/filepath"
	"reflect"
	"testing"
	"time"

	bolt "go.etcd.io/bbolt"
)

func TestHigherEpochKeepsDeploymentRoleAndEndpoint(t *testing.T) {
	store, err := Open(filepath.Join(t.TempDir(), "members.db"), 2)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	first, err := store.Register("alpha", candidate(1), 0)
	if err != nil {
		t.Fatal(err)
	}
	for _, field := range []string{"role", "address"} {
		changed := candidate(1)
		changed.PeerID = candidate(2).PeerID
		if field == "role" {
			changed.Role = Planet
		} else {
			changed.Address = candidate(2).Address
		}
		if _, err := store.Register("alpha", changed, 1); !errors.Is(err, ErrConflict) {
			t.Fatal("deployment identity changed", field, err)
		}
		again, err := store.Register("alpha", candidate(1), 0)
		if err != nil || !reflect.DeepEqual(first, again) {
			t.Fatal("rejected registration changed snapshot")
		}
	}
}

func TestSnapshotReplacementRemainsOrderedAndOwnedAfterReopen(t *testing.T) {
	path := filepath.Join(t.TempDir(), "members.db")
	store, err := Open(path, 3)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	for _, index := range []int{3, 1, 2} {
		if _, err := store.Register("alpha", candidate(index), 0); err != nil {
			t.Fatal(err)
		}
	}
	next := candidate(1)
	next.PeerID, next.Group = candidate(4).PeerID, "new-group"
	snapshot, err := store.Register("alpha", next, 1)
	if err != nil || len(snapshot) != 3 || snapshot[2].PeerID != next.PeerID || snapshot[2].Epoch != 2 {
		t.Fatal("replacement was not sorted", snapshot, err)
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = Open(path, 3)
	if err != nil {
		t.Fatal(err)
	}
	again, err := store.Register("alpha", next, 1)
	if err != nil || !reflect.DeepEqual(snapshot, again) {
		t.Fatal("snapshot differs from persisted transaction", err)
	}
	again[2].Group = "caller-write"
	if snapshot[2].Group != "new-group" {
		t.Fatal("returned snapshots share mutable memory")
	}
}

func TestEpochExhaustionAndFailedOpenReleaseFileLock(t *testing.T) {
	path := filepath.Join(t.TempDir(), "members.db")
	store, err := Open(path, 1)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = store.Close() })
	member := candidate(1)
	member.Epoch = math.MaxUint64
	encoded, err := json.Marshal(member)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.db.Update(func(tx *bolt.Tx) error {
		bucket, err := tx.CreateBucketIfNotExists([]byte("alpha"))
		if err != nil {
			return err
		}
		return bucket.Put([]byte(member.Principal), encoded)
	}); err != nil {
		t.Fatal(err)
	}
	next := candidate(1)
	next.PeerID = candidate(2).PeerID
	if _, err := store.Register("alpha", next, math.MaxUint64); !errors.Is(err, ErrConflict) {
		t.Fatal("epoch wrapped", err)
	}
	if epoch, err := store.Epoch("alpha", member.Principal); err != nil || epoch != math.MaxUint64 {
		t.Fatal("exhausted epoch changed", err)
	}
	if second, err := bolt.Open(path, 0600, &bolt.Options{Timeout: 30 * time.Millisecond}); err == nil {
		_ = second.Close()
		t.Fatal("second writer acquired owned database")
	}
	if err := store.Close(); err != nil {
		t.Fatal(err)
	}
	store, err = Open(path, 1)
	if err != nil {
		t.Fatal("closed store retained lock", err)
	}
}

func FuzzPersistedMemberDecodeRoundTrips(f *testing.F) {
	member := candidate(1)
	member.Epoch = 1
	encoded, err := json.Marshal(member)
	if err != nil {
		f.Fatal(err)
	}
	f.Add(encoded)
	f.Add([]byte(`{"role":"star"}`))
	f.Add([]byte{0xff})
	f.Fuzz(func(t *testing.T, data []byte) {
		decoded, err := decode(data)
		if err != nil {
			return
		}
		canonical, err := json.Marshal(decoded)
		if err != nil {
			t.Fatal(err)
		}
		again, err := decode(canonical)
		if err != nil || decoded != again {
			t.Fatal("accepted member is not stable under canonical encoding")
		}
	})
}
