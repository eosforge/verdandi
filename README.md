# Astra

Astra 是正在开发的分布式服务发现与状态同步项目. 系统以星图组织: Star 构成 Galaxy,
Supervisor 管理准入与成员信息, Admin 展示拓扑. Planet 的业务推进暂缓, 优先完成 Star 与第一版 SDK.

当前是 `0.1.0` Alpha 连接骨架, 尚未开放 Catalog/Registry 业务接口, 不宣称生产就绪.
已有内部 SyncStore 和同步协议草案, 不等于业务同步已接入网络.

## 活动代码

| 目录 | 职责 |
| --- | --- |
| [astra/](astra/README.md) | C++26 Star/Planet, Linux x64 / GCC 16.2 |
| [supervisor/](supervisor/README.md) | Go Supervisor, 账号准入及持久成员表 |
| [proto/](proto/README.md) | `proto.astra.v1 / proto.orbit.v1 / proto.comet.v1` gRPC 服务协议与生成器 |
| [admin/](admin/README.md) | Astra 星图管理界面 |
| [testkit/](testkit/README.md) | 服务回归、故障测试及资源清理 |

先读 [服务基础说明](service-foundation.md)、[身份契约](cluster/identity-contract.md) 和 [贡献指南](CONTRIBUTING.md).
代码注释与格式遵循 [coding.md](coding.md).

## 构建与验证

只使用已准备的项目工具和依赖. 构建不会隐式下载; 缺失时按各组件 README 单独准备.

```bash
bash astra/build.sh build --profile debug
bash scripts/test-services.sh
```

Windows 可运行 `./scripts/check-services.ps1 -Service supervisor`; C++26 服务在 Linux 验证.
长时测试需显式指定, 默认回归不会无限运行.

工具放 `build/tools`, Go/Rust/C++ 依赖缓存放 `build/deps`, 产物与报告放 `build/`.
项目脚本仅为子进程设置缓存位置, 不改变用户或系统配置. 这些目录不提交到 Git.

## 更名与历史版本

当前代号为 Astra. 最外层目录 `D:\projects\verdandi`、`/home/ubuntu/verdandi` 和
仓库地址 `git@github.com:eosforge/verdandi.git` 暂时保留. Go module、import 和 `go_package`
随仓库地址保持不变. [更名说明](astra-migration.md) 记录协议边界和 Linux 编译缓存复用方式.

`sdk/`、Lua、Redis 协议及旧版测试是冻结的 Verdandi 历史版本, 不迁移为 Astra SDK, 不用于新的上层实现.
旧 Rust 服务 `cluster/{common,star,planet}` 和 `testkit/transport` 实验也已退休.
保留 [旧 SDK 文档](legacy-sdk.md) 与历史测试证据, 不将其成绩算作 Astra 验收.
