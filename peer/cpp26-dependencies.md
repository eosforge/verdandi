# Star / Planet C++26 依赖版本锁定

状态: 普通依赖与 TSan 独立插桩依赖均已在 Ubuntu 项目内构建并完成服务回归. 核对日期 2026-09-11. 适用范围仅为 C++26 Star/Planet,
不升级现有 Rust、Go Supervisor 或 Redis SDK 依赖. 机器可读的来源和归档 SHA-256 固定在
[dependencies.lock.json](../peer-cpp/dependencies.lock.json). 主设计见 [C++26 骨架设计](cpp26-skeleton-design.md).

维护者已要求锁定当前最新稳定版. 下表采用核对时官方 latest release 中非预发布、非草稿的版本,
同时解析 tag 对应的完整源码 commit. 今后的构建不得重新查询 latest 自动改变此表.
固定来源已完成普通构建及 Debug/Release/ASan 服务验证; 完整并发和长时结果见独立 [C++ 测试记录](../peer-cpp/validation.md).
构建选项见 [prepare_dependencies.py](../peer-cpp/prepare_dependencies.py), 上游许可证原文保存在
[licenses](../peer-cpp/licenses/README.md). 产物摘要由显式准备入口写入忽略的 `build/deps/peer-cpp/artifacts.json`.

## 1. 稳定发布版本

