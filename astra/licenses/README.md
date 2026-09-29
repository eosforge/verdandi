# Third-Party Notices

[English](README.md) | [简体中文](README_CN.md)

This directory preserves original licenses/NOTICEs for sources in the [dependency lock](../dependencies.lock.json).
Do not rewrite upstream terms. Distribution packages must retain these files and the project LICENSE.

| Source | Original text |
| --- | --- |
| gRPC, including internal third-party notices | [LICENSE](grpc/LICENSE), [NOTICE](grpc/NOTICE.txt) |
| gRPC protocol submodule | [LICENSE](grpc-proto/LICENSE) |
| Protobuf | [LICENSE](protobuf/LICENSE) |
| Abseil | [LICENSE](abseil/LICENSE) |
| c-ares | [LICENSE](cares/LICENSE.md) |
| RE2 | [LICENSE](re2/LICENSE) |
| zlib | [LICENSE](zlib/LICENSE) |
| yyjson | [LICENSE](yyjson/LICENSE) |
| BoringSSL, including upstream terms | [LICENSE](boringssl/LICENSE) |
| GCC 16.2 libstdc++exp contract runtime | [COPYING3](gcc-runtime/COPYING3), [Runtime Exception](gcc-runtime/COPYING.RUNTIME) |

GCC runtime terms come from the project's GCC 16.2.0 source. Dynamic libstdc++ and the system C runtime belong to the deployment toolchain; record actual bundled scope/provenance with releases.
These are source-bundled notices, not completed binary release or cross-distribution compatibility acceptance.
