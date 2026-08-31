# Registration/Selector Optimization Review

Date: 2026-08-29

## Result

The Registration/Selector implementation was reorganized around field
ownership that already existed in the public codec contract. Go typed selection
now avoids a second deep copy of freshly encoded result fields, releases reused
transaction tails when a view shrinks, and no longer retains duplicate overlay
scratch state. Rust now shares immutable remote overlay baselines through
`Arc<Fields>` and removes the always-allocated event-name duplicate tree plus
several avoidable MessagePack string copies. Lua was reviewed line by line and
left unchanged because no proposed edit had measured or structural value.

No public API, Redis key, MessagePack envelope, Lua executable, configuration
rule, or selection policy changed.

The current 104-file qualification fingerprint is
`2d3235af5a7a63049e4ba63c3a4fe2a933cd71ce829d753dbdfd9f1a89c8100b`.

## Go changes

### Transfer already-owned encoded fields into detached results

`Encoder.Encode` transfers a complete Fields map and its buffers to Verdandi.
After `selectedEntry` re-encodes a selected candidate to detect forbidden direct
mutation, those fields are already detached from the immutable Selector view.
The previous implementation discarded them, cloned the internal fields again,
and then decoded the clone for the return value.

`selectedEntry` now returns the newly owned Attr/Data Fields. `One` and `Any`
pass them directly to owned-field decoders. Internal view fields still take the
defensive cloning path; only fields freshly created under the Encoder ownership
contract use the transfer path.

The detached result is now fully decoded before overlay commit. Consequently,
an application Decoder failure cannot leave a committed local prediction while
the selection operation returns an error. `Any` also builds its returned slice
directly and no longer allocates a temporary selected-entry slice.

### Share immutable overlay state

Fields stored in Selector records and committed overlays are SDK-owned and
never mutated in place. A successful commit now shares the immutable remote
base map and the newly encoded staged map rather than deep-copying both.
Remote-overlay reconciliation copies only the map header/entries it needs to
change and shares immutable value buffers. Application decoders still receive
detached copies, so this does not expose internal buffers.

### Release reusable transaction references

When a view shrinks, the reused `selectionEntry` and `Candidate` backing arrays
previously kept removed records reachable in their unused tails. The current
implementation clears only the removed tail; stable-size views pay no extra
clear pass. Commit scratch is also cleared immediately after publication or a
decode failure, preventing the Selector transaction object from retaining a
second historical reference to overlay data.

New tests explicitly cover:

- detached result decode failure rolling back all staged overlays;
- commit scratch containing no published pointers after success;
- a shrunk view clearing removed transaction and Candidate tail references;
- existing direct borrowed mutation, duplicate Any choice, cancellation,
  detached result, and multi-overlay atomicity behavior.

## Rust changes

### Share overlay baselines through Arc

`SelectorRecord.data` was already an `Arc<Fields>` and immutable. Staged and
committed local overlays now retain an `Arc` clone for their remote comparison
baseline instead of cloning every field name and value on each mutation. On a
remote revision change, reconciliation swaps to the new record's Arc.

This is the Rust-native equivalent of Go's immutable base sharing: Rust uses
type-enforced shared ownership instead of reproducing Go's map convention.

### Reduce Registration event decoder allocation

The decoder previously inserted every envelope name into a `BTreeSet<String>`
after cloning it, including the seven fixed reserved names and every
application field. It now uses:

- one seven-bit mask for fixed reserved fields;
- an optional `HashSet` created only when an unknown control field occurs;
- the Attr/Data maps themselves for application-field duplicate detection;
- consuming conversion of already-owned `rmpv` strings/binary values;
- direct protocol-byte comparison without allocating a temporary String.

Duplicate fixed, unknown-control, Attr, and Data names remain rejected. The
bounded scalar/container rules and lifecycle-shape checks are unchanged and
their existing exhaustive marker/truncation tests still pass.

## Lua review

The four generated operation programs already use fixed positional arguments,
localized runtime-function bindings, direct Redis primitives, operation-
specific event construction, and no application-data parsing. Splitting or
adding another abstraction layer would not reduce Redis work and would enlarge
the generated glue.

No Lua line was changed. This is intentional: optimization requires evidence,
not code churn.

## Linux benchmark evidence

Environment: Go 1.27.0, Linux/amd64 under WSL2, Intel Core i7-13700F,
`GOMAXPROCS=24`, ten samples per benchmark, 300 ms minimum sample time.

| Benchmark | Baseline median | Current median | Raw change | Bytes/op | Allocs/op |
| --- | ---: | ---: | ---: | ---: | ---: |
| Typed Selector One, 500 candidates with one mutation | 12.184 us | 11.474 us | -5.83% | 3,882 -> 2,226 (-42.66%) | 43 -> 28 (-34.88%) |
| Typed Selector Any, 8 of 500 | 15.742 us | 12.833 us | -18.48% | 14,723 -> 8,067 (-45.21%) | 154 -> 97 (-37.01%) |
| Registration event decode control | 0.831 us | 0.808 us | -2.81% | 896 -> 896 | 12 -> 12 |

The unchanged event-decoder control shows a 2.81% session drift. Therefore the
accepted elapsed-time claims use the earlier immediate targeted comparison:
`One -3.20%` and `Any -16.90%`. Allocation counts and bytes are deterministic
across every sample and retain the exact reductions shown above. All ten old
and new One/Any latency samples were non-overlapping, but the conservative
numbers avoid attributing machine-wide drift to the code.

