package admission

import (
	"context"
	"fmt"
	"slices"
	"sync"
	"testing"

	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// member 为独立部署生成明确测试字段, 不把当前网络时间用于身份顺序.
func member(index, epoch int, role orbit.Role) *orbit.Member {
	principal := make([]byte, 32)
	principal[0], principal[31] = byte(index>>8), byte(index)
	return &orbit.Member{Galaxy: "alpha", Id: []byte(fmt.Sprintf("node-%02d-%02d", index, epoch)), Principal: principal, Advertise: fmt.Sprintf("127.0.0.1:%d", 7400+index), Epoch: uint64(epoch), Role: role, Group: "default"}
}

// directory 创建独立内存目录, 不启动网络或访问磁盘成员库.
func directory(t *testing.T, role orbit.Role, limit int) *Directory {
	t.Helper()
	value, err := NewDirectory(identity(t), "alpha", role, limit)
	if err != nil {
		t.Fatal(err)
	}
	return value
}

// TestDirectoryReplacement 覆盖旧查询晚到、缺项保留、同代冲突和对外返回值寿命隔离.
func TestDirectoryReplacement(t *testing.T) {
	t.Parallel()
	directory := directory(t, orbit.Role_ROLE_POLARIS, 4)
	a, b := member(1, 1, orbit.Role_ROLE_STAR), member(2, 1, orbit.Role_ROLE_POLARIS)
	if err := directory.Install([]*orbit.Member{a, b}); err != nil {
		t.Fatal(err)
	}
	fresh := member(1, 2, orbit.Role_ROLE_STAR)
	if err := directory.observe(fresh); err != nil {
		t.Fatal(err)
	}
	if err := directory.Install([]*orbit.Member{a}); err != nil || !directory.Current(fresh) || directory.Current(a) || !directory.Current(b) {
		t.Fatal("late query downgraded identity or absence deleted deployment")
	}
	conflict := proto.Clone(fresh).(*orbit.Member)
	conflict.Id = []byte("same-epoch-conflict")
	if directory.Install([]*orbit.Member{conflict}) == nil || !directory.Current(fresh) {
		t.Fatal("conflicting epoch modified directory")
	}
	view := directory.Members()
	view[0].Id = []byte("mutated-by-reader")
	if !directory.Current(fresh) {
		t.Fatal("reader mutated internal identity")
	}
	if directory.observe(a) == nil {
		t.Fatal("accepted superseded handshake")
	}
}

// TestDirectoryBoundaries 一个坏完整列表不会安装其合法前缀, 合并后的别名也必须拒绝.
func TestDirectoryBoundaries(t *testing.T) {
	t.Parallel()
	directory := directory(t, orbit.Role_ROLE_POLARIS, 2)
	a := member(1, 1, orbit.Role_ROLE_STAR)
	if err := directory.Install([]*orbit.Member{a}); err != nil {
		t.Fatal(err)
	}
	for _, invalid := range [][]*orbit.Member{{a, a}, {member(2, 1, orbit.Role_ROLE_STAR), a}, {member(2, 1, orbit.Role_ROLE_ASTROLABE)}, {nil}} {
		if directory.Install(invalid) == nil || !directory.Current(a) || len(directory.Members()) != 1 {
			t.Fatal("invalid complete list changed state")
		}
	}
	alias := member(2, 1, orbit.Role_ROLE_STAR)
	alias.Advertise = a.Advertise
	if directory.Install([]*orbit.Member{alias}) == nil {
		t.Fatal("alias against retained missing member accepted")
	}
	if err := directory.Install([]*orbit.Member{member(2, 1, orbit.Role_ROLE_POLARIS)}); err != nil {
		t.Fatal(err)
	}
	if err := directory.Install([]*orbit.Member{member(3, 1, orbit.Role_ROLE_STAR)}); status.Code(err) != codes.ResourceExhausted {
		t.Fatal("combined directory exceeded bounded deployment slots")
	}
}

// TestAuthorize 检查原始 metadata 的唯一性、签名、角色与替换, 不调用密码 KDF 或改启动请求.
func TestAuthorize(t *testing.T) {
	t.Parallel()
	directory := directory(t, orbit.Role_ROLE_POLARIS, 4)
	current := member(1, 1, orbit.Role_ROLE_ASTROLABE)
	data, err := proto.Marshal(current)
	if err != nil {
		t.Fatal(err)
	}
	signature := sign(t, data, "proto.orbit.v1.admission\x00")
	values := metadata.Pairs("astra-admission-bin", string(data), "astra-signature-bin", string(signature))
	ctx := metadata.NewIncomingContext(context.Background(), values)
	if got, err := directory.Authorize(ctx, orbit.Role_ROLE_ASTROLABE); err != nil || !same(got, current) {
		t.Fatal("valid admission rejected")
	}
	if _, err := directory.Authorize(ctx, orbit.Role_ROLE_STAR); status.Code(err) != codes.Unauthenticated {
		t.Fatal("wrong role authorized")
	}
	duplicate := values.Copy()
	duplicate.Append("astra-admission-bin", string(data))
	if _, err := directory.Authorize(metadata.NewIncomingContext(ctx, duplicate), orbit.Role_ROLE_ASTROLABE); status.Code(err) != codes.Unauthenticated {
		t.Fatal("duplicate admission authorized")
	}
	if err := directory.observe(member(1, 2, orbit.Role_ROLE_ASTROLABE)); err != nil {
		t.Fatal(err)
	}
	if _, err := directory.Authorize(ctx, orbit.Role_ROLE_ASTROLABE); status.Code(err) != codes.Unauthenticated {
		t.Fatal("old credential authorized after replacement")
	}
}

// TestDirectoryConcurrent 单部署的新凭证与旧查询竞争, 最终保留已观察到的最高代次.
func TestDirectoryConcurrent(t *testing.T) {
	t.Parallel()
	directory := directory(t, orbit.Role_ROLE_ASTROLABE, 8)
	var workers sync.WaitGroup
	for index := range 8 {
		workers.Go(func() {
			for epoch := 1; epoch <= 30; epoch++ {
				_ = directory.observe(member(index+1, epoch, orbit.Role_ROLE_STAR))
				_ = directory.Install([]*orbit.Member{member(index+1, 1, orbit.Role_ROLE_STAR)})
			}
		})
	}
	workers.Wait()
	if members := directory.Members(); len(members) != 8 || slices.ContainsFunc(members, func(member *orbit.Member) bool { return member.Epoch != 30 }) {
		t.Fatal("concurrent directory lost latest identities")
	}
}
