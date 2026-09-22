# 当前实现进度

本文件仅保留当前状态. 设计见 [架构](architecture.md), 语义见 [协议](../proto/README.md), 用例见 [验收规约](../testkit/comet.md), 实际执行证据见 [validation.md](../testkit/validation.md).

## 范围与验证边界

本轮范围为 Pulsar、Polaris、Star 三域存储及多 Star 复制、原生 Comet C++、必要 Go Astrolabe 与 Admin 管理适配. Moon、Planet、其他语言 SDK 和完整 Orrery 继续搁置. 不恢复旧 Redis SDK 或旧 Supervisor 的开发.

当前为 alpha 工作副本. 用户于 2026-09-22 授权的最近一次构建与回归已完成: **Debug、Release、ASan/UBSan、TSan 均完整通过 53 项 CTest**, Linux Go 的八个测试包及 race、Windows Go 的五个纯 Go 包、Admin 的 101 项测试/严格类型检查/生产构建和真实浏览器操作均通过. 该基线修复了 Protobuf 布局混用、Alarm 引用环、公共 TLS 接入和浏览器 fetch 等实际问题, 源码与产物摘要已核对. 下述后续修改不包含在该完整基线中; 新一轮已获准性能测试并通过 13 项定向 Release CTest 和两个 Go 包常规测试, 实际性能范围见验证记录. 未提交或推送, 不把功能回归通过称为全部生产规模、覆盖率或性能资格已完成.

## 当前实现

最近一次回归后, Scene 字符串所有权和异常回滚审核未发现所述悬垂引用或 editing 永久置位问题, 已补充交接注释与用例. 随后修正 Scene/Origin 历史读取: 超龄但仍连续保留的事件可用于恢复, 年龄只在写入裁剪时生效, 业务 TTL 与发送预算不变; 补充真实淘汰、零预算和双域 TTL 重放用例. Catalog/Ephemeris 的十个 expected 读取方法进一步收敛到私有 execute, 配合 expected 链式转换, 保留锁/回收顺序和原错误映射; Origin/Scene 使用 requires 表达记录类型要求, 补充错误路径、异常恢复和析构重入用例. **这些后续修改已通过定向 Release 回归**, 不延用上一轮 Sanitizer 结果. 详细边界见 [验证记录](../testkit/validation.md).

当前 [全量范围静态审核](review.md) 又修复三个行为问题: Catalog/Ephemeris 只读未知范围消耗永久写入配额、Publisher 未立即反映共享 Client 关闭、Astrolabe 指标新鲜度因 UTC 转换丢失单调时间. 同时减少三域 Edition 有序后缀的重复排序及 Polaris 历史裁剪的全窗口元数据读取, 补充对应行为用例并纠正文档状态/性能承诺. 这些修复已通过相应 Release/Go 定向验证; 性能与原生/RPC 基准的具体范围见验证记录. 大基线域锁、按 Scope 扫描订阅和重复 TTL 调度的进一步优化保留在审核与性能矩阵, 不把理论收益当成实测结果.

| 组件 | 已写入源码的能力 | 主要入口 |
| --- | --- | --- |
| Pulsar | 明确四种基础设施角色、签名准入、可信目录、SQLite 成员事务、连续纪元对时; ready 与同步质量分开 | [服务](../astra/pulsar/src/server.cpp), [持久库](../astra/pulsar/src/ledger.cpp) |
| Polaris | Go/GORM SQLite 唯一 Almanac 权威、显式初始化/部署绑定、WAL/FULL、每 Scope 连续提交、COMMIT 不确定停止写入、共享快照及有界后缀/ACK | [存储](../astra/polaris/internal/storage/store.go), [同步](../astra/polaris/internal/server/stream.go) |
| Star Almanac | 两级 Library 路由、权威原生状态、完整基线私有准备/原子安装、连续增量、凭据索引与撤销同边界、Reader 下行 | [Library](../astra/star/src/library.hpp), [Receiver](../astra/star/src/receiver.hpp), [Readout](../astra/star/src/readout.hpp) |
| Star Catalog | 每 Key 正内容版本、本机来源与跨来源水位分开、有限 TTL、版本冲突/过期保护、来源快照/精确回补、独立公开投影 | [State](../astra/star/src/catalog_state.hpp), [副本](../astra/star/src/catalog_replica.cpp) |
| Star Ephemeris | 原生 UUID/Attr/Data、独立 Update/Renew 顺序、同源写入与 TTL、按来源恢复、目标覆盖、不广播副本删除、完整恢复 UUID | [State](../astra/star/src/ephemeris_state.hpp), [副本](../astra/star/src/ephemeris_replica.cpp) |
| Star 对等复制 | 每对一条双向流、只广播自有动态来源、连续位置及完整 ACK、冻结根分页、单目标回补不跳过其他目标、可信替换、恢复总预算及无进展超时 | [Exchange](../astra/star/src/exchange.hpp), [Session](../astra/common/src/grpc_session.hpp), [Runtime](../astra/common/src/runtime.cpp) |
| 业务开放 | Pulsar 准入/首次时钟、Polaris 完整基线、首轮来源同步; 动态恢复 30 s 明确降级边界、独立公共 TLS/登录、标准 gRPC 健康状态 | [Runtime](../astra/common/src/runtime.cpp) |
| Comet C++ | 共享 Client/Session/两类 Channel/单 Alarm、Reader/Subscriber/Observer 不可变视图、Publisher/Beacon 自动保活、最新未发内容合并、明确 future 结果、换 Star 恢复及实际 OnDone 清理 | [公共头](../astra/comet/cpp/include/comet/client.hpp), [私有实现](../astra/comet/cpp/src/core.cpp) |
| Astrolabe | 单账号摘要/八小时 Cookie/Origin 防护、Polaris 同步提交和完整 NDJSON、凭据脱敏、可信目录、显式部署目标的有界指标采样; 无自身数据库 | [入口](../astra/astrolabe/main.go), [采样](../astra/astrolabe/internal/bridge/metrics.go) |
| 指标 | Star 独立可选 HTTP /metrics, 固定八个状态 gauge、16 条连接/4 KiB 头/两秒总截止, 不遍历业务数据、不携带秘密、不依赖外部监控 | [Metrics](../astra/star/src/metrics.hpp) |
| Admin | 真实管理模式与演示星图显式分开、Cookie 登录、目录/陈旧指标、完整读取、单 Key Set/Delete 与凭据管理; 不虚构真实 3D 连线 | [适配](../admin/src/app/management/api.ts), [界面](../admin/src/app/management/Management.vue) |
| 打包/工具 | CMake 静态 SDK 导出与独立消费用例、项目内缓存、离线 Go 构建/回归入口、按实际内存控制并发、所属进程/目录清理 | [构建入口](../astra/build.py), [消费验收](../astra/test_comet_package.py) |

