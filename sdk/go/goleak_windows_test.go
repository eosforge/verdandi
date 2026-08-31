//go:build windows

package verdandi

import "go.uber.org/goleak"

func platformGoleakOptions() []goleak.Option {
	// Windows GetAddrInfoW cannot be interrupted after a dialing context is
	// canceled. Remote Sentinel tests can therefore outlive the client briefly
	// in this standard-library resolver frame; SDK-owned frames remain checked.
	return []goleak.Option{goleak.IgnoreAnyFunction("net.(*Resolver).lookupIP.func1")}
}
