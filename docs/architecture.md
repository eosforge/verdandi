# Astra 当前架构

本文维护已确认的组件边界、数据权威和实施边界. 当前核心已接入 Pulsar、Polaris、Star 三域业务与多 Star 恢复、原生 Comet C++ 和必要管理功能. 具体实现及尚未推进的组件见 [进度](progress.md), 实际构建和用例执行见 [验证记录](../testkit/validation.md); 文档选择或代码存在本身不代表验证通过.

## 组件与阶段

| 组件 | 当前目标职责 | 实现边界 |
| --- | --- | --- |
| Pulsar | 基础设施准入、成员登记、连续 Unix 时间参考 | C++ 实现; 时间字段使用纳秒单位, 不承诺纳秒准确度或用物理时间仲裁业务版本 |
| [Polaris](../astra/polaris/README.md) | Almanac 唯一发布权威与持久存储 | Go + GORM + SQLite; 首版由部署保证单点, 不实现选主或自动主备 |
| Star | 三类数据的内存状态、订阅分发及对等复制 | C++26, 不引入业务 SQLite 或异步业务落盘 |
| Comet | 原生业务接入、视图与对象生命周期 | 首版为 astra/comet/cpp, 不连接 Pulsar; Go/Rust 等其他语言后续推进 |
| Astrolabe | 必要的管理入口及实时观测 | Go; 可以先实现核心管理功能, 不引入自身持久数据库或外部监控系统 |
| Orrery | 管理与星图界面 | 由 admin/ 继续演进, 完整真实界面暂缓 |
| Planet / Moon | 中继、边缘接入与聚合 | 提案阶段, 不作为本轮业务闭环的前置依赖 |

取消架构级 standalone 降级分支. 单机或 All-in-One 只作为部署形态, 使用同一组核心组件和协议; 不因部署在同机就绕过内部初始化链路. 旧 Go Supervisor 和既有 Planet 骨架的存在, 不代表它们是新业务架构的新增实现范围.

