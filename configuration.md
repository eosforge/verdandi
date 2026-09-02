# Verdandi JSON Configuration Reference

Verdandi v1 uses one canonical external JSON structure.
[`configuration.schema.json`](configuration.schema.json) is its structural
companion; byte-count limits, endpoint grammar, exact error fields and
cross-field relationships are authoritative in the SDK validators plus
[`testkit/conformance/v1/configuration.json`](testkit/conformance/v1/configuration.json).
[`configuration.example.json`](configuration.example.json) contains every v1
field. Go, Rust, and C++ load that same example in their unit suites before
converting it into their own language-native Redis, Registration/Selector, and
Catalog configuration types. C# strictly encodes the same JSON as UTF-8 and
passes it through C ABI v1 to the C++ loader; it intentionally owns no fourth
configuration DTO or defaults table. `Configuration.Validate` exposes the same
native parser as a connection-free C# validation API.

Go exposes `configuration.LoadFile`, `configuration.LoadJSON`, and
`configuration.ParseJSON`; Rust exposes `configuration::Config::load_json` and
`configuration::Config::from_json`; C++ exposes
`verdandi::configuration::load_json` and `from_json`. All three loaders:

- accept at most 1 MiB of UTF-8 JSON;
- require `version: "v1"` and one Redis topology;
- reject unknown or duplicate fields, JSON `null`, trailing values,
  non-canonical negative zero, fractional numbers, wrong JSON shapes, and
  values outside the documented ranges;
- distinguish an omitted optional number from an explicit zero;
- validate all enabled domains before returning, without opening Redis, a
  local checkpoint, or a TLS certificate file.

The JSON DTO is only the stable cross-language boundary. After loading, each
SDK converts milliseconds to `time.Duration`/`Duration`, addresses and auth to
its driver topology, TLS to its native representation, byte/count limits to
checked integer types, and the checkpoint string to its native path type.
Native `Check`/`check` methods remain the final owners of field and relationship
validation. No exported defaults/ranges constants surface or configuration
code generator is introduced.

`redis.tls` is an object rather than a boolean. Structural validation is pure:
it checks enablement, trust-source relationships, path bounds, mTLS pairing,
and the fixed certificate identity without reading files. Go, Rust, and C++ each
read at most 1 MiB from one PEM file while constructing the native TLS driver;
all require TLS 1.2 or newer, always verify the peer certificate, and
support an optional private CA bundle plus an unencrypted PEM client
certificate/private-key pair. No insecure skip-verification option exists.

Every duration whose minimum is greater than zero is a finite waiting or
maintenance budget and cannot be disabled. In particular, Redis commands,
connection establishment and Selector/Catalog synchronization always have a
positive upper bound. A zero marked as `value`
is reserved for non-waiting semantics such as disabling jitter, publishing a
view immediately, adding no artificial clock uncertainty, or applying no
aggregate Catalog view limit.

In JSON, omission selects the documented default. Explicit zero is accepted
only where the table marks zero as `value`. All JSON converters materialize
the same effective native values. Go applications that construct native
configs directly may still use the package's documented zero-value defaults;
Rust applications normally begin with its constructors; C++ native structs
carry their documented member initializers and expose `check()`.

Redis physical reconnection intentionally has one portable setting:
`redis.reconnect.delay_ms`. It is a fixed delay, defaults to 100 ms, accepts
10..30,000 ms, and never authorizes replaying a command whose result is
unknown. Go maps it to `DialerRetryTimeout` without a callback, Rust uses a
constant Fred policy with driver jitter disabled, and C++ maps it to the
Boost.Redis reconnect interval. Selector and Catalog recovery are separate
business-level authoritative repairs; their `recovery` objects retain
initial/max delay, multiplier, and jitter.

Sentinel TLS requires a non-empty `redis.tls.server_name`. This name is a fixed
deployment identity, not an address returned by Sentinel: every configured
Sentinel and every Redis primary or replica that Sentinel may select must
present a certificate whose SAN contains that same DNS name or IP identity.
Certificate chain, validity, handshake signature, and identity verification
remain mandatory during initial discovery, reconnect, and promotion. This
prevents a compromised or misconfigured Sentinel response from changing the
certificate identity that the SDK trusts.

Go and C++ send a configured DNS identity as SNI on every Sentinel and data
connection. Rust's current Fred transport cannot propagate one fixed SNI value
to all dynamically discovered nodes, so Sentinel mode suppresses address-
derived SNI while rustls still verifies every peer against the configured fixed
identity. Redis and Sentinel must therefore terminate TLS directly rather than
depend on DNS-SNI virtual-host routing in front of dynamically announced
addresses. An IP `server_name` is verified as an IP SAN and is not sent as SNI.

