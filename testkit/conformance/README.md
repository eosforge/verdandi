# Cross-language conformance corpus

`v1/` contains language-neutral vectors. Go, Rust, and C++ consume the shared
configuration acceptance and stable-error corpus; C# reaches the same parser
through C ABI v1. Go and Rust additionally consume the Catalog MessagePack
notification corpus. The vectors fix observable behavior without forcing any
SDKs to share implementation code.

The version directory is immutable once a public protocol is released. During
the current unreleased alpha, incompatible changes update the existing vectors
and all language implementations in the same working change.
