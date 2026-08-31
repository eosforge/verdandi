package catalog

import (
	"bytes"
	"encoding/binary"
	"errors"
	"sync"
	"sync/atomic"
	"time"

	"github.com/vmihailenco/msgpack/v5"
	bolt "go.etcd.io/bbolt"
)

var (
	storeMetaBucket    = []byte("catalog-v2-meta")
	storeEntriesBucket = []byte("catalog-v2-entries")
)

type storedEntry struct {
	Revision        uint64            `msgpack:"revision"`
	ReplaceRevision uint64            `msgpack:"replace_revision"`
	Status          uint8             `msgpack:"status"`
	Kind            uint8             `msgpack:"kind"`
	EncodedBytes    int               `msgpack:"encoded_bytes"`
	Fields          map[string][]byte `msgpack:"fields,omitempty"`
}

type localStore struct {
	database *bolt.DB
	disabled atomic.Bool
	closeMu  sync.Mutex
	closed   bool
}

// openLocalStore 打开或创建 bbolt 检查点，并用 timeout 限制文件锁等待。
func openLocalStore(path string, timeout time.Duration) (*localStore, error) {
	database, err := bolt.Open(path, 0o600, &bolt.Options{Timeout: timeout})
	if err != nil {
		return nil, err
	}
	store := &localStore{database: database}
	if err := database.Update(func(transaction *bolt.Tx) error {
		if _, createErr := transaction.CreateBucketIfNotExists(storeMetaBucket); createErr != nil {
			return createErr
		}
		_, createErr := transaction.CreateBucketIfNotExists(storeEntriesBucket)
		return createErr
	}); err != nil {
		_ = database.Close()
		return nil, err
	}
	return store, nil
}

// load 读取 zone/scope 的 cursor 和全部 Entry，逐条按 maximumBytes 校验。
func (store *localStore) load(
	zone string,
	scope string,
	maximumBytes int,
) (uint64, map[Path]*rawState, error) {
	if store == nil || store.disabled.Load() {
		return 0, nil, nil
	}
	entries := make(map[Path]*rawState)
	var cursor uint64
	err := store.database.View(func(transaction *bolt.Tx) error {
		meta := transaction.Bucket(storeMetaBucket)
		prefix := checkpointPrefix(zone, scope)
		encodedCursor := meta.Get(prefix)
		if len(encodedCursor) != 0 {
			if len(encodedCursor) != 8 {
				return errors.New("invalid Catalog cursor length")
			}
			cursor = binary.BigEndian.Uint64(encodedCursor)
			if cursor > maximumRevision {
				return errors.New("invalid Catalog cursor revision")
			}
		}
		bucket := transaction.Bucket(storeEntriesBucket)
		entryCursor := bucket.Cursor()
		for key, value := entryCursor.Seek(prefix); bytes.HasPrefix(key, prefix); key, value = entryCursor.Next() {
			path, ok := pathFromMember(string(key[len(prefix):]))
			if !ok {
				return errors.New("invalid Catalog checkpoint path")
			}
			var record storedEntry
			if decodeErr := msgpack.Unmarshal(value, &record); decodeErr != nil {
				return decodeErr
			}
			state, decodeErr := stateFromStored(record, maximumBytes)
			if decodeErr != nil {
				return decodeErr
			}
			entries[path] = state
		}
		return nil
	})
	if err != nil {
		store.disabled.Store(true)
		return 0, nil, err
	}
	return cursor, entries, nil
}

