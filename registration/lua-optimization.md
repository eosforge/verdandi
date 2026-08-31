# Registration Lua line-optimization review

Status: accepted, line-audited, and fully requalified on Redis 8.8.0. Date:
2026-08-24.

2026-08-28 maintenance note: the optimization and executable statements below
remain current. Temporary detailed Chinese comments increase generated source
sizes to Register 4,480, Update 4,975, Renew 3,314, and Unregister 1,343 bytes
(14,112 total) and therefore change script SHAs. EVALSHA still sends only the
SHA and arguments, and Redis executes the same statements. Generator freshness,
independent NOSCRIPT reload, the Registration Lua contract, short 500-live load,
and Sentinel recovery pass on the regenerated artifacts. The original table is
retained as the measured compact-source baseline rather than rewritten as if
the historical evidence used the new comments.

## 1. Outcome

Verdandi now uses four generated, operation-specific Registration Lua programs
whose control arguments are identified by fixed position rather than repeated
field names. Dynamic Attr and Data still use explicit field/value pairs because
their names are application schema data.

The accepted implementation also:

- writes the Registration Hash directly from `ARGV` without constructing a
  second request map or payload table;
- uses numeric Lua values directly for Redis-generated timestamps and
  deadlines;
- combines Registry membership value and field expiry with Redis 8 `HSETEX
  PXAT`;
- inlines every one-call-site state, clock, deadline, expiry, and fixed error
  block so successful calls create no Lua helper closures;
- binds `ARGV` once only in Register and Update, which traverse dynamic tails,
  and uses an explicit next-write index rather than repeated `#event`;
- gives Renew and Unregister operation-specific bindings so they do not
  initialize locals needed only by wider actions;
- skips `DEL` when Register's existing-state read proves both known Meta fields
  absent, while retaining replacement for every valid existing record; and
- enforces Redis 8's Hash-field absolute-expiry ceiling of `2^46-1` in both
  SDKs and again in Lua after the authoritative Redis timestamp is known.

One mutation still performs one `EVALSHA` and atomically joins current-state
checks, Redis time, the Registration Hash, Registry membership, matched expiry,
the reply, and the Pub/Sub event. No validation or application decoding moved
from the SDK into Lua.

The four canonical generated files now total 11,278 UTF-8 bytes. This is 23.61%
smaller than the previous positional production set of 14,763 bytes and 43.46%
smaller than the pre-positional 19,948-byte set:

| Operation | Bytes | Lines |
| --- | ---: | ---: |
| Register | 3,542 | 109 |
| Update | 3,955 | 131 |
| Renew | 2,771 | 86 |
| Unregister | 1,010 | 32 |

The sizes include license, generated-file, exact ABI, and fragment-boundary
comments. Redis caches each complete source by SHA; after `SCRIPT LOAD`, an
ordinary call sends the SHA and arguments, not the Lua body.

## 2. Exact production ABI

Every operation receives exactly two keys:

```text
KEYS[1] = verdandi:registration:<zone>:<type>:<uuid>
KEYS[2] = verdandi:registry:<zone>:<type>
```

The selected SHA already identifies protocol `v1` and the operation. Repeating
`&protocol`, `v1`, `&kind`, or fixed Meta field names in every request would
only make Lua rebuild information that the SDK and script dispatch already
know. Protocol `v1` therefore freezes these value slots:

### Register

```text
ARGV[1] = 7f4a0d92c8154e7f8a31b922d314ae60  # UUID
ARGV[2] = 42                                # content revision
ARGV[3] = 30000                             # immutable TTL in milliseconds
ARGV[4] = 12                                # positive application version
ARGV[5] = .build                            # first Attr field name
ARGV[6] = <binary>                          # first Attr value
ARGV[7] = .region
ARGV[8] = <binary>
ARGV[9] = address                           # first Data field name
ARGV[10] = <binary>
ARGV[11] = load
ARGV[12] = <binary>
```

Slots 1-4 are fixed values. Slots 5 onward are the complete SDK-validated Attr
sequence followed by the complete Data sequence, both in canonical bytewise
field-name order.

### Update

```text
ARGV[1] = 7f4a0d92c8154e7f8a31b922d314ae60  # UUID
ARGV[2] = 43                                # next content revision
ARGV[3] = ""                                # no version change
ARGV[4] = load                              # changed Data field name
ARGV[5] = <binary>                          # changed Data value
```

`ARGV[3]` is always present. It is the new positive version or the empty bulk
string when version is unchanged. Slots 4 onward are changed Data field/value
pairs in canonical order. A typed zero, false, empty, or encoded null is a real
replacement value; fixed-structure Data has no unset operation.

