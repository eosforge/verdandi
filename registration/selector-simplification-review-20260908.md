# Selector 简化设计审核

日期：2026-09-08。范围：当前工作树的 Registration Selector、订阅协议、选择事务和快照，以及 Go、Rust、C++、C ABI、Legacy、C# 的相关实现。本轮仅新增审核文档，没有修改实现、启动数据库或执行新一轮 SDK 测试。

## 结论

建议把下一版内部设计收敛为 **Registry 统一加锁，内部 Service 封装每个注册实例的状态，订阅直接合并到 Service，优先用已经完整的数据，只有缺口才按 UUID 回读，全量快照按需生成并缓存**。

进一步审核后，协议层建议显式支持两种内容操作：`Update` 完整替换应用 Version/Data，`Patch` 只覆盖指定 Data 字段并可选修改 Version；Attr/TTL 仍按 UUID 不变。两者共用同一个 content revision，不引入字段 revision、Registry 日志或自动按大小切换。现有 Redis 8.8 基准证明窄 Patch 有实际性能价值；完整 Update 则提供可跳过中间 revision 的自包含基线。该结论属于候选协议设计，尚未修改实现。

用户提到的 Service 在本报告中解释为每个 UUID 对应一个内部对象。它拥有数据与状态转换逻辑；监听连接、同步任务、容量预算和定时调度由 Registry/Selector 统一拥有。

三个判断需要分开：

| 提议 | 审核意见 |
| --- | --- |
| 用锁管理当前 Registry 和引用视图 | 支持；能减少全量视图发布与多层对账，但长回调会阻塞状态更新，需要验证锁竞争 |
| 同步期间立即处理 SUB，取消独立消息正文缓冲 | 有条件支持；Service 可保存最新 Fields，完整 Register 或完整基线上的连续 Update 可以直接完成；不完整状态仍需回读 |
| 单独缓存全量快照 | 支持按需缓存不可变内容；现有公开 Snapshot 返回独立可写结果，不能直接共享其中的可变对象 |
| `map[key]*Service` | 支持稳定对象和逻辑内聚；当前底层已经用记录引用，收益来自合并职责与状态，而非单纯换容器类型 |
| 每次合并后检查能否构建完整 Registration，跳过同步 | 支持跳过该实例不再需要的读取；需要完整性与版本连续性依据，Registry 整体成员发现及恢复栅栏另行判断 |
| 显式支持 Patch 和完整 Update | 支持；Patch 保留窄写性能，Update 提供自包含基线；复杂度必须封装在 Service 内，不能扩展为两套 Registry 同步协议 |

这份审核给出可实施的设计方向及验收条件，不宣称未经实现的新方案已经具备现有测试成绩，也不承诺预先确定的性能或净行数降幅。

## 当前复杂度来自哪里

三种原生实现都同时维护可变同步状态与不可变发布视图：

- Go 已是 `map[string]*selectorRecord`，发布时复制索引、建立引用数组并排序：[selector_core.go:49](D:/projects/verdandi/sdk/go/registration/selector_core.go:49)、[publish:1202](D:/projects/verdandi/sdk/go/registration/selector_core.go:1202)。
- Rust 使用 `HashMap<String, Arc<SelectorRecord>>` 和 `ArcSwap<SelectorView>`，发布时同样复制索引并排序：[selector.rs:68](D:/projects/verdandi/sdk/rust/src/registration/selector.rs:68)、[materialize_view:1965](D:/projects/verdandi/sdk/rust/src/registration/selector.rs:1965)。
- C++ 使用 `shared_ptr<const selector_record>`，另建有序映射和数组作为视图：[selector_state.hpp:22](D:/projects/verdandi/sdk/cpp/src/internal/selector_state.hpp:22)、[make_view:896](D:/projects/verdandi/sdk/cpp/src/selector.cpp:896)。

因此，一次视图发布包含随 Registry 大小增长的索引构造和排序。记录正文已有共享，并不是每次都深拷贝所有 Fields；发布也允许按配置合并，并非所有事件都触发一次全量构造。

选择入口还维护事务锁、候选数组、强类型投影和本地预测。Go 的 `operation`、Rust 的 `selection: AsyncMutex`、C++ 的 `operation_: timed_mutex` 已经串行化策略回调。真正可简化的是“可变状态 → 整份发布视图 → 类型缓存/预测对账 → 候选事务”的多层关系。

此外，当前局部 Update 协议下，同步期间的消息正文合并单独占据 Go 275 行、Rust 271 行、C++ 头文件 146 行，并带来 drain、复制、容量回滚和同步结果对接代码。物理行包含注释；删除正文缓冲后仍需少量读取有效性和回读调度逻辑。

