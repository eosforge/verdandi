//go:build !windows

package registration

import (
	"os"

	"go.uber.org/goleak"
)

func platformGoleakOptions() []goleak.Option {
	if os.Getenv("VERDANDI_SENTINEL_ADDRS") == "" {
		return nil
	}
	// Go 的纯 Go DNS 解析器不能保证在取消远端 Sentinel 拨号后立即回收
	// 并行 A/AAAA 查询；只忽略标准库解析帧，SDK 自有协程仍由 goleak 检查。
	return []goleak.Option{
		goleak.IgnoreAnyFunction("net.(*Resolver).lookupIPAddr.func2"),
		goleak.IgnoreAnyFunction("net.(*Resolver).goLookupIPCNAMEOrder.func3.1"),
		goleak.IgnoreAnyFunction("net.(*Resolver).goLookupIPCNAMEOrder.func4"),
	}
}