### Renew

```text
ARGV[1] = 7f4a0d92c8154e7f8a31b922d314ae60  # UUID
ARGV[2] = 43                                # unchanged content revision
```

### Unregister

```text
ARGV[1] = 7f4a0d92c8154e7f8a31b922d314ae60  # UUID
```

Replies and Pub/Sub events intentionally retain named alternating key/value
arrays. They cross a component boundary and must be self-describing to future
SDK decoders. Only the private SDK-to-known-SHA request ABI is positional.

A future incompatible control layout must use a new generated script/SHA and
protocol layout. It must not silently reinterpret these v1 positions. Raw
direct callers that bypass an authorized SDK remain outside the supported ACL
trust boundary.

## 3. Block-by-block implementation audit

The maintained fragments are the review unit. Generated Lua contains explicit
fragment boundaries so a production stack trace or source listing still maps
back to one maintained file.

| Block | Final treatment | Runtime reason | Correctness boundary |
| --- | --- | --- | --- |
| `redis.call` | bind once per operation | every script issues multiple Redis commands | no semantic change |
| `cmsgpack.pack` | direct one-call-site access | a local binding would repeat the same lookup during initialization | event bytes are unchanged |
| `KEYS[1]`, `KEYS[2]` | bind once to locals | both are used repeatedly | keys stay explicit for Redis routing |
| `ARGV` table | bind only in Register/Update | their dynamic tail rereads the table; Renew/Unregister are too small to benefit | exact positional ABI is unchanged |
| generic `ARGV` key/value map | removed | avoided a table, one insertion per pair, and later string-key lookups | fixed v1 slots are generated by both SDKs |
| protocol/kind request fields | removed | selected SHA already carries both facts | events and replies remain self-describing |
| UUID/revision/TTL/version lookup | direct fixed slots | array access is cheaper than Hash lookup | SDK validates type/range before I/O |
| `tonumber` | only where arithmetic/comparison or numeric MessagePack is required | avoids unnecessary conversions | corrupt stored scalars are still detected |
| `TIME` | retained | Redis is the authoritative wall clock | prevents SDK/Redis clock skew from deciding expiry |
| monotonic timestamp | retained | guards Redis clock rollback | uses `max(TIME, stored @timestamp)` |
| deadline check | retained | final timestamp exists only inside Redis | rejects a deadline above `2^46-1` |
| fixed-point number formatting | removed | numeric Redis arguments are exact in the accepted range | boundary vectors cover max and max+1 |
| fixed replies and errors | inlined | removes the per-call helper closure and all dynamic `#reply` appends | named alternating layouts remain unchanged |
| state helper | inlined `HMGET` decoding | removes a per-call closure and return dispatch | still distinguishes missing from corrupt state |
| clock/deadline/expiry helpers | inlined | each specialized executable has one call site | Redis time, rollback fence, ceiling, and matched TTL remain intact |
| Register event | one table, pair loop with one next-write index | avoids repeated `#event` and one addition per pair | event remains self-contained |
| Update event | one table, optional version, pair loop with one next-write index | publishes changed values without a read-back | empty version slot is not emitted |
| Renew event | fixed literal table | no Attr/Data payload exists | carries unchanged revision and new timestamp |
| Register write table | removed | `HSET ... unpack(ARGV, 5)` avoids a second payload copy | preceding `DEL` preserves full replacement |
| Update write table | removed | `HSET ... unpack(ARGV, 4)` keeps work proportional to patch | separate branches avoid inserting empty version |
| Registration `PEXPIREAT` | retained | the complete Hash key needs its own TTL | uses the same absolute deadline as membership |
| Registry `HSET` + `HPEXPIREAT` | replaced by one `HSETEX ... PXAT` | one native command writes value and field TTL | Redis 8 is the minimum server version |
| publish wrapper | inlined | removes a one-call-site layer | `cmsgpack.pack` and `PUBLISH` remain required |
| `DEL` in Register | conditional on known prior Meta presence | avoids one Redis command for first/recovery creation | valid existing records still receive complete replacement |
| `HMGET` state reads | retained | stale/idempotent/transition decisions require current Redis state | no `HGETALL` appears on Update/Renew |

### Register hot body

Register parses four control values, reads only the current revision/timestamp,
derives Redis time and deadline, builds one complete event, replaces a present Hash,
sets both expiries, publishes, and returns. The application tail is traversed
once for the event and passed directly to Redis for storage. A new or
Redis-loss recovery Register does not issue an empty-key `DEL`.

