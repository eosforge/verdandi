# Astra

[English](README.md) | [简体中文](README_CN.md)

Astra is a distributed service-discovery and state-synchronization project. Its intended independent repository is `github.com/eosforge/astra`; this Astra directory currently remains in the Verdandi workspace.
Stars exchange source state within a Galaxy; Pulsar provides admission and continuous time; Polaris owns authoritative data; Astrolabe and Admin provide management.

The implementation includes Almanac, Catalog, Ephemeris, the Comet C++ SDK, atomic multi-key commits, and multi-Star recovery. Servers target Linux/GCC 16.2 with C++26; Comet C++ uses C++23 on Linux/GCC and Windows/MSVC. Comet Go, Moon, and full Planet capabilities remain incomplete. Source availability, earlier regression passes, and release readiness are distinct; consult the source identity and limits in [current validation](docs/validation.md).

## Getting started

Run from this repository root, using existing tools and dependencies:

```bash
bash build.sh build --profile release
```

Tools/dependencies default to `build/tools` and `build/deps`; new compilation output goes to `build/cmake/<profile>`, raw reports to `build/results`. Exclude `build/` from Git/source releases. Missing dependencies fail explicitly without downloads.

A transitional workspace may explicitly set `ASTRA_CACHE_ROOT` to reuse previous tool/dependency caches. New output still belongs under this repository's `build/`; do not reuse old CMakeCache files or count old results as validation of new paths. See the [build guide](docs/build.md). Tests and prerequisite builds require [project authorization](AGENTS.md).

## Layout

| Path | Responsibility |
| --- | --- |
| `common/`, `star/`, `pulsar/`, `polaris/`, `astrolabe/` | Shared foundations and services |
| `comet/` | Native SDKs; C++ supports independent configuration/installation |
| `admin/` | Management frontend |
| `proto/`, `internal/generated/` | Protocol inputs and generated Go; generated C++ is in `common/src/generated/` |
| `tools/` | Builds, protocol generation, dependency preparation, offline diagnostics |
| `tests/` | Shared identities, cross-process scenarios, Python cases; component units stay in components |
| `bench/` | Independent performance workloads; Redis comparisons explicitly select an external legacy SDK |
| `docs/` | Current design, usage, and the single current validation record |
| `licenses/` | Original third-party licenses/NOTICEs matched to dependency versions |

Start with [documentation](docs/README.md), [maintenance](docs/project.md), [Comet C++ APIs](comet/cpp/README.md), and [tests](tests/README.md).
Ordinary builds do not depend on the parent directory, legacy Redis SDK, or obsolete Rust services. `planet/` still contains policy code used by CMake; `moon/` has no implementation, and neither implies full feature delivery.

## License

Owned Astra code is [MIT](LICENSE). Third-party code, runtimes, and notices retain their upstream licenses; see [third-party notices](licenses/README.md). Project MIT does not override upstream terms.
