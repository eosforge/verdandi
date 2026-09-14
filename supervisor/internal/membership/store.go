// Package membership 保存已登记成员, 登记提交和完整快照在同一事务中完成.
package membership

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"slices"
	"strings"
	"time"

	bolt "go.etcd.io/bbolt"
)

var (
	// ErrConflict 表示身份, 地址或已被替换的启动请求冲突; 重试不能恢复旧实例.
	ErrConflict = errors.New("membership conflict")
	// ErrCapacity 表示该群组已经达到部署配置的成员容量.
	ErrCapacity = errors.New("membership capacity exceeded")
	// ErrInvalid 表示输入或持久成员记录不满足协议约束.
	ErrInvalid = errors.New("invalid membership record")
)

// 两种角色使用独立容量计数, Planet 不占用 Star 全互联名额.
const (
	Star   = "star"
	Planet = "planet"
)

// Member 是一个部署凭据当前登记的进程. Principal 标识授权凭据, 不替代进程 ID.
// 新进程替换只递增 Epoch, 不创建第二个持久成员名额, 也不影响业务数据的所有权版本.
type Member struct {
	// Role 是认证层确认的 star 或 planet, 不从存储模式推断.
	Role string `json:"role"`
	// Group 是连接偏好组, 默认 default, 与业务 Zone 和权限 scope 分离.
	Group string `json:"group"`
	// ID 是 Supervisor 为本次进程启动签发的不透明字符串, 进程重启后必须变化.
	ID string `json:"id"`
	// Principal 是账号和规范端点的指纹, 由认证层提供, 不能直接信任请求中的声明.
	Principal string `json:"principal"`
	// Address 是规范化的具体 IP:PORT, 不接受通配, 多播或端口零.
	Address string `json:"address"`
	// Epoch 从一开始单调递增, 只隔离同一授权凭据的进程替换, 不用于业务数据排序.
	Epoch uint64 `json:"epoch"`
}

// Store 独占 bbolt 数据文件, Close 释放文件锁. 它不拥有后台 goroutine 或网络连接.
type Store struct {
	// db 拥有文件锁及事务生命周期, 不允许在 Store.Close 后继续调用.
	db *bolt.DB
	// maximum 是启动时验证的每群组成员上限, 范围 1..4096.
	maximum int
	// maximumStarts 是每 Galaxy 已提交启动请求的容量, 默认 1,048,576; 满后拒绝新启动, 不驱逐旧请求.
	maximumStarts uint64
}

// DefaultMaximumStarts 是默认启动记录预算, 运维可提高预算但不能驱逐记录来重新授权旧请求.
const DefaultMaximumStarts = 1 << 20

// Open 打开已有文件或创建新库. maximumStarts 范围 1..16,777,216, 零非法.
// maximum 是每群组上限, 范围 1..4096, 零非法.
// 文件锁最多等待一秒, 同一路径不能由两个 Supervisor 同时写入.
func Open(path string, maximum int, maximumStarts uint64) (*Store, error) {
	if path == "" || maximum < 1 || maximum > 4096 || maximumStarts == 0 || maximumStarts > 1<<24 {
		return nil, ErrInvalid
	}
	db, err := bolt.Open(path, 0600, &bolt.Options{Timeout: time.Second})
	if err != nil {
		return nil, err
	}
	store := &Store{db: db, maximum: maximum, maximumStarts: maximumStarts}
	// 已有库先完整验证, 损坏或超过当前部署上限时失败, 不返回伪完整名单.
	err = db.View(func(tx *bolt.Tx) error {
		return tx.ForEach(func(name []byte, bucket *bolt.Bucket) error {
			if !Name(string(name)) {
				return ErrInvalid
			}
			_, err := members(bucket, maximum)
			if err != nil {
				return err
			}
			return validateStarts(bucket, store.maximumStarts)
		})
	})
	if err != nil {
		return nil, errors.Join(err, db.Close())
	}
	return store, nil
}