### Update hot body

Update reads only revision/timestamp/TTL. Equal revision returns immediately
without changing timestamp, TTL, or publication. The next revision writes only
`@revision`, `@timestamp`, optional `@version`, and changed Data. It never
reconstructs the complete Registration or reads Zone configuration.

### Renew hot body

Renew reads the same three Redis-owned scalars, requires the exact current
revision, writes only `@timestamp`, refreshes both expiries, rewrites the same
membership revision to repair a missing field, and publishes the compact Renew
event. Content revision does not advance.

### Unregister hot body

Unregister has no clock, state-read, or reply helper. It deletes the target
Hash and membership field; it publishes only when the Hash existed and always
returns success. A retry is therefore terminal and idempotent.

## 4. Measurements that selected the changes

The pre-promotion candidate matrix is retained in
[`testkit/results/lua-register-line-optimization-20260824.json`](../testkit/results/lua-register-line-optimization-20260824.json).
It used Redis 8.8.0, disabled persistence, no subscribers, eleven alternating
trials, and a non-transactional client pipeline. Each trial executed 2,000
fresh small Registrations and 500 fresh default-maximum Registrations.

| Candidate | Parent | Small us | Paired / wins | 16+32 us | Paired / wins |
| --- | --- | ---: | ---: | ---: | ---: |
| Baseline | - | 14.23 | - | 51.83 | - |
| Inline pair append | Baseline | 13.96 | +0.73%, 6/11 | 51.00 | +1.40%, 7/11 |
| Numeric Redis arguments only | Baseline | 13.40 | +6.10%, 10/11 | 51.34 | +0.06%, 6/11 |
| `HSETEX` only | Baseline | 14.04 | -0.22%, 5/11 | 51.09 | +1.21%, 6/11 |
| Fixed controls/application range | Inline pairs | 12.38 | +11.17%, 11/11 | 43.11 | +14.63%, 11/11 |
| Direct `HSET` from `ARGV` | Fixed controls | 11.86 | +3.48%, 10/11 | 40.63 | +5.54%, 10/11 |
| Full candidate except `HSETEX` | Direct `HSET` | 10.55 | +12.20%, 11/11 | 39.41 | +4.38%, 10/11 |
| Full candidate with `HSETEX` | Full candidate | 10.19 | +2.51%, 10/11 | 38.70 | +1.20%, 7/11 |

The final candidate improved paired Redis server time by 28.68% for 2 Attr + 2
Data and 25.40% for the default 16 Attr + 32 Data with 128-byte values. All
eleven pairs improved. Isolated success-reply, publication, local-call, and
`HSETEX` changes were near noise; they were accepted only in the already-faster,
smaller composite.

That raw matrix is historical promotion evidence. With the optimized generated
source present, `registration_line_benchmark.py` now measures the current
Register directly. An old body is compared only when its already-cached SHA and
source size are supplied; the benchmark never reconstructs superseded source.

### Second line-audit pass

The second pass retained the old production SHAs in the same Redis script
cache, loaded each candidate under a new SHA, alternated execution order, and
compared Redis server time by trial. The final cumulative comparison uses 21
paired trials. Register executes 4,000 small or 1,000 default-maximum fresh
records per variant. Update/Renew executes 5,000 calls per phase and trial,
including a 31-Data-field paired Update shape. A corrected canonical
32-field current-source run is recorded separately below.

The accepted changes and their isolated evidence were:

| Candidate | Relevant shape | Paired server improvement | Positive trials |
| --- | --- | ---: | ---: |
| Inline all one-call-site helpers and fixed errors | small Register | +4.07% | 10/11 |
| Inline all one-call-site helpers and fixed errors | one-field Update | +9.36% | 11/11 |
| Inline all one-call-site helpers and fixed errors | Renew | +6.89% | 11/11 |
| Use a next-write event index | default-maximum Register | +1.83% | 9/11 |
| Bind `ARGV` for tail-traversing scripts | default-maximum Register | +2.30% | 8/11 |
| Bind `ARGV` for tail-traversing scripts | 31-field Update | +2.27% | 10/11 |
| Skip absent-key Register `DEL` | small Register | +2.20% | 11/11 |
| Specialize Unregister bindings | Unregister | +0.62% | 11/21 |

Unregister is classified as server-time neutral rather than faster. Its
specialized source is retained because it removes unused initialization,
shrinks the executable, and does not regress the median. The final cumulative
comparison against the previous production scripts is:

