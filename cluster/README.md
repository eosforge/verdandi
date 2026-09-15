# Star / Planet 服务设计与共享夹具

当前实现为 [C++26 Star/Planet](../astra/README.md) 和 [Go Supervisor](../supervisor/README.md).
身份、命名、Astra 协议 v1 和升级边界以[身份与准入契约](identity-contract.md)为准.

- [C++ 骨架设计](cpp26-skeleton-design.md): 组织与生命周期.
- [按 Key 流式同步](key-stream-sync-design.md): 后续业务数据设计.
- [Supervisor 设计](supervisor-design.md): 管理与准入职责.
- `tests/fixtures/`: 当前 Go/C++ 共用的公开证书、账号与身份边界向量.

此目录中的 Cargo 配置、`common/`、`star/`、`planet/` 和 Cargo 包装脚本属于已废弃的 Rust 服务源码.
它们仅作历史参考, 不再维护、生成、构建或作为兼容性对照. 旧设计与验收报告保留原时点结论.
独立协议生成工具继续维护; 所有 Verdandi SDK 按[更名边界](../astra-migration.md)冻结, 不迁移为 Astra SDK.
