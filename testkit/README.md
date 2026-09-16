# Astra Testkit

活动范围是 C++26 Star/Planet 与 Go Supervisor. Linux 一键回归:

```bash
bash scripts/test-services.sh
```

限时故障测试使用 `bash scripts/test-services.sh --mode soak --duration 3600`.
两个入口都清理自己创建的测试进程与临时资源, 报告保留在 `build/testkit/results/`.
Windows 使用 `scripts/check-services.ps1 -Service supervisor` 验证 Go; C++26 当前仅支持 Linux / GCC 16.2.
独立配置检查见 [C++ 服务说明](../astra/README.md).

当前覆盖准入、TLS/RPC、拓扑、故障恢复和内部 Store, 不代表已经实现 Catalog/Registry 业务服务或新 SDK.
Planet 仅维持现有连接回归, 业务推进暂停. 已退休 Rust 服务不参与当前测试.

[Verdandi SDK 测试文档](legacy-sdk.md)、Redis 场景、`transport/` 旧实验和历史 `results/` 保留原样.
共享 `support.py` 中旧 SDK 环境变量及清理辅助仍供历史测试使用, 不作为 Astra 对外接口.
活动 C++ 推流探针独立使用 `astra/bench/proto/probe.proto`.

Linux 测试源码及编译缓存的复用路径见 [Astra 更名说明](../astra-migration.md).
