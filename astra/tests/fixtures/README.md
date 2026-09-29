# Public Test Identities

[English](README.md) | [简体中文](README_CN.md)

Certificates, private keys, accounts, and signing material here are deliberately public isolated fixtures, never for deployment. Only tests explicitly loading the fixture CA trust them; do not install them in system trust stores.

## Uses

- pulsar: admission-signing keys and public test accounts.
- star-a through star-d: distinct alpha deployment identities.
- planet-a, planet-b: reserved Planet identities, not claims of implementation.
- wrong-cluster: wrong-cluster/unknown-account cases.
- expired, rogue: expired certificates and invalid signatures.
- untrusted: independent certificates verifying rejection of nonfixture CAs.
- admission-v1.json, admission-v5.json: shared encoding/identity vectors.

Primary TLS fixtures use a public ECDSA P-256/SHA-256 CA with pinned Go and C++ gRPC/BoringSSL. Certificates have matching SKI/AKI; do not disable strict verification to accommodate incomplete certificates. The independent untrusted rejection fixture is outside this CA.

Admission uses Ed25519 separately from TLS algorithms. Historical Verdandi URI SANs no longer grant roles; v5 authorization uses public login.json and Pulsar accounts.json test accounts. Astra's signing domain is proto.orbit.v1.admission followed by NUL. No real signed deployment credentials are stored here.

## Maintenance

Use [tools/generate_fixtures.py](../../tools/generate_fixtures.py) only when fixture updates are explicitly needed, in the existing project Python/cryptography environment. Generation and tests are separate; do not download dependencies or refresh certificates automatically. Discard the primary CA private key after generation; retain leaf private keys as test input.

Directory moves preserve identity bytes. Temporary databases, copied keys, process records, and logs belong in ignored build/, not public fixtures. Actual results are in [validation](../../docs/validation.md).
