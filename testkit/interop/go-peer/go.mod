module github.com/eosforge/verdandi/testkit/interop/go-peer

go 1.27.0

require (
	github.com/eosforge/verdandi/sdk/go v0.1.0
	github.com/redis/go-redis/v9 v9.22.0
)

require (
	github.com/cespare/xxhash/v2 v2.3.0 // indirect
	github.com/vmihailenco/msgpack/v5 v5.4.1 // indirect
	github.com/vmihailenco/tagparser/v2 v2.0.0 // indirect
	go.etcd.io/bbolt v1.4.3 // indirect
	go.uber.org/atomic v1.11.0 // indirect
	golang.org/x/sys v0.30.0 // indirect
)

replace github.com/eosforge/verdandi/sdk/go => ../../../sdk/go
