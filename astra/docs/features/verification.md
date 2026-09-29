# Development Gate and First Pilot

Feature ID: `development-verification`.

Status: the user authorized formal schemas, gate decisions, and pilot delivery. The initial local increment includes schemas, tools, negative cases, and Polaris integration. Formal collection, gate behavioral self-checks, and pilot acceptance remain unrun. Later ordinary regression ran Go builds/package tests, but cannot replace this evidence loop. No accepted-pilot claim or standard effective date is available yet.

The [development standard](../development.md) defines configuration/evidence/gating/adoption; [AGENTS.md](../../AGENTS.md) governs execution. Implementation permission does not authorize tests, dependency installation, or CI changes.

## 1. Verified facts and scope

- Existing [build tools](../../tools/build.py), [test support](../../tests/support.py), and [validation](../validation.md) provide foundations. Initial investigation found no formal schema/obligation gate/CI workflow; Section 2 now lists delivered schemas/tools, while CI remains absent.
- [Storage tests](../../polaris/internal/storage/storage_test.go) and [batch tests](../../polaris/internal/storage/batch_test.go) cover commits, retries, history, cancellation, concurrency, SQL rollback, and reopen. They require linux && cgo; unexecuted Windows cases are not passes.
- Existing Windows project Python is 3.14.7 with Black 26.5.1 but no jsonschema. Ubuntu has /usr/bin/python3 3.14.4/jsonschema 4.19.2, reused without installation. Use Draft202012Validator/local-only references. Missing Windows engine fails explicitly, without downloading or substituting a handwritten subset.
- Old validation cannot be upgraded to new-standard evidence without obligation mapping, input identity, and collection boundaries.

Deliver reusable validation/collection/gating and one real component adoption, not a multi-CI framework. Project docs own concrete entries; do not append specialist incidents to common policy. No deployment changes, resumed soak, or repository-wide release extrapolation.

## 2. Files and responsibilities

These source entries are delivered; behavior checks await authorization:

| Path | Responsibility |
| --- | --- |
| development.json | Components/contracts/scenario-platform matrix, commands, budgets, design approval, convergence |
| tools/schemas/development.schema.json | Formal configuration structure |
| tools/schemas/evidence.schema.json | Native records/evidence/gate input |
| tools/schemas/authority.schema.json | Independent approvals, exact binding, expiring per-obligation exceptions |
| tools/schemas/rules.schema.json | Registry/loading dependencies |
| tools/rules.json | Stable IDs, section locations, global set, loading edges |
| tools/verify.py | Explicit validate/collect/gate; validation/decisions never implicitly test |
| tests/test_verify.py | Positive/negative validation, synthetic events, Linux lifecycle cases; fixtures generated inside tests, not fake run JSON |
| polaris/internal/storage/verification_test.go | Actual hits for four finite contracts, retaining all original storage tests |
| build/results/verification/<run-id>/ | Separate local native records/source-artifact manifests/gate output; never overwrite failures |

Split verify.py only when scale warrants, not into a speculative plugin system. Keep design here and conclusions in [validation](../validation.md), without dated reports.

## 3. Configuration validation and obligation sources

