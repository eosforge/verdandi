//go:build linux && cgo

package server

import (
	"context"
	"errors"
	"fmt"
	"path/filepath"
	"testing"

	"github.com/eosforge/verdandi/astra/internal/generated/comet"
	"github.com/eosforge/verdandi/astra/internal/generated/orbit"
	"github.com/eosforge/verdandi/astra/internal/generated/polaris"
	"github.com/eosforge/verdandi/astra/polaris/internal/storage"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/proto"
)

// fixture 独占测试权威库, 清理只发生在所有引用退出之后, 不触碰部署文件.
func fixture(t *testing.T, limits storage.Limits) *storage.Store {
	t.Helper()
	store, err := storage.Open(t.Context(), filepath.Join(t.TempDir(), "polaris.db"), storage.Binding{Galaxy: "alpha", Username: "polaris", Advertise: "127.0.0.1:7445"}, limits, true)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() {
		if err := store.Close(); err != nil {
			t.Error(err)
		}
	})
	return store
}

// TestPages 确认空基线、零字节、大记录、跨页完成位置和发送失败, 不以完整编码预遍历所有数据.
func TestPages(t *testing.T) {
	t.Parallel()
	for _, count := range []int{0, 1, 100} {
		snapshot := storage.Snapshot{Position: storage.Position{Scope: storage.Scope{Sector: "s", Spectrum: "p"}, Version: 9}}
		for index := range count {
			value := make([]byte, 8192)
			if index == 0 {
				value = nil
			}
			if index == 1 {
				value = make([]byte, 1<<20)
			}
			snapshot.Records = append(snapshot.Records, storage.Record{Key: fmt.Sprintf("k%04d", index), Value: value})
		}
		var received []*polaris.Snapshot
		if err := pages(&snapshot, 8<<20, func(page *polaris.Snapshot) error {
			received = append(received, proto.Clone(page).(*polaris.Snapshot))
			return nil
		}); err != nil {
			t.Fatal(err)
		}
		seen := 0
		for index, page := range received {
			last := index == len(received)-1
			if page.Complete != last || page.Version == nil || page.GetVersion() != 9 || string(page.Scope.Sector) != "s" || string(page.Scope.Spectrum) != "p" {
				t.Fatal("invalid snapshot completion boundary")
			}
			for _, entry := range page.Entries {
				if entry.Key != snapshot.Records[seen].Key {
					t.Fatal("snapshot reordered keys")
				}
				if _, ok := entry.Action.(*comet.AlmanacChange_Value); !ok {
					t.Fatal("zero byte value lost action presence")
				}
				seen++
			}
		}
		if seen != count || len(received) == 0 {
			t.Fatal("snapshot incomplete")
		}
		failure := errors.New("owned send failure")
		if err := pages(&snapshot, 8<<20, func(*polaris.Snapshot) error { return failure }); !errors.Is(err, failure) {
			t.Fatal("send failure swallowed")
		}
	}
}

// TestSmallPages 将完整 Packet 编码计入协商预算, 小帧不得被默认分页目标绕过.
func TestSmallPages(t *testing.T) {
	t.Parallel()
	snapshot := storage.Snapshot{Position: storage.Position{Scope: storage.Scope{Sector: "s", Spectrum: "p"}, Version: 1}}
	for index := range 20 {
		snapshot.Records = append(snapshot.Records, storage.Record{Key: fmt.Sprintf("k%02d", index), Value: make([]byte, 400)})
	}
	pagesSeen, recordsSeen, completions := 0, 0, 0
	if err := pages(&snapshot, 1024, func(page *polaris.Snapshot) error {
		if proto.Size(&polaris.Packet{Body: &polaris.Packet_Snapshot{Snapshot: page}}) > 1024 {
			t.Fatal("negotiated frame limit exceeded")
		}
		pagesSeen++
		recordsSeen += len(page.Entries)
		if page.Complete {
			completions++
		}
		return nil
	}); err != nil || pagesSeen < 2 || recordsSeen != 20 || completions != 1 {
		t.Fatal("small frame snapshot incomplete", err, pagesSeen, recordsSeen, completions)
	}
	snapshot.Records[0].Value = make([]byte, 1024)
	if err := pages(&snapshot, 1024, func(*polaris.Snapshot) error {
		t.Fatal("oversized first record was sent")
		return nil
	}); status.Code(err) != codes.ResourceExhausted {
		t.Fatal("oversized single record did not fail explicitly", err)
	}
}

// TestCache 只共享尚有发送引用的同目标快照, 新版本不能复用旧内容, 最后释放后返还准备预算.
func TestCache(t *testing.T) {
	t.Parallel()
	store := fixture(t, storage.Default())
	scope := storage.Scope{Sector: "s", Spectrum: "p"}
	if _, err := store.Commit(t.Context(), scope, storage.Change{Version: 1, Key: "k", Value: []byte{1}}); err != nil {
		t.Fatal(err)
	}
	cache := newCache(store, store.Limits(), 256<<20)
	a, err := cache.acquire(t.Context(), scope, 1)
	if err != nil {
		t.Fatal(err)
	}
	defer a.release()
	b, err := cache.acquire(t.Context(), scope, 1)
	if err != nil || a.Snapshot != b.Snapshot {
		t.Fatal("identical active target was not shared", err)
	}
	defer b.release()
	if _, err := store.Commit(t.Context(), scope, storage.Change{Version: 2, Key: "k", Value: []byte{2}}); err != nil {
		t.Fatal(err)
	}
	c, err := cache.acquire(t.Context(), scope, 2)
	if err != nil || c.Snapshot.Version != 2 || a.Snapshot.Version != 1 || a.Snapshot.Records[0].Value[0] != 1 {
		t.Fatal("new snapshot changed pinned old content", err)
	}
	c.release()
	a.release()
	b.release()
	if cache.used != 0 || len(cache.entries) != 0 {
		t.Fatal("unreferenced snapshot retained budget")
	}
	ctx, cancel := context.WithCancel(t.Context())
	cancel()
	if pin, err := cache.acquire(ctx, scope, 2); err == nil {
		pin.release()
		t.Fatal("cancelled preparation succeeded")
	}
	if cache.used != 0 {
		t.Fatal("failed preparation leaked budget")
	}
}

// TestCredentialValidation 普通值不解析, 内部登录表拒绝空 SECRET、畸形正文与超限 APIKEY.
func TestCredentialValidation(t *testing.T) {
	t.Parallel()
	scope := storage.Scope{Sector: "__auth", Spectrum: "comet"}
	valid, err := proto.Marshal(&orbit.Credential{Secret: []byte{0, 1, 255}})
	if err != nil {
		t.Fatal(err)
	}
	if !credential(scope, storage.Change{Key: "key", Value: valid}) || !credential(scope, storage.Change{Key: "key", Erase: true}) {
		t.Fatal("valid opaque credential rejected")
	}
	for _, change := range []storage.Change{{Key: "key"}, {Key: "key", Value: []byte{255}}, {Key: "", Value: valid}, {Key: "a\x00b", Erase: true}, {Key: string(make([]byte, 129)), Value: valid}} {
		if credential(scope, change) {
			t.Fatal("invalid internal credential accepted")
		}
	}
	if !credential(storage.Scope{Sector: "business", Spectrum: "comet"}, storage.Change{Key: "key", Value: []byte{255}}) {
		t.Fatal("ordinary content interpreted as credential")
	}
}