| 组件 / 用途 | 固定版本 | 完整源码 commit | 官方发布来源 |
| --- | --- | --- | --- |
| gRPC C++ / grpc_cpp_plugin | v1.84.0 | `3252a89f10d8e92997862167ca7d095ecda85973` | [gRPC 1.84.0](https://github.com/grpc/grpc/releases/tag/v1.84.0) |
| Protobuf C++ / protoc | v36.1 | `f377bfefc5e2cfab68b816903c25b23e091c439d` | [Protobuf 36.1](https://github.com/protocolbuffers/protobuf/releases/tag/v36.1) |
| Abseil | 20260817.0 | `2065f4ded0558c6f89fee67c8e5228feb4eb960e` | [Abseil 20260817.0](https://github.com/abseil/abseil-cpp/releases/tag/20260817.0) |
| c-ares / DNS | v1.34.8 | `c7a3138dcfe3bb0eaaf10c0c24c36dc66dc790ab` | [c-ares 1.34.8](https://github.com/c-ares/c-ares/releases/tag/v1.34.8) |
| RE2 | 2025-11-05 | `927f5d53caf8111721e734cf24724686bb745f55` | [RE2 2025-11-05](https://github.com/google/re2/releases/tag/2025-11-05) |
| zlib | v1.3.2 | `da607da739fa6047df13e66a2af6b8bec7c2a498` | [zlib 1.3.2](https://github.com/madler/zlib/releases/tag/v1.3.2) |
| yyjson / 本地 JSON | 0.13.0 | `6447536015f3d600f3d65323b10976103b337ca7` | [yyjson 0.13.0](https://github.com/ibireme/yyjson/releases/tag/0.13.0) |

gRPC 1.84.0 的官方发布时间为 2026-09-11 03:31:13 UTC, 即北京时间 11:31:13,
因此替代设计初稿中的 1.83.1 候选. 核对依据同时包括 [官方发布 API](https://api.github.com/repos/grpc/grpc/releases/latest)
和 [固定源码树](https://api.github.com/repos/grpc/grpc/git/trees/3252a89f10d8e92997862167ca7d095ecda85973).
latest API 只用于本次核对, 不是后续构建输入.

GCC/libstdc++ 保持已选、已有实测的 16.2.0. 本次依赖锁不扩展为 CMake、clang-format 或系统软件升级.
已有 protoc 36.1 优先核对后复用. grpc_cpp_plugin 从上述 gRPC 源码构建, 不能用 Go/Rust 插件代替.

## 2. BoringSSL 随 gRPC 锁定

TLS provider 固定为 `gRPC_SSL_PROVIDER=module`, BoringSSL 使用 gRPC 1.84.0 的子模块引用:

- 源码: [google/boringssl](https://github.com/google/boringssl).
- commit: `2b44a3701a4788e1ef866ddc7f143060a3d196c9`.
- 上游路径: `third_party/boringssl-with-bazel`.
- 核对依据: [gRPC 固定递归源码树](https://api.github.com/repos/grpc/grpc/git/trees/3252a89f10d8e92997862167ca7d095ecda85973?recursive=1)
  与 [固定子模块来源](https://raw.githubusercontent.com/grpc/grpc/v1.84.0/.gitmodules).

BoringSSL 按之前已确认的“gRPC 配套版本”规则固定 commit, 不独立追逐 HEAD 或替换为另一份 crypto.
TLS 与 identity 私有适配器共用同一构建, 不对公共 API 导出 BoringSSL 类型.
这份 commit 是上游引用证据, 不是本项目已通过 TLS/Ed25519 测试的证明.

## 3. 与 gRPC 上游依赖组合的差异

各组件最新稳定版并不等于 gRPC 此 tag 的全部子模块版本. 以下是官方固定源码树中的引用,
用于解释组合验证风险, 不是第二套可自动回退的依赖锁:

| gRPC 上游路径 | 上游固定 commit | 本项目选择 |
| --- | --- | --- |
| `third_party/abseil-cpp` | `76bb24329e8bf5f39704eb10d21b9a80befa7c81` | 第 1 节的最新稳定发布 |
| `third_party/protobuf` | `35cd01f9fe9afbeea38cc7b979a3b6bfcde82c03` | 第 1 节的最新稳定发布 |
| `third_party/cares/cares` | `d3a507e920e7af18a5efb7f9f1d8044ed4750013` | 第 1 节的最新稳定发布 |
| `third_party/re2` | `0c5616df9c0aaa44c9440d87422012423d91c7d1` | 第 1 节的最新稳定发布 |
| `third_party/zlib` | `f1f503da85d52e56aae11557b4d79a42bcaa2b86` | 第 1 节的最新稳定发布 |

Abseil、Protobuf、c-ares、RE2、zlib 已在项目前缀统一构建, 通过对应 `gRPC_*_PROVIDER=package`
显式导入; BoringSSL 仍为 module. gRPC 提供这些 provider 入口, 但提供入口不表示已保证上述新版本组合兼容.
[gRPC 固定 CMake 配置](https://raw.githubusercontent.com/grpc/grpc/v1.84.0/CMakeLists.txt),
[Protobuf provider](https://raw.githubusercontent.com/grpc/grpc/v1.84.0/cmake/protobuf.cmake).

M0 需要确认 C++ 生成器与 runtime 精确匹配, 所有组件只使用一份 Abseil/Protobuf 构建,
以及 gRPC 内部 upb/生成源与替换后的依赖相容. 不为解决不兼容而静默混入另一版 runtime 或临时修改上游生成代码.
若锁定组合失败, 记录实际错误与最小版本调整方案后修订清单, 不把“各自稳定”写成“组合已合格”.
[Protobuf C++ 兼容要求](https://protobuf.dev/support/cross-version-runtime-guarantee/).

gRPC 源树自带或固定引用的其他内部生成数据随 gRPC commit 固定, 不单独追逐其仓库最新版.
实际归档需补齐 gRPC 自己的 `grpc/grpc-proto` 子模块 `ec30f589e2519d595688b9a42f88a91bdd6b733f`,
使用上游 `grpc_deps.bzl` 指定的 SHA-256 `5e9b520b22afbd53a662cc29017064be253c1dfa6df8958738594e7fec6ade33`.
它只提供同版本 gRPC 的协议源文件, 不安装额外运行服务或引入另一套协议生成框架.
只准备实际构建所需项, 不因为子模块列表存在就拉取全部 benchmark、GoogleTest 或 OpenTelemetry 组件.
新增必需的传递依赖应在实施清单中记录来源与固定版本, 不能通过构建脚本隐式获取.

## 4. 准备与可复现记录

已有缓存必须匹配上述来源版本及工具链, 不因“本机能找到”就混用其他 ABI. 实际位置:

- 源码与下载缓存: `build/deps/peer-cpp`.
- 依赖安装前缀: `build/deps/peer-cpp/linux-gcc16/install`.
- TSan 独立安装前缀: `build/deps/peer-cpp/linux-gcc16-tsan/install`, 复用相同来源缓存.
- 生成工具: `build/tools/protoc` 与 `build/tools/grpc-cpp-plugin`, 子目录包含固定版本.
- C++ 服务产物: `build/peer-cpp/<profile>`, core-only 为 `core-<profile>`.

准备入口记录实际归档 SHA-256、递归来源、编译器/标准库 ABI、构建选项、链接方式、许可证与产物摘要;
源码 commit 不能冒充尚未下载归档的 SHA-256. 正常配置、生成检查、构建和测试保持离线.
本表批准的是版本选择, 获取缺失的软件或依赖仍遵循用户对具体项目的下载授权约定.

普通静态库和生成器的摘要在 `build/deps/peer-cpp/artifacts.json`; 完整 TSan 准备成功后另写 `artifacts-tsan.json`.
两者单任务编译, 普通配置限制 2 GiB 虚拟地址空间; TSan 生成工具需要 shadow 映射, 因此不套用该虚拟地址限制.
不改变系统配置或普通生成工具. 安装显式指定项目前缀, gRPC 构建所需完整安装目标, 不构建其上游测试目标.
BoringSSL 上游测试的 GCC 编译诊断及本轮覆盖边界见验证记录, 不将本项目测试当成整个上游测试集通过.
