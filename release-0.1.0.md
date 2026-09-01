# Verdandi 0.1.0 Alpha Release Contract

## 1. Status

Version `0.1.0` is the first non-production Verdandi Alpha release line. It is
intended for distributed SDK development and controlled integration of the
current APIs into services. It is not a production-readiness, availability,
stable ABI, or stable wire-compatibility promise.

The runtime and protocol evidence is anchored to frozen commit
`33193a3c0e14a2d9c2e11565a05642f7e7cafc33`. The `0.1.0` preparation changes
package versions, the canonical repository/import identity, documentation, and
post-freeze evidence only; it must not silently change Redis behavior or public
API semantics. A commit, tag, package publication, or GitHub Release requires a
separate explicit maintainer instruction.

## 2. Implemented release surface

`0.1.0` includes:

- the shared strict JSON configuration model and Redis Standalone/Sentinel
  transport configuration;
- root Redis Client, bounded Key, and typed Hash operations;
- Registration `register`, `update`, `renew`, and `unregister` lifecycle;
- Registry synchronization, retained expiry recovery, and local transactional
  Selector views;
- persistent Catalog Replace, exact-base Patch, Delete, revision/floor repair,
  Subscriber views, and optional local checkpoints;
- Go and Rust native SDKs;
- the C++23 compiled SDK, C ABI v1, and C++11/14/17 legacy facade; and
- the managed .NET 8/10 C# facade over the same C++ core.

Redis Cluster, Campaign/Leader, desired configuration, acknowledgements, and
Command are not implemented release surfaces. C++/C# platform packages, TLS
qualification, NativeAOT/trimming, and stable binary ABI guarantees also remain
outside `0.1.0`.

## 3. Compatibility policy

- All source, documentation, and package metadata use the canonical repository
  identity `github.com/eosforge/verdandi`.
- `0.1.0` consumers must pin the exact SDK release. A later `0.x` minor release
  may contain coordinated breaking API, storage, or wire changes.
- Mixed `0.x` versions in one Redis deployment are supported only when a later
  release explicitly documents that compatibility.
- Protocol marker `1.0` remains experimental until the stable SDK `1.0.0`
  contract is declared complete.
- The first stable `1.0.0` release must include qualified Leader election and
  concise standard English production-source comments, plus the complete
  stable release matrix in [`alpha.md`](alpha.md).

## 4. Frozen-source qualification

The exact frozen runtime passed the complete short regression recorded in
[`freeze-20260831.md`](freeze-20260831.md), followed by two concurrent detached
twelve-hour Redis 8.8 fault campaigns:

| Domain | Redis elapsed | Work | Planned faults | Result |
| --- | ---: | ---: | ---: | --- |
| Registration/Selector | 43,213.948 s | 21,600,000 Updates and 3,707,005 selections | 214/214 | pass |
| Catalog | 43,201.858 s | 5,932,160 mutation attempts | 113/113 | pass |

Both campaigns:

- exceeded the 43,200-second Redis `TIME` acceptance floor;
- matched independently recomputed frozen-source fingerprints;
- completed every pre/post contract and interoperability check;
- recorded 1,441 monotonic JSONL Redis samples with no sampling failure;
- reported no unexpected asynchronous error, eviction, rejected connection,
  or sustained memory-growth violation; and
- ended at `DBSIZE=0`, removed both owned containers and data directories, and
  closed both dedicated ports.

Machine-readable final results are retained in:

- [`testkit/results/registration-soak-12h-freeze-20260901.json`](testkit/results/registration-soak-12h-freeze-20260901.json)
- [`testkit/results/catalog-soak-12h-freeze-20260901.json`](testkit/results/catalog-soak-12h-freeze-20260901.json)

## 5. Package and tag identities

When publication is explicitly authorized, one source commit may carry
language-specific release identities:

- root source/GitHub release: `v0.1.0`;
- Go subdirectory module: `sdk/go/v0.1.0` for module
  `github.com/eosforge/verdandi/sdk/go`;
- Rust crates: `verdandi-derive` `0.1.0`, then `verdandi` `0.1.0`;
- CMake project/source SDK: `0.1.0`; and
- C# assembly/source package metadata: `0.1.0`.

The current C++ and C# trees are source releases, not yet portable CMake or
NuGet binary packages. Publication must not claim artifacts that were not built
and qualified.

## 6. Stable 1.0.0 boundary

The `0.1.0` decision does not reduce the stable target. Verdandi `1.0.0` still
requires at least:

- complete, version-aware, fail-closed Leader election and Sentinel fencing;
- the remaining stable protocol and cross-version compatibility matrix;
- standard English production-source comments;
- supported-platform package/install artifacts and ABI checks; and
- final clean-clone, security, license, and release-artifact verification.
