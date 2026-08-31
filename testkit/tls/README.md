# TLS test fixture

`certificate.pem` and `private-key.pem` are a public, self-signed test-only
certificate and its private key. They protect no deployed identity or data.
The certificate is both a CA and a client/server certificate so the Go and
Rust configuration tests can exercise private roots and mTLS parsing with the
same small fixture.