三域未重新包装成统一 Store. Origin/Scene 只共用真正相同的页、事务准备和序列机制, TTL、水位、Attr/Data、来源权威继续独立. Proto 使用各自 v1 命名空间; 生成源码由已有生成器更新, 不手改第三方或生成代码.

## 用例落地

以下测试已有授权回归基线; 后续新增/修改的用例与该基线分开, 具体配置、结果和未覆盖范围以验证记录为准:

- 原生状态与分配故障: Almanac/Library、Catalog/Ephemeris、Origin/Scene/Pages、完整候选回滚、历史容量、期限推进、旧视图及重入/并发.
- 控制面持久与失败: Pulsar SQLite VFS 故障, Polaris 真实 SQLite 提交、重启、版本及不确定结果, 两语言身份与目录边界.
- 真实公共 RPC: 登录撤销、TLS/匿名配置、Readout 与两动态域推流、零历史并发更新、精确订阅、取消和资源归还.
- Comet: 投影、保守租约、共享身份、对象/future/回调寿命、切换与续租, Beacon 提交后丢回执/慢 Data; [Catalog 故障用例](../astra/comet/cpp/tests/catalog_fault_test.cpp) 覆盖 Publish 已提交但回执丢失、首帧停滞与半批停滞.
- 对等: Dispatch/Landing/Exchange、整包结构检查、两个域双向闭环、无历史全量、跨 Scope 精确回补、来源本地 TTL/完整恢复、退役来源与预算归还.
- 多进程: [cpp_comet_process](../astra/test_comet_process.py) 启动专有 Pulsar/Polaris/Astrolabe/两台 Star 和 SDK 探针; 持久重启、缓存可读、凭据脱敏、实际指标抓取、复制、强制结束原 Star、SDK 换节点/新 UUID、旧租约到期、原端口重启恢复.
- 管理: Go HTTP/KDF/Cookie/CORS、指标身份/长度/部分正文; [Admin 适配用例](../admin/tests/management.test.mjs) 验证 uint64 精度及版本耗尽、UTF-8 跨块、半份快照拒绝、单次写入不重试、错误正文脱敏和陈旧状态. 界面轮询等待本轮全部读取结束后再调度, 读取失败将旧指标标为陈旧. 真实浏览器已验证登录、Cookie 恢复、完整读写和服务中断后的陈旧提示.

## 已知限制与后续验证

业务 RPC 批量化已完成静态评估: 保留现有单目标协议, 将共享 Client 续租密度和多精确 Watch 开销纳入 [性能矩阵 B10/B11](../testkit/comet.md#rpc-batching), 确认热点后再决定是否扩展. 已保留协议边界并增加可复现的 [性能入口](../astra/bench/README.md), 未修改 Schema/生成代码; 不宣称已有批量续租、多目标订阅或跨 Key 原子发布.

1. 当前后续修复已有定向 Release 与 Go 回归, 完整 Sanitizer 结果仍受原源码身份限制. 不自动启动长期或无限测试.
2. 全量来源安装的准备仍在域锁内, 已测有限工作集恢复和真实推流, 尚无大规模恢复并发锁等待及生产 SLA 证据. 计费为逻辑内存预算, 不包含全部 gRPC/分配器/应用持有旧视图的 RSS. 未采集覆盖率百分比.
3. 当前指标为实际已实现的固定状态 gauge, 未伪造请求计数、延迟直方图或每 Scope 安装进度. Admin 当前以表格管理和观测, 完整实时 3D 拓扑仍属后续 Orrery.
4. 编译器的第三方 SQLite 提示、前端 chunk 提示及 Sanitizer 插桩边界保留在 [验证记录](../testkit/validation.md), 不通过测试通过推断其已消失.
5. 原始回归证据保存在 build/regression-20260922, 当前性能证据在 build/performance-current, 最新人工汇总仅维护 validation.md. 历史证据继续使用 Git 查询.