Polaris 已确认使用 GORM 官方 CGO SQLite 驱动与 WAL + synchronous=FULL, 持久提交、连接配置及恢复要求只在 [Polaris](../astra/polaris/README.md#sqlite) 定义. Pulsar 的独立成员库仍使用 DELETE + EXTRA; 两者不共享数据库或驱动层.

Polaris 保留独立服务, 不合并进 Astrolabe. Astrolabe 不拥有持久库, 其重启或观测故障不直接中断 Polaris 向 Star 同步及新 Star 恢复. 管理修改仍通过 Astrolabe 提交给 Polaris; 管理入口不可用不等于已经运行的权威发布或 Star 数据面必须停止.

## 三类数据

<a id="data"></a>

| 类型 | 内容与权威 | 持久化 | 接口方向 |
| --- | --- | --- | --- |
| Almanac | Polaris 发布的权威数据, Star 接收快照及连续更新 | Polaris SQLite | authority.commit() / reader.load() |
| Ephemeris | 固定 Attr、可变 Data 与 TTL, 来源 Star 管理其受理的注册 | 无业务持久化 | beacon.renew() / observer.select() |
| Catalog | 带 TTL 的动态数据, 每个 Key 由业务保证单一发布者 | 业务自行持久化内容及版本 | publisher.publish() / subscriber.watch() |

三域使用各自原生结构, 不再统一为 map<Sector, map<Spectrum, Store>>. Sector/Spectrum 是业务地址; Almanac 按 Scope 管理, Catalog/Ephemeris 按来源 Star 分组, 各来源在各域拥有一条覆盖全部 Scope 的复制序列. 本机组只广播直接受理的事实, 远端副本不再次广播. 具体记录、索引和版本职责只在 [三域存储](../astra/common/README.md#data) 定义.

### Almanac

唯一发布权威为 Polaris, Astrolabe 是管理写入口, Star 不互相同步 Almanac. 单 Key Set/Delete 按 Scope 权威版本严格 +1 提交; 成功表示 Polaris 持久提交, 不表示全部 Star 已安装. 有界历史不足时恢复完整范围, 正常重启保留数据版本. 事务、原请求确认及 [安装版本核对](../astra/polaris/README.md#assessment) 由 Polaris 文档定义.

Comet Reader 持续同步并返回不可变视图, 保留同一范围已观察到的权威版本下限. “已追平”只表示取得接入 Star 的完整视图, 不证明该 Star 已取得 Polaris 最新提交. 读取与跨节点恢复见 [Almanac 协议](../proto/README.md#almanac).

### Ephemeris

来源 Star 负责完整注册、Data 更新、续租及权威结束, SDK 后续操作固定到该来源实例/UUID. 来源到期或切换 Star 后重新创建 UUID. 副本本地到期只影响本地及下游, 仍有效的来源新期限可在补齐 Attr/Data 后恢复相同 UUID.

已可信替换的旧来源保留原 TTL, 拒绝旧进程后续变更, 不立即整批删除或续满期限. 注册、独立 Data/Renew 顺序见 [Ephemeris 协议](../proto/README.md#ephemeris), 回补与来源替换见 [来源恢复](../proto/README.md#replication).

### Catalog

业务按 Key 保证唯一发布者和同版本唯一内容. Publish 可以提交更高正版本, 不要求逐版本补齐; 合法同版本完整发布与纯续租也是新的保活. 写入成功只表示当前 Star 内存提交. Publisher 固定一个 Key/TTL 并保存最新期望, 自动续租及换 Star 完整恢复, 没有发布 FIFO 或 Delete.

Star 保留其已知最高内容版本到当前进程结束, 内容到期不清水位, 容量满时拒绝新增. 来源水位与合并水位分开, 本地到期不向对等广播删除. Subscriber 跨 Star 可以暂时看到较低版本或缺项, 不在 SDK 永久保存逐 Key 水位. 接纳、期限、同版本恢复及水位合并只在 [Catalog 协议](../proto/README.md#catalog) 和 [来源水位](../proto/README.md#catalog-watermark) 定义, SDK 生命周期见 [Publisher](../astra/comet/cpp/README.md#catalog).

## 准入与内部边界

APIKEY/APISECRET 只负责 Comet 准入, 登录后可访问全部普通业务范围, 不建立 Grant、读写 ACL 或注册所有者. 外部入口始终拒绝以 __ 开头的 Sector; 凭据属于内部 Almanac["__auth"]["comet"]. 业务认证/TLS 开关和内部受保护入口分开, 见 [监听边界](../astra/README.md#监听与配置入口).

Session 有效性仍在请求提交/发送边界与撤销定序, 不逐次重新校验 SECRET. 凭据完整快照跨过中间历史时统一使旧 Session 重新认证; 连续补丁及相同版本重放的行为由 [凭据协议](../proto/README.md#credential-snapshot) 唯一定义.

基础设施沿用 Pulsar 准入. Polaris 采用 [固定部署和同库重启](../astra/polaris/README.md#deployment), 首版无迁移或自动接管; Pulsar 新 SQLite 后端只支持显式初始化新群组和恢复该库, 不导入或清除已有 journal. Astrolabe 使用 [部署管理账号与内存会话](../astra/astrolabe/README.md#管理账号), 不混用浏览器、Comet 和节点身份.

## 启动与运行

已确认的业务启动顺序为 Pulsar 准入及首次可信校准、Polaris Almanac 完整初始快照、Star 首轮互联与动态数据同步, 然后开放业务. Almanac 初始快照包含合法空状态及内部凭据范围, 未完成时不能用空数据代替成功. 动态数据包含 Ephemeris 和 Catalog, 不要求所有业务发布者先在线才能开放 Star.

Star 从 Pulsar 名单发现同 Galaxy 的唯一 Polaris, 主动建立一条内部双向同步流, 接收其推送并报告已安装版本. 不另配 Polaris 地址、不由 Polaris 反向拨号, 不按 Almanac Scope 建连接. 缺少目标时退避等待, 多个独立 Polaris 属于部署冲突而非自动选主候选; 发现、身份及重连规则只在 [Polaris 同步流](../proto/README.md#polaris-stream) 定义.

登记后通过轻量只读 RPC 按需及低频刷新成员名单, 使用已签发身份, 不重复密码登录或持久登记. 每个进程复用一个有界刷新调度, 带抖动和退避; 当前完整名单与实际在线状态分开, 不引入目录长流或全局 revision. 身份替换、迟到响应及资源规则只在 [成员查询](../proto/README.md#directory) 定义.

内部监听及恢复入口在对应同步阶段可用, 不等待公共业务开放, 避免同时启动的 Star 相互等待. 首轮对等连接和动态数据同步采用有界等待预算, 到达预算后允许降级开放业务, 明确记录尚未完成同步的来源并在后台继续恢复. 缺失来源可暂时没有可见记录, 不把超时、半份快照或容量拒绝计为完整同步; 登记名单不能直接作为在线证明.

本地业务就绪和来源同步进度分别报告. 降级开放不承诺全网动态数据完整, 对等等待预算也不能跳过 Pulsar 准入、首次可信校准或 Polaris 初始快照. 首轮采用明确的成员集合边界, 后续新成员进入后台同步, 不因成员持续加入无限延长本次启动.

已运行 Star 可以承受其他组件暂时不可用. 首次可信校准后, 本地计时正常的 Star 在 Pulsar 离线时继续推进 TTL、受理新租约和续租; 同步质量另行报告, 不使用 5 秒观测年龄拒绝这些操作. 具体时间模型见 [Pulsar](../astra/pulsar/README.md#clock).

新 Star 主动连接名单中的其他 Star, 每对 Star 收敛为一条双向 gRPC 逻辑流, 双方在同一流发送各自负责的 Catalog/Ephemeris 数据. 同时拨号的裁决和旧流完成隔离见 [Star 双向流](../proto/README.md#star-stream). 当前 Topology/Runtime 已接入单流裁决和双向复制; 入站与出站实现类仍分别承担 gRPC 生命周期, 不表示每对节点保留两条业务流. 逻辑流数量不等于底层 TCP 连接数量.

Catalog/Ephemeris 各按 (来源 Member.id, 业务域) 维护组版本及连续恢复位置, Scope 仅定位组内记录. 来源历史不足时恢复本来源、本域全部 Scope 的完整组快照, 不再为每个 Scope 保存复制位置; 空组和裁剪不重置版本. 同域源端提交需要短边界定序, 组恢复覆盖和准备规模也随之变大, 具体边界见 [来源恢复](../proto/README.md#replication). SDK 仍使用接入 Star 按 Scope 维护的合并视图游标, 不参与来源对账.

缺少一条载荷时已确认精确请求该 Key/UUID 的完整来源记录, 不因此重取整范围. 回补可能比当前处理位置更新, 只能覆盖该目标, 不能跳过其他 Key 的事件或让旧帧覆盖修复结果. 同目标请求合并, 回补/标记/积压有界; 具体规则只在 [单条回补](../proto/README.md#repair) 定义. 历史断档仍走完整来源组快照, 不因精确回补取消这一保底路径.

Almanac 的低频版本核对不直接扩成所有 Star 两两定时扫描. 上述来源组进度和回补复用现有双向流, 不加入新的全局业务代次、每 Key 连接或跨三域业务写锁.

## 首版最小实现

<a id="minimal"></a>

| 实现位置 | 保留的最小职责 | 暂不增加 |
| --- | --- | --- |
| Pulsar | 一个持久登记路径、一个只读目录入口、独立 Pulse | 新目录日志、业务存储、多时源系统服务 |
| Polaris | 当前底稿与有界历史、单 Key 事务、每 Star 一条同步流 | 第二份 Outbox、逐 Star 持久发送队列、通用 Repository |
| Star | 三域原生结构、直接索引、来源历史与下游投影、共享期限调度 | 通用 Store 包装层、逐 Scope 线程/时间轮、每对端数据副本 |
| Comet C++ | 一个 Client 共享核心、三域对象、私有 Watch 流程 | Task/Executor、完整发布 FIFO、每对象连接或线程 |
| Astrolabe | 部署登录、同步管理转发、有界最新观测 | 自身数据库、持久补发队列、内容源适配器和长期时序库 |

最小实现按真实职责共享机械能力, 不把三种业务强行塞入通用记录或统一版本. 同步发送只合并运输工作, 不改变来源提交或 Almanac +1 语义; 具体见 [批量发送](../proto/README.md#stream-batching). 数据持有和锁边界见 [原生索引](../astra/common/README.md#indexing) 及 [恢复预算](../astra/common/README.md#capacity).

全互联的结构性成本仍存在: n 台 Star 有 n(n-1)/2 条逻辑对等流, 每条本机动态事实通常发往 n-1 个对端. 批量发送和共享载荷减少本地重复工作, 不消除这种扇出, 也不使来源组全量恢复成为 O(1). 首版保留已确认的来源/域序列和短提交保护; 按 Scope 拆复制序列、进一步分锁、预编码跨连接复用或无锁回收只在测出对应瓶颈后单独评估, 不提前变成实现前置条件.

## 后续实施边界

原生数据单元、真实单 Star 业务、Comet C++、完整来源复制及两台 Star 的进程故障恢复已接线并有回归基线. 后续优先验证最新修复和当前规模边界, 不重复搭建另一套单机协议. 单 Star 部署仍使用真实 Pulsar/Polaris 和必要 Astrolabe 管理入口, 不恢复 standalone 分支.

内部流 Schema、类型化索引、调度持有关系、资源初值及必要管理 HTTP/RPC 已落到实现. 容量配置必须能容纳单条合法编码与完整恢复峰值, 不能先按正常写入占满全部预算再期待恢复自行成功. 当前仍有来源安装域锁、订阅索引及调度扫描的 [性能缺口](review.md#performance); 大小、并发和锁成本按 [验收计划](../testkit/comet.md) 分层测量, 不以“采用现代 C++”或“使用 gRPC”代替性能与安全证据.

Polaris 保留独立服务及已选 GORM/SQLite 模式. 本轮已授权完整推进三域及多 Star, Comet 暂只实现 C++; Comet Go、Planet、Moon 和完整 Orrery 界面继续搁置, 不作为本轮交付内容. 当前检查点和阶段顺序见 [推进进度](progress.md). 依赖版本与下载授权仍在实施前核对; 实现授权不授权自动下载、构建或运行测试.

## 文档与验证边界

本页只定义组件关系、业务权威和阶段. 协议字段归 [proto/README.md](../proto/README.md), 存储实现归 [三域存储](../astra/common/README.md), C++ API 归 [Comet](../astra/comet/cpp/README.md), 持久事务归 [Polaris](../astra/polaris/README.md). 不在架构页复制各文的字段、恢复分支和数值配额.

旧 Catalog/Registry 两分法、统一 Store、双物理 Key、Grant、standalone 和 Astrolabe 持久库均不作为新功能实施依据, 冻结 SDK 的历史 API 保持其原意. 最新实际验证只维护 [validation.md](../testkit/validation.md), 保留真实日期、源码身份与边界; 设计、验收条目和静态文档检查不能转记为功能通过.
