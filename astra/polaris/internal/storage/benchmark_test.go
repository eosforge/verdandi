//go:build linux && cgo

package storage

import (
	"fmt"
	"path/filepath"
	"testing"
)

// BenchmarkCommit 使用真实生产 SQLite 配置及磁盘事务, 比较不同已满历史长度的稳态提交.
// 建库和预填在计时外, 不将内存数据库结果冒充持久提交性能; 每次输入均为新的 version.
func BenchmarkCommit(b *testing.B) {
	for _, history := range []int64{16, 4096} {
		b.Run(fmt.Sprint(history), func(b *testing.B) {
			limits := Default()
			limits.History = history
			store, err := Open(b.Context(), filepath.Join(b.TempDir(), "polaris.db"), Binding{Galaxy: "measure", Username: "polaris", Advertise: "127.0.0.1:45101"}, limits, true)
			if err != nil {
				b.Fatal(err)
			}
			defer func() {
				if err := store.Close(); err != nil {
					b.Error(err)
				}
			}()
			scope := Scope{"measure", "commit"}
			value := make([]byte, 128)
			version := Version(0)
			commit := func() {
				version++
				confirmed, err := store.Commit(b.Context(), scope, Change{Version: version, Key: "key", Value: value})
				if err != nil || confirmed != version {
					b.Fatalf("commit=%d error=%v", confirmed, err)
				}
			}
			for range history {
				commit()
			}
			b.ReportAllocs()
			b.ResetTimer()
			for range b.N {
				commit()
			}
			b.StopTimer()
		})
	}
}
