package admission

import (
	"bytes"
	"context"
	"crypto/pbkdf2"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"slices"
	"unicode/utf8"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"github.com/eosforge/verdandi/supervisor/internal/membership"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

// 固定工作量同时用于存在和不存在的账号, 不接受文件输入任意昂贵的迭代次数.
const passwordIterations = 600000

// Account 只保存加盐密码摘要和允许的角色. 一个账号可以登记多个端点.
type Account struct {
	// Username 为 1..64 个安全 ASCII 字节, 大小写敏感.
	Username string `json:"username"`
	// Salt 为 16 字节随机盐的十六进制文本.
	Salt string `json:"salt"`
	// Hash 为固定 PBKDF2 参数计算的 32 字节摘要, 不保存明文密码.
	Hash string `json:"hash"`
	// Roles 只允许 star 或 planet, 非空且不重复.
	Roles []string `json:"roles"`
}

type accounts struct {
	entries map[string]Account
	// 限制密码计算并发, 等待者还受连接数和每连接 RPC 上限约束.
	work chan struct{}
}

// loadAccounts 在启动时验证完整文件, 失败不发布部分账号表, 不回显输入.
func loadAccounts(data []byte) (*accounts, error) {
	if !utf8.Valid(data) {
		return nil, errors.New("invalid accounts.json encoding")
	}
	var entries []Account
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&entries); err != nil {
		return nil, errors.New("invalid accounts.json")
	}
	var extra any
	if decoder.Decode(&extra) != io.EOF || len(entries) == 0 || len(entries) > 64 {
		return nil, errors.New("invalid account count or trailing data")
	}
	result := &accounts{entries: make(map[string]Account), work: make(chan struct{}, 4)}
	for _, account := range entries {
		salt, e1 := hex.DecodeString(account.Salt)
		hash, e2 := hex.DecodeString(account.Hash)
		if !validAccount(account.Username, account.Roles) || e1 != nil || e2 != nil || len(salt) != 16 || len(hash) != 32 {
			return nil, errors.New("invalid account configuration")
		}
		if _, exists := result.entries[account.Username]; exists {
			return nil, errors.New("duplicate account")
		}
		result.entries[account.Username] = account
	}
	return result, nil
}

// validAccount 统一文件读取与离线账号生成的语义, 无效输入在昂贵 KDF 前失败.
func validAccount(username string, roles []string) bool {
	return membership.Name(username) && len(roles) >= 1 && len(roles) <= 2 &&
		(len(roles) != 2 || roles[0] != roles[1]) &&
		!slices.ContainsFunc(roles, func(role string) bool { return role != membership.Star && role != membership.Planet })
}

// authenticate 在有界并发中验证密码, 取消只等待已经开始的一次固定 KDF, 不启动额外 goroutine.
// 排队前后都观察 ctx, 避免 select 同时就绪时让已取消请求继续消耗密码计算资源.
func (a *accounts) authenticate(ctx context.Context, username, password string, role wire.Role) error {
	if ctx.Err() != nil {
		return status.FromContextError(ctx.Err()).Err()
	}
	denied := status.Error(codes.Unauthenticated, "invalid login")
	if !membership.Name(username) || len(password) == 0 || len(password) > 1024 {
		return denied
	}
	select {
	case a.work <- struct{}{}:
		defer func() { <-a.work }()
	case <-ctx.Done():
		return status.FromContextError(ctx.Err()).Err()
	}
	if ctx.Err() != nil {
		return status.FromContextError(ctx.Err()).Err()
	}
	account, exists := a.entries[username]
	// 未知账号也执行相同 KDF, 不通过快速失败直接暴露账号存在性.
	salt, expected := make([]byte, 16), make([]byte, 32)
	if exists {
		salt, _ = hex.DecodeString(account.Salt)
		expected, _ = hex.DecodeString(account.Hash)
	}
	derived, err := pbkdf2.Key(sha256.New, password, salt, passwordIterations, 32)
	if ctx.Err() != nil {
		return status.FromContextError(ctx.Err()).Err()
	}
	if err != nil || subtle.ConstantTimeCompare(derived, expected) != 1 || !exists {
		return denied
	}
	if role == wire.Role_ROLE_UNSPECIFIED {
		return nil
	}
	for _, allowed := range account.Roles {
		if allowed == membership.Star && role == wire.Role_ROLE_STAR || allowed == membership.Planet && role == wire.Role_ROLE_PLANET {
			return nil
		}
	}
	return status.Error(codes.PermissionDenied, "role not authorized")
}

// endpointPrincipal 与 C++ 使用相同的零分隔编码, 地址必须已规范化.
func endpointPrincipal(username, cluster, address string) string {
	digest := sha256.Sum256([]byte(username + "\x00" + cluster + "\x00" + address))
	return hex.EncodeToString(digest[:])
}
