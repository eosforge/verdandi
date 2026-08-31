//go:build windows

package catalog

import "go.uber.org/goleak"

func platformGoleakOptions() []goleak.Option {
	// Windows GetAddrInfoW cannot be interrupted after a dialing context is
	// canceled. SDK-owned goroutines remain checked.
	return []goleak.Option{goleak.IgnoreAnyFunction("net.(*Resolver).lookupIP.func1")}
}
