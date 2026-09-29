# Project Working Agreements

## Standards

- At task start, read applicable [development and maintenance rules](docs/development.md) and [coding/file-organization rules](docs/coding.md). These are the only common normative texts; this file retains authorization/environment agreements.
- See [maintenance](docs/project.md) for languages, formatters, implementation constraints, and [Admin-specific rules](docs/project.md#admin). Navigate through [docs/README.md](docs/README.md).
- Read the owning maintenance guide's coding differences and derive effective rules under [project overrides](docs/coding.md#overrides). Unoverridden common rules still apply.

## C++ conventions

- Before C++ design, implementation, cleanup, or review, read/follow [coding.md's C++ section](docs/coding.md#cpp).
- These are confirmed user preferences and need not be restated each session. Naming, scope, performance/simplicity tradeoffs, comments, and formatting follow that section and registered project differences; other quality/authorization rules remain applicable.
- Rules cover handwritten production, tests, and templates. Do not edit generated/third-party code to enforce them.

## Explicit test authorization

- Ordinary build/regression/benchmark permission does not include sanitizers or prerequisite builds. Start them only when the user explicitly requests complete testing this turn or names ASan/UBSan/TSan, not as automatic regression additions.
- After code changes, cleanup, optimization, or review, report changes/recommended checks and await explicit permission.
- Without current-task authorization, do not start tests or prerequisite builds, including units, regression, sanitizers, benchmarks, stress, or soak on Windows or Linux.
- Writing cases, static review, and required source formatting are allowed; do not invoke tests indirectly through scripts.
- Explicit current requests/approved scope are sufficient; do not ask repeatedly within that scope. Historical authorization does not automatically cover future edits.
- On user stop, stop this task's tests and clean their resources without affecting other tasks/deployments.

Downloads/installations remain subject to global user agreements; testing permission is not download permission.

## Choices and waiting

- Ask choices in numbered batches in ordinary chat, not popup tools, countdowns, or automatic preselection.
- After a batch, pause subsequent implementation/edits until all answers arrive. Record partial answers and ask only remaining questions; do not fill gaps.
- Silence, elapsed time, and recommendations are not decisions/authorization; do not proceed on defaults.

## Linux test concurrency

- Ubuntu VM configuration is 16 cores with an 8 GiB memory maximum. When testing is authorized, parallelize independent builds/tests where resources permit.
- Maximum memory is not current allocation/availability. Preflight actual CPUs, available memory, and limits; choose concurrency for workload memory rather than fixed serial work or 16 high-memory tasks.
- Control build/test concurrency separately within one aggregate budget. Reduce high-memory sanitizer concurrency to avoid OOM.
- Parallelize only isolated ports, temporary paths, data, and processes. Honor CTest RUN_SERIAL/RESOURCE_LOCK and serialize shared/timing-dependent scenarios.
- This preference authorizes neither tests nor downloads and does not prioritize utilization over reliability.

## System-test topology

- Standard integration/performance starts with three Stars, each accepting local writes, with cross-node subscriptions and bidirectional source replication. Multiple nodes with writes only to the first are insufficient.
- Single/two-Star and memory-component cases remain targeted diagnostics/boundary checks, not multinode acceptance replacements. Redis comparisons use explicit total-resource/load accounting including replication, SDK, and control plane.
- This topology grants no execution permission; complete edits/static checks and await approval.

## Documentation

- [docs/README.md](docs/README.md) is the current entry. Maintain only effective standards/design/validation, without workspace historical archives or dated reports.
- Query history through Git. Maintain current test results in docs/validation.md with actual date, source identity, and limits; never call unrun changes passed.
- GEMINI.md only links shared agreements; do not duplicate policies or append session logs.

## Git and releases

- The independent repository is `github.com/eosforge/astra`. Owned code/docs use root MIT; retain original third-party licenses, NOTICEs, and runtime exceptions rather than relabeling them MIT. `main` is the release branch. Organization/license confirmation does not authorize commits/pushes. Review actual publication files, excluding build, caches, real credentials, and local evidence.
- Root is the independent build boundary; ordinary builds do not depend on the parent. Describe source/docs relative to this repository without importing legacy publication allowlists. docs/validation.md is the single current results record; raw evidence belongs in ignored build/results.
- Create/change/sign commits only on explicit current-task request; pushing branches/tags needs explicit instruction. Permission to push main does not cover alpha, and past permission does not cover this task.
- Restore local push protection before using alpha in a new checkout. Authorized pushes permit only the exact reviewed commit, without disabling protection or creating ongoing permission.
- Inspect Git before/after work and preserve user changes; never discard/reset/overwrite them without authorization.