Catalog has no external Path lock or lock-related configuration. Replace and
Delete are atomic Redis-order last-write-wins operations. Patch performs an
unlocked HMGET projection in the SDK and then commits only if Lua still sees
the exact `base_revision`; a same-Path race therefore has one winner and a
stale loser. Timeout after a mutation was sent remains `ambiguous` because the
reply can be lost after Redis committed the write.

Non-numeric JSON fields are fixed as follows:

| Path | JSON type | Default/requirement | Meaning |
| --- | --- | --- | --- |
| `version` | string | required, exactly `v1` | JSON contract version |
| `redis.mode` | string | required, `standalone` or `sentinel` | Redis topology; Cluster is rejected |
| `redis.addresses` | string array | required, at least one `host:port`; Standalone exactly one | Valid UTF-8 Redis or Sentinel endpoints; IPv6 uses `[address]:port`, host rejects whitespace/control bytes, port is ASCII digits in 1..65535 |
| `redis.master_name` | string | Sentinel required; Standalone empty | Sentinel monitored service name |
| `redis.auth.username/password` | string | empty | Redis data-node ACL identity |
| `redis.sentinel_auth.username/password` | string | empty; Standalone must remain empty | Sentinel's independent ACL identity |
| `redis.tls.enabled` | boolean | false | Enables TLS 1.2+ with mandatory peer verification |
| `redis.tls.system_roots` | boolean | true | Includes operating-system trust roots; false requires a non-empty `ca_file` |
| `redis.tls.server_name` | string | Standalone empty; Sentinel TLS required; at most 253 UTF-8 bytes | Fixed certificate identity; Standalone empty uses its configured host, while every Sentinel/data-node certificate must contain the configured Sentinel identity |
| `redis.tls.ca_file` | string | empty, at most 4096 UTF-8 bytes | PEM CA bundle appended to the selected trust set; each file is capped at 1 MiB |
| `redis.tls.cert_file` | string | empty, at most 4096 UTF-8 bytes | PEM client certificate chain; must be configured together with `key_file` |
| `redis.tls.key_file` | string | empty, at most 4096 UTF-8 bytes | Unencrypted PEM client private key; must be configured together with `cert_file` |
| `registration.zone` | string | required when object exists; `[A-Za-z]{1,32}` | Registration/Selector management isolation |
| `catalog.zone` | string | required when object exists; `[A-Za-z]{1,32}` | Catalog management isolation |
| `catalog.local_store_path` | string | omitted disables; explicit empty rejected; 1..4096 UTF-8 bytes, no NUL | Disposable local Catalog checkpoint path |

When changing a value, update this table together with the owning Go, Rust, and
C++ configuration structure methods and their tests, then rerun the C# facade
configuration/opening tests against the updated C++ runtime. The SDK intentionally
exposes no separate defaults/ranges constants API.

