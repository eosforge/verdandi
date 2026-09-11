// Package membership 保存已登记成员, 登记提交和完整快照在同一事务中完成.
package membership

import (
	"bytes"
	"encoding/json"
	"errors"
	"math"
	"slices"
	"strings"
	"time"

	bolt "go.etcd.io/bbolt"
)

var (
	// ErrConflict 表示身份, 地址或预期实例代次冲突; 调用方不能自动升级旧请求的代次.
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

// Member 是一个部署凭据当前登记的进程. Principal 标识授权凭据, 不替代进程 PeerID.
// 新进程替换只递增 Epoch, 不创建第二个持久成员名额, 也不影响业务数据的所有权版本.
type Member struct {
	// Role 是认证层确认的 star 或 planet, 不从存储模式推断.
	Role string `json:"role"`
	// Group 是连接偏好组, 默认 default, 与业务 Zone 和权限 scope 分离.
	Group string `json:"group"`
	// PeerID 是本次进程启动的 UUIDv4, 进程重启后必须变化.
	PeerID string `json:"peer_id"`
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
}

// Open 打开已有文件或创建新库. maximum 是每群组上限, 范围 1..4096, 零非法.
// 文件锁最多等待一秒, 同一路径不能由两个 Supervisor 同时写入.
func Open(path string, maximum int) (*Store, error) {
	if path == "" || maximum < 1 || maximum > 4096 {
		return nil, ErrInvalid
	}
	db, err := bolt.Open(path, 0600, &bolt.Options{Timeout: time.Second})
	if err != nil {
		return nil, err
	}
	store := &Store{db: db, maximum: maximum}
	// 已有库先完整验证, 损坏或超过当前部署上限时失败, 不返回伪完整名单.
	err = db.View(func(tx *bolt.Tx) error {
		return tx.ForEach(func(name []byte, bucket *bolt.Bucket) error {
			if !Name(string(name)) {
				return ErrInvalid
			}
			_, err := members(bucket, maximum)
			return err
		})
	})
	if err != nil {
		return nil, errors.Join(err, db.Close())
	}
	return store, nil
}

// Close 等待当前事务结束并释放持久文件. 生命周期由 app 层负责恰好关闭一次.
func (s *Store) Close() error { return s.db.Close() }

// Epoch 读取部署凭据当前的代次, 未登记时为零. 结果只供本次进程首次登记的 CAS 使用.
// 已经提交或开始重试的旧进程不得重新调用它来获取更高代次并抢回成员身份.
func (s *Store) Epoch(cluster, principal string) (uint64, error) {
	if !Name(cluster) || !lowerHex(principal, 64) {
		return 0, ErrInvalid
	}
	var epoch uint64
	err := s.db.View(func(tx *bolt.Tx) error {
		bucket := tx.Bucket([]byte(cluster))
		if bucket == nil {
			return nil
		}
		data := bucket.Get([]byte(principal))
		if data == nil {
			return nil
		}
		member, err := decode(data)
		epoch = member.Epoch
		return err
	})
	return epoch, err
}

// Register 使用 expectedEpoch 比较并替换同一授权凭据的进程, 提交后返回完整快照.
// candidate.Epoch 必须为零. 同一 UUID/地址的请求重试幂等, 响应丢失不会增加代次.
// 新进程遇到 CAS 冲突即拒绝; 旧请求不能因自动重取代次而覆盖较新的登记.
func (s *Store) Register(cluster string, candidate Member, expectedEpoch uint64) ([]Member, error) {
	if !Name(cluster) || candidate.Epoch != 0 || !valid(candidate, false) {
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
		var epoch uint64
		if index >= 0 {
			previous := current[index]
			epoch = previous.Epoch
			// v4 槽位绑定账号和端点, 新 UUID 也不能移动同一槽位或改换角色绕过独立容量.
			if previous.Role != candidate.Role || previous.Address != candidate.Address {
				return ErrConflict
			}
			if previous.PeerID == candidate.PeerID {
				if previous.Group != candidate.Group {
					return ErrConflict
				}
				// 即使外层请求认为响应丢失, 事务中已有同一登记便原样返回当前完整名单.
				snapshot = current
				return nil
			}
		}
		if epoch != expectedEpoch || epoch == math.MaxUint64 {
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
			if member.Principal != candidate.Principal && (member.PeerID == candidate.PeerID || member.Address == candidate.Address) {
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
		// current 已在本事务完整校验, 上面已检查新记录的容量和唯一性. 直接更新拥有的快照,
		// 避免再读取, 解码和校验整个数据库. Update 成功前仍等待 bbolt 的同步持久提交.
		if index >= 0 {
			current[index] = candidate
		} else {
			current = append(current, candidate)
		}
		slices.SortFunc(current, func(a, b Member) int { return strings.Compare(a.PeerID, b.PeerID) })
		snapshot = current
		return nil
	})
	if err != nil {
		return nil, err
	}
	return snapshot, nil
}

// members 校验并复制整个有界群组. 返回值不借用 mmap 页面, 事务结束后仍可使用.
func members(bucket *bolt.Bucket, maximum int) ([]Member, error) {
	result := make([]Member, 0)
	ids, addresses := make(map[string]bool), make(map[string]bool)
	counts := make(map[string]int)
	err := bucket.ForEach(func(key, value []byte) error {
		if value == nil {
			return ErrInvalid
		}
		member, err := decode(value)
		if err != nil || string(key) != member.Principal || ids[member.PeerID] || addresses[member.Address] {
			return ErrInvalid
		}
		counts[member.Role]++
		if counts[member.Role] > maximum {
			return ErrInvalid
		}
		ids[member.PeerID], addresses[member.Address] = true, true
		result = append(result, member)
		return nil
	})
	slices.SortFunc(result, func(a, b Member) int { return strings.Compare(a.PeerID, b.PeerID) })
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
	return (member.Role == Star || member.Role == Planet) && Name(member.Group) && UUID(member.PeerID) && lowerHex(member.Principal, 64) &&
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