// Close 等待当前事务结束并释放持久文件. 生命周期由 app 层负责恰好关闭一次.
func (s *Store) Close() error { return s.db.Close() }

// Register 在同一持久事务中去重启动请求, 分配部署代次并返回完整快照.
// requestID 是 32 字节随机幂等键, 不是身份; candidate.ID 由 Supervisor 签发, 重试使用已提交的 ID.
// 已被替换请求永久拒绝. 全新请求按事务提交顺序替换同部署实例, 不推断进程启动的墙钟先后.
func (s *Store) Register(cluster string, candidate Member, requestID []byte) ([]Member, error) {
	if !Name(cluster) || candidate.Epoch != 0 || !valid(candidate, false) || len(requestID) != 32 {
		return nil, ErrInvalid
	}
	var snapshot []Member
	err := s.db.Update(func(tx *bolt.Tx) error {
		bucket, err := tx.CreateBucketIfNotExists([]byte(cluster))
		if err != nil {
			return err
		}
		current, err := members(bucket, s.maximum)
		if err != nil {
			return err
		}
		index := slices.IndexFunc(current, func(member Member) bool { return member.Principal == candidate.Principal })
		starts, err := bucket.CreateBucketIfNotExists(startBucket)
		if err != nil {
			return err
		}
		// 去重检查先于容量检查. 满额时当前实例仍能重试或刷新, 旧实例仍然被拒绝.
		if previous := starts.Get(requestID); previous != nil {
			if len(previous) != 40 {
				return ErrInvalid
			}
			principal := hex.EncodeToString(previous[:32])
			epoch := binary.BigEndian.Uint64(previous[32:])
			if index < 0 || principal != candidate.Principal || current[index].Epoch != epoch ||
				current[index].Role != candidate.Role || current[index].Group != candidate.Group || current[index].Address != candidate.Address {
				return ErrConflict
			}
			snapshot = current
			return nil
		}
		if starts.Sequence() >= s.maximumStarts {
			return ErrCapacity
		}
		var epoch uint64
		if index >= 0 {
			previous := current[index]
			epoch = previous.Epoch
			if previous.Role != candidate.Role || previous.Address != candidate.Address || previous.ID == candidate.ID {
				return ErrConflict
			}
		}
		if epoch == math.MaxUint64 {
			return ErrConflict
		}
		count := 0
		for _, member := range current {
			if member.Role == candidate.Role {
				count++
			}
		}
		if index < 0 && count == s.maximum {
			return ErrCapacity
		}
		for _, member := range current {
			if member.Principal != candidate.Principal && (member.ID == candidate.ID || member.Address == candidate.Address) {
				return ErrConflict
			}
		}
		candidate.Epoch = epoch + 1
		encoded, err := json.Marshal(candidate)
		if err != nil {
			return err
		}
		if err := bucket.Put([]byte(candidate.Principal), encoded); err != nil {
			return err
		}
		// 启动请求和成员一起提交, 没有只记住请求或只替换身份的中间状态.
		principal, _ := hex.DecodeString(candidate.Principal)
		record := make([]byte, 40)
		copy(record, principal)
		binary.BigEndian.PutUint64(record[32:], candidate.Epoch)
		if err := starts.Put(requestID, record); err != nil {
			return err
		}
		if _, err := starts.NextSequence(); err != nil {
			return err
		}
		// current 已在本事务完整校验, 上面已检查新记录的容量和唯一性. 直接更新拥有的快照,
		// 避免再读取, 解码和校验整个数据库. Update 成功前仍等待 bbolt 的同步持久提交.
		if index >= 0 {
			current[index] = candidate
		} else {
			current = append(current, candidate)
		}
		slices.SortFunc(current, func(a, b Member) int { return strings.Compare(a.ID, b.ID) })
		snapshot = current
		return nil
	})
	if err != nil {
		return nil, err
	}
	return snapshot, nil
}

