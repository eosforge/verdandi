# 当前代码审核

本轮新增动态域公共 Pagination 与 Reading, 已完成静态审阅及用例扩展, **尚未构建或测试**. 此前 SDK 就绪弱队列、多页弱缓存和 Catalog 2048 字节场景已通过的结果只对应原源码清单, 不覆盖本次抽取. 已测范围统一见 [validation.md](../testkit/validation.md).

本页记录当前实现的静态审核结论, 不保存逐轮日志. 审核范围为活动的 Pulsar、Polaris、Star、公共 C++ 核心、Comet C++、Astrolabe、Admin 管理入口、协议、构建和测试接线. 冻结 Redis SDK、旧 Supervisor/Rust 网络、Planet/Moon 不重新推进; 生成代码和第三方代码以 Schema、生成接线、版本与 ABI 边界审查为主, 不手工重写.

结论: 三域权威、来源复制、不可变视图和真实业务闭环已经落地, 不需要再次统一为 Store 或重建网络框架. 当前最值得优化的是每次事件触发的重复扫描、每个订阅重复准备的状态、视图构建的固定成本和域锁内的工作量. 语法、容器或传输库的整体替换不是优先方向. 本页区分已修复问题、已测现象和静态候选, 不宣称全部生产规模、故障组合和性能目标已经验证.

当前 HEAD 为 alpha `0f4fd34`. 工作副本中的远端 Replica 独立准备锁、域锁外原生候选、同范围下行批次/在途页共享和 Ephemeris 范围覆盖修复已通过普通 Release 回归, 包括确定性交错、分配失败及多 Watch 用例. 三 Star 前后对照的当前产物均通过最终数据核验, 性能收益不均匀, 不宣称全面加速. 本轮未运行 Sanitizer、下载或提交. 实际日期、源码身份、失败样本和性能证据统一见 [validation.md](../testkit/validation.md).

## 动态域复用边界

Catalog/Ephemeris 的成对文件确有共同机制, 但不能由文件名推导出 99% 语义一致或可直接删除 3000..4000 行. 当前 Catalog 是每 Key 内容版本、载荷与有限期限, 不携带权限 Credential; 凭据由内部 Almanac 承载. Catalog 过期后保留水位并合并多来源, Ephemeris 则按来源拥有 UUID, Attr 固定, Data/续租各有独立 order. 这些差异影响回滚、删除、恢复与投影提交, 不只是编码字段.

| 部分 | 当前取舍 | 原因与验证边界 |
| --- | --- | --- |
| 公共根与来源日志 | 继续使用已有 Pages/Origin/Scene 模板 | 已共享 COW、候选提交和日志机制, 不是两份独立实现 |
| 完整来源恢复与流 | 继续使用 Restore、Borrowing、Dispatch、Landing 和 Exchange::Pipe | 恢复、ACK、回补和来源准备锁已有共用实现. receive 中独立消息分支只是类型安全路由, 不是重复的整个状态机 |
| 动态域公开分页 | 新增 [Pagination](../astra/star/src/pagination.hpp), 两个 Edition 仅选择各自私有 Encoding | 统一冻结引用、后缀排序/压缩、页预算、失败不推进位置及完成标记. Catalog 末值与 Ephemeris Attr 基线由各域 merge/编码保留, 不引入运行时域判断 |
| 公开读取锁边界 | 新增 [Reading](../astra/star/src/reading.hpp), 两个 State 委托 execute | 统一共享快路径、独占推进、释放域锁等待来源、重取时间及锁外回收. advance 的业务删除/合并仍由 State 决定, 不改成简单读出口过滤 |
| SDK Watch 生命周期 | 继续使用 Watching<Projection/Subscription/Selection> | 三域已共享网络恢复、取消、回调与计费. subscription.cpp/selection.cpp 是内容安装, 并非各写了一份网络状态机 |
| SDK 写入 | 保留 Publishing/Beaming, 继续共用 Core/Lifetime/请求基础设施 | Publish 可以在新 Star 以完整内容恢复; Beacon 需要重新分配 UUID、独立 Update/Renew 尝试、缺 Attr 恢复与尽力注销. 泛化为单一 Writer 会把业务分支搬进策略, 不能自动消除状态 |
| SDK 内容安装 | 仍有页头校验、去重和候选计费重复, 留为后续局部抽取候选 | Ephemeris 缺 Attr 只允许一次强制 reset; Almanac 还保留权威版本下限. 抽取应共享候选事务并显式保留这些差异, 不增加新的恢复状态机 |

本轮生产源码物理行数净减少 74 行, 已扣除新增公共头并保留中文说明; 该数字不是性能指标. 共用模板仍针对不同内容类型实例化, 不能据此承诺二进制变小或编译加速. 没有新增虚调用、type erasure、运行时领域选择或正文副本; 字段长度校验内联, 消息追加仍按各域静态派发. 实际产物大小、构建时间与运行性能需另行验证.

新增两域共用的空/零字节点查、独立复制位置、错误后位置保留、合法大页/硬预算拒绝及过滤前批次校验; 另覆盖逆序同 Key 合并、Catalog 业务版本与游标区分、Ephemeris Create+Data 保留 Attr、末次删除和冻结后变化. 原有读取/回收、准备交错与异常用例继续作为 Reading 的回归入口. 这些用例本轮尚未执行, 不创建 Planet/Moon 或新域占位策略.

## 当前锁与调度整理

