package admission

import (
	"bytes"
	"context"
	"slices"
	"sync"

	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// Directory 保留每个部署已观察到的最高身份, 缺项/离线不删除替换依据或业务数据.
// incoming 名单和握手共用同一提交锁, 旧查询晚到不能覆盖已验证的新凭证.
type Directory struct {
	mutex    sync.RWMutex
	identity *Identity
	galaxy   string
	role     orbit.Role
	maximum  int
	known    map[string]*orbit.Member
}

// NewDirectory 的 maximum 是全部部署槽位预算, 1..16384, 包括尚未在线的已知成员.
func NewDirectory(identity *Identity, galaxy string, role orbit.Role, maximum int) (*Directory, error) {
	if identity == nil || !Name(galaxy) || !Role(role) || role == orbit.Role_ROLE_PLANET || maximum < 1 || maximum > 16384 {
		return nil, ErrIdentity
	}
	return &Directory{identity: identity, galaxy: galaxy, role: role, maximum: maximum, known: make(map[string]*orbit.Member)}, nil
}

// Install 在锁外验证完整名单, 锁内与握手已观察身份合并, 任意冲突保留原完整状态.
func (directory *Directory) Install(members []*orbit.Member) error {
	if len(members) > directory.maximum {
		return status.Error(codes.ResourceExhausted, "Directory capacity exceeded")
	}
	prepared := make(map[string]*orbit.Member, len(members))
	addresses := make(map[string]bool, len(members))
	var previous []byte
	for index, member := range members {
		if !Valid(member) || member.Galaxy != directory.galaxy || directory.role != orbit.Role_ROLE_ASTROLABE && member.Role != orbit.Role_ROLE_STAR && member.Role != orbit.Role_ROLE_POLARIS || index != 0 && bytes.Compare(previous, member.Id) >= 0 || prepared[string(member.Principal)] != nil || addresses[member.Advertise] {
			return ErrIdentity
		}
		previous = member.Id
		addresses[member.Advertise] = true
		prepared[string(member.Principal)] = proto.Clone(member).(*orbit.Member)
	}
	directory.mutex.Lock()
	defer directory.mutex.Unlock()
	for principal, old := range directory.known {
		incoming := prepared[principal]
		if incoming == nil || incoming.Epoch < old.Epoch {
			prepared[principal] = old
			continue
		}
		if incoming.Epoch == old.Epoch && !same(incoming, old) || incoming.Role != old.Role || incoming.Advertise != old.Advertise || incoming.Epoch > old.Epoch && bytes.Equal(incoming.Id, old.Id) {
			return ErrIdentity
		}
	}
	if len(prepared) > directory.maximum {
		return status.Error(codes.ResourceExhausted, "Directory capacity exceeded")
	}
	// 合并后重新检查别名, 防止新列表的地址/ID 与保留的缺项旧部署冲突.
	clear(addresses)
	ids := make(map[string]bool, len(prepared))
	for _, member := range prepared {
		if addresses[member.Advertise] || ids[string(member.Id)] {
			return ErrIdentity
		}
		addresses[member.Advertise], ids[string(member.Id)] = true, true
	}
	directory.known = prepared
	return nil
}

// observe 接受经 Verify 验证的握手身份, 不依据未知/过时凭证制造降级或新部署别名.
func (directory *Directory) observe(member *orbit.Member) error {
	directory.mutex.Lock()
	defer directory.mutex.Unlock()
	old := directory.known[string(member.Principal)]
	if old != nil {
		if member.Epoch < old.Epoch || member.Epoch == old.Epoch && !same(member, old) || member.Role != old.Role || member.Advertise != old.Advertise || member.Epoch > old.Epoch && bytes.Equal(member.Id, old.Id) {
			return ErrIdentity
		}
		if same(member, old) {
			return nil
		}
	} else if len(directory.known) == directory.maximum {
		return status.Error(codes.ResourceExhausted, "Directory capacity exceeded")
	}
	for principal, known := range directory.known {
		if principal != string(member.Principal) && (bytes.Equal(known.Id, member.Id) || known.Advertise == member.Advertise) {
			return ErrIdentity
		}
	}
	directory.known[string(member.Principal)] = proto.Clone(member).(*orbit.Member)
	return nil
}

// Verify 为流首帧或管理 metadata 验签, 限定允许角色, 并记录可信替换; 不在线回查 Pulsar.
func (directory *Directory) Verify(data, signature []byte, roles ...orbit.Role) (*orbit.Member, error) {
	member, err := directory.identity.Verify(data, signature)
	if err != nil || member.Galaxy != directory.galaxy || !slices.Contains(roles, member.Role) {
		return nil, status.Error(codes.Unauthenticated, "Infrastructure admission rejected")
	}
	if err := directory.observe(member); err != nil {
		if status.Code(err) == codes.ResourceExhausted {
			return nil, err
		}
		return nil, status.Error(codes.Unauthenticated, "Infrastructure admission replaced or conflicting")
	}
	return member, nil
}

// Authorize 只接受唯一原始凭证, 在每个短管理 RPC 中使用; 长流只在首帧验签.
func (directory *Directory) Authorize(ctx context.Context, roles ...orbit.Role) (*orbit.Member, error) {
	values, _ := metadata.FromIncomingContext(ctx)
	data, signature := values.Get("astra-admission-bin"), values.Get("astra-signature-bin")
	if len(data) != 1 || len(signature) != 1 {
		return nil, status.Error(codes.Unauthenticated, "Current admission required")
	}
	return directory.Verify([]byte(data[0]), []byte(signature[0]), roles...)
}

// Current 检查已经通过签名的身份是否仍是本进程已知当前值, 不声称获知所有隔离替换.
func (directory *Directory) Current(member *orbit.Member) bool {
	if member == nil {
		return false
	}
	directory.mutex.RLock()
	defer directory.mutex.RUnlock()
	return same(directory.known[string(member.Principal)], member)
}

// Members 返回独立消息副本, 调用方不能修改目录; 结果按 ID 稳定排序, 不表示实时健康.
func (directory *Directory) Members() []*orbit.Member {
	directory.mutex.RLock()
	defer directory.mutex.RUnlock()
	result := make([]*orbit.Member, 0, len(directory.known))
	for _, member := range directory.known {
		result = append(result, proto.Clone(member).(*orbit.Member))
	}
	slices.SortFunc(result, func(a, b *orbit.Member) int {
		return bytes.Compare(a.Id, b.Id)
	})
	return result
}

// same 比较已经校验的业务身份字段, 未知 Protobuf 字段不成为新的身份代次.
func same(a, b *orbit.Member) bool {
	return a != nil && b != nil && a.Galaxy == b.Galaxy && bytes.Equal(a.Id, b.Id) && bytes.Equal(a.Principal, b.Principal) && a.Advertise == b.Advertise && a.Epoch == b.Epoch && a.Role == b.Role && a.Group == b.Group
}