// 内部嵌套索引不会进入成员名单, key 是请求随机值, value 固定为部署摘要和已提交代次.
var startBucket = []byte("@starts")

// validateStarts 在打开数据库时扫描有界索引, 拒绝损坏或缺失成员引用, 不在每次登记重扫历史.
func validateStarts(bucket *bolt.Bucket, maximum uint64) error {
	starts := bucket.Bucket(startBucket)
	// v5 成员可保留, 新协议请求首次提交时创建索引.
	if starts == nil {
		return nil
	}
	var count uint64
	err := starts.ForEach(func(key, value []byte) error {
		count++
		if count > maximum || len(key) != 32 || len(value) != 40 {
			return ErrInvalid
		}
		member, err := decode(bucket.Get([]byte(hex.EncodeToString(value[:32]))))
		epoch := binary.BigEndian.Uint64(value[32:])
		if err != nil || epoch == 0 || epoch > member.Epoch {
			return ErrInvalid
		}
		return nil
	})
	if err != nil {
		return err
	}
	if count != starts.Sequence() {
		return ErrInvalid
	}
	return nil
}

// members 校验并复制整个有界群组. 返回值不借用 mmap 页面, 事务结束后仍可使用.
func members(bucket *bolt.Bucket, maximum int) ([]Member, error) {
	result := make([]Member, 0)
	ids, addresses := make(map[string]bool), make(map[string]bool)
	counts := make(map[string]int)
	err := bucket.ForEach(func(key, value []byte) error {
		if bytes.Equal(key, startBucket) && value == nil {
			return nil
		}
		if value == nil {
			return ErrInvalid
		}
		member, err := decode(value)
		if err != nil || string(key) != member.Principal || ids[member.ID] || addresses[member.Address] {
			return ErrInvalid
		}
		counts[member.Role]++
		if counts[member.Role] > maximum {
			return ErrInvalid
		}
		ids[member.ID], addresses[member.Address] = true, true
		result = append(result, member)
		return nil
	})
	slices.SortFunc(result, func(a, b Member) int { return strings.Compare(a.ID, b.ID) })
	return result, err
}

// decode 不容忍未知字段和尾随 JSON, 文件版本变化必须显式迁移而非悄悄忽略.
func decode(data []byte) (Member, error) {
	var member Member
	if len(data) > 2048 {
		return member, ErrInvalid
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&member); err != nil || decoder.InputOffset() != int64(len(data)) {
		return Member{}, ErrInvalid
	}
	// v2 数据库只有 Star, 同时缺少两个新字段时按旧格式解释. 不接受部分缺失或未知角色.
	if member.Role == "" && member.Group == "" {
		// 区分旧记录没有字段与新记录显式写入空值/null, 后者不能借迁移分支逃过校验.
		var fields struct {
			Role  json.RawMessage `json:"role"`
			Group json.RawMessage `json:"group"`
		}
		if err := json.Unmarshal(data, &fields); err != nil || len(fields.Role) != 0 || len(fields.Group) != 0 {
			return Member{}, ErrInvalid
		}
		member.Role, member.Group = Star, "default"
	}
	if !valid(member, true) {
		return Member{}, ErrInvalid
	}
	return member, nil
}

// valid 组合共享身份规则与持久状态要求, 写入候选和读取记录使用同一入口.
func valid(member Member, persisted bool) bool {
	return (member.Role == Star || member.Role == Planet) && Name(member.Group) && ID(member.ID) && lowerHex(member.Principal, 64) &&
		(!persisted || member.Epoch != 0) && Address(member.Address)
}

// DescribeError 提供有限错误类别, 不泄露远端输入, 数据库路径或凭据.
func DescribeError(err error) string {
	switch {
	case errors.Is(err, ErrConflict):
		return "conflict"
	case errors.Is(err, ErrCapacity):
		return "capacity"
	case errors.Is(err, ErrInvalid):
		return "invalid"
	default:
		return "storage"
	}
}