## Patch 与完整 Update 双操作审核

当前公开类型化 API 已要求 Update 提供完整期望 Data：[api.md:120](D:/projects/verdandi/registration/api.md:120)、[Go registration.go:185](D:/projects/verdandi/sdk/go/registration/registration.go:185)、[Rust mod.rs:268](D:/projects/verdandi/sdk/rust/src/registration/mod.rs:268)、[C++ registration.hpp:139](D:/projects/verdandi/sdk/cpp/include/verdandi/registration/registration.hpp:139)。三种 SDK 现在都先编码完整 Data，再与本地期望状态比较，最终只把变化字段交给 Lua。因此可以保持 `Update(Data)` 作为完整替换，并新增显式 `Patch(Fields)`；不能继续让名称为 Update 的操作在线上悄悄表现为 Patch，否则调用语义和恢复语义仍不一致。

“同时支持”可以发生在不同层，结果并不相同：

| 方案 | 收益与代价 | 审核结论 |
| --- | --- | --- |
| API 有 Update/Patch，线协议一律完整 | 调用方便、Selector 最简单；Patch 不再节省 Redis 与网络工作 | 可作为低复杂度退路，但失去本项目已有的窄写优势 |
| Redis 只写 Patch，Pub/Sub 发布完整内容 | 若由 SDK 同时发送完整正文，入口网络仍是完整大小；若由 Lua 回读拼装，会增加阻塞脚本工作 | 不采用 |
| API、Redis 写入和事件都显式区分 Update/Patch | 同时保留完整基线与窄写性能；Selector 仍需处理 Patch 缺口 | 推荐候选，但缺口状态只能存在于单个 Service 的有界窗口 |

完整 Update 不能只携带完整 Data 而继续省略未变化的应用 Version。假如客户端漏掉了只改变 Version 的 revision 12，随后收到 Data 完整但 Version 省略的 revision 13，仍无法重建完整状态。因此，每次完整 Update 事件和写入都包含完整 `@version + Data`。Patch 可以携带可选 Version 和指定 Data 字段。SetVersion 可以视为只含 Version 的 Patch。无变化仍由发布端本地判定为 no-op，不访问 Redis 或推进 revision。

当前 v1 的 `update` 明确定义为局部 Patch，不能在相同协议标识下改解释为完整状态。候选线协议必须升级版本并使用明确的 `update`/`patch` 模式或两个不含糊的 kind；新 Selector 只有看到完整模式才能跨 revision 安装。若要求新旧 SDK 混合部署，还需单独设计能力隔离或协调升级，不能仅靠检查事件是否碰巧包含所有已知字段来猜测语义。

公开 API 不必为所有组合继续扩张。保留现有 `Update(Data)`、`UpdateContent(version, Data)` 和 `SetVersion(version)`，只新增一个 `Patch(Fields)` 用于 Data 窄更新即可；需要 Version 与 Data 原子变化时继续提交完整 `UpdateContent`。Patch 不允许删除字段或引入首次 Register 中不存在的字段。

“完整替换”先定义为逻辑语义，不要求把 Data 物理合并成单个 Redis blob。继续沿用当前展开的固定字段，在同一次 HSET 和事件中携带全部 Data，已经能获得自包含 Update 的主要简化。改成单 blob 还会新增跨语言容器编码、迁移和检查工具，当前没有足够额外收益。

| 环节 | 双操作下的规则 |
| --- | --- |
| Registration 邮箱 | 按准入顺序折叠：仅有 Patch 时合并同名字段；出现 Update 后以其完整 Data 为新基线，再合并其后的 Patch |
| Lua | `update` 写完整 Version/Data；`patch` 只写指定字段。二者都原子刷新租约并发布明确的事件种类 |
| Service 合并 | 完整 Update 可在同连接代直接覆盖到任意更高 revision；Patch 只有恰好连续时直接应用 |
| 读取与 SUB 竞态 | 完整 Update 可使旧读取自然失效；Patch 缺口只在该 Service 内触发权威回读和有界补全 |
| 本地预测 | Patch 直接给出变化字段；完整 Update 比较旧、新 Data，只清除远端实际改变字段的预测 |

它不能消除成员发现、租约校准或未知 UUID 的首次读取。未知 UUID 即使收到完整 Data，仍缺少不可变 Attr/TTL；重连时也仍需通过 HSCAN 确认可见成员。它解决的是单实例内容补全和消息乱序复杂度，不应被描述成完整的断线历史恢复方案。