| Operation | Shape | Previous | Final | Paired improvement | Positive trials |
| --- | --- | ---: | ---: | ---: | ---: |
| Register | 2 Attr + 2 Data | 9.60 us | 8.69 us | +9.03% | 21/21 |
| Register | 16 Attr + 32 Data, 128 B | 39.00 us | 35.57 us | +7.65% | 20/21 |
| Update | one Data field | 9.05 us | 8.43 us | +6.66% | 21/21 |
| Update | version + one Data field | 9.33 us | 8.73 us | +6.18% | 21/21 |
| Update | 31 Data fields | 19.86 us | 18.28 us | +7.12% | 21/21 |
| Renew | unchanged content | 8.43 us | 7.82 us | +7.25% | 21/21 |
| Unregister | existing record | 3.72 us | 3.72 us | -0.27% | 9/21 |

The audit also measured and rejected seemingly attractive rewrites:

- modulo-based TIME truncation was -0.21% for Update, -0.51% for versioned
  Update, and +0.11% for Renew;
- removing the repeated `tonumber` local was neutral for Renew and -0.31% for
  versioned Update;
- arithmetic request coercion, absent-state short-circuit conversion, and a
  local `KEYS` table failed the consistency/win-count gate;
- caching the two version-presence comparisons in a `has_version` local was
  -0.23% for one-field Update and -0.16% for 32-field Update by median server
  time, with inconsistent paired direction, so the direct comparisons remain;
  and
- short string literals such as `"@revision"` remain direct constants. Lua
  already stores them in the compiled function's constant table, whereas a
  per-invocation local would consume initialization and a register.

Raw accepted, rejected, and final trials are retained under
`testkit/results/lua-*-20260824.json`. The final authoritative comparisons are
[`lua-register-line-final-20260824.json`](../testkit/results/lua-register-line-final-20260824.json)
and
[`lua-registration-line-final-20260824.json`](../testkit/results/lua-registration-line-final-20260824.json).
The rejected version-predicate trial is retained in
[`lua-registration-has-version-20260824.json`](../testkit/results/lua-registration-has-version-20260824.json).

## 5. Current absolute Redis-body cost

Fresh post-promotion results are retained in:

- [`testkit/results/lua-register-line-final-20260824.json`](../testkit/results/lua-register-line-final-20260824.json);
- [`testkit/results/lua-registration-line-final-20260824.json`](../testkit/results/lua-registration-line-final-20260824.json);
- [`testkit/results/lua-registration-hot-path-final-20260824.json`](../testkit/results/lua-registration-hot-path-final-20260824.json).

The same isolated Redis 8.8 fixture measured:

| Production operation | Shape | Median Redis time | Median wall rate |
| --- | --- | ---: | ---: |
| Register | 2 Attr + 2 Data | 8.69 us/call | 49,877.36/s |
| Register | 16 Attr + 32 Data, 128 B each | 35.57 us/call | 7,364.20/s |
| Update | one Data field | 8.43 us/call | 49,803.82/s |
| Update | version plus one Data field | 8.73 us/call | 49,802.33/s |
| Update | canonical 32 Data fields | 19.74 us/call | 15,813.84/s |
| Renew | no content | 7.82 us/call | 60,092.47/s |
| Unregister | existing record | 3.72 us/call | 93,566.38/s |

All rows except canonical 32-field Update are the final side of the 21-pair
comparison. The 32-field row is the corrected current-source-only run; it is
not assigned an old-SHA improvement percentage.

Server time is the optimization metric because Lua executes on Redis's main
thread. Wall rate also includes client encoding, network, pipeline scheduling,
and host scheduling. These isolated no-subscriber results must not be compared
directly with paced end-to-end SDK loads with eight Selectors.

At 500 one-field Updates per second, 8.43 microseconds means about 4.22
milliseconds of Redis script-body time per wall-clock second in this fixture.
It does not mean an Update takes 20+ microseconds in every deployment, nor does
it include subscriber fan-out, persistence, or application work.

## 6. Hash-field expiry correctness

Redis 8 reserves two bits from its internal 48-bit expiration representation
for Hash fields. The public absolute-millisecond maximum is:

```text
HFE_MAX_ABS_TIME_MSEC = 2^46 - 1 = 70,368,744,177,663
```

Redis 8.8 accepted that exact value for both `HPEXPIREAT` and `HSETEX PXAT` and
rejected the value plus one. The larger Lua safe integer
`9,007,199,254,740,991` remains exact when stored in a Hash and is accepted by
key-level `PEXPIREAT`, but is not a valid Hash-field expiry.

