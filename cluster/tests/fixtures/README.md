# Public test identities

These certificates and private keys are intentionally public test fixtures.
They are trusted only when a test explicitly loads this fixture CA. They are
not deployment credentials and must not be used by a deployed cluster.

`pulsar` contains the test admission signing key. `star-a` through `star-d`
are distinct alpha deployment identities; `wrong-cluster` is authorized only
for beta; `expired` is outside its validity interval; `rogue` has an invalid
signature under the trusted test CA. Long validity of the other fixtures
keeps ordinary source tests independent of certificate renewal dates.

`planet-a` and `planet-b` carry the `verdandi://cluster/alpha/planet` role.
All TLS fixtures use ECDSA P-256 with SHA-256 under one public test CA. This is
the shared TLS profile for Go and C++ gRPC/BoringSSL: the pinned BoringSSL
does not advertise Ed25519 in its default TLS verification algorithms.
Admission signatures still use Ed25519; they are independent of TLS certificates.
Root and leaf certificates contain matching SKI/AKI extensions for strict X.509
verification, including Python's default HTTPS client. Tests never disable that
verification to accommodate incomplete fixtures.
Run `cluster/tests/generate_tls_fixtures.py` explicitly with the existing project
Python/cryptography environment to renew these public fixtures. It preserves
SANs, roles, expiration and invalid-signature scenarios. The CA private key is
discarded after fixture generation; leaf private keys are intentionally public. No test root is installed
into system trust. In v5, certificate URIs no longer authorize roles. Public login.json files use
shared stars/planets accounts, and Pulsar accounts.json contains public test
password hashes. Real Go gRPC tests verify account role authorization. The
wrong-cluster fixture uses an unknown account; expired/rogue certificates fail
local advertised-endpoint validation.

The tests do not install these certificates into an operating-system trust
store. Transient databases, copied keys, processes and logs belong in build/.

Astra reuses these public TLS fixtures byte-for-byte. Historical Verdandi URI SANs are not authorization inputs. New admission signatures use `proto.orbit.v1.admission` followed by NUL; no signed admission credential is stored in these fixtures.
