package admission

import (
	"crypto/ed25519"
	"crypto/x509"
	"encoding/hex"
	"encoding/json"
	"encoding/pem"
	"os"
	"path/filepath"
	"testing"

	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"google.golang.org/protobuf/proto"
)

// fixture 仅使用仓库公开测试身份, 不读取用户部署凭据.
func fixture(name string) string {
	return filepath.Join("..", "..", "..", "cluster", "tests", "fixtures", name)
}

// identity 为每例独立装载材料, 返回的只读对象不会被其他用例替换.
func identity(t *testing.T) *Identity {
	t.Helper()
	identity, err := Load(fixture("star-a"), "127.0.0.1:7443")
	if err != nil {
		t.Fatal(err)
	}
	return identity
}

// sign 使用公开夹具 Ed25519 私钥, prefix 可显式指定以验证跨域拒绝.
func sign(t *testing.T, data []byte, prefix string) []byte {
	t.Helper()
	dataKey, err := os.ReadFile(fixture("pulsar/admission.key"))
	if err != nil {
		t.Fatal(err)
	}
	block, _ := pem.Decode(dataKey)
	if block == nil {
		t.Fatal("missing fixture key")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		t.Fatal(err)
	}
	key, ok := parsed.(ed25519.PrivateKey)
	if !ok {
		t.Fatal("fixture is not Ed25519")
	}
	return ed25519.Sign(key, append([]byte(prefix), data...))
}

// TestIdentity 用真实夹具签名验证角色、格式、域隔离和篡改, 不让被测解析器自行生成期望结果.
func TestIdentity(t *testing.T) {
	t.Parallel()
	identity := identity(t)
	if hex.EncodeToString(identity.Principal("alpha", "127.0.0.1:7443")) != "40ff2c81b912f4ba03daa166aa9ad70a81d80c2b1c33e85cf9e2c18e43a072dd" {
		t.Fatal("principal differs from cross-language vector")
	}
	for _, role := range []orbit.Role{orbit.Role_ROLE_STAR, orbit.Role_ROLE_PLANET, orbit.Role_ROLE_POLARIS, orbit.Role_ROLE_ASTROLABE} {
		member := &orbit.Member{Galaxy: "alpha", Id: []byte("opaque/星体\n"), Principal: identity.Principal("alpha", "127.0.0.1:7443"), Advertise: "127.0.0.1:7443", Epoch: 1, Role: role, Group: "default"}
		data, err := proto.Marshal(member)
		if err != nil {
			t.Fatal(err)
		}
		signature := sign(t, data, "proto.orbit.v1.admission\x00")
		decoded, err := identity.Verify(data, signature)
		if err != nil || !same(member, decoded) {
			t.Fatal("signed member did not round trip")
		}
		for _, invalid := range [][]byte{signature[:63], sign(t, data, "proto.astra.v1.admission\x00"), make([]byte, 64)} {
			if _, err := identity.Verify(data, invalid); err == nil {
				t.Fatal("accepted invalid signature")
			}
		}
		if _, err := identity.Verify(append(data, 0x78, 1), signature); err == nil {
			t.Fatal("accepted unsigned unknown field")
		}
		member.Role = orbit.Role(99)
		if Valid(member) {
			t.Fatal("unknown role became valid")
		}
		// bytes 字段允许任意原始编码, 准入层仍须与 C++ 一致地拒绝畸形 UTF-8.
		member.Role = role
		for _, invalid := range [][]byte{{0xff}, {0xc0, 0x80}, {0xed, 0xa0, 0x80}, {0xf4, 0x90, 0x80, 0x80}, {0xe4, 0xb8}} {
			member.Id = invalid
			data, err := proto.Marshal(member)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := identity.Verify(data, sign(t, data, "proto.orbit.v1.admission\x00")); err == nil {
				t.Fatal("accepted invalid UTF-8 member ID")
			}
		}
		member.Id = []byte{'a', 0, 'b'}
		if !Valid(member) {
			t.Fatal("rejected valid UTF-8 containing NUL")
		}
	}
	if _, err := Load(fixture("expired"), "127.0.0.1:7443"); err == nil {
		t.Fatal("accepted expired certificate")
	}
	if _, err := Load(fixture("star-a"), "192.0.2.1:7443"); err == nil {
		t.Fatal("accepted wrong certificate SAN")
	}
	for _, input := range []string{`{}`, `{"username":"a","password":"x","password":"y"}`, `{"username":"a","password":"x","extra":1}`, `{"username":"a","password":null}`, `{"username":"a","password":"x"} {}`, `{"username":"a","password":""}`} {
		if new(Identity).login([]byte(input)) == nil {
			t.Fatal("accepted invalid login object")
		}
	}
}

// TestSharedVectors 与 C++ 使用同一公开输入向量, 角色扩展不能改变原名称/端点/ID 规则.
func TestSharedVectors(t *testing.T) {
	t.Parallel()
	var vectors struct {
		Names, Addresses []struct {
			Value string
			Valid bool
		}
	}
	data, err := os.ReadFile(fixture("admission-v1.json"))
	if err != nil || json.Unmarshal(data, &vectors) != nil {
		t.Fatal("missing shared vectors")
	}
	for _, item := range vectors.Names {
		if Name(item.Value) != item.Valid {
			t.Fatalf("name differs: %q", item.Value)
		}
	}
	for _, item := range vectors.Addresses {
		_, err := Endpoint(item.Value)
		if (err == nil) != item.Valid {
			t.Fatalf("endpoint differs: %q", item.Value)
		}
	}
}