Schemas use [Draft 2020-12](https://json-schema.org/draft/2020-12). Executable schema_version: 1 is independent of policy-document version; conceptual version 2 is not valid input and is not silently migrated. Objects explicitly require fields/reject unknowns under [object constraints](https://json-schema.org/understanding-json-schema/reference/object). These two sources were consulted during design; that does not redate unrelated references.

Errors identify file, field path, and stable reason code through three layers:

1. Strict reading rejects duplicate keys, invalid numbers, size/depth excess, and escaping paths, without overwrite/truncation.
2. Schema rejects unknown/missing fields, wrong types/enums/ranges/versions. Bundle/hash schemas/references; never fetch references at runtime.
3. Semantics verifies IDs, uniqueness, references, minimum tiers, platform/scenario matrix, exception scope, approved provenance, and inheritance. Check full rule IDs, not prefixes. Expiry/actual path boundaries require more than schemas.

Expand O from approved policy, contracts, and the complete matrix, distinguishing rule/contract/component/platform-profile/scenario. Reports provide observations, not permission to delete O. Missing platforms/adapters/tools remain gaps, not empty arrays/unimplemented/not-applicable escapes.

Initial configuration is Polaris storage, Linux amd64/cgo, L3, build/test parallelism four, outer check timeout 240 s, inner Go 180 s, stdout/stderr 8 MiB each. Preflight affinity/memory/cgroup; reject insufficient fixed budgets rather than expand resources/change policy. Target gaps zero; unmeasured baseline/unapproved deadline null. Tested configuration cannot issue its own run/exception/policy authorization.

## 4. Native collection and gating

collect executes only explicitly selected approved commands using argument arrays/cwd/environment, never log-provided commands. Verify existing offline dependency/resource helpers before reuse; the entire support script is not inherently trusted control plane.

Freeze relevant source/tests/configuration/rules/runner/toolchain identity including dirty/untracked/deleted facts. Directly capture actual timestamps, monotonic duration, exit/signal/timeout, complete output lengths/digests. Separate declarations, facts, interpretation; unexecuted values are not zero. Output-budget exhaustion, lost contact, and interrupted collection mark incomplete evidence. Verify descendants/resources after stop; undrained work cannot be clean.

gate independently rereads sealed contents, ignoring self-reported gate status. Missing/replaced/truncated/wrong-source/escaping files reject explicitly. Another successful same-name check cannot cancel required failure/unknown.

- P contains only obligations whose required observations/oracles all succeed; exit zero, test count, or rule labels alone are insufficient.
- E comes only from policy-approved authority channels, checking identity/scope/expiry/revocation/alternatives using real control-plane time, not tested virtual time. Preserve original statuses.
- D = O - (P union E); identity/authorization/integrity/isolation/cleanup hard blockers H are independent.
- Nonempty H/D means blocked; actual full passes mean eligible; valid exceptions mean eligible-with-exceptions. Identify exact scope/gaps.

Exit codes: 0 for successful selected action (gate: eligible only), 10 eligible-with-exceptions, 2 blocked or failed/interrupted collection, 3 invalid format/authorization/environment/evidence. collect zero means selected commands completed/collected, not component qualification; run gate separately.

Collection supports Linux only. subreaper tracks descendants; pidfd/start ticks prevent signalling reused PIDs. SIGINT/SIGTERM stops spawning and drains/cleans owned temporaries. Escalated termination remains failed/interrupted and preserves first failure. No host-service/cgroup/old-evidence mutation. Seal native records separately; summaries reference digests. Gate rereads records and both full outputs, independently parsing Go JSON required terminal states/contract hits. Missing probes, skips, unfinished packages, and truncation cannot pass.

Limited initial adapters are acceptable but unsupported obligations block. Gap/expiry/TEMPLATE/truncation/missing-platform self-checks alone are an initial increment, not all Section 9/release gating.

## 5. Three evidence trust boundaries

Trust follows verified collection, not arbitrary report labels:

| Tier | Collection/storage | Claims/checks |
| --- | --- | --- |
| local | Workspace account, ignored build/results/verification/ | Integrity/replay identity; acknowledge same-account rewriting, no isolated-collection claim |
| ci | User-specified isolated jobs; protected control plane owns runner/policy/terminal records, artifacts in independent controlled storage | Verify job/collector/policy/source/configuration identity and artifact lengths/digests from trusted sources; tested code lacks policy/terminal-record write credentials |
| release | Release-policy-compliant controlled evidence tied to actual distribution/independent release approval | Verify provenance, acceptance scope, distributed digests, permissions; not relabelled CI |

Another directory, readonly attributes, same-account signatures, or another same-permission host do not establish ci. Trusted collectors prove observations, but tested code can still print false assertions; independent oracles/self-checks remain necessary.

Before CI adoption identify provider, principal, protected policy, artifact storage/retention, and editor/tested-process permissions. These facts are currently unconfirmed; do not mark the pilot ci-compliant. Complete local tools/report gaps without lowering release trust automatically.

## 6. Rule slices and loading dependencies

Use a versioned registry; documentation references/generates its table. Always load at least DEV-SCOPE, DEV-CONFIG, DEV-EVIDENCE, DEV-GATE and required authorization/budget/stop/protection text. Actual architecture triggers specialist rules and explicit recursive dependencies.

Loading dependencies define necessary reading, not required technology. Expand applicability/obligations separately; loading closure does not force databases into nonpersistent components.

Cover all standard IDs; reject missing targets, unknown IDs, and nonterminating resolution. Distinguish textual references from executable-obligation dependencies to avoid meaningless cycles. Freeze registry digest, triggers, expansion, and provenance; verify slices/full policy produce identical required obligations for the same configuration.

## 7. Pilot and acceptance

Pilot polaris/internal/storage follows storage-core L3, never downgraded to become green. TestVerification emits actual oracle counts for four finite examples while original tests run. Concurrent snapshot cases remain; serial examples are not exhaustive interleavings.

| Contract | Existing starting points | Required additional/verified evidence |
| --- | --- | --- |
| Atomic multi-key commit/snapshot visibility | TestBatchPersistence, TestBatchSnapshot | Hits, key interleavings, independent small-state model |
| Retry/same-version content conflict | TestCommit, TestUncertain | Finite history boundaries, known/unknown outcomes, negative oracle |
| SQL failure leaves no partial state/history | TestBatchRollback, TestAtomicity | Fault denominator, rollback/cleanup state, not error code only |
| History/recovery | TestHistoryPrefix, TestRestart | Complete data/version; clean reopen is not crash/power-loss evidence |

Preflight actual Linux/cgo, Go/compiler/module identities before authorized execution; disable implicit downloads and size concurrency by memory. Component pilot does not replace three-Star acceptance. Freeze run scope/counts/resources/stop before results.

First native coverage/obligations establish baseline. Go coverage is not branch coverage/MC/DC; add suitable oracles or retain gaps. Mutation, independent finite models, and key faults remain visible when incomplete. A few passing cases do not establish whole-component L3.

Convergence records need comparable raw baseline counts/denominators/source identity, targets, and user-accepted deadlines. Standards set targets; measurement sets baseline; budget/scope decisions set deadlines. Do not invent three numbers. A first real blocked decision demonstrates gap detection, not accepted pilot.

Pair at least valid scoped acceptance with rejection of unknown/duplicate/missing fields/rules; TEMPLATE, truncation, missing platform, empty obligations, expired/out-of-scope exceptions, replacement, self-reporting; and correct handling of static zero findings/expected rejection. Other applicable Section 9.3 classes remain required, not erased by this initial list.

Record scoped pilot acceptance only after valid configuration, native collection, obligation decisions, self-checks, and all selected-component obligations. Approved exceptions remain eligible-with-exceptions with visible gaps, never all passed.

## 8. Reference dates and sequence

Separate policy version, effective date, consultation date, and execution date. The former Section 13 date 2026-09-29 must mean an actual verifiable consultation or be removed/replaced with per-source dates. Revision day is not every link's consultation date. Schema research does not reverify other sources.

Implementation admission, engine investigation, and initial tool coding are complete. Next: static review/proposed scope → authorized self-checks/pilot → measured baseline/gaps → actual isolated CI adoption/acceptance. Documentation/computable scripts are not acceptance.

| Failure path | Protection and pending negative case |
| --- | --- |
| Summary claims success after raw replacement/truncation | verify.reference recomputes lengths/hashes; go_observations checks terminal completeness; EvidenceTests.test_integrity_rejected, ObservationTests.test_truncated_json |
| Hits printed before skip/failure, or another required check fails | go_observations validates each required test; decide conjuncts all requirements; test_non_vacuity_and_failure, test_pass_does_not_override_other_required_failure |
| Timed-out parent exits while setsid child retains pipes | owned_members/drain combines subreaper/pidfd; CollectorTests.test_process_exit_timeout_output_limit_and_orphan |

These are source/case mappings, not executed passes. Same-account alteration of logs/hashes/approval cannot be eliminated locally; real isolated CI must close that risk.

## 9. Commands and remaining acceptance

Command descriptions, not execution claims; use existing Ubuntu Python from Astra root:

```sh
python3 -B tools/verify.py validate
python3 -B -m unittest tests.test_verify -v
ASTRA_CACHE_ROOT=/home/ubuntu/verdandi/build python3 -B tools/verify.py collect \
  --platform linux-amd64-cgo --check storage-examples \
  --authority build/verification-approval.json --authority-sha256 APPROVED_SHA256 \
  --authorization USER_RUN_APPROVAL_REFERENCE
python3 -B tools/verify.py gate \
  --authority build/verification-approval.json --authority-sha256 APPROVED_SHA256 \
  build/results/verification/RUN_UUID
```

validate emits pins for configuration, registry, collector, design, policy, and schema set. Authorized approvers check digests/scope, then create authority.schema.json records outside evidence: schema_version 1, real approval_ref, UTC expires_at, revoked false, unchanged pins, exceptions []. Pass the digest independently on CLI. AI may prepare content but cannot self-issue authorization by filling references. Local files/hashes fix approved input, not isolation/signature trust; CI needs protected control plane.

This increment does not support inheritance, Windows collection, general plugins, CI/release isolation, or automatic MC/DC/model/mutation/full-matrix qualification. Reject or retain unavailable explicitly; unknown fields never silently downgrade. development.json retains all 29 maintenance-rule obligations and six L3 specialist obligations; finite examples discharge only their four items. First actual aggregate collection is expected blocked, a pending expectation rather than an existing result.

Pending permission covers tool self-checks and this component's ordinary Go/cgo tests only, excluding sanitizers, performance, soak, and deployment. Fill baseline after real evidence; the user sets convergence deadlines. Actual CI principal/isolation/retention remain unspecified and cannot be inferred from SSH availability. Effective date remains pending accepted pilot.
