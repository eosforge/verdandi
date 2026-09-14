package admission

import (
	"crypto/sha256"
	"encoding/binary"
	"slices"
	"strings"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"github.com/eosforge/verdandi/supervisor/internal/membership"
)

// encodeMember 统一两种应答及签名正文的字段, 避免名单和凭据的角色或分组不一致.
func encodeMember(cluster string, member membership.Member) *wire.RegistrationResponse_Member {
	role := wire.Role_ROLE_STAR
	if member.Role == membership.Planet {
		role = wire.Role_ROLE_PLANET
	}
	return &wire.RegistrationResponse_Member{ClusterId: cluster, Id: member.ID, Principal: member.Principal,
		Advertise: member.Address, Epoch: member.Epoch, Role: role, Group: member.Group}
}

// candidates 最多返回 8 个 Star, 尽量各保留 4 个本组与跨组入口, 数量不足时由另一组补齐.
// 每个 Planet 用不同的确定性起点分散入口, 应答仍按本组优先和 id 排序, 不声称提供实时健康名单.
func candidates(stars []*wire.RegistrationResponse_Member, group, process string, round uint32) []*wire.RegistrationResponse_Member {
	local, remote := make([]*wire.RegistrationResponse_Member, 0), make([]*wire.RegistrationResponse_Member, 0)
	for _, star := range stars {
		if star.Group == group {
			local = append(local, star)
		} else {
			remote = append(remote, star)
		}
	}
	digest := sha256.Sum256([]byte(process))
	rotate := func(pool []*wire.RegistrationResponse_Member) []*wire.RegistrationResponse_Member {
		if len(pool) == 0 {
			return pool
		}
		offset := int((binary.BigEndian.Uint64(digest[:8]) + uint64(round)*4) % uint64(len(pool)))
		return append(pool[offset:], pool[:offset]...)
	}
	local, remote = rotate(local), rotate(remote)
	l, r := min(4, len(local)), min(4, len(remote))
	l = min(len(local), 8-r)
	r = min(len(remote), 8-l)
	result := make([]*wire.RegistrationResponse_Member, 0, l+r)
	result = append(result, local[:l]...)
	result = append(result, remote[:r]...)
	slices.SortFunc(result, func(a, b *wire.RegistrationResponse_Member) int {
		if (a.Group == group) != (b.Group == group) {
			if a.Group == group {
				return -1
			}
			return 1
		}
		return strings.Compare(a.Id, b.Id)
	})
	return result
}
