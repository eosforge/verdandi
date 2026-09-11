// Package admission 负责部署身份验证, 持久登记和准入凭据签发.
package admission

import (
	"crypto/ed25519"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"io"
	"os"
	"path/filepath"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"google.golang.org/protobuf/proto"
)

// SignatureDomain 隔离准入签名和其他 Ed25519 用途, 两种语言必须逐字节一致.
const SignatureDomain = "verdandi-admission-v4\x00"

// Authority 的密钥及 TLS 配置只读共享, 不进入日志或协议应答.
type Authority struct {
	// TLS 强制服务端身份验证和 TLS 1.3; Load 后不得修改.
	TLS *tls.Config
	// key 仅用于域隔离准入签名, 不供 TLS 使用.
	key      ed25519.PrivateKey
	accounts *accounts
}

// Load 读取 TLS 材料, admission.key/pub 和 accounts.json, 配置只读共享.
// 私钥为 PKCS#8 PEM, admission.pub 为 32 字节 Ed25519 原始公钥. 每个文件最多 16 KiB.
func Load(directory string) (*Authority, error) {
	read := func(name string) ([]byte, error) { return ReadFile(filepath.Join(directory, name)) }
	ca, err := read("ca.pem")
	if err != nil {
		return nil, err
	}
	cert, err := read("cert.pem")
	if err != nil {
		return nil, err
	}
	key, err := read("key.pem")
	if err != nil {
		return nil, err
	}
	pair, err := tls.X509KeyPair(cert, key)
	if err != nil {
		return nil, errors.New("invalid TLS identity")
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(ca) {
		return nil, errors.New("invalid TLS trust roots")
	}
	// 与 Rust 节点一样在启动时验证自己的服务端证书, 不让过期或异链身份先占据监听端口.
	// 公布主机名由连接方核对; Supervisor 可以监听通配地址, 此处不拿 bind 地址替代 SAN.
	leaf, err := x509.ParseCertificate(pair.Certificate[0])
	if err != nil {
		return nil, errors.New("invalid TLS certificate")
	}
	intermediates := x509.NewCertPool()
	for _, encoded := range pair.Certificate[1:] {
		certificate, err := x509.ParseCertificate(encoded)
		if err != nil {
			return nil, errors.New("invalid TLS certificate chain")
		}
		intermediates.AddCert(certificate)
	}
	if _, err := leaf.Verify(x509.VerifyOptions{Roots: roots, Intermediates: intermediates,
		KeyUsages: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}); err != nil {
		return nil, errors.New("TLS certificate is not a valid server identity")
	}
	signing, err := read("admission.key")
	if err != nil {
		return nil, err
	}
	block, remainder := pem.Decode(signing)
	if block == nil || block.Type != "PRIVATE KEY" || len(remainder) != 0 {
		return nil, errors.New("invalid admission private key")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, errors.New("invalid admission private key")
	}
	private, ok := parsed.(ed25519.PrivateKey)
	if !ok {
		return nil, errors.New("admission key must be Ed25519")
	}
	public, err := read("admission.pub")
	if err != nil {
		return nil, err
	}
	if len(public) != ed25519.PublicKeySize || !private.Public().(ed25519.PublicKey).Equal(ed25519.PublicKey(public)) {
		return nil, errors.New("admission public and private keys differ")
	}
	accountData, err := read("accounts.json")
	if err != nil {
		return nil, err
	}
	accounts, err := loadAccounts(accountData)
	if err != nil {
		return nil, err
	}
	return &Authority{key: private, accounts: accounts, TLS: &tls.Config{
		MinVersion: tls.VersionTLS13, MaxVersion: tls.VersionTLS13,
		Certificates: []tls.Certificate{pair},
		ClientAuth:   tls.NoClientCert, NextProtos: []string{"h2"},
		// 不颁发恢复票据, 新连接的客户端重新验证服务端证书.
		SessionTicketsDisabled: true,
	}}, nil
}

// ReadFile 对本地配置也使用硬上限, 防止错误路径指向无界设备或大文件.
func ReadFile(path string) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return nil, err
	}
	if !info.Mode().IsRegular() || info.Size() > 16*1024 {
		return nil, errors.New("identity file must be a regular file no larger than 16 KiB")
	}
	data, err := io.ReadAll(io.LimitReader(file, 16*1024+1))
	if len(data) > 16*1024 {
		return nil, errors.New("identity file exceeds 16 KiB")
	}
	return data, err
}

// Sign 返回 Member 原始编码和域隔离的签名. 验证方必须验原始字节, 不重编码后验签.
func (a *Authority) Sign(member *wire.RegistrationResponse_Member) ([]byte, []byte, error) {
	payload, err := proto.MarshalOptions{Deterministic: true}.Marshal(member)
	if err != nil {
		return nil, nil, err
	}
	message := append([]byte(SignatureDomain), payload...)
	return payload, ed25519.Sign(a.key, message), nil
}
