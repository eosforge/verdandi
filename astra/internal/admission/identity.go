// Package admission 复用 Orbit 签名和 TLS 规则, 供 Go 控制服务使用, 不向 Comet 开放.
package admission

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/netip"
	"os"
	"path/filepath"
	"unicode/utf8"

	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"google.golang.org/protobuf/proto"
)

// ErrIdentity 不包含凭据正文、文件路径或远端返回的未受控诊断.
var ErrIdentity = errors.New("invalid infrastructure identity")

// Identity 加载后只读, TLS 配置通过 Clone 返回, 账号只在登录请求中使用.
type Identity struct {
	username, password string
	public             ed25519.PublicKey
	client, server     *tls.Config
}

// Load 读取有界材料并在登记之前验证本地证书授权规范数值端点.
func Load(directory, advertise string) (*Identity, error) {
	endpoint, err := Endpoint(advertise)
	if err != nil {
		return nil, ErrIdentity
	}
	materials := make(map[string][]byte, 5)
	for _, name := range []string{"ca.pem", "cert.pem", "key.pem", "admission.pub", "login.json"} {
		path := filepath.Join(directory, name)
		if info, err := os.Lstat(path); err != nil || !info.Mode().IsRegular() {
			return nil, ErrIdentity
		}
		file, err := os.Open(path)
		if err != nil {
			return nil, ErrIdentity
		}
		info, staterr := file.Stat()
		data, readerr := io.ReadAll(io.LimitReader(file, 16385))
		closeerr := file.Close()
		if staterr != nil || !info.Mode().IsRegular() || readerr != nil || closeerr != nil || len(data) > 16384 {
			return nil, ErrIdentity
		}
		materials[name] = data
	}
	// 账号 JSON 逐字段读取, 拒绝重复/未知字段, 不依赖 encoding/json 最后一个值获胜的行为.
	identity := &Identity{public: ed25519.PublicKey(materials["admission.pub"])}
	if len(identity.public) != ed25519.PublicKeySize || identity.login(materials["login.json"]) != nil {
		return nil, ErrIdentity
	}
	certificate, err := tls.X509KeyPair(materials["cert.pem"], materials["key.pem"])
	if err != nil || len(certificate.Certificate) == 0 {
		return nil, ErrIdentity
	}
	roots, intermediates := x509.NewCertPool(), x509.NewCertPool()
	if !roots.AppendCertsFromPEM(materials["ca.pem"]) {
		return nil, ErrIdentity
	}
	leaf, err := x509.ParseCertificate(certificate.Certificate[0])
	if err != nil {
		return nil, ErrIdentity
	}
	for _, data := range certificate.Certificate[1:] {
		certificate, err := x509.ParseCertificate(data)
		if err != nil {
			return nil, ErrIdentity
		}
		intermediates.AddCert(certificate)
	}
	if _, err := leaf.Verify(x509.VerifyOptions{DNSName: endpoint.Addr().String(), Roots: roots, Intermediates: intermediates, KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}); err != nil {
		return nil, ErrIdentity
	}
	identity.client = &tls.Config{RootCAs: roots, MinVersion: tls.VersionTLS13, MaxVersion: tls.VersionTLS13}
	identity.server = &tls.Config{Certificates: []tls.Certificate{certificate}, MinVersion: tls.VersionTLS13, MaxVersion: tls.VersionTLS13}
	return identity, nil
}