Go and Rust reject a TTL outside the Hash-field representable range before
I/O. Event/record validation rejects `timestamp + ttl` overflow or a deadline
above the ceiling. Lua repeats the final absolute-deadline check because only
Redis knows the authoritative timestamp used by the atomic write. This is not
duplicate schema validation; it is a required Redis-state-dependent command
precondition.

The ceiling still allows a finite Registration deadline more than two thousand
years after the Unix epoch. Persistent Catalog records should use no TTL rather
than synthesize a huge Registration lease.

## 7. Maintenance model

Maintainers edit only fragments under `lua/src/registration` and the explicit
manifest. The deterministic generator emits canonical LF scripts and
byte-identical Go/Rust copies:

```text
python testkit/lua/generate_registration.py
python testkit/lua/generate_registration.py --check
```

One feature does not automatically mean one fragment or one Lua function.
Shared fragments exist only for behavior reused by multiple operations. A
one-call-site fixed success reply or publication remains inline when that keeps
the generated hot path smaller. Each generated file is independently loaded,
cached, reloaded after `NOSCRIPT`, reviewed, and tested.

## 8. Qualification result

After promotion, the following all passed:

- canonical Lua contract and per-operation `SCRIPT FLUSH` reload on Redis 8.8;
- exact ceiling, ceiling-plus-one, clock rollback, matched key/field TTL, stale,
  idempotent, transition, missing, corrupt, and raw SDK-bypass vectors;
- Go unit tests, generation freshness, `vet`, shuffled repetition, WSL/Linux
  real-Redis race, and a 60-second 25,420,541-execution decoder fuzz run;
- Rust 31 unit tests, all-target tests, formatter, Clippy with warnings denied,
  and documentation build;
- Go and Rust standalone integration plus bidirectional live Pub/Sub
  interoperability;
- four fresh five-minute 500-writer Update/Renew phases with eight Selectors;
  Go/Rust Update completed 150,000 each at 500.0/s, Renew completed 149,597 at
  498.7/s and 148,610 at 495.4/s, and 5,000-record synchronization completed in
  56.489 ms and 322.066 ms; and
- three Redis nodes plus three Sentinels across two promotions, acknowledged
  write loss and full-state repair, `SCRIPT FLUSH`, complete Sentinel outage,
  and Go/Rust convergence in 152.842 seconds; and
- a Redis-clock-gated 7,263.649-second endurance run with 500 writers, eight
  Selectors, 3,750,000 Updates, 25 expiry cycles, 25 explicit churn cycles,
  34/34 injected faults, zero unexpected errors, exact final convergence, and
  goroutines returning from a 1,541 peak to the baseline of two.

The final standalone database was empty. The remote test containers, temporary
directories, and dedicated ports were absent after cleanup.

## 9. Assessment

Accepted-contract implementation score for the Registration Lua layer:
**10/10**. Production-evidence confidence is **9.9/10**. The first score
means every currently approved Lua responsibility is implemented, line-audited,
measured, and regression-qualified; it does not claim that all future Redis
versions, deployment shapes, or workloads have been exhausted.

Strengths:

- one bounded atomic Redis execution per lifecycle mutation;
- the SDK/Lua ownership boundary is explicit and tested;
- fixed control positions remove recurring parsing/allocation work;
- partial Update cost is independent of the complete Registration size;
- matched key and membership expiry uses one Redis-owned deadline;
- generated specialization keeps reviewable sources without runtime dispatch;
- successful hot paths create no Lua helper closures or generic reply builders;
- repeated string literals remain compiled constants instead of per-call locals;
- Go and Rust embed byte-identical canonical scripts; and
- measured gains were followed by complete functional, load, race, fuzz, and
  failover qualification.

Trade-offs and remaining optimization space:

- positional v1 controls cannot be reordered; an incompatible layout needs a
  new script/SHA;
- Register must still copy application pairs into its complete Pub/Sub event
  and must `DEL` before replacement of a present valid record;
- Update/Renew still require Redis `TIME`, state `HMGET`, MessagePack encoding,
  and publication because those are protocol invariants;
- Pub/Sub fan-out cost grows with subscribers; eight-subscriber sustained tests
  pass, but the paired Lua microbenchmark deliberately isolates no-subscriber
  script-body cost;
- only Redis 8.8 supplied the line-by-line timing and endurance evidence;
  arbitrary Redis clock jumps, TLS, managed Redis, wider fan-out, sustained
  maximum payloads, and multi-day operation remain wider production-
  qualification work; and
- further micro-optimization should require a same-fixture paired result and
  must not weaken revision, time, expiry, atomicity, or readable event/reply
  contracts.

There are no unresolved decisions blocking this Registration Lua promotion.