// stateFromStored 把磁盘记录还原为不可变 rawState，并重新验证 kind、字段和容量。
func stateFromStored(record storedEntry, maximumBytes int) (*rawState, error) {
	status := Status(record.Status)
	if status != StatusPresent && status != StatusAbsent && status != StatusDeleted {
		return nil, errors.New("invalid Catalog checkpoint status")
	}
	if record.Revision > maximumRevision || record.ReplaceRevision > record.Revision ||
		record.EncodedBytes < 0 || record.EncodedBytes > maximumBytes {
		return nil, errors.New("invalid Catalog checkpoint header")
	}
	state := &rawState{
		revision:        record.Revision,
		replaceRevision: record.ReplaceRevision,
		status:          status,
		kind:            Kind(record.Kind),
		encodedBytes:    record.EncodedBytes,
		fields:          cloneFields(record.Fields),
	}
	if status == StatusPresent {
		_, size, err := validateValue(state.kind, state.fields, maximumBytes)
		if err != nil || size != state.encodedBytes || state.revision == 0 ||
			state.replaceRevision == 0 {
			return nil, errors.New("invalid Catalog checkpoint value")
		}
	} else if len(state.fields) != 0 || state.kind != 0 || state.encodedBytes != 0 ||
		state.replaceRevision != 0 || (status == StatusAbsent && state.revision != 0) ||
		(status == StatusDeleted && state.revision == 0) {
		return nil, errors.New("invalid Catalog checkpoint absence")
	}
	return state, nil
}

// saveEntry 在一个 bbolt 事务中保存 Path 当前完整状态；检查点只是恢复加速。
func (store *localStore) saveEntry(
	zone string,
	scope string,
	path Path,
	state *rawState,
) error {
	if store == nil || store.disabled.Load() || state == nil {
		return nil
	}
	record := storedEntry{
		Revision:        state.revision,
		ReplaceRevision: state.replaceRevision,
		Status:          uint8(state.status),
		Kind:            uint8(state.kind),
		EncodedBytes:    state.encodedBytes,
		Fields:          cloneFields(state.fields),
	}
	encoded, err := msgpack.Marshal(record)
	if err != nil {
		store.disabled.Store(true)
		return err
	}
	key := append(checkpointPrefix(zone, scope), path.member()...)
	err = store.database.Update(func(transaction *bolt.Tx) error {
		bucket := transaction.Bucket(storeEntriesBucket)
		if previous := bucket.Get(key); len(previous) != 0 {
			var stored storedEntry
			if decodeErr := msgpack.Unmarshal(previous, &stored); decodeErr != nil {
				return decodeErr
			}
			if stored.Revision >= record.Revision {
				return nil
			}
		}
		return bucket.Put(key, encoded)
	})
	if err != nil {
		store.disabled.Store(true)
	}
	return err
}

// saveCursor 单调保存 zone/scope 的最大已对齐 revision，较旧值不会覆盖较新检查点。
func (store *localStore) saveCursor(zone string, scope string, revision uint64) error {
	if store == nil || store.disabled.Load() {
		return nil
	}
	if revision > maximumRevision {
		store.disabled.Store(true)
		return errors.New("invalid Catalog cursor revision")
	}
	encoded := make([]byte, 8)
	binary.BigEndian.PutUint64(encoded, revision)
	err := store.database.Update(func(transaction *bolt.Tx) error {
		bucket := transaction.Bucket(storeMetaBucket)
		key := checkpointPrefix(zone, scope)
		if previous := bucket.Get(key); len(previous) != 0 {
			if len(previous) != 8 {
				return errors.New("invalid Catalog cursor length")
			}
			if binary.BigEndian.Uint64(previous) >= revision {
				return nil
			}
		}
		return bucket.Put(key, encoded)
	})
	if err != nil {
		store.disabled.Store(true)
	}
	return err
}

// checkpointPrefix 构造隔离 zone 和规范 scope 的二进制 bucket 前缀。
func checkpointPrefix(zone string, scope string) []byte {
	key := make([]byte, 0, len(zone)+len(scope)+2)
	key = append(key, zone...)
	key = append(key, 0)
	key = append(key, scope...)
	key = append(key, 0)
	return key
}

// close 幂等关闭 bbolt 数据库；nil 或已关闭 store 返回 nil。
func (store *localStore) close() error {
	if store == nil {
		return nil
	}
	store.closeMu.Lock()
	defer store.closeMu.Unlock()
	if store.closed {
		return nil
	}
	store.closed = true
	return store.database.Close()
}
