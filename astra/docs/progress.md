# 当前实现进度

本文描述当前源码及剩余工作, 不记录逐轮测试成绩. [验证记录](validation.md) 是实际执行结果的唯一入口; 迁移前冻结快照不代表当前未提交工作区已通过发布验收.

## 当前实现

| 组件 | 当前能力 | 实现入口 |
| --- | --- | --- |
| Pulsar | 基础设施准入、可信目录、SQLite 成员事务、连续时间参考; 业务就绪与时钟质量分开 | [服务](../pulsar/src/server.cpp), [持久库](../pulsar/src/ledger.cpp) |
| Polaris | Go/GORM SQLite 唯一 Almanac 权威, WAL/FULL, 同 Scope 原子提交、共享快照、有界历史及安装确认 | [存储](../polaris/internal/storage/store.go), [同步](../polaris/internal/server/stream.go) |
| Star Almanac | Scope 路由、完整候选原子安装、连续增量、凭据撤销与读取同步 | [Library](../star/src/library.hpp), [Receiver](../star/src/receiver.hpp) |
| Star Catalog | 多 Key 原子发布、内部版本查询、来源与合并水位、TTL、来源快照与精确回补 | [State](../star/src/catalog_state.hpp), [副本](../star/src/catalog_replica.cpp) |
| Star Ephemeris | 逻辑 UUID/能力、独立注册代次与 Data/Renew 顺序、多来源去重、原子 TTL 与恢复 | [State](../star/src/ephemeris_state.hpp), [副本](../star/src/ephemeris_replica.cpp) |
| 对等复制 | 每对 Star 一条双向逻辑流, 仅广播自有来源, 连续 ACK、分页恢复、回补及恢复预算 | [Exchange](../star/src/exchange.hpp), [Session](../common/src/grpc_session.hpp) |
| Comet C++ | 共享 Client/Session、同步 Publisher/Beacon、beat/tick、同 ID 恢复及三域读取/选择 | [公共头](../comet/cpp/include/comet/client.hpp), [Core](../comet/cpp/src/core.cpp) |
| Astrolabe | 部署管理账号、内存 Cookie 会话、Polaris 提交、NDJSON 读取、脱敏凭据和有界指标采样; 无自身数据库 | [入口](../astrolabe/main.go), [采样](../astrolabe/internal/bridge/metrics.go) |
| Admin | 真实管理模式与演示星图分开; 登录、目录/指标、完整读取、单 Key 编辑和凭据管理 | [适配](../admin/src/app/management/api.ts), [界面](../admin/src/app/management/Management.vue) |
| 工具与交付 | 独立 Astra 根目录, CMake 静态 SDK 导出、离线构建和跨进程测试入口 | [构建](../tools/build.py), [测试](../tests/README.md) |

同 Scope 多 Key 原子提交已经接线. Almanac 在 Polaris 一个事务内推进一个范围版本, 不设批次 Key 数或总字节上限; 分段接收及超帧快照恢复不暴露半批. Catalog 在同一接入 Star 提交 1..128 个唯一 Key, 键和正文合计至多 1 MiB. 单值、资源预算和请求截止仍按协议约束; 不提供跨 Star 同时可见、跨域事务或多个精确 Watch 的联合快照. 细节只在 [协议](../proto/README.md) 定义.

三域保持原生模型. Pagination、Reading 和私有 Context 共享机械流程, 各域仍拥有版本、水位、归属和到期规则. 下行共享不可变批次及仍在途的同页; Comet 普通完成事件进入去重就绪队列. 这些机制及剩余锁边界见 [审核](review.md), 不据代码存在推断性能收益.

## SDK 本轮实现与验证边界

C++ 已实现同步 Beacon 工厂/update、强制 beat、可选有界 tick、已确认 Data 恢复缓存与稳定逻辑 ID; Star/复制协议一起增加能力验证、注册代次和多来源选择. Publisher 保持同步原子提交和自动版本. Reader/Subscriber 提供完整 Map 或 Key/optional 回调及 state/changed/stop; Observer.one 返回受控 Item, 支持本地估计 CAS 和网络权威优先.

故障用例、独立包消费入口和三节点探针已同步修改. 本轮整理补充回调捕获析构的锁外释放与自身等待保护, 保留 Observer 单条更新的免去重分配路径; 采样容量/排空、截止/取消和本地估计边界均有定向用例. 实际执行结果只在 [验证记录](validation.md) 维护. 旧 View/close/wait 入口保留, Go/Rust SDK 未纳入本次. 当前可调用接口见 [Comet C++](../comet/cpp/README.md#c-公共接口), 字段见 [协议](../proto/README.md#comet).

## 尚未完成的验收

当前普通回归按新的冻结输入覆盖目录、模块名、生成路径和 Store/Agenda/Origin/Context、Broadcast 诊断改动. 各项是否通过仍以最新验证页的实际执行状态为准, 不沿用旧完整矩阵中的 Sanitizer 或性能结论. 已停止的无限长测不自动恢复; 有界三 Star 冒烟不替代长期稳定性.

剩余重点为新 Star 首次可信时钟、跨机器分区与交叉恢复、大来源安装、长期 RSS/FD/线程趋势, 以及浏览器真实交互/GPU 画面. 性能工作先测锁等待、调度扫描、页面重建和分配, 再决定算法替换; 不将静态分析或 Sanitizer 无诊断解释为完整安全证明.

Moon、Planet、Comet Go 和完整实时 3D Orrery 继续暂缓. Planet 保留骨架, Moon 尚无实现; 旧 Redis SDK 冻结. 构建、回归、Sanitizer、性能及长测分别遵循 [授权约定](../AGENTS.md), 文档中的命令与待办不构成运行授权.

## 已批准并缓存的 Polaris 依赖

版本声明以 [go.mod](../go.mod) 和 [Polaris SQLite 说明](../polaris/README.md#sqlite) 为准. 既有缓存可用于获准的离线工作, 不代表任意新环境已经具备依赖, 也不授予下载或升级权限. 缺失时明确报告, 不自动恢复依赖.
