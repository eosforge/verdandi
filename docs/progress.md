# 当前实现进度

当前 alpha HEAD `2a17172` 加工作副本. 2026-09-26 直接推进当前版本发布审查, 不冻结开发. 对等收发重叠、下行唤醒/后缀查找和探针改动已通过 Release、探针版、ASan/UBSan、TSan 四配置完整矩阵, 各 59/59 CTest. 最终夹具四配置定向回归、多记录三轮故障/常驻冒烟及实际中断清理也通过, 详见 [validation.md](../testkit/validation.md).

当前优先级为稳定性和三节点业务验收, 交付能力最后处理. 2026-09-25 的性能数据保留原验证身份, 不把本轮正确性回归解释为性能提升. 探针轻量目标尚未在所有负载得到证明.

本文件仅保留当前状态. 设计见 [架构](architecture.md), 语义见 [协议](../proto/README.md), 用例见 [验收规约](../testkit/comet.md), 实际执行证据见 [validation.md](../testkit/validation.md).

## 范围与验证边界

本轮范围为 Pulsar、Polaris、Star 三域存储及多 Star 复制、原生 Comet C++、必要 Go Astrolabe 与 Admin 管理适配. Moon、Planet、其他语言 SDK 和完整 Orrery 继续搁置. 不恢复旧 Redis SDK 的开发. 已删除的 Go Supervisor 只从 Git 历史查阅.

本轮包含三 Star 同时写入基础验收、Go 回归、Sanitizer 和 Admin 检查, [长测](../testkit/soak.md) 入口已验证并获准启动正式运行, 尚未完成. 未下载、提交或推送, 未恢复旧 SDK. 具体源码身份与执行范围只在 [验证记录](../testkit/validation.md) 维护.

## 当前实现

SDK 保留共享连接/准入、固定范围 Watch、最后完整游标和不可变视图. 完成事件定向推进 Activity, 到期/共享身份/关闭仍唤醒. Star 自有来源导出独立且不触发 GC, 公开读取在下一整拍前共享. 远端原生候选改用独立来源锁在域锁外准备, 最终水位/投影/期限/预算提交仍在域锁内; 忙碌来源的到期等待释放域锁, 公开读取在完整推进后才返回. 同范围订阅共享不可变批次和仍在途的同页, 每流确认及预算独立. 具体边界见 [并发复核](review.md#sdk-与-scope-并发复核).

已有优化覆盖 Comet 调度工作空间和 expected 工厂交接、Table 计量/脏页/名称拥有、三域单项去重快路径、Star 写入复核和分页比较、Exchange 重复大小计算. Astrolabe/Admin 已取消独立 64 节点指标上限, 改为完整 Star 目录展示和有界并发采样, 复用连接并用目录索引拒绝旧实例结果; 指标地址仍需部署映射. C++/Go 部分纳入此前普通回归基线, Admin 前端已完成自动化、类型、格式与构建检查. 具体已改项、其余待归因候选统一见 [当前审核](review.md).

Scene 字符串所有权和 Edit 回滚由共享拥有与 RAII 保证, 相关异常/移动用例已纳入回归. Scene/Origin 允许重放超龄但仍连续保留的历史, 年龄只在写入裁剪时生效, 不延长业务 TTL. 两动态域的 expected 读取方法使用私有 execute, 保持锁和回收次序; requires 表达模板记录要求. 当前 execute 的忙来源等待分支及已有机制均已通过此前普通回归基线, 不由固定交错用例推出全部并发安全已证明.

当前 [审核](review.md) 中的未知范围配额、共享 Client 关闭可见性、Go 单调时间新鲜度修复, 连同 Edition 有序后缀、Polaris 历史前缀裁剪、共享批次和锁外准备, 已包含在此前相应普通回归基线. 范围进度排空和重复 TTL 调度仍是优化候选, 不把理论收益当成实测结果.

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
- 多进程: [cpp_comet_process](../astra/test_comet_process.py) 启动专有 Pulsar/Polaris/Astrolabe/三台 Star 和 SDK 探针; 持久重启、缓存可读、凭据脱敏、实际指标抓取、三来源同时写入、九条路径内容核对、静默续租、强制结束原 Star、SDK 换节点/新 UUID、旧租约到期、原端口重启恢复. 长测可轮换故障并增加多记录常驻阶段.
- 管理: Go HTTP/KDF/Cookie/CORS、指标身份/长度/部分正文; [Admin 适配用例](../admin/tests/management.test.mjs) 验证 uint64 精度及版本耗尽、UTF-8 跨块、半份快照拒绝、单次写入不重试、错误正文脱敏和陈旧状态. 界面轮询等待本轮全部读取结束后再调度, 读取失败将旧指标标为陈旧. 这些用例和构建入口保留; 本轮已完成 Admin 自动化、类型和生产构建, 未执行真实浏览器交互.

## 已知限制与后续验证

业务 RPC 批量化已完成静态评估: 保留现有单目标协议, 将共享 Client 续租密度和多精确 Watch 开销纳入 [性能矩阵 B10/B11](../testkit/comet.md#rpc-batching), 确认热点后再决定是否扩展. 已保留协议边界并增加可复现的 [性能入口](../astra/bench/README.md), 当前 Schema 仅变更标识字段编码并同步生成代码; 不宣称已有批量续租、多目标订阅或跨 Key 原子发布.

1. 下行索引锁和同轮调度调整已有普通回归及优化 A/B 支持. 32→8 额度候选未纳入生产. 分层探针已经覆盖回调/消费和发送入口, 下一步优先细分下行 advance 与对等 begin_write, 再验证有界就绪调度和通知合并. 不直接引入 Actor/RCU. 边界见 [审核](review.md#优化分析的证据边界).
2. 来源最终合并/投影/预算仍串行, 当前增量路径已测域锁/导出锁及持有时间, 但未测大快照恢复和更高写并发. 下行共享不消除逐订阅 pending 收集及逐流序列化. 未采集分配调用栈或覆盖率百分比, 不宣称完全拆锁或零复制.
3. 当前指标为实际已实现的固定状态 gauge, 未伪造请求计数、延迟直方图或每 Scope 安装进度. Admin 当前以表格管理和观测, 完整实时 3D 拓扑仍属后续 Orrery.
4. 本轮未重跑 Admin、Debug、Sanitizer/race, 不将此前通过记录当作新增代码的当前证据; 既有第三方/前端提示不因本轮普通测试通过而自动消失.
5. 最新有效证据位于 build/profile-verification/evidence.zip, 包含源码/夹具/产物摘要、回归与全部性能样本、原始探针和离线结果. 所属进程全部退出; 最新汇总只维护 validation.md, 较早证据不混入当前采样统计.
