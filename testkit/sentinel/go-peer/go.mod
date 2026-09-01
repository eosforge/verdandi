module github.com/eosforge/verdandi/testkit/sentinel/go-peer

go 1.27.0

require github.com/eosforge/verdandi/sdk/go v0.1.0

require (
	github.com/cespare/xxhash/v2 v2.3.0 // indirect
	github.com/redis/go-redis/v9 v9.22.0 // indirect
	github.com/stretchr/testify v1.10.0 // indirect
	github.com/vmihailenco/msgpack/v5 v5.4.1 // indirect
	go.uber.org/atomic v1.11.0 // indirect
	golang.org/x/sys v0.30.0 // indirect
)

replace github.com/eosforge/verdandi/sdk/go => ../../../sdk/go