当前新增实现以缩短既有同步边界为主, 不新增传输协议或客户端恢复状态. 同步规则集中定义在 [原子提交与锁边界](../astra/common/README.md#commit), 本节记录审核结果与剩余限制.

- 远端原生候选持 Replica 私有锁, 不持域锁枚举旧项、校验/复用正文或分配原生页/期限节点. 同来源等待会释放域锁, 稳定 shared_ptr 保证退役/回收期间对象寿命. 最终公共投影、合并水位、预算、通知仍串行; 未把整段安装或所有 Scope 写入变为完全并行.
- 移出准备后必须重查身份和时间, Catalog 使用本地并发提交后的水位合并. 到期清理遇忙来源只尝试锁; 公开读取在域锁外等待后重新推进, 不能返回过期投影. 两域并行准备/本地写入/到期/异常注入和同 Key 竞争用例已通过普通回归.
- 相同 Scope/目标/游标区间共享冻结 Edition, 最近公共前缀可供其他流继续使用, 更高事件仍留在其待发区. 活动消息页用共享不可变所有权保持到各自 OnWriteDone, 只保存弱缓存. 排序/批次准备移出下行队列锁; 原始 changed 收集仍逐订阅维护 pending, 共享消息不等于 gRPC 序列化零复制.
- Ephemeris 逐 Scope 安装曾清空全部 coverage, 现仅替换当前范围记录, 完整来源确认后才清空全部. 两域三个 Scope 中途停止并接续旧增量的回归已通过. 共用测试头补齐自身包含依赖, 不再依赖测试翻译单元的包含顺序.

- 自有 Origin 独立 `export_` 读写锁: `source/events/deliver/resolve` 不再进入域锁或调用 GC; 本地原生提交按 `gate_ -> export_` 顺序发布. 原生期限由接收端验证, 导出不会延长 TTL. 远端来源安装仍需域级写锁, 尚未变成全量每 Scope 并发提交.
- 公开读取在下一整拍之前使用共享锁, 跨边界才重新独占取时并提交到期删除. `due_` 记录所有轮真实相位的最早下一拍, 新轮/来源退出会更新它. 不截断时间追赶, 不在每条记录添加第二份期限. 不单独过滤 `find`: Scene 不保存 TTL, 静音续租也不改 Scene; 单独过滤会破坏点查、快照、后缀和游标一致性.
- Comet RPC/Watch 完成进入按对象去重的弱引用就绪队列, 普通事件只消费实际就绪对象. 期限到达和共享绑定/关闭/满额归还仍扫描目录, 不降为每秒检查自动续租. 处理期间新到事件可再次入队, 最多八轮后让出线程, 临时强引用在锁外释放. 接纳时预留队列容量, 唤醒不分配且不保存裸指针; 目录移除撤销旧对象入队资格. 调度细节见 [SDK](../astra/comet/cpp/README.md), 普通回归通过, 大量空闲 Watch 中少数活跃的收益仍待定向测量.

用户此次五段建议包含三个独立问题. SDK 的 O(N) 事件消费与 Broadcast 单槽覆盖成立, 已分别改为就绪队列和每页弱缓存; drain 是 O(K), 不是与事件数无关的 O(1). 当前 Client 的 Watch 上限为 4096, 不是示例中的 10000, 但不影响问题成立. 分页缓存共享 Protobuf 消息对象, 不承诺复用 gRPC 的最终线编码. 多页缓存的元数据随冻结批次页数增长, 正文不强缓存, 并防止非连续下标扩容.

读出口直接过滤期限的建议未采用. 两动态域的 Scene::Content 不含 deadline, 纯续租也不改投影; 只改 find 不能覆盖 capture/changes/Watch, 按读取时刻过滤同版本快照还会破坏游标含义. due_ 是来源轮下一整拍的维护边界, 越界不等于所有记录已经过期或每次进行全量 GC. 共享锁也不是无锁. 保留到期删除的统一版本提交与短共享读路径, 剩余锁尖刺作为测量方向, 不引入第二份 TTL 状态或提前假报性能结论.
- 完整来源恢复每轮仍只安装一个 Scope. 尚有本地候选时显式唤醒下一轮, 不再为每个 Scope 等待 10 ms 兜底定时器. 实际在途写仍等待 gRPC 完成, 网络等待不自旋.

### 传输建议的取舍

- 高频流式续租可以减少重复 RPC 建立, 但必须管理流恢复、关联与背压. 现有 Unary 已有实际 SDK、重试和错误契约; 本轮先隔离锁和调度收益, 不凭理论引入第二套写入流. [gRPC 性能指南](https://grpc.io/docs/guides/performance/).
- gRPC Callback 包装器已经使用其内部 arena; 这不代表所有 Protobuf 子对象都使用 `google::protobuf::Arena`. 额外 Arena 必须结合具体消息分配器和异步寿命验证, 跨 arena 移动可能复制, 也并非永远没有析构列表. 本轮不添加无分配剖析依据的池. [Protobuf Arena](https://protobuf.dev/reference/cpp/arenas/).
- 固定版本 gRPC 的 optimization target 为小写 `latency/throughput/blend`; 默认 blend 当前等价 latency. minimal stack 改变可选功能默认值, 不是任意跳过拦截器的开关. 不把重复默认值作为性能优化, 不关闭现有观测和故障语义来提高报数.
- 当前 Exchange 需要合并版本、判定期限及生成不同接收者的消息, 不是透明字节转发. `ByteBuffer` 可共享已编码数据, 不代表 NIC 到业务全链路零复制. UUID 继续采用已确认的文本契约, 不为理论 20 字节收益重写全线协议.
- 小消息维持不启用全局 gzip. 2 字节 Protobuf 只是部分 payload, gRPC 的 5 字节前缀之外还有 HTTP/2/TLS/TCP/IP 开销, 不能宣称线上心跳只有 7 字节. [gRPC HTTP/2 协议](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md).

## SDK 与 Scope 并发复核

当前锁与共享批次改动已完成普通回归、三 Star 基线和恢复争用诊断, 以 validation.md 的源码身份为准. 标准拓扑保持三台 Star 同时写入及跨节点订阅, 详见 [基线契约](../testkit/baseline/README.md). 单 Star 与冻结 Redis 仍作为诊断对照, 不预设多线程必然超过 Redis, 也不靠增加总资源来宣称优化成功.

### SDK 的实际状态

- [Watching](../astra/comet/cpp/src/watching.hpp) 已经使用共享 Client 完成连接/准入, 每个订阅只开一条固定范围的有序 Watch. SDK 不复制 Star 的来源组, 不上传逐 Key 版本清单. 重连只携带最后完整 Scope 游标, 半批候选不进入应用 View.
- 真正需要保留的是未完成页、最后完整视图、取消后等 OnDone 的所有权和有界预算. 不能用“连接断开即释放”替代 gRPC 实际完成, 也不能移除不可变旧 View 契约来缩短代码.
- 本次将 available/consumed/ended/overflow 四个可组合标志收敛为一个互斥 Stage, 顺序为 reading -> ready -> consumed -> reading, EOF 和超预算分别终止. 超时只针对实际等待 Read 的阶段, 不覆盖已读 EOF 的收尾. 保留独立 hold/OnDone 和关闭状态, 不额外建立队列或执行器. Reader/Subscriber/Observer 共用此实现; [故障用例](../astra/comet/cpp/tests/catalog_fault_test.cpp) 补充首帧/半批 EOF 和关闭并通过普通回归.
- Core 仍串行安装同一 Client 的收到页并派发用户回调, 单轮仍遍历活动对象. 这是需要三节点实测归因的限制; 阶段枚举整理本身不应被报告为吞吐提升.

### Star 已确认的耦合

| 边界 | 当前事实 | 拆分必须保持的内容 |
| --- | --- | --- |
| 域级提交 | 两动态域各一个 gate. 本地写入、远端增量及快照最终投影发布持独占; Replica 原生候选独立锁外准备, capture 无整拍推进时共享, deliver 独立 | 两域继续特化. 公共合并和预算仍需协调, 不声称每 Scope 都可以并行提交 |
| 本机来源 | Origin 的 position/history 跨本来源全部 Scope 连续, 只有一个活动 Edit, 同时有全来源页树与目录 | 来源位置分配/历史接续需要短提交边界; 不能简单把每个 Scope 都变成独立 position 而继续发送原协议 |
| 公共 Scope | Catalog 同 Key 要合并多个来源版本/期限; Ephemeris 要维护 UUID 归属及公开在线集合. Scene 还有自己的历史和稳定游标 | 同一 Scope 的公开变更需要定序. 独立来源可以并行准备, 但不能承诺同时修改同一 Scope 时完全不协调 |
| TTL | advance 在所有 Agenda 最早下一整拍前 O(1) 返回; 跨界才遍历各副本, 自有发送 deliver 已独立且不触发 GC | 大量同时到期仍会增加域锁工作. 到期最终提交必须复查原记录, 避免旧到期事件删除刚续租的记录 |
| 全量替换 | 完整 Draft 接收后每轮安装一个 Scope; Replica 独立准备原生根, 最终同范围原生/投影/期限共同提交 | 不同 Scope 先后可见, 失败保留已装范围及其覆盖证据, 全部完成才 ACK. Scene::Batch 仍只在其 gate 内准备 |
| 读完成屏障 | Origin/Scene 的 View::Fence 持有原 gate, 保护共享页随后恢复唯一拥有并原地修改的顺序 | 改锁必须同时改对应根的同步域, 不能让读者仍锁旧 gate、写者却只锁新 Scope 锁 |
| 预算/通知 | 远端字节和下游历史共用域预算, notify 在提交锁内按序入队 | 全局额度使用短预留/提交边界; 同 Scope 通知不能在解锁后反序, 分配失败不能导致成功提交永久丢通知 |

用户已确认 B 并已实现逐 Scope 恢复. 未完整接收的 Draft 不发布; 收到 complete 后由 Restore 在控制轮逐 Scope 安装. 来源 position 保持最后完整前缀, 已装 Scope 用有界内部覆盖证据拒绝旧事件. 中断、后续容量失败、TTL 清理不会撤销已公开数据, 也不会伪造 ACK. 容量失败可在其他范围处理后重试一次. 覆盖范围和精确回补分别保留, 取匹配的最大覆盖位置, 整组完成后清理. 没有增加协议字段、重连代次或新的客户端状态.

本次同时为 Origin 内部批候选增加删除、按 Scope 遍历及最终确认, 覆盖删除后槽复用、放弃、同键重复修改和旧根有效性. 两个域恢复不再维护范围到多计划的中间 map/list, 当前范围只有一份变化列表和一个可选 Scene::Batch. Origin/Scene 读完成屏障始终使用所属根的提交锁: 自有来源改为 export, 公开投影/远端来源仍用 gate. 写者与读区同步域同时变更.

短目录锁、每来源独立提交锁、每 Scope 独立投影锁仍是后续结构目标, **本次没有宣称全部拆锁完成或复制与 SDK 完全无竞争**. 必须结合三 Star 基线确定剩余域锁、全副本 TTL 推进和 SDK 调度各自占比, 不能将恢复分步误写成常态吞吐已线性扩展.

当前组件回归覆盖准备暂停期间本地写入、其他来源准备、TTL、退役、内存拒绝、旧 View 及通知边界, 三 Star 基线覆盖同时写入与跨节点订阅. 大来源恢复仍主要用原生探针诊断, 未形成跨机器进程矩阵. 缺少锁等待/持有时长证据, 不能宣称 SDK 写入已经与复制完全隔离.

## 本轮已验证整理

- Core 工作数组复用容量, 常态不执行关闭用 all_of. 每轮退出通过局部 RAII 在锁外释放强引用, 不产生 Core/Activity 跨轮拥有环. 当前区分 O(K) 就绪消费和 O(A) 期限/共享状态维护, 未改变回调执行上下文; 新队列的重复唤醒、通知中关闭、迟到事件及反复接纳用例已通过普通回归.
- Table 缓存页头/桶空间, footprint 为 O(1); 脏页发布用四个 64 位占用字定位. 哈希表复制后的实际桶数、插入扩桶、删除空页都更新计量. 根自身的 256 个引用交接仍存在, 不宣称根发布整体只与脏页数有关.
- Table 单记录 key.size + payload 的加法补充独立溢出检查. 生产协议已经限制键/正文大小, 此处属于私有泛型容器边界加固, 不宣称已有合法 RPC 能索取 SIZE_MAX 字节.
- Table 覆盖已有 Key 使用借用输入与既有迭代器, 只在新增时拥有式复制名称, 不重复哈希/计量桶; 已修改页的 Delete 同样避免 contains/find 两次查询. 页首次 COW 仍会复制页中名称, 不宣称整个更新零分配. 长/短输入缓存复用、重复覆盖/删除及旧根用例覆盖拥有关系.
- Almanac/Catalog/Ephemeris 三种安装器均合并常态 contains/insert; 只有完整批次恰好单页单项时跳过去重分配. 跨页尾部即使单项仍完整去重, 满额保持“重复先报 protocol、新项报 limit”的错误次序; 完成/失败仍归还全部桶. 新用例核对单项/分片准备计费、跨批复用同 Key 和非满额跨页重复.
- 五类 Comet 工厂用 expected::transform 表达成功后的所有权移交, 失败仍在原作用域销毁未接纳对象, 不创建异步执行器. 三域 Edition 用 ranges 引用投影和 tuple 字典序比较, 保持按 Key/版本排序与 Attr 合并顺序; 动态域移交捕获根/点查内容, 清理 Catalog 私有编码器未使用的 Data-only 参数.
- Astrolabe 部署解析和 Admin 响应原本分别拒绝超过 64 项的指标, 与完整目录观测目标不一致. 已取消独立数量上限, 配置整体保留 8 MiB 字节边界, 前端容量与目录一致; 未采集 Star 不再从指标结果中消失. 并发仍为 4, 单端 2 s/64 KiB, 空闲连接池按目标数量保留. 结果核对使用同边界目录索引, 消除每项锁内全目录扫描; JSON/文本读取省去完整字节到字符串副本, 取消/完成共用退出责任. 新增 128 节点、未配置节点和在途身份替换用例, Admin 同步覆盖大目录与重复身份拒绝. Orbit 没有指标地址字段, 自动发现尚未实现.
- Ephemeris Update 最终只重新检查原租约活性, 不重复比较已确认不变的 Data; Renew 仍从最终读数计算期限. Catalog 最终 publish 复用首次校验后选定的正文引用, 保留版本、合并和期限检查. Exchange 复用一次响应线长计算.
- 新增 cpp_comet_table: 位图字首尾页、候选移动、旧根不变、独立参考表核对、清空后空间回收、放弃/溢出. 三域新增共用满额跨页重复用例; Client 清理覆盖真实 Reading 的拥有环; Ephemeris 新增同/异 Data 准备期间恰好到期的回滚用例. 均已随当前回归通过, 不以用例数代替代码覆盖率.

扩散检查: Table/去重/排序覆盖全部三种 C++ 投影或对应 Edition; Catalog/Ephemeris 分别检查最终写入复核, Almanac 无 TTL 不套用此精简. 指标覆盖限制同时检查 Go Astrolabe、Admin 解析/展示和部署文档; 原生 Pulsar/Polaris 不采用这套 Star 指标契约. 本轮 Protobuf 未变化, 不修改生成代码、冻结 SDK 或第三方代码.

## 确认问题与修复

| 优先级 | 触发条件及影响 | 当前修正与回归依据 |
| --- | --- | --- |
| P2 | Catalog/Ephemeris 对未知 Scope 的 capture/find/changes 调用 obtain, 永久创建投影目录. 顺序访问足够多的未知范围即可耗尽 scopes 配额, 后续正常写入被拒绝, 关闭订阅不能归还这些业务目录 | 两域只读路径改用 locate; 未创建范围返回版本 0 的空根、缺项或空后缀, 超前游标仍拒绝. Scene::empty 只保留同步域. 两个 State 用例覆盖 scopes=1 下读取四个未知范围、随后真实写入、满额后继续读取、旧空视图不变和写入额度仍有效. [Catalog](../astra/star/src/catalog_state.cpp), [Ephemeris](../astra/star/src/ephemeris_state.cpp), [Scene](../astra/star/src/scene.hpp) |
| P2 | Client::close 已生效, Publisher::state 只检查对象自身关闭位, 在下一个控制轮前仍可能报告 ready. Beacon/Watch 已检查共享核心停止状态, 同一 Client 的对象表现不一致 | Publisher 同步读取 core_->stopped. 新增 publications_closed, 在 Reader 回调内关闭共享 Client 并立即检查 Publisher, 以回调阻止后续控制轮来暴露原问题, 不靠 sleep 抢时序. [实现](../astra/comet/cpp/src/publishing.cpp), [用例](../astra/comet/cpp/tests/client_test.cpp) |
| P2 | Astrolabe 将抓取时间提前 UTC 化, 丢失 Go Time 的单调分量. 后续新鲜度计算退回墙钟, 校时前跳可误报陈旧, 后跳可延迟过期 | 缓存保存 time.Now 原值, 新鲜度按单调时间计算, 仅输出副本转换 UTC. 新增 TestMetricFreshness 检查成功采样保留单调分量、15 秒边界两侧状态及输出不修改缓存. [实现](../astra/astrolabe/internal/bridge/metrics.go), [用例](../astra/astrolabe/internal/bridge/metrics_test.go) |

Go 的 UTC/Round 会移除单调分量, 两个时间都含单调分量时 Sub 才使用它, 依据 [Go time 文档](https://pkg.go.dev/time#hdr-Monotonic_Clocks). C++ 活动租约/调度使用本地连续业务时间或 steady clock; 本轮跨语言搜索未发现需要同样修改的 Go 会话、目录计时路径.

以上修复保留既有命名、错误类别、业务版本、认证范围及线协议. 只读空范围不取消活动 Watch 的独立预算, 也不回收已经发生过真实提交的空 Scope 游标.

## 已吸收的精简和优化

1. 三域 Edition 原先对每批变化执行排序, 但实时 pending 本来就是按 Key 排序的唯一映射. 现在先作线性有序检查, 有序输入跳过排序; 历史回放仍支持 Key/版本交错与同 Key 压缩. 没有删除 Ephemeris 的 Attr 基线传播或扩大公开 API. 复用 [Almanac Edition](../astra/star/tests/edition_test.cpp) 和 [动态 Edition 用例](../astra/star/tests/ephemeris_rpc_test.cpp) 检查排序、合并及单项路径. 静态减少比较工作不等于已经测得吞吐收益.
2. [Polaris retain](../astra/polaris/internal/storage/commit.go) 原先在窗口满时把全部历史元数据装入切片, 即使只淘汰一项. 改为沿现有 (sector,spectrum,version) 主键读取必要前缀, 在同表 DELETE 前关闭游标. 不增加索引、缓存计数副本或持久队列. 新增 [TestHistoryPrefix](../astra/polaris/internal/storage/storage_test.go) 覆盖共同预算、多项淘汰、连续后缀和跨 Scope 隔离. 本轮 Go 常规测试已覆盖, 未重跑 race; 没有重新执行 SQLite 持久提交性能矩阵, 不声称已测该项前后收益.
3. 保留前序已完成的私有 execute、std::expected 链式转换和 requires 约束. execute 明确先构造 Retired、后构造锁, 返回及异常均先解锁再释放旧载荷; 不把它抽象为运行期 Task/Executor. 不适合链式表达的事务准备、回滚和多条件失败继续使用显式分支.
4. 保留 Scene/Origin 的历史修正: 物理上仍连续保留的后缀可以重放, 年龄只控制写入时裁剪. 业务 TTL 不因历史可读而续命; 数量/字节/连续性预算不取消. 已包含在本轮普通 Release 回归中; 本轮未运行 Debug 或 Sanitizer.

## 不应作为缺陷修复的机制

| 机制 | 审核结论 |
| --- | --- |
| Scene 的 string_view 索引 | Name 位于共享拥有的稳定对象, 当前树持有同一 Name; 移动 Event 不移动其 SSO 字符串, 淘汰历史也不释放仍在当前树中的键. 不需要因此改成每层复制字符串 |
| Edit/Batch 的防重入位 | 析构/移动/放弃由 RAII 交接, 异常退出有回滚路径; 用例继续覆盖移动后抛错, 不另建手动补偿状态机 |
| View 退出时的 Fence | 遍历不长时间持有写锁, 退出同步为后续 COW 唯一拥有者写入建立顺序. 不能把 shared_ptr::use_count 当成对象读写同步, 也不能宣称整个视图系统完全无锁 |
| 来源位置与公开游标 | 前者覆盖来源 Star 的整个域, 后者覆盖接入 Star 的单 Scope 合并视图. TTL、跨来源合并及内容变化会使两者不同; 不是应合并的双份业务版本映射 |
| Update/Renew 顺序 | TCP 的字节有序不保证多个 unary RPC 的服务端完成顺序. 独立 order 和重试原号仍有必要, 不能依据共享 Channel 删除 |
| 应用预算与 HTTP/2 流控 | 流控限制传输推进, 不限制已经解码的 Map、恢复候选、共享旧根和用户保存的 View. gRPC 的背压不能替代这些预算, 见 [官方流控说明](https://grpc.io/docs/guides/flow-control/) |
| 完整时间追赶 | 不能重新加入 max_ticks 并让新写入基于落后的内部时间计算租约. 若优化空轮跳跃, 必须保持截止边界、级联及异常重排的行为 |

## 架构与代码一致性

| 边界 | 当前实现及审查结果 |
| --- | --- |
| Almanac 权威 | Polaris SQLite 事务决定成功, Star 内存安装和 Watch 不反向改变权威版本; 正常重启保留版本, 管理回执丢失不能伪称未提交 |
| 动态数据复制 | Catalog/Ephemeris 原生结构不同, 只广播本机来源. 副本 TTL 删除只影响本地及下游; 精确回补不冒充整组连续 ACK |
| 冷启动 | Pulsar 准入和首次校准、Polaris 完整初始状态为前置条件; 动态来源有首轮有界等待及明确降级, 不恢复 standalone 分支 |
| 时间与持久性 | Pulsar 提供连续参考而非纳秒准确度保证. 首次校准后 holdover 保持 TTL 与新租约能力. Pulsar 成员库为 DELETE/EXTRA, Polaris 业务库为 WAL/FULL, 两者不混用 |
| 认证边界 | Comet 只登录准入, 无业务 ACL; __ 范围始终被公共入口拒绝. 内部角色和 TLS 独立, 关闭公共认证不能授权节点或直接写 Almanac |
| 客户端所有权 | Client 共享核心与 Channel, 已取消 RPC 的消息和额度保留到真实完成. future 与观察者、对象关闭与旧视图寿命分别处理 |
| 管理与展示 | Astrolabe 不保存第二份权威库, 同步提交给 Polaris. Admin 对 uint64 使用十进制字符串, 部分快照不安装, 管理写入不透明重试, 凭据读取脱敏 |
| 构建与资源 | 项目缓存/离线依赖解析、两端工具边界、Sanitizer ABI 和安装消费路径已有接线. 新下载、测试和提交仍需各自授权 |

已清理仍声称“单流未实现”“业务只读”“SQLite/三域仍是草案”等过时描述. 精确 Watch 只唤醒命中者的说法改为明确的当前差距, 而非以修改文档冒充完成性能优化. 规范、实施与执行证据继续分开维护.

<a id="performance"></a>

## 优化分析的证据边界

固定内存的 [三 Star / Redis 结果](../testkit/validation.md#baseline) 显示: 低负载两域都能完成计划速率, 高并发提交有多 Star 优势, 但多数全订阅可见吞吐仍落后冻结 Redis SDK, 三 Star 合计 CPU 更高. 本次两组 Comet/Pulsar/Polaris 完全相同, 只更换 Star; 部分扇出/大载荷/多 Client 场景改善, 也有退化和短窗口波动. CPU 为进程采样, 本轮没有重新采集调用栈、完整分配计数或锁等待追踪, 不能把全部差距归给某一个函数或 gRPC.

本轮原生六写者与两个远端恢复的对比支持“大单范围恢复减少最长阻塞”, 但不支持“所有恢复场景全面改善”: 多 Scope 的 p99 变差而 p99.9 降低. 完整数值仅维护在验证页. 同步 replace 与真实 Exchange 控制轮调度不同; 不将短窗口差异解释成确定的独立因果. 原生准备分锁不消除最终合并/投影的域锁, 下一步应测这些阶段与通知的等待和持锁时间. 旧单 Star 矩阵参数不同, 不能与本轮数字直接相除.

基线使用同一 VM 的回环网络、每轮短窗口和公开视图轮询, 业务认证/TLS 关闭. 不能由它推出真实跨机带宽上限、认证成本、长期稳定吞吐或生产 SLA. 不复写完整成绩表; 旧 SDK 的失败样本及合并窗口差异也只按验证页解释.

下面 A 表示优先准备的小范围改动, B 表示先做定向归因再改变结构, C 表示当前不推进. 这是实施顺序, 不是漏洞等级或收益保证. N 为记录数, A 为 Client 活动对象数, W 为同 Scope 订阅数, P 为对端数, R 为来源组数, H 为保留历史数.

## Comet: 高频固定成本和共享调度

| 项目 | 当前证据及代价 | 最小优化、限制与验收 |
| --- | --- | --- |
| A1. 调度工作空间 | [Core::tick](../astra/comet/cpp/src/core.cpp) 复用数组、弱就绪队列去重并跳过普通事件的目录扫描; partition/poll 只处理本轮就绪集合 | 普通回归通过; 回调中创建/关闭、会话失效、OnDone 和每轮强引用清空保持原契约. 期限与共享状态维护仍有目录扫描, 不把局部工作量减少等同于整体 O(1) |
| B1. 就绪/到期调度 | 已实现有界弱就绪队列及保守最早期限, 处理期间到达的新事件最多连续推进八轮; 普通事件 O(K), 期限维护仍 O(A) | 先测大量空闲 Watch 中少数活跃的队列工作量和期限扫描占比, 再判断是否需要截止索引; 不重建公开 Task/Executor 或每对象线程 |
| A2. 视图空间计量 | [Table::footprint/Draft::footprint](../astra/comet/cpp/src/table.hpp) 已改为缓存页空间, 修改后更新差额, 脏页发布使用位图 | 相关回归已通过; 单项性能收益尚未隔离. 三域共同受益, 保留候选、当前根、解码页和去重元数据计费; 仍不能把逻辑预算解释为 RSS 硬限制 |
| B2. 根复制和页布局 | Draft 捕获固定 256 个 shared_ptr, 第一次修改某页时复制该页 unordered_map. 大批摊销良好, 单条更新仍支付根成本, 页很大时还复制许多未变条目 | A2 之后再比较 16×16 两层根、不同页数与当前实现; 小表不能为了树高引入更多分配. 保持旧 View 不变、无可写别名、点查及遍历语义. 不直接把服务端 Pages 搬进 SDK: 两者读完成同步和销毁约束不同 |
| A3. 批次去重分配 | 三域 accept 已合并常态查找; 完整单页单项批次省去去重集合, 其余仍保留跨页检查. 完成/失败后 swap 空集合归还全部桶 | 相关回归已通过; 单项性能收益尚未隔离. 有界桶容量复用仍为候选; 单项快路径不得适用于之前已经收到页面的批次 |
| B3. 页面和正文拥有 | [Watching](../astra/comet/cpp/src/watching.hpp) 解码页先 SpaceUsedLong, accept 再 ByteSizeLong; 复制 bytes 到公开 vector 后销毁页面. [View 获取](../astra/comet/cpp/src/watching.hpp) 还复制状态元数据, 长 Scope/target 可能分配 | 区分线字节与实际拥有空间, 不能因为都像大小计算就删一个. 可以缓存同一不可变消息的重复测量, 有界复用小消息容量, 共享不变描述信息. vector 公共契约与 Protobuf string 不能靠强转实现零拷贝 |
| B4. 回调影响续租 | Watching::poll 在共享控制路径执行用户回调, 返回后才 StartRead 下一页. 某回调阻塞会拖延其他对象维护, 即使本轮先处理了 Beacon | 保留快速回调契约并记录回调耗时/调度迟滞. 先改善一次控制轮工作预算; 若要隔离慢回调, 需明确执行上下文和有界队列, 不悄悄增加线程或改成并发回调. 增加慢回调与短 TTL、回调关闭 Client 的组合验收 |

高阶语法只用于减少真实重复: Table 的脏页索引、私有通用投影接收骨架可以研究, 但三域的版本下限、Attr 修复、删除及恢复错误不同. 不把三个 accept 生硬压成一套运行期策略框架. 原有 expected/RAII/requires 已应用, 无需再以同一建议重复重构.

## Star: 提交锁、订阅扇出和期限推进

| 项目 | 当前证据及代价 | 最小优化、限制与验收 |
| --- | --- | --- |
| B5. 通知位于域提交锁内 | [Catalog::State](../astra/star/src/catalog_state.cpp) 和 [Ephemeris::State](../astra/star/src/ephemeris_state.cpp) 各有域级 gate, 不同 Scope 共享它. publish 调用订阅通知时尚未解锁; 通知又遍历订阅并准备 pending 项 | 首先减少 changed 本身的工作, 测锁内准备与通知各占多少. 若移动通知到锁外, 必须有严格有序、受预算约束的发布责任, 覆盖提交成功后分配失败、并发通知反序和新 Watch 挂入时序. 不直接解锁后任意调用 notify |
| B6. 精确目标通知 | [Downstream::changed](../astra/star/src/downstream.hpp)、[Readout::changed](../astra/star/src/readout.cpp) 扫同 Scope 全部 W 个流; 无关精确目标也更新覆盖游标、enqueue 并唤醒 | 全 Scope 集合加精确 target 索引, 使正文分发成本接近命中订阅数. 需以共享 Scope 进度维持连续覆盖, 避免跳过无关通知后误判断档; reset、删除、订阅刚建立和重连必须一致. 此项帮助精确订阅, 不能消除全 Scope 多订阅的真实扇出 |
| B7. 相同订阅的重复准备 | 已加入相同区间/目标的冻结 Edition 及在途页共享, 保留一个最近批次/页弱缓存; pending 仍逐流收集 | 普通回归与三 Star 性能已执行, 不同负载收益不一致. 仍需测缓存命中、快慢流和精确目标交错, 不把减少消息构建次数等同于消除逐流序列化或 pending 分配 |
| A4/B8. 空闲来源扫描 | deliver/source/events/resolve 已只读 export; 公开 execute 在 due 前共享读取, advance 在 due 前 O(1) 返回 | due 保持各 Agenda 实际相位, 创建新轮、退役和失败均保持维护责任. 跨界仍遍历全部来源, 空轮跳段和每来源到期索引继续按实际热点评估; 不恢复 max_ticks |
| B9. 长暂停追赶 | [Agenda](../astra/star/src/agenda.hpp) 按 10 ms 补拍, 成本与时间差及 Agenda 数有关; 空轮也推进 | 空/稀疏轮可研究占用位图和安全跳段, 正常高频续租仍保留时间轮 O(1) 摊销优势. 完整时间必须追平, 跨级级联、异常重排和边界删除不变. 不恢复 max_ticks, 不直接换 O(log N) 堆 |
| B10. 大来源替换 | 原生根、正文校验/复用和期限节点已移到 Replica 独占准备边界; 最终合并、投影候选及总预算仍持域锁. Scene::Batch 仍复制对应 Scope 旧历史 deque | 原生并发恢复已测写入 p99/p99.9, 不同 Scope 数有取舍; 持锁时长与真实网络恢复中的续租延迟仍缺定向证据. 保留身份撤销、到期、并发新水位及分配失败边界, 不把锁外准备宣称为安装 O(1) |
| A5. 重复内容检查 | Ephemeris Update 已将最终正文重比改为活性复核; Catalog 最终 publish 使用首轮校验选定的正文引用 | 相关回归已通过; 单项性能收益尚未隔离. Catalog 不同来源的相同正文仍可能需要比较, 不增加摘要缓存. 提交前时钟采样和 Renew 期限计算必须保留 |
| B11. COW/索引布局 | [Pages](../astra/common/src/pages.hpp)、[Origin](../astra/star/src/origin.hpp)、Scene 共享 Name/正文, 同时维护点查与不可变遍历树, 写入仍有路径复制/引用计数及 timer 分配 | 按持有旧快照的比例测复制页数、分配数和缓存缺失. 先预留有界候选/回收容器和改善局部性; 分配池须活过最后读者, 不能由已销毁 State 回收旧 View 内存. 不用 pragma pack 或全局对象池替代生命周期设计 |

必须保留的判断: 纯 Renew 和同正文新 order 已通过 candidate.visible 绕开公开 Scene 提交与通知; 但仍更新来源事实和远端 TTL, 因而不是完整 O(1) 无网络操作. 不能再次把它们当成“向所有 Comet 广播 Attr/Data”的现存缺陷, 也不能把必要的 Star 间续租传播删除. 相关行为已有 [State 用例](../astra/star/tests/ephemeris_state_test.cpp).

Almanac 已是单权威特化, prepare/reset、只读根和版本 +1 不应与动态域重新统一. 当前 apply 已先把新项 push_back 再淘汰含新项的前缀, history=0/单条超预算不构成此前引用片段中的空队列 pop 问题. Almanac 的主要候选是共用 Readout 分发成本, 不是重做存储抽象. Pages 已有子树计数分页跳过、空根回收等机制, 不把“每页从头扫描 N 项”当成当前事实.

## RPC、对等复制与运行循环

| 项目 | 当前证据及代价 | 最小优化、限制与验收 |
| --- | --- | --- |
| A6/B12. Runtime 空转 | [Runtime::step/pump_sessions](../astra/common/src/runtime.cpp) 每轮推进各会话及服务. [Wakeup](../astra/common/src/process.cpp) 有 10 ms 兜底等待, 网络事件可立即唤醒 | 先识别没有入站、写完成、来源变更及期限事件的空转. 再考虑复用就绪列表和截止驱动. 不能把 10 ms 写成每条业务消息固定等待, 也不能让批量数据工作饿死控制帧、关闭和对时维护 |
| A7. 发送额度和计量 | Edition 保留有序检查/就地合并, 改用引用 tuple 投影避免独立的字符串大小/相等两次比较, 动态域捕获根直接移交; [Exchange](../astra/star/src/exchange.cpp) 的重复 ByteSizeLong 已消除, 已通过相关回归 | 后续按记录数、字节数和耗时共同限制批次工作, 避免同为 32 个流但负载差数百倍. 不默认加固定合并等待 |
| B13. Protobuf 分配/编码 | 同一业务正文仍进入各流 protobuf bytes, 不等于端到端零拷贝 | 从消息容量的有界复用开始; 再对单页/在途批次评估 Arena. 发送完成前不能 Reset, 跨 Arena move/Swap 可能深复制. 只有序列化确实占热点时才评估 GenericStub/ByteBuffer 编码共享, 避免为节省复制失去类型与生命周期清晰度 |
| B14. 批量续租 | 多 Beacon 共用 Client 仍各自 unary Renew; 独立进程之间无法靠 SDK 本地批处理自动合并 | 按同一 Client/会话/节点, 聚合自然同时到期的续租; repeated Item 包含 scope/uuid/order, 不用两个平行数组. 每项独立结果、原 order 重试、接收时间与请求总大小上限明确. 不增加故意等待, 不把普通 Publish 改回多 Key 原子事务 |
| B15. 同客户端重复订阅 | 同 Scope/target 的多个对象可有重复流、重复投影 | 只有真实应用存在重复监听时, 才评估私有共享接收/不可变视图. 独立关闭、预算、回调、精确目标及恢复语义会增加成本; 多目标协议不是仅把 string 改 repeated 就完成 |
| C1. 全互联替换 | 当前每对 Star 一条逻辑流, 拓扑 O(P²), 本机来源更新向其余 Star 扇出; Almanac 由 Polaris 单独分发 | 这是设计成本, 不由换容器/Arena消除. 先测 Star 数、总出站字节、分区恢复和慢对端隔离; 当前不加入 Gossip、Planet/Moon 或新拓扑层来掩盖本地重复工作 |

Downstream/Readout 的 pump 已使用就绪队列和单轮额度, 不应再建议“把所有 Watch 全扫描改队列”作为首次优化. Dispatch 和 Polaris 窗口已有流水线、累计 ACK, 不逐包等待网络往返. 优化应针对实际的 changed 扫描、批次准备和循环调度.

gRPC 流并不一一占用 TCP/fd; Channel 可复用连接. 长流、Channel 池及预序列化各有适用条件, 不承诺 RPC 减少 50 倍就使 CPU 或延迟改善 50 倍, 参见 [gRPC 性能指南](https://grpc.io/docs/guides/performance/). 回调中阻塞会影响其他 RPC, 参见 [C++ 回调约束](https://grpc.io/docs/languages/cpp/best_practices/). Arena 的 Reset 与使用线程必须同步, 跨 Arena 操作也可能复制, 参见 [Protobuf Arena](https://protobuf.dev/reference/cpp/arenas/).

## Pulsar、时钟和准入

| 项目 | 当前判断 | 优化边界 |
| --- | --- | --- |
| B16. Clock 共享锁 | [Clock::now](../astra/common/src/clock.cpp) 在锁内推进本地连续时间及校准状态; 两个动态域和控制循环会读取, 写路径最终采样也必需 | 先测锁等待占比. 不能缓存陈旧 now 绕过到期, 不能仅将成员换 atomic 就获得一致时间, 也不能用有数据竞争的 seqlock. 不同批次共享读数需要共同受理边界; 无证据不重写时钟 |
| B17. 公共会话检查 | [Access::enter](../astra/star/src/access.cpp) 的共享锁随 Permit 覆盖受理/提交边界, 凭据轮换独占它. 无认证基线没有测这个成本 | 补认证开启、轮换与大量业务同时运行的锁等待. 保留“撤销后不能新受理”的明确线性化, 不能单纯提前解锁. KDF、登录身份和常量时间比较不属于应删的高频浪费 |
| C2. Pulsar 调度重写 | [Pulse](../astra/pulsar/src/pulse.cpp) 已使用 Callback Reactor, 有流额度和单调截止; [PulseClient](../astra/common/src/pulse_client.cpp) 是每 Star 的专用采样路径, 有抖动及可取消等待 | 不再按早期同步阻塞 Read 的设想改造. 一般关注大量 Star 同时启动/参考恢复, 不能将业务对象数乘成对时 RPC 数. 降低采样率还会改变误差预算, 不用于美化写入基准 |
| C3. 准入/SQLite 热路径化 | Pulsar 成员持久操作是启动/身份变更工作, 已发布目录供正常校验读取; 不是每条 Renew 都写 SQLite | 先测启动峰值和目录传播. 不删签名、重启身份或持久提交来优化与其无关的 Data 更新. Go 准入 Context 当前会克隆 Hello, 可缓存不可变 metadata, 但属低优先级控制面微优化 |

## Polaris 与 Astrolabe

| 项目 | 当前证据及代价 | 最小优化、限制与验收 |
| --- | --- | --- |
| A8/B18. WAL 检查 | [Store::maintain](../astra/polaris/internal/storage/store.go) 在读请求/提交前 stat WAL; 文件超过阈值后, 即使空间可复用仍尝试 PASSIVE checkpoint. [readOnly](../astra/polaris/internal/storage/read.go) 也走这条路径 | 建立有界、单责任的检查节奏或页进度依据, 避免多个读请求重复触发检查点. 必须继续限制被长读钉住的 WAL, 不能简单每秒检查而允许无限突增. 测 WAL 达高水线且读事务长期存活的行为 |
| B19. 快照准备共享 | [server/cache.go](../astra/polaris/internal/server/cache.go) 按 Scope+请求 minimum 分组; minimum 不同却可由同一实际版本满足的请求不会共享. 每次候选按最大 Scope 预留, 小数据也占该预算 | 可研究按 Scope 合并正在准备的工作, 完成后逐请求验证实际版本下限. 保持容量预留、首请求取消不永久拖累其他请求、旧版本不能满足更高下限. 不长期缓存第二份权威全库 |
| B20. 交错 Scope 装包 | [stream.go](../astra/polaris/internal/server/stream.go) 只把相邻同 Scope 事件合到一页; A/B/A/B 交错会产生小包 | 在已经取出的有界窗口内按 Scope 稳定分组, 保持各 Scope +1 次序及 ACK. 不能跨越 reset/对账屏障, 不合并掉权威版本、同值 Set 或缺失 Delete. 已有发送窗口无需再造 |
| C4. SQL/ORM 替换 | 当前单写队列、显式事务、WAL/FULL、读池和必要历史前缀裁剪适合唯一权威. 持久提交本来包含耐久成本 | 可按 SQL 剖析考虑固定语句复用, 但不因控制面有 GORM 就换数据库/ORM. FULL 改 NORMAL 会改变断电后的成功语义; group commit 也改变延迟/结果边界, 不当作无代价优化 |
| A9/B21. 指标抓取周期 | 已取消独立 64 节点上限, 覆盖完整可信 Star 目录. [Monitor/sample](../astra/astrolabe/internal/bridge/metrics.go) 仍为 4 工作者、每请求 2 s; 全慢时一轮约 ceil(N/4)×2 s, 5 s ticker 不保证全量新鲜度, 64 节点示例仍约 32 s | 覆盖与身份过滤通过普通 Go 回归, 周期问题未声称解决. 后续考虑目标级到期调度、失败退避和可配置有界并发, 让新鲜度对应实际容量. 不再用限制总节点数替代限制同时采样数 |
| B22. 管理面低频工作 | 已在目录发布时建立端点/实例索引, 每个采样结果期望 O(1) 核对最新身份; 解析省去完整中间字符串, 连接池按目标数量保留. 每轮仍新建 4 工作者 | 相关回归已通过; 单项性能收益尚未隔离. 复用工作者需连同 B21 调度一起考虑; 不因局部工作减少就宣称大目录能在五秒内全部抓取. 未采集/替换节点显式输出空值, 不借旧实例指标制造成功 |

SQLite 的 FULL 与 NORMAL 耐久差异、检查点与活跃读者的关系见 [SQLite WAL](https://sqlite.org/wal.html). 不能为获得漂亮的管理提交数字破坏“Polaris 持久化成功才确认”的契约. Astrolabe 仍是实时管理/观测入口, 不增加自己的持久库或新指标协议.

## 工程结构、算法取舍与验收

1. 构建边界可以继续收敛. [CMake](../astra/CMakeLists.txt) 默认仍建立 Planet, star_runtime 还声明 star_sync_store 链接, 即使活动业务已使用原生三域. 先核对全部直接/传递符号和测试依赖, 再将搁置目标置于明确选项或移除无用依赖边. 静态库链接声明不意味着其全部对象进入最终可执行文件, 不能虚称已经减少运行时内存. 构建拆分也不得恢复 Planet 的功能推进.
2. 最高价值的算法变化是从“任何事件扫描所有对象”收敛到“就绪/到期对象”, 从“所有订阅都准备”收敛到“命中订阅共享准备”. 位图、侵入式队列、最早截止和不可变批次应以现有私有结构实现; 不是引入新框架的理由.
3. 域锁不能直接替换为每 Scope shared_mutex. 来源位置跨 Scope 连续, 完整来源 ACK 仍需连续, 同 Scope 合并和全域预算仍需协调; 来源安装已允许 Scope 先后可见. 真要分片, 应先定义来源提交顺序和原子发布协议, 而不是以读写锁语法改动掩盖一致性变化. 当前先缩短域锁内工作.
4. 不全面改 flat hash、PMR、持久树或 lock-free. 容器替换会影响地址稳定性、异常回滚、旧视图和峰值双份内存. 引入第三方依赖还需单独授权. 热点证据不足时, 保留已有标准容器和稳定名称所有权更合适.
5. LTO/PGO 可作为最后一层编译优化候选, 在目标 GCC/gRPC/Protobuf 组合上独立对比, 不默认加 -march=native 破坏交付可移植性. 不用编译选项掩盖 O(A)、O(W)、O(P×R) 的额外工作.
6. 先补可归因的有限指标: 每提交/每安装视图分配数、复制字节、调度对象数、空轮次数、锁等待/持有时间、每流消息大小与待发年龄、快照安装时间. 常态指标用有限标签、分桶或采样; 不按 Key/UUID 输出无界序列, 不把逐操作计时开销偷偷计入对照某一侧.

建议按以下顺序实施, 每阶段使用同一数据集和明确源码身份单独 A/B, 不一次把所有候选混成一个难以归因的大改:

| 阶段 | 实施候选 | 必须观察的收益和不变量 |
| --- | --- | --- |
| 1. 小改与归因 | A1/A2/A3/A5/A7, 并记录 Star/Comet 分项 CPU、分配与调度计数 | 多 Beacon/Observer、Publisher/Subscriber 的 receipt/visible 都报告; 无更新对象增加时 CPU 不应异常放大; 最终视图/正文/Attr 正确, 旧 View 保持不变 |
| 2. 主要结构热点 | 依据阶段 1 证据选择 B1、B5–B8, 不同时替换三项核心结构 | 精确订阅无关更新、全 Scope 高扇出、慢消费者、短 TTL、相同 Client 与多 Client 分别验证; 吞吐、p99、CPU、峰值内存一起比较 |
| 3. 大恢复与扩展 | B9/B10/B12/B14/B18–B21 中实际有压力的部分 | 长暂停后时间追平, 大来源替换不打断正常续租, 多 Star 分区恢复、慢对端不拖全局, WAL 与抓取周期在上界下可控 |

现有统一基线已经覆盖多注册/选择器、多发布/订阅及分组/载荷/短租约, 无需从头另造测试系统. 但短窗口、同 VM 及视图 1 ms 轮询不足以准确分离微秒级网络/服务端成本. 后续需在兼容比较之外增加 Astra 自身的 callback 时间戳和持久视图消费模型, 同时保留原对照口径. 不把控制面或大型恢复优化的收益用单 Star 小消息基准证明.

除页首明确标为已修改的部分外, 以上仍是候选. 当前就绪队列及多页缓存已完成普通构建/回归和有限性能对照, 包含 2048 字节 Catalog 场景. 未下载或启动长期任务. 后续优先测剩余临界区、就绪/目录工作量和缓存实际命中, 不将小规模基线等同于大 N 收益证明.

## 编码规范与精简边界

- 新修正遵循 [cpp-coding.md](../cpp-coding.md): 沿用核心单词命名, 复用私有上下文, 多阶段函数首行空白, 长表达式不按列宽折行, 中文注释及 ASCII 标点. C++/Go 使用现有 clang-format/gofmt, 不动生成代码和第三方源码.
- 现有 requires、std::expected、ranges、move_only_function、RAII 和拥有式视图已经用于真实职责. 不把全部 if 错误分支机械改成多层 lambda, 不用静态反射替代已经生成的 Protobuf 类型或业务校验.
- 三域 Edition、State 仍存在相似流程, 但 Almanac 权威安装、Catalog 水位、Ephemeris Attr/Data 不同. 优先保持独立业务代码, 只共享页、来源历史和真实相同的投递机制; 再建通用 Store/继承层的收益不足.
- 全仓注释仍有可改进之处, 特别是 Runtime 的重复说明、残留 Supervisor 泛称及一些局部测试变量的契约不够明确. 本次不以批量改名或大量无信息注释宣称已经逐变量完美符合规范. 后续随模块维护收敛, 不与行为修复混成不可审阅的机械重写.

## 测试完整度

| 范围 | 已有用例或基线 | 仍缺的证据 |
| --- | --- | --- |
| 原生状态与所有权 | Pages/Origin/Scene、三域状态/副本、分配故障、期限、历史、旧视图与回滚, 来源准备并发及覆盖记录用例均随普通回归通过 | 没有覆盖率百分比或长期随机参考模型对照 |
| RPC/SDK | 登录撤销、TLS、精确/全范围 Watch、零历史并发、半批超时、回复丢失、关闭/恢复、独立安装消费, 共享分页/多流交错均随普通回归通过 | 固定交错不能证明所有回调次序已覆盖, 本轮未做 Sanitizer |
| 对等复制 | Dispatch/Landing/Exchange 双域、回补、断档、预算、三个真实 Star 下的停止/重启/SDK 切换 | 三台及以上同时分区、交叉重连、滚动重启与大来源恢复还缺系统化进程矩阵; 组件模拟不等于真实网络验证 |
| 时间与持久库 | Pulsar 时钟/SQLite 故障、Polaris 事务/恢复/历史及进程重启 | 真实宿主休眠、时钟突变、断电、磁盘满/长 I/O 抖动的组合证据不足; 历史前缀用例有本轮 Go 常规测试证据 |
| 管理 | Go HTTP/KDF/Cookie/指标解析已重跑, Admin 保留协议适配用例 | 本次未重跑 Admin 前端, 完整真实 3D Orrery 不在当前交付范围 |
| 性能与资源 | 逻辑预算和容量拒绝、当前三 Star/Redis 基线和原生恢复对照 | B01–B14 未全部覆盖, 缺大规模分配/锁等待、长期及跨机器证据; 不以短矩阵替代生产容量评估 |

优先补充可重复的随机操作/参考模型验证, 覆盖发布、续租、到期、快照、回补及断链组合; 对畸形分页/序列/大小进行属性或模糊测试. 这些是待办, 不虚构为已有测试. 在测得覆盖率前不能回答“当前覆盖率达到某百分比”, 即使 Sanitizer 全绿也不能保证不存在竞态或生命周期缺陷.

当前普通回归和有限性能验证见 [validation.md](../testkit/validation.md), 进一步验证仍按失败或未解决边界及当轮授权选择. 普通回归不默认附加 Sanitizer; 仅本轮明确要求完整测试或点名 San 时运行. 不自动启动长期压力、依赖下载、提交或推送.
