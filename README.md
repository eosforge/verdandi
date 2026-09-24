# Astra

Astra 是开发中的分布式服务发现与状态同步项目. Star 构成 Galaxy, 控制服务管理准入, Pulsar 提供连续纪元时间, Admin 展示星图.

当前已实现 C++26 连接骨架、Pulsar 和内部 Store/Wheel. **Catalog/Registry 业务接口、Star 间业务复制、业务持久化及 Comet SDK 尚未完成**, 不宣称生产就绪.

## 阅读入口

| 需要了解什么 | 唯一入口 |
| --- | --- |
| 当前文档与维护规则 | [文档导航](docs/README.md) |
| 角色、已实现边界、业务约束与下一阶段 | [当前架构](docs/architecture.md) |
| gRPC 命名、准入身份和同步草案 | [协议契约](proto/README.md) |
| C++ 构建与启动 | [Astra 服务](astra/README.md) |
| 物理参考、四时间戳与连续 Unix 时间 | [Pulsar](astra/pulsar/README.md) |
| 存储、快照、历史、TTL | [Store](astra/common/README.md) |
| 测试入口与最新结果 | [Testkit](testkit/README.md), [验证记录](testkit/validation.md) |
| 修改与贡献 | [贡献指南](CONTRIBUTING.md), [通用规范](coding.md), [C++ 规范](cpp-coding.md) |

## 活动目录

`astra/` 为 Linux/GCC 16.2 的 C++26 服务; `proto/` 为协议与生成工具; `admin/` 为管理界面; `testkit/` 为测试工具.
Pulsar 提供 Orbit 登记与 Pulse 对时, 持久文件格式见其 README. 需要 Pulse 对时的 Star 使用 Pulsar 的登记入口.

构建仅消费已经准备的工具和依赖:

```bash
bash astra/build.sh build --profile debug
```

测试及其前置构建须获得当轮授权, 下载另行授权, 见 [AGENTS.md](AGENTS.md). 工具在 `build/tools`, 依赖在 `build/deps`, 产物和原始报告在 `build/`; 不提交缓存、部署凭据或运行日志.

## 名称与冻结边界

产品名为 Astra. 外层目录 `D:\projects\verdandi`, `/home/ubuntu/verdandi` 及仓库地址 `git@github.com:eosforge/verdandi.git` 暂留原名, Go module/import/go_package 同步保留现有仓库路径.
Linux 继续复用原项目目录及编译缓存, 不因文档整理移动或重建依赖.

旧 Redis SDK、Lua 和 Rust 服务不作为 Astra 新功能基础, 见 [冻结组件](legacy-sdk.md). 历史报告和旧方案只从 Git 历史查阅, 不作为当前能力或测试通过的依据.