// login 接收且仅接收 username/password, 校验成功之前不发布 Identity.
func (identity *Identity) login(data []byte) error {
	if !utf8.Valid(data) {
		return ErrIdentity
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	if token, err := decoder.Token(); err != nil || token != json.Delim('{') {
		return ErrIdentity
	}
	seen := make(map[string]bool, 2)
	for decoder.More() {
		token, err := decoder.Token()
		name, ok := token.(string)
		if err != nil || !ok || seen[name] || name != "username" && name != "password" {
			return ErrIdentity
		}
		seen[name] = true
		var value string
		if err := decoder.Decode(&value); err != nil {
			return ErrIdentity
		}
		if name == "username" {
			identity.username = value
		} else {
			identity.password = value
		}
	}
	if token, err := decoder.Token(); err != nil || token != json.Delim('}') {
		return ErrIdentity
	}
	if _, err := decoder.Token(); err != io.EOF || len(seen) != 2 || !Name(identity.username) || len(identity.password) == 0 || len(identity.password) > 1024 {
		return ErrIdentity
	}
	return nil
}

// Client 返回独立可配置的服务端验证 TLS 配置, 不附带客户端私钥或关闭主机校验.
func (identity *Identity) Client() *tls.Config { return identity.client.Clone() }

// Server 返回仅服务端 TLS 的配置, 节点身份由 Orbit bearer 验证.
func (identity *Identity) Server() *tls.Config { return identity.server.Clone() }

// Username 用于权威库固定部署绑定, 不暴露密码.
func (identity *Identity) Username() string { return identity.username }

// Principal 与 C++/旧 Go 保持同一字节序列, 密码轮换不会改变部署槽位.
func (identity *Identity) Principal(galaxy, advertise string) string {
	digest := sha256.Sum256([]byte(identity.username + "\x00" + galaxy + "\x00" + advertise))
	return hex.EncodeToString(digest[:])
}

// Verify 验证原始签名字节和完整身份格式; 当前性仍由 Directory 的已知替换关系判定.
func (identity *Identity) Verify(data, signature []byte) (*orbit.Member, error) {
	if len(data) == 0 || len(data) > 1024 || len(signature) != ed25519.SignatureSize {
		return nil, ErrIdentity
	}
	input := append([]byte("proto.orbit.v1.admission\x00"), data...)
	if !ed25519.Verify(identity.public, input, signature) {
		return nil, ErrIdentity
	}
	member := new(orbit.Member)
	if proto.Unmarshal(data, member) != nil || !Valid(member) {
		return nil, ErrIdentity
	}
	return member, nil
}

// Valid 只检查格式, 不把结构有效的成员当成已经通过签名或当前身份验证.
func Valid(member *orbit.Member) bool {
	if member == nil || !Name(member.Galaxy) || !Name(member.Group) || len(member.Id) == 0 || len(member.Id) > 128 || !utf8.ValidString(member.Id) || member.Epoch == 0 || !Role(member.Role) || len(member.Principal) != 64 {
		return false
	}
	for _, char := range member.Principal {
		if !(char >= '0' && char <= '9' || char >= 'a' && char <= 'f') {
			return false
		}
	}
	_, err := Endpoint(member.Advertise)
	return err == nil
}

// Role 仅接受四种明确基础设施身份, 不把未知数值转换成默认 Star.
func Role(role orbit.Role) bool {
	return role == orbit.Role_ROLE_STAR || role == orbit.Role_ROLE_PLANET || role == orbit.Role_ROLE_POLARIS || role == orbit.Role_ROLE_ASTROLABE
}

// Name 检查既有 1..64 字节安全 ASCII 名称, 不进行大小写或 Unicode 归一化.
func Name(value string) bool {
	if len(value) == 0 || len(value) > 64 {
		return false
	}
	for _, char := range value {
		if !(char >= 'a' && char <= 'z' || char >= 'A' && char <= 'Z' || char >= '0' && char <= '9' || char == '.' || char == '_' || char == '-') {
			return false
		}
	}
	return true
}

// Endpoint 接受规范数值 IP:PORT, 拒绝零端口、通配、多播、IPv4 映射及 zone 别名.
func Endpoint(value string) (netip.AddrPort, error) {
	endpoint, err := netip.ParseAddrPort(value)
	if err != nil || endpoint.String() != value || endpoint.Port() == 0 || endpoint.Addr().IsUnspecified() || endpoint.Addr().IsMulticast() || endpoint.Addr().Is4In6() || endpoint.Addr().Zone() != "" {
		return netip.AddrPort{}, ErrIdentity
	}
	return endpoint, nil
}