| Path | Kind | Default | Minimum | Maximum | Zero | Meaning |
|---|---:|---:|---:|---:|---|---|
| `redis.timeout_ms` | `duration_ms` | 2000 | 10 | 15000 | `default` | 普通Redis命令的总等待上限 |
| `redis.connect_timeout_ms` | `duration_ms` | 5000 | 20 | 30000 | `default` | 单次TCP连接及TLS握手的等待上限 |
| `redis.database` | `count` | 0 | 0 | 255 | `value` | Redis逻辑数据库编号 |
| `redis.pool.min_connections` | `count` | 1 | 1 | 1024 | `default` | 连接池保持的最少连接数 |
| `redis.pool.max_connections` | `count` | 4 | 1 | 1024 | `default` | 连接池允许的最多连接数 |
| `redis.pool.idle_timeout_ms` | `duration_ms` | 10000 | 1000 | 3600000 | `default` | 空闲连接被回收前允许保持的时长 |
| `redis.reconnect.delay_ms` | `duration_ms` | 100 | 10 | 30000 | `default` | Redis驱动重新建立物理连接前的固定等待，不重放业务命令 |
| `registration.error_buffer_capacity` | `count` | 16 | 1 | 1024 | `default` | Registration领域异步诊断缓冲容量 |
| `registration.buffer_capacity` | `count` | 8 | 1 | 256 | `default` | 单个Registration Fields邮箱同时接纳的最多结果等待者 |
| `registration.min_renew_interval_ms` | `duration_ms` | 100 | 10 | 60000 | `default` | 允许配置的最短显式或自动续期间隔 |
| `registration.renew_jitter_percent` | `percent` | 10 | 0 | 50 | `value` | 自动续期间隔的随机抖动百分比 |
| `registration.policy_refresh_jitter_percent` | `percent` | 10 | 0 | 50 | `value` | Redis策略刷新周期的随机抖动百分比 |
| `registration.policy.attr_max_fields` | `count` | 16 | 1 | 128 | `default` | Registration Attr顶层字段数默认限制 |
| `registration.policy.data_max_fields` | `count` | 32 | 1 | 128 | `default` | Registration Data顶层字段数默认限制 |
| `registration.policy.field_name_max_bytes` | `bytes` | 64 | 1 | 64 | `default` | 应用字段名的最大UTF-8字节数 |
| `registration.policy.attr_value_max_bytes` | `bytes` | 128 | 1 | 16384 | `default` | 单个Attr字段值的最大字节数 |
| `registration.policy.data_value_max_bytes` | `bytes` | 128 | 1 | 16384 | `default` | 单个Data字段值的最大字节数 |
| `registration.policy.record_max_bytes` | `bytes` | 16384 | 1 | 65536 | `default` | 完整Registration记录的最大估算字节数 |
| `registration.policy.refresh_ms` | `duration_ms` | 30000 | 1000 | 86400000 | `default` | SDK重新读取Redis Registration策略的周期 |
| `registration.selector.scan_page_size` | `count` | 256 | 1 | 1024 | `default` | 一次HSCAN请求的目标条目数 |
| `registration.selector.max_pending_entries` | `count` | 4096 | 1 | 65536 | `default` | 同步期间最多保留的待处理UUID数 |
| `registration.selector.max_pending_bytes` | `bytes` | 67108864 | 1 | 1073741824 | `default` | 同步期间待处理事件的最大估算字节数 |
| `registration.selector.view_publish_interval_ms` | `duration_ms` | 10 | 0 | 1000 | `value` | 不可变视图两次发布之间的最短间隔 |
| `registration.selector.sync_timeout_ms` | `duration_ms` | 30000 | 100 | 3600000 | `default` | 一代完整或定向同步的总等待上限 |
| `registration.selector.max_active_bytes` | `bytes` | 268435456 | 1 | 1073741824 | `default` | 活动候选视图的最大估算字节数 |
| `registration.selector.max_retained_bytes` | `bytes` | 67108864 | 0 | 1073741824 | `value` | 不可选择 retained 视图的最大估算字节数 |
| `registration.selector.clock_refresh_interval_ms` | `duration_ms` | 30000 | 1000 | 3600000 | `default` | 连接级RedisClock的周期校准间隔 |
| `registration.selector.clock_uncertainty_ms` | `duration_ms` | 1 | 0 | 1000 | `value` | RedisClock样本额外加入的保守误差 |
| `registration.selector.error_buffer_capacity` | `count` | 16 | 1 | 1024 | `default` | Selector异步诊断缓冲容量 |
| `registration.selector.recovery.initial_delay_ms` | `duration_ms` | 100 | 10 | 5000 | `default` | Selector首轮恢复重试的基础延迟 |
| `registration.selector.recovery.max_delay_ms` | `duration_ms` | 5000 | 100 | 30000 | `default` | Selector恢复重试的最大延迟 |
| `registration.selector.recovery.multiplier` | `multiplier` | 2 | 1 | 8 | `default` | Selector恢复延迟的指数增长倍数 |
| `registration.selector.recovery.jitter_percent` | `percent` | 50 | 0 | 50 | `value` | Selector恢复延迟的随机抖动百分比 |
| `catalog.sync_timeout_ms` | `duration_ms` | 30000 | 100 | 3600000 | `default` | Catalog初始同步或修复的总等待上限 |
| `catalog.scan_page_size` | `count` | 256 | 1 | 4096 | `default` | Catalog一次索引扫描的目标条目数 |
| `catalog.max_inflight_reads` | `count` | 32 | 1 | 256 | `default` | Catalog同步允许并行的权威读取数 |
| `catalog.event_buffer_capacity` | `count` | 256 | 1 | 65536 | `default` | Catalog订阅事件交接缓冲容量 |
| `catalog.error_buffer_capacity` | `count` | 64 | 1 | 4096 | `default` | Catalog异步诊断缓冲容量 |
| `catalog.max_view_bytes` | `bytes` | 0 | 0 | 68719476736 | `value` | Catalog内存视图上限，零表示不额外限制 |
| `catalog.max_record_bytes` | `bytes` | 524288 | 1 | 4194304 | `default` | 单条Catalog完整结构化值的最大编码字节数 |
| `catalog.recovery.initial_delay_ms` | `duration_ms` | 250 | 10 | 5000 | `default` | Catalog首轮恢复重试的基础延迟 |
| `catalog.recovery.max_delay_ms` | `duration_ms` | 5000 | 100 | 30000 | `default` | Catalog恢复重试的最大延迟 |
| `catalog.recovery.multiplier` | `multiplier` | 2 | 1 | 8 | `default` | Catalog恢复延迟的指数增长倍数 |
| `catalog.recovery.jitter_percent` | `percent` | 10 | 0 | 50 | `value` | Catalog恢复延迟的随机抖动百分比 |