An attempted Go event-kind optimization reduced one 8-byte allocation but made
the decoder slower. It was rejected and reverted. Current Go event-decode code,
bytes, and allocations are unchanged from the accepted baseline.

Rust's Arc and event-name changes have clear ownership/allocation reductions,
but this repository does not yet have an allocator-counted Rust microbenchmark
harness. No numeric Rust speedup is claimed.

## Verification

Static and local checks passed:

- deterministic configuration and Registration Lua generation;
- Python testkit bytecode compilation;
- Go formatting, all packages, vet, and WSL/Linux race detection;
- Rust formatting, 52 library tests, four endpoint-free external tests,
  all-target/all-feature Clippy with `-D warnings`, and rustdoc with
  `-D warnings`;
- the localized Windows Rust import-library linker message remains an
  informational compiler warning that the strict lint flag does not govern.

The isolated Redis 8.8.0 functional matrix passed all 14 suites:

- Registration and Catalog Lua contracts;
- Go standalone integration and WSL/Linux race integration;
- six Rust Registration/Selector integration scenarios;
- Rust Catalog and both root Redis API scenarios;
- Go-to-Rust and Rust-to-Go live Registration/Catalog interoperability.

It processed 4,770 Redis commands, peaked at 2,968,704 Redis used-memory bytes,
left an empty database, and closed its owned port. Evidence is stored in
`testkit/results/registration-selector-optimization-functional-20260829.json`.

The separate three-Redis/three-Sentinel matrix passed in 39.608 seconds:

- primary movement `16381 -> 16382 -> 16383`;
- Go and Rust Selector generations `1 -> 2 -> 3`;
- total-Sentinel-loss unavailable views;
- Registration UUID preservation;
- acknowledged-loss full-state republish, Pub/Sub recovery, script reload,
  second promotion, and cross-language convergence.

All six Sentinel/Redis ports were closed after the run. Evidence is stored in
`testkit/results/registration-selector-optimization-sentinel-20260829.json`.

The prior accepted one-hour campaign used fingerprint
`38448c747230a72eb4d0b1a4ea838b83467a2b8d66d366909bbb1b73b6dd8f77`.
Because this optimization changes the current fingerprint, that hour remains
historical baseline evidence and is not relabelled as a one-hour run of the
current source. The current source has microbenchmark, full short functional,
race, and Sentinel evidence.

## Scores

### Lua: 10.0/10 within its glue-layer scope

Strengths:

- four small operation-specific deployed scripts generated from maintained
  fragments;
- atomic Hash, field-TTL, membership, and Pub/Sub mutation;
- fixed positional inputs, direct calls, and no duplicated SDK validation;
- repeated script flush, restart, standalone, and Sentinel evidence.

Scope limitations rather than implementation defects:

- Redis scripts execute on Redis's serialized command path;
- the implementation intentionally depends on Redis 8 Hash-field TTL and
  Redis's bundled MessagePack support.

### Go: 9.85/10

Strengths:

- explicit one-worker Registration and one-listener/optional-sync Selector
  ownership with deterministic close and race/leak evidence;
- single-slot Fields mailbox and bounded waiter admission;
- immutable projected views plus atomic, rollback-safe local prediction;
- current selection allocation reductions are measured on Linux and preserve
  public behavior;
- view shrink and commit scratch no longer retain stale pointer graphs.

Remaining deductions:

- Go cannot expose a const `*D`; detecting forbidden direct callback mutation
  still requires re-encoding every selected Attr/Data value. This is the main
  reason One remains at 28 allocations and Any(8) at 97 allocations;
- every selection still materializes an O(N) borrowed Candidate array and one
  Selector serializes policy callbacks by design;
- `selector_core.go` and `selector.go` remain large state-machine modules whose
  maintenance burden is higher than their public API suggests;
- the current fingerprint has not yet repeated the prior one-hour campaign.

### Rust: 9.8/10

Strengths:

- borrowed `CandidateRef` is immutable by the type system, avoiding Go's
  selected-value revalidation cost;
- Tokio cancellation/task ownership and Arc-backed immutable state are native
  Rust designs rather than source-shape copies of Go;
- overlay baseline sharing and event decoding now avoid structurally
  unnecessary clones while preserving bounded hostile-input behavior;
- standalone, interoperability, strict lint/doc, and two-promotion Sentinel
  evidence all pass.

Remaining deductions:

- no allocator-counted statistical Rust benchmark currently quantifies these
  structural allocation improvements;
- Registration event decoding still materializes `rmpv::Value` and uses
  `BTreeMap` Fields, so it is heavier than Go's specialized byte cursor;
- `selector.rs` remains a large module combining public selection, recovery,
  retained state, event application, and connection generation logic;
- the current fingerprint has not run a new hour-long Rust/Go campaign.

### Overall Registration/Selector slice: 9.8/10

The implementation is production-oriented and currently passes its short
functional, race, cross-language, and fault-topology gates. The remaining work
needed for a higher score is evidence and maintainability rather than a known
correctness failure: add a Rust allocation benchmark harness, split internal
Selector state-machine source without changing task ownership, and repeat a
long campaign only when the maintainer wants a new frozen release candidate.

No commit or push was created.