代价集中在稳态数据面。当前 Registration 完整记录默认限制 16 KiB、最大可配置到 64 KiB，Data 默认最多 32 个字段、协议上限 128 个字段：[configuration.md:133](D:/projects/verdandi/configuration.md:133)。原来只改变一个小负载字段时，Redis HSET、MessagePack 事件和每个 Selector 的接收量都接近 patch 大小；完整替换后接近整个 Data 大小，并按订阅者数量放大。发布端本来就编码完整 Data，但 Redis 和订阅端此前没有承担这部分完整负载。

项目已有的 Redis 8.8、无订阅者基准提供了直接证据：[lua-optimization.md:298](D:/projects/verdandi/registration/lua-optimization.md:298)。单字段 Update 的脚本中位耗时为 8.43 μs、墙钟吞吐约 49,804/s；32 字段 Update 为 19.74 μs、约 15,814/s，即脚本时间约为 2.34 倍、墙钟吞吐低约 68.2%。宽 Update 的 32 个值都只有 1 byte，连同字段名仍远低于 1 KiB，因此“Data 小于 1 KiB”不能消除字段数量带来的 Lua、HSET 和 MessagePack 成本。该宽行是未来完整 Update 的最近似现有测量，不是新协议实测；新协议还要求总是携带 Version，实际结果需重新基准。Redis 官方也把多字段 HSET 标为 O(N)，Lua 在执行期间阻塞其他服务器活动；PUBLISH 的工作还随订阅客户端和模式订阅数量增长。[HSET 文档](https://redis.io/docs/latest/commands/hset/)、[Lua 文档](https://redis.io/docs/latest/develop/programmability/eval-intro/)、[PUBLISH 文档](https://redis.io/docs/latest/commands/publish/)

另一方面，当前 Go 的 Patch 应用会复制完整 Data map，[selector_core.go:920](D:/projects/verdandi/sdk/go/registration/selector_core.go:920)；Rust 也先克隆完整 Data 再逐字段覆盖，[selector.rs:1337](D:/projects/verdandi/sdk/rust/src/registration/selector.rs:1337)。完整 Update 可以直接接管或共享已解码的不可变 Data，因而客户端成本未必高于 Patch。Redis 写入侧偏向 Patch，Selector 合并和恢复侧偏向完整 Update，双操作有真实而非假设性的适用区间。

两个操作必须由调用语义显式选择，不做“Data 小则 Update、变化少则 Patch”的隐式阈值。建议 `Update(Data)` 表示调用方提交完整权威状态，适合配置、地址、就绪状态组合变化和主动建立新基线；`Patch(Fields)` 表示只覆盖列出的固定字段，适合 load、queued 等高频窄变化。这样性能选择可见、可测试，也不会因调参改变消息语义。

双操作无法完全删除 Patch 的恢复复杂度，但可以把它严格限制在单个 Service：保存完整基线 revision、最高观察 revision，以及一个只保留每字段最后值的连续 Patch 窗口。字段集合固定且完整 Data 硬限制 1 KiB 时，该窗口正文不超过一份 Data；另设 Registry 级总容量上限。遇到缺口时只标记该 UUID 并保持至多一个在途读取；完整 Update、完整 Register 或足够新的权威读取会清空窗口。不得保存无限事件列表，也不为 Patch 引入字段 revision、全局游标或持久日志。

如果调用方只需要 Patch 的编程便利、并不要求它节省 Redis 和网络成本，也可以在 SDK 内先应用 Patch，再统一发送完整 Update；这是最简单的兼容层。项目现有基准表明窄写性能确有明显差异，因此本审核更倾向于让 Patch 在线协议中真实存在，但其使用应是显式的。

## 锁为何不能单独解决 SUB 与扫描交错

以下是对“删除消息缓冲并直接覆盖”的反例，不是声称现有代码仍存在这些漏洞。

**旧读取晚到。** Redis 执行 HGETALL 时 A 为 revision 10，结果尚未安装；随后 Update 11 被订阅端处理；最后同步线程拿到锁安装 revision 10。整个过程可以完全没有内存数据竞争，却把新数据覆盖成旧数据。

**删除后恢复。** Redis 已生成 A 的旧读取结果；随后 Unregister 删除 A，SUB 处理后本地也删除 A；旧读取最后到达并重新插入 A。当前 [Unregister 消息](D:/projects/verdandi/lua/src/registration/actions/unregister.lua.inc:9)只有 UUID，没有可用于普通大小比较的 revision。只比较“新版本是否更大”不能保护已经不存在的对象。

**当前 v1 局部事件缺少基线。** [Update](D:/projects/verdandi/lua/src/registration/actions/update.lua.inc:64)只有本次 Data patch，不含完整 Attr、TTL 和其他 Data；[Renew](D:/projects/verdandi/lua/src/registration/actions/renew.lua.inc:57)也不带完整记录。本地尚未取得 A 的基线时，收到 Update 11，不能凭它构造完整 A。若直接忘记这条观察，再接纳稍后到达的旧读取 10，缺口也会被遗漏。采用完整可变内容 Update 后，已知 UUID 的内容缺口消失；未知 UUID 仍需读取 Attr/TTL。

**分页不是统一时间点。** 当前实现先 HSCAN 成员索引，再分批读取记录。SCAN 家族允许重复返回元素，对扫描中途新增或删除的元素不提供完整快照保证，因此扫描结果必须与订阅变化合并。不能在最后用扫描集合无条件覆盖已经收到的新注册。[Redis SCAN 文档](https://redis.io/docs/latest/commands/scan/)

**重连不能只取历史最大 revision。** Sentinel 故障切换可能丢失已确认写入。新主的权威状态可能比旧连接曾观察到的 revision 小；必须隔离连接代并重新建立基线。[Redis Sentinel 文档](https://redis.io/docs/latest/operate/oss_and_stack/management/sentinel/)

## 建议的 Registry 与 Service 分工

```text
Selector / Registry
  一把保护当前状态与选择事务的锁
  连接代、同步状态、容量统计与过期调度
  map[UUID] -> Service 引用
  去重的待回读 UUID
  按需生成的快照缓存

Service（内部对象）
  当前权威记录：Meta、Attr、Data
  活动 / 保留 / 尚待完整读取等状态
  本地预测及必要的类型投影
  相关远端变化的本地序号
  应用事件、安装完整读取、续期、退出和预测对账的方法
```

Service 方法在 Registry 锁的保护下执行。这样，单个实例的规则集中在对象内，`One/Any` 涉及多个实例时仍能在同一锁内完成一致读取和原子预测提交。

第一版使用一把逻辑互斥锁即可；各语言保留自己的可取消或有超时的获取方式。读写锁、每个 Service 独立加锁和跨对象多锁提交都增加额外规则，暂没有测量依据支持引入它们。

Service 内部仍需要 Fields 或等价的协议表示，也可以保留只在内容变化时更新的强类型投影。它不是每个实例各开连接、协程或计时器。容器拥有对象，读取期间的引用只负责保证对象存活。

本地预测可以从 `Selector.overlays[uuid]` 等外部映射移入 Service。收到远端变化时由该 Service 对账，不必等下一次选择再遍历全部 overlay。当前“远端实际改变的字段覆盖预测，未改变字段保留预测”的规则应保持。

## Service 按事件完成，优先跳过不必要的读取

用户进一步提出让 Service 累积 SUB Fields，每次写入后检查能否构成完整 Registration。这个方向可以纳入设计，但完整性必须来自协议事实。这里的完整 Registration 指 Selector 中的注册记录，不是会向 Redis 发布数据的写端 Registration 对象。

[Register 消息](D:/projects/verdandi/lua/src/registration/actions/register.lua.inc:47)已经带有完整 Meta、Attr 和 Data。因而可以在一次合法事件原子合并后直接完成 Service；无需等待整个 Registry 扫描结束，也无需为此实例再发一次 HGETALL。

| Service 已有状态与新信息 | 合并后的动作 |
| --- | --- |
| 尚无基线，收到当前连接代有效的完整 Register | 校验后安装完整基线；只有它覆盖了已经观察到的版本要求，才清除回读需要 |
| 同一连接代已有完整 revision 10，收到连续 Update 11 | 直接更新对应字段，保持完整状态；无需 Redis 回读 |
| 已有完整基线，收到同 revision 较新 Renew | 只推进时间和租约；无需再次拼装、解码全部 Attr/Data |
| 尚无基线，只收到 Update 或 Renew | 不能由这些消息证明完整；可以在 Service 内暂存有界信息，但仍需完整 Register 或权威读取 |
| 已有完整 revision 10，直接看到 v1 局部 Update 13 | 存在版本缺口；除非后续完整状态覆盖缺口，否则必须回读 |
| 已有同 UUID 的 Attr/TTL，直接看到候选新协议的完整 Update 13 | 直接以完整 Version/Data 更新到 13；中间 revision 不影响最终状态 |
| 已排队读取，随后 SUB 使 Service 完整并消除了缺口 | 发请求前复查并跳过；已经发出的读取不一定能取消，结果仍按票据检查，不能重新破坏完整状态 |

不能只用“必需元字段都在”或“应用 Decoder 成功”判断收齐了数据。该项目没有为每个 Registration 在所有 Update 中附带完整字段清单；原始 Fields 接口和用户自定义 Decoder 也不提供通用的完整性证明。合并多个 partial Fields 可能遗漏从未变化的字段，也可能把不同版本的片段拼成并不存在的状态。

若引入未完成 Service 的字段累积，仍须记录基线来源、所属连接代和版本连续性；遇到缺口就保留回读需要。必须以一条已校验事件为合并单位，不能在解码半条消息或写入其中一个字段后就向外发布。同步算法补充中的增量仅服务于有界的补全窗口，和完整 Register、已有完整基线一起由 Service 判断，避免另建独立的通用消息合并组件。

**可以跳过实例读取，不等于可以跳过首次或重连后的成员枚举。** 假设 Redis 原有 A、B，订阅后只收到 A 的完整 Register，A 已完整仍不能证明 B 不存在。HSCAN 可以继续发现成员，但对当前连接代已验证、版本满足扫描提示且没有缺口的 Service，可跳过对应的记录读取。同步收尾仍需要覆盖订阅处理顺序、过期和回读缺口。

只有“成员发现已经完成、相关事件跨过栅栏、没有待补 Service”的 Registry 才满足当前 Ready 约定。若希望 Registry 未完全同步时也允许选择已完整的部分 Service，那是另一项可用性语义变化：可能漏掉其他候选，需要单独作为 API 决策，不混入本次内部简化。

## 取消正文缓冲的最小安全路径

1. 确认订阅成功后启动分页读取。保持一个顺序消费订阅的监听者，以及最多一个按需同步/回读任务。
2. 每次发起记录读取前，在锁内关联到具体 Service，并取得读取票据：**连接代、Service 身份、该实例的相关远端变化序号**。票据持有对象引用。
3. 发请求前先复查 Service 是否仍需完整读取；需要时释放锁进行 Redis I/O。结果回来时重新加锁，核对票据；旧连接、已移除或已替换的 Service 结果不得写回。
4. SUB 不等待整轮同步结束。Register 等具备完整信息的事件直接更新；已有正确基线的 Update/Renew 直接应用；没有基线或版本有缺口时，仅把 UUID 加入去重回读集合。
5. 读取期间有 SUB 不应一律导致结果失效。通过身份检查后，按下节的版本覆盖规则决定接纳、忽略或用连续增量补齐；只有仍不能证明完整时才再次回读。若 SUB 已使 Service 完整，清除相应回读需要。不得倒退同一连接代的 revision/timestamp，也不得把同 revision 的 Renew 当成必须重读内容的变化。
6. Unregister 在锁内结束并移除对应 Service。旧读取票据指向的对象已不在 Registry 中，结果自然失效。这可以避免另外维护一套长期删除缓存；前提是**所有在途记录读取都先取得对象票据**，且旧任务汇合、对象引用和容量回收受管理。
7. 扫描完成后，在同一订阅连接做带标识的 PING/PONG 栅栏。栅栏前的处理及所需回读完成后才能把 Registry 标记 Ready；出现新缺口继续回读或重同步。处理同步中的消息不等于向选择 API 开放不完整 Registry。

Pub/Sub 的交付是 at-most-once，断线后不会补发遗漏消息，因此连接代隔离、接收滞后/丢失检测和重同步仍需要保留。[Redis Pub/Sub 文档](https://redis.io/docs/latest/develop/pubsub/)

这里取消的是 SDK 领域层独立的“合并消息正文”缓冲组件；Service 自己的当前 Fields、完整性和回读状态仍保留。Redis、网络和驱动仍有传输缓冲；监听线程不能持锁等待网络，也不能在解码后丢失顺序。待回读 UUID、在途对象和传输缓冲需要有界，溢出时明确失效并恢复。

读取票据的身份保护与版本合并各负其责。原先“收到任何相关 SUB 就丢弃读取”的保守方案会在高频更新时反复回读，不作为最终同步策略。原有正文合并帮助同步追上写入的作用，应由 Service 内有界、仅在需要补全时存在的增量状态接替；仍需实测其收敛与内存边界。

## 同步算法优化：持续发现和补全

Service 封装与锁只是结构基础。同步算法应从“构建整份临时 Registry，完成后替换”转向“持续发现成员，逐个补全 Service，最后确认就绪”。本节是后续算法设计目标，尚未修改 SDK 实现。

### 当前实现存在的优化差异

| 项目 | Go / Rust | C++ 当前实现 |
| --- | --- | --- |
| 单实例版本缺口 | 已有 UUID 集合的定向修复 | `applied->repair` 调用同一个 `start_sync`，重新执行完整 HSCAN |
| 缓存内容 revision 未变 | 先 HMGET revision/timestamp，必要时再 HGETALL | 每个扫描条目直接 HGETALL |
| 扫描与读取 | 按页发现后等待该页读取，再继续扫描 | 同样按页串行推进 |

对应位置：[Go repair](D:/projects/verdandi/sdk/go/registration/selector_core.go:848)、[Rust fetch_records](D:/projects/verdandi/sdk/rust/src/registration/selector.rs:1124)、[C++ 缺口处理](D:/projects/verdandi/sdk/cpp/src/selector.cpp:1151)、[C++ 全量读取](D:/projects/verdandi/sdk/cpp/src/selector.cpp:641)。这是算法和工作量的差异，不是断言三种实现返回不同协议结果。

### 1. 统一一个有界的待补集合

HSCAN 返回 UUID/revision 提示后，在 Registry 中发现或复用 Service。PUB/SUB、扫描和读取结果都更新同一对象及其完整性要求：

- 当前连接代的完整状态已经覆盖观察到的版本要求：不排读取。
- 完整基线可信，但还需校验租约或权威元数据：批量读取头部。
- 无完整基线、观察到版本缺口或头部表明内容变更：排完整读取。
- 后续 SUB 消除了缺口：撤销尚未发出的读取需要。

每个 UUID 最多有一个在途读取。新的缺口只更新该 Service 的版本要求，不重复追加相同请求。待补工作按有界批次公平处理，持续变化的热点实例不能让其他实例一直等不到读取机会。

跨连接代的旧缓存仍需重新验证，不能仅凭旧 Service 曾经完整就跳过新代校验。保留原来有效的头部读取优化，同时把它补齐到 C++，再增加同代 SUB 已经补全时的零读取路径。

### 2. 把下一页扫描和当前页补全组成流水线

拿到一页的 `next cursor` 后，可以把“下一次 HSCAN”和“本页仍需要的记录读取”放进同一批命令。一个同步任务即可驱动，无需为每个 UUID 创建任务。

流程为：首次 HSCAN → 批量发送下一页 HSCAN 与本页所需读取 → 合并结果并重新筛选待补项 → 继续；最后一页结束后排空剩余读取。

这是减少网络往返等待，不是消除 HSCAN 的 cursor 依赖，也不是保证这些命令构成原子快照。Redis pipelining 可以减少往返开销，同时会占用回复缓冲，因此仍须限制批次大小和响应内存。[Redis pipelining 文档](https://redis.io/docs/latest/develop/using-commands/pipelining/)

HSCAN 的 COUNT 是提示，不能当作严格返回条数上限；实际页结果需要按条目和内存预算拆分读取。第一版采用一个有界流水窗口，先不引入自适应并发控制或每实例任务。

### 3. 当前 v1 下让旧读取结果与连续 SUB 增量合流

本节适用于 Patch 路径；完整 Update 不进入连续区间，直接形成新的可变内容基线。无论采用哪种操作，都先检查连接代、Service 身份和终止状态，再判断内容版本。

| 读取结果与当前状态 | 动作 |
| --- | --- |
| 返回完整 revision 13，已经观察到的内容要求最多为 13 | 接纳；同 revision 的较新 Renew 只推进租约 |
| Service 已完整到 13，旧读取只返回 11 | 忽略读取，不再排额外回读 |
| 读取返回 11，Service 保存了连续的 Update 12、13 | 在完整 11 上合并连续增量，直接得到完整 13 |
| 读取返回 11，只观察到 Update 13，缺少 12 | 无法证明完整，继续回读或等待覆盖缺口的完整 Register |
| 读取返回 14，暂存增量只到 13 | 采用完整 14，不能把较旧的增量重新覆盖上去 |
| 收到完整 Register 且覆盖所有版本要求 | 建立新完整基线、清理已覆盖增量和待补需要 |

空读取没有可比较的内容 revision，需要单独检查读取期间的实例变化。若 SUB 已证明该 Service 新建或恢复为存在状态，较早发起的空读取不能把它删除或降为 retained；必要时再验证缺口，而不是套用普通版本大小比较。

增量状态仅在 Service 尚待补全时存在。对于同一连接代、期间没有 Register 重置或 Unregister 的连续 Update 区间，可保存每个字段的最后值、必要的 Version/timestamp 以及区间边界，避免保留每条消息的正文列表。发生缺口就失去“凭此区间补全”的资格；完整读取或完整 Register 覆盖缺口后恢复。

例如，暂存区间为 revision 11..13，完整读取的版本落在 10..13，且整个区间连续，则以读取为基线合并字段最后值可得到版本 13；若读取比 13 更新，应丢弃这些旧增量；若读取早于 10，仍有未覆盖区间，不能直接拼成 13。完整记录的形状、容量、租约和用户转换校验仍应在发布前完成。

本轮对这个受限字段合并规则执行了离线抽象模型：两字段、四次连续更新的 4,096 种序列，分别检查五个完整读取位置，共 20,480 次比较通过。模型不包含真实 Redis、并发调度、删除、主切换、字段结构变化或异常，因此不计作 SDK 测试通过。缺失中间 revision 的反例仍会拼出错误字段，必须走完整回读。

### 4. 局部修复不升级为整库扫描

成员已发现且订阅连接代仍可信时，K 个 Service 有缺口只修 K 个。C++ 应补齐这个路径，不再因单实例缺口调用完整 HSCAN。初次连接、订阅断开/滞后丢消息、主切换等使成员连续性失效时，才重新进行成员发现。

在有 5,000 个成员且仅一个实例出现缺口的例子中，目标是读取该实例并完成必要栅栏，避免重读其他 4,999 个实例；这是目标工作量，不是已经测出的加速倍数。

对外仍先保持当前 Ready 语义：一个待修 Service 可以让完整选择暂不可用，但不要求内部把所有 Service 重建一遍。局部修复不增加公开连接代 generation；完成新的连接代同步才推进 generation。

### 5. 按一轮补全确认就绪，避免每条记录单独等待

完成成员发现、所需读取及应用后，对本轮做一次同订阅连接的 PING/PONG 栅栏。若栅栏前的事件产生新缺口，只补这些 UUID，再确认下一轮。不要给每个 Service 分配独立 PING 或完整同步任务。

同一轮同步共享原始绝对截止时间与容量预算，不能每次补几条记录就重新开始计时。Ready 必须同时满足：成员发现完成、没有未解决的相关读取/版本缺口、订阅栅栏已处理、连接代仍有效，以及到期状态已处理。这里证明的是当前订阅前缀已被完整处理，不是所有记录处于一个全局原子时间点。

### 验证收益的指标

在相同数据与故障条件下比较：首次 Ready 时间、局部缺口恢复时间、HSCAN/HMGET/HGETALL 次数、重复读取次数、锁等待、订阅滞后、补全期间增量字节和内存峰值。分别覆盖冷启动、缓存可复用、SUB 提前补全、单实例缺口、高频持续更新和主切换。

冷启动时，安静且从未获知完整字段的实例仍需要完整读取；不能预设所有场景都能减少读取条数。流水线收益主要来自等待缩短，Service 补全与定向修复的收益主要来自避免不必要的工作，两者应分别测量。

## 引用视图、选择事务与全量快照

**引用视图。** 在锁持有期间通过内部 Service 提供只读访问，退出回调或 guard 作用域即结束借用。业务代码不获得可以长期保留并任意修改的 `*Service`。跨 UUID 的迭代顺序保持现有约定；可以按成员变化惰性重建有序 UUID 索引，避免每次 Data 变化都重新排序全部实例。

**锁的代价。** 当前不可变视图让接收者能够在业务回调期间继续更新自己的状态。改成同一把锁后，长回调也会阻塞 SUB 应用和过期处理。必须保留同步短回调、禁止同一 Selector 重入、锁等待取消/超时等约定。释放锁或关闭对象时不能持锁等待工作线程退出；关闭及连接失效还需要能及时拒绝新的操作。

**预测事务。** `Mutate` 成功之后回调仍可能报错、取消或返回重复候选。锁只负责互斥，不会自动回滚已经写入 Service 的预测。因此仍需一份只包含本次实际修改实例的暂存集合；先完成校验、返回值构造和可能失败的转换，再统一提交。现有错误原子性及候选所属对象/事务身份校验要保留。

**全量快照。** 缓存键采用连接代和本地可见状态版本；不能只用单个 Registration 的 revision 或仅在重连时递增的 generation。远端可见变化、本地预测提交、活动/保留状态变化、过期和 Ready/Closed 变化都要正确使缓存失效；读取缓存时还要检查最近到期时间。

快照内容必须与可变 Service 隔离。可在锁内捕获不可变记录引用，然后锁外完成昂贵复制；写回缓存时再次检查版本。若记录本身原地可变，则必须在锁内取得独立内容，不能只复制 Service 指针后释放锁。

现有公开 Snapshot 承诺返回独立结果。即使内部快照缓存命中，向调用者交付可写 Attr/Data 时仍需要独立复制。若未来提供可长期共享的只读 SnapshotHandle，需要作为明确的 API 设计单独审核；不能暗中改变原有别名和所有权语义。

## 各语言的影响

| 实现 | 可以收敛的部分 | 需要保持的边界 |
| --- | --- | --- |
| Go | `selectorState/view` 索引、外置 overlays、pending 正文合并；内部 Service 可保留类型投影 | 当前 channel gate 支持 Context 取消，直接换普通 `Mutex.Lock` 会改变等待行为；引用不得逃逸，用户可变切片/map 不得污染快照 |
| Rust | `ArcSwap` 整份视图、外置类型/预测对账及 drain 命令；用稳定 Service 所有权管理在途读取 | 异步等待锁和取消语义、借用生命周期；状态锁不得跨 Redis await，失败/取消不能留下半提交 |
| C++23 | 原子整份 `shared_ptr` 视图、pending、外置预测表；Service 封装版本与生命周期 | timed lock、异常原子性、读票据的对象存活、关闭时解锁后 join |
| C ABI / Legacy | 复用同一原生 Service 与 Registry | 公开 opaque handle、候选身份、回调期借用和显式释放约定保持一致 |
| C# | 原生核心的上述变化会直接受益；托管借用包装继续保持集中 | SafeHandle 父子链、回调异常回传、Dispose 与独立快照结果；不能只加托管锁就视为保护了原生更新 |
| Lua / Python | 候选协议需明确区分完整 Update 与 Patch；Python 承载新设计的跨语言场景 | 两种事件共用 revision 规则，Unregister 仍无 revision；所有语言使用同一组混合时序断言 |

当前 `SelectorPublishInterval` 和 pending 容量配置涉及公开配置约定。新设计需要明确哪些废弃、哪些保留；不能直接把发布间隔改称快照缓存有效期，或把消息字节限制悄悄解释成其他容量。

## 候选迁移顺序与验收

先引入 Service，把记录转换、完整性判断、预测对账和生命周期放到同一对象；随后用 Registry 锁替代重复发布索引，将快照改为按需构造。协议候选采用显式 Patch/Update 和单一 content revision：原型先加入完整 Update 的简单覆盖路径，再把现有 Patch 连续性规则缩进 Service 的有界窗口。验证两种操作的 Redis 时间、事件字节、Selector CPU、缺口恢复和混合顺序后，才能决定是否进入正式实现。不能把现有 Registry 级正文缓冲原样带入新架构。

除现有回归外，新设计应增加确定性时序覆盖：

- 旧完整读取晚于 Update、新完整读取早于旧 SUB。
- Unregister 先应用、旧读取后返回，包括本地原先没有活动实例。
- 空读取在新的 Register 之后返回，不能删除新实例。
- 无基线 Update/Renew、跨 revision 缺口与同 revision 较新 timestamp。
- Patch→Update、Update→Patch、多个 Patch 同字段覆盖，以及同一邮箱批次中的确定准入顺序。
- Patch 缺口后由完整 Update 直接恢复；Version-only Patch 缺口不能被仅含 Data 的伪完整消息掩盖。
- v1 局部 update 与候选新协议事件不得互相误判，混合部署要么被隔离，要么明确拒绝。
- 完整 Register 先到后取消未发出的读取；迟到旧读取不会再次把 Service 置为待补。
- 读取版本落入连续增量区间时直接补全；缺少中间 revision 时回读；读取更高版本时丢弃较旧增量。
- Pipeline 混合扫描与补全的回复对应关系、头部读取 fallback、超出 COUNT 提示的页拆分和最后一批排空。
- partial Fields 即使能通过用户 Decoder，也不会被错误标记为完整。
- 全部已知 Service 都完整，但仍有未被 SUB 提及的 Registry 成员时，继续完成成员发现。
- 扫描中途新增、删除、重复返回，以及临近结束才出现的实例。
- 主切换后较低权威 revision、旧连接迟到结果、连续两次恢复。
- 没有新消息时自然过期、retained 到期，以及快照缓存的对应失效。
- 跨多个 Service 的预测提交在异常、取消、错误候选及重复选择时全部回滚。
- 已返回全量快照在后续更新、本地预测和关闭之后保持内容不变。
- 慢回调、慢扫描和热点持续更新：验证锁等待、订阅滞后、回读风暴、Ready 收敛及内存上限。

优先采用“稳定 Service 对象 + Registry 锁 + 按需快照”的收敛方向。去掉消息正文合并也值得尝试，但应以这些时序测试和负载结果作为保留该改法的依据。

关于同 UUID Attr 复用及增量同步的复杂度审核，见 [Selector 重连剪枝与分层增量同步探索性审核](D:/projects/verdandi/registration/selector-incremental-sync-review-20260908.md)。当前只将 Attr 复用纳入基础实现范围；显式 Patch/完整 Update 和条件读取均须先做独立原型，Registry/全局游标、变化索引、历史 epoch 和字段 revision 均不属于当前实施计划。
