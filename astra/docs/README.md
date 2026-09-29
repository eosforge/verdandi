# Current Documentation

[English](README.md) | [简体中文](README_CN.md)

This directory and component READMEs describe current effective content. Git retains historical designs, audit rounds, and old reports; no workspace archive is maintained.

Source includes Almanac, Ephemeris, Catalog, the control plane, current Comet C++ APIs, and multi-Star recovery; see [architecture/status](architecture.md). Do not base new work on the old Catalog/Registry split, Grant, architectural standalone, or an Astrolabe database. Implementation, execution, and static review are distinct; new changes do not inherit old test results.

| Document | Responsibility |
| --- | --- |
| [AGENTS.md](../AGENTS.md) | Authorization, tools, test concurrency |
| [Development and maintenance](development.md) | Cross-language standards, coverage, trusted evidence, rule IDs, project adoption, references |
| [Coding and file organization](coding.md) | Sole coding/organization standard, with C++, C, Go, Rust, Python, JS/TS, web, Java/Kotlin, C#, Lua, SQL, Shell, protocol/configuration sections |
| [Development gate and pilot](features/verification.md) | Formal schemas, commands/authorization, gate decisions, Polaris storage pilot; results recorded separately |
| [Visual Studio and Windows](features/windows.md) | Solution/project outputs, standard/platform boundaries, server-port contracts; IDE/native adaptation implemented, full services still blocked |
| [Architecture](architecture.md) | Topology, component boundaries, implementation status, outstanding acceptance |
| [Protocol](../proto/README.md) | Schemas, identity/signatures, domain/source recovery contracts |
| [Build/run](build.md) / [maintenance](project.md) | Languages/tools, builds/startup, file responsibilities/lifecycles, Admin constraints |
| [Storage](../common/README.md) | Native records, source groups, projections, commits, shared primitives and design rationale |
| [Pulsar](../pulsar/README.md) | Admission, readonly directory, SQLite membership, reference/continuous time |
| [Polaris](../polaris/README.md) | Go/GORM official SQLite, WAL durability, bounded history, Star recovery |
| [Admin](../admin/README.md) | Real management, independent demo galaxy, frontend development; complete Orrery deferred |
| [Astrolabe](../astrolabe/README.md) | Go management/observation, deployment accounts, Polaris commits/credentials |
| [Comet C++](../comet/cpp/README.md) | APIs, ownership, recovery, timing, static delivery, lifecycle rationale |
| [Comet Go](../comet/go/README.md) | Future native SDK scope, not a C++ prerequisite |
| [Comet acceptance](comet.md) | Business scenarios, expectations, case mapping; execution in validation |
| [Test methods](../tests/README.md) / [current validation](validation.md) | Methods and actual outcomes, maintained separately |
| [Performance workloads](../bench/README.md) | Bounded offline domain/Comet/Polaris measurements; outcomes in validation |
| [Probes and performance analysis](profile.md) | C++ switches, sampling costs, source sites, offline interpretation, measured optimization candidates |
| [Three-Star soak](soak.md) | Real service fault rotation, continuous consistency/resources/cleanup |

## Current SDK design entry

Confirmed Beacon, Observer, Publisher, Subscriber, and Reader interfaces/lifecycles live in [Comet C++ APIs](../comet/cpp/README.md#c-public-api). [Go mapping](../comet/go/README.md) remains future implementation scope.

[Protocol boundaries](../proto/README.md#confirmed-targets-and-implementation-gaps) describe wired Catalog version queries, stable Beacon identity/Update renewal, and reads. [Architecture status](architecture.md#status) distinguishes source completion from execution. Only validation records actual runs.

## Maintenance conventions

- Define each fact in its owning document; link elsewhere. GEMINI.md links common instructions/navigation, not copied policy/session logs.
- Distinguish implemented, confirmed but unimplemented, and undecided. Schema drafts/connectivity passes do not prove business synchronization.
- Update stable designs with implementation changes. Maintain docs/validation.md with source identity, platform, scope, failures, and limits, without new dated reports.
- Without reruns retain original dates/source identity, not claims that the current workspace passed. Keep necessary repaired-defect context; Git holds earlier complete history.
- Ignored build/ holds temporary analysis, raw logs, and one-off scripts; no growing worklog.md as policy.
- Markdown cleanup does not delete protocol vectors, fixtures, licenses, or structured raw evidence.
- Paths/links are documentation contracts. Update references on removal; do not leave obsolete redirects claiming authority.

- Language policy: development.md and coding.md contain complete English first, then matching Chinese. Each English README.md links its same-directory README_CN.md, which links back. Other owned Markdown is English. Preserve upstream license text verbatim. Content, path, and heading changes update both language presentations and their links together.
