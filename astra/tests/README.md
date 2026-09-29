# Astra Tests

[English](README.md) | [简体中文](README_CN.md)

Shared identities and cross-process scenarios live here. C++ units remain in component `tests/`, Go tests beside packages, and Admin tests in `admin/tests/`.
Maintain results only in [docs/validation.md](../docs/validation.md); raw logs, JSON, and source/binary digests belong in ignored `build/results/`. Test definitions and passing conclusions are separate; premigration results do not automatically validate a relocated workspace.

## Explicit entry points

Run from Astra's root, with current-task build/test authorization under [AGENTS.md](../AGENTS.md):

```bash
bash build.sh regression --profile release
python3 -B tests/test_build.py
python3 -B tests/test_pulsar_harness.py
python3 -B tests/test_soak.py
python3 -B -m unittest tests.test_verify -v
```

`regression` runs offline builds, Go cases, CTest, and generated-source comparison sequentially, respecting resource limits and CTest serialization. Sanitizers, benchmarks, and soak require separate authorization and are not implicit regression additions. Missing tools/dependencies are not downloaded.

Keys/accounts/certificates in `fixtures/` are public isolated test identities, never deployment credentials. Service tests own temporary directories, ports, and processes and clean them on exit without affecting deployed services.

## Methods and evidence

- [Comet scenarios](../docs/comet.md): contracts and case mapping.
- [Development gate/pilot](../docs/features/verification.md): formal schemas, negative gate cases, native Linux collection, and Polaris storage observations; synthetic fixtures are not real evidence.
- [Three-Star soak](../docs/soak.md): local writes at each node, propagation/recovery through `tests/soak.py`.
- [Diagnostic probes](../docs/profile.md): collection boundaries and offline `tools/profile.py` analysis only after owning processes exit.
- [Performance workloads](../bench/README.md) and [optional Redis comparison](../bench/baseline/README.md): separate from ordinary regression.

Documentation reorganization neither runs these commands nor restarts previously stopped soak.
