package admission

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"

	wire "github.com/eosforge/verdandi/supervisor/internal/generated"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

func TestCanceledLoginWithFreeCapacityDoesNotComputePassword(t *testing.T) {
	a := &accounts{entries: map[string]Account{}, work: make(chan struct{}, 1)}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	// 空闲名额与取消同时就绪, 不应随机选择 KDF 分支并返回认证失败.
	for range 32 {
		if err := a.authenticate(ctx, "unknown", "wrong", wire.NodeRole_NODE_ROLE_STAR); status.Code(err) != codes.Canceled {
			t.Fatal(err)
		}
		if len(a.work) != 0 {
			t.Fatal("password permit leaked")
		}
	}
}

func TestProvisionInvalidInputNeverWritesPartialAccount(t *testing.T) {
	for _, input := range []string{
		`null`, `{}`, `{"username":"node","password":"p","roles":["star"]} {}`,
		`{"username":"bad/name","password":"p","roles":["star"]}`,
		`{"username":"node","password":"","roles":["star"]}`,
		`{"username":"node","password":"p","roles":["star","star"]}`,
		`{"username":"node","password":"p","roles":["admin"]}`,
		`{"username":"node","password":"p","roles":["star"],"extra":true}`,
		`{"username":"node","password":"` + strings.Repeat("x", 1025) + `","roles":["star"]}`,
		`{"username":"node","password":"` + string([]byte{0xff}) + `","roles":["star"]}`,
		strings.Repeat(" ", 4097),
	} {
		var output bytes.Buffer
		if err := WriteAccount(strings.NewReader(input), &output); err == nil || output.Len() != 0 {
			t.Fatal("invalid input produced an account")
		}
	}
}

type brokenReader struct{}

func (brokenReader) Read([]byte) (int, error) { return 0, io.ErrUnexpectedEOF }

type brokenWriter struct{}

func (brokenWriter) Write([]byte) (int, error) { return 0, io.ErrClosedPipe }

func TestProvisionPreservesInputAndOutputFailures(t *testing.T) {
	var output bytes.Buffer
	if err := WriteAccount(brokenReader{}, &output); err == nil || output.Len() != 0 {
		t.Fatal("input failure ignored")
	}
	input := `{"username":"node","password":"p","roles":["star"]}`
	if err := WriteAccount(strings.NewReader(input), brokenWriter{}); !errors.Is(err, io.ErrClosedPipe) {
		t.Fatal("output failure ignored", err)
	}
}

func TestAuthorityRejectsExpiredOrUntrustedLocalCertificate(t *testing.T) {
	for _, role := range []string{"expired", "rogue"} {
		t.Run(role, func(t *testing.T) {
			directory := t.TempDir()
			for _, name := range []string{"cert.pem", "key.pem", "ca.pem", "admission.pub", "admission.key", "accounts.json"} {
				from := "supervisor"
				if name == "cert.pem" || name == "key.pem" {
					from = role
				}
				data, err := os.ReadFile(filepath.Join(fixture(from), name))
				if err != nil {
					t.Fatal(err)
				}
				if err := os.WriteFile(filepath.Join(directory, name), data, 0600); err != nil {
					t.Fatal(err)
				}
			}
			if _, err := Load(directory); err == nil {
				t.Fatal("invalid local certificate accepted")
			}
		})
	}
}

func TestIdentityReadRejectsDirectoryAndOversizedFiles(t *testing.T) {
	directory := t.TempDir()
	if _, err := ReadFile(directory); err == nil {
		t.Fatal("directory accepted")
	}
	path := filepath.Join(directory, "bounded")
	for _, size := range []int{16384, 16385} {
		if err := os.WriteFile(path, bytes.Repeat([]byte{'x'}, size), 0600); err != nil {
			t.Fatal(err)
		}
		_, err := ReadFile(path)
		if (err == nil) != (size == 16384) {
			t.Fatalf("size %d: %v", size, err)
		}
	}
}

func TestInvalidServerClosesOwnedListener(t *testing.T) {
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	server := &Server{}
	if err := server.Serve(nil, listener); err == nil {
		t.Fatal("invalid server accepted")
	}
	if _, err := listener.Accept(); !errors.Is(err, net.ErrClosed) {
		t.Fatal("listener not released", err)
	}
	if err := server.Serve(context.Background(), nil); err == nil {
		t.Fatal("nil listener accepted")
	}
}

func FuzzAccountFileNeverAcceptsInvalidRoles(f *testing.F) {
	data, err := os.ReadFile(filepath.Join(fixture("supervisor"), "accounts.json"))
	if err != nil {
		f.Fatal(err)
	}
	f.Add(data)
	f.Add([]byte("null"))
	f.Add([]byte{0xff})
	f.Fuzz(func(t *testing.T, data []byte) {
		if len(data) > 16384 {
			t.Skip()
		}
		parsed, err := loadAccounts(data)
		if err != nil {
			return
		}
		if len(parsed.entries) < 1 || len(parsed.entries) > 64 {
			t.Fatal("account budget bypassed")
		}
		for _, account := range parsed.entries {
			if !validAccount(account.Username, account.Roles) {
				t.Fatal("invalid authorization")
			}
		}
	})
}
