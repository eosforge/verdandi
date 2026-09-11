**Verdandi 全量源码审计记录 · 2026-09-07**

本轮已完成当前工作树中应用源码、测试源码、构建/测试脚本和规范 Lua 源片段的静态审读；**完整 SDK 集成与 Windows/Linux 矩阵测试尚未完成**。共整理 **25 项可定位问题**：P1 12 项、P2 12 项、P3 1 项，其中 15 项有真实执行或隔离验证证据，10 项由源码路径/复杂度确认。另列 5 项需要故障或并发测试的风险，不计入上述缺陷数。

审计基于 `alpha` 分支、HEAD `81cb8b916d1bee155fe9b58ff8ee71fc53f2bb67` 加当时未提交的工作树修改。本轮未修改生产代码，也未替换既有整理结果；新增的是本报告、证据清单及 `build/review-20260907/` 中被忽略的验证夹具。每条发现均审查 Go、Rust、原生 C++、C ABI/Legacy、C# 及共享协议/工具的影响。

完整路径、SHA-256、审读依据、测试范围和发现记录保存在 [审计证据 JSON](<D:/projects/verdandi/testkit/results/code-audit-20260907.json>)。缺陷探针以“旧代码确实表现出错误”为成功条件，**探针通过不表示问题已修复**。

统计以审计开始时的 677 个版本化或非忽略文件为基线。源代码口径为 319 个通常代码文件，加 12 个规范 `.lua.inc` 和 1 个 C++ 代码模板，共 **332 个文件、79,711 物理行**。所有非生成 Lua 源文件的已记录审读区间覆盖当前文件哈希，未读代码区间为 0；32 份生成 Lua 通过两个生成器逐字节一致性检查覆盖，不声称重复人工读过四份。

物理行包含空行与注释，不能等同有效代码行。审读器可以省略空行和纯 `//` 注释；关键契约和构建规范另行核对。历史报告、结果数据、锁文件不按业务代码逐行评级；229 个 JSON/TOML/锁文件完成语法解析，不能因此声称所有历史测试已重新运行。外部第三方库源码不在本次全量审计范围内。

| 类别 | 文件数 | 物理行 |
|---|---:|---:|
| 手写运行时代码及代码模板 | 169 | 44,834 |
| 测试、测试工具与夹具 | 122 | 25,985 |
| 构建与开发工具 | 8 | 2,852 |
| 生成 Lua 与 Go 示例代码 | 33 | 6,040 |

手写运行时约占 56%。因此不应把约 8 万行全部作为生产逻辑的精简目标，也不应靠删除测试、注释、兼容包装或生成副本来制造降幅。Lua 的 6,756 行中，规范片段仅 864 行，生成副本为 5,892 行。

| 语言 | 文件数 | 物理行（含测试/生成） |
|---|---:|---:|
| Go | 105 | 24,289 |
| Rust | 64 | 15,222 |
| C++ | 66 | 16,162 |
| C | 9 | 856 |
| C# | 18 | 6,449 |
| Lua | 44 | 6,756 |
| Python | 18 | 7,554 |
| PowerShell | 1 | 1,036 |
| Bash | 1 | 860 |
| CMake | 6 | 527 |

下面的评分按正确性、资源与生命周期、可维护性、验证质量综合判断，满分 10 分；是当前实现的审查意见，不是语言优劣或性能排名。C# 与 C ABI 的接口质量单独评价，依赖的 C++ 核心问题仍计入交付风险。Legacy 保持 C++11/14/17 兼容，不以缺少 C++23 语法扣分。

| 实现 | 评分 | 优点 | 主要不足 |
|---|---:|---|---|
| Go | 7.5 | 类型化 API、独立生命周期计数、共同协议向量与测试较丰富；索引堆和事务身份约束较完整。 | 空 Map panic、初始化退出通知缺口、严格 JSON 大小写偏差；Selector 与测试支撑代码较长。 |
| Rust | 7.0 | 核心禁止 unsafe，Result 与所有权边界清晰；有界解码、共享快照与索引队列有较好基础。 | 构造期取消和 await 前后 RAII 不完整；UTF-8 错误路径 panic；配置/批次结构复制较多。 |
| C++23 | 6.0 | std::expected、拥有型字段和明确 core/C ABI 边界；当前值/事件辅助逻辑已抽出，可离线验证。 | 同步状态机、事件合并、异常提交、checkpoint 和生命周期均有缺口；手工资源与重复辅助逻辑最多。 |
| C ABI / C 消费端 | 7.0 | 接口使用明确的句柄、字节视图和释放函数，能复用同一 C++ 核心；兼容边界清楚。 | 继承原生核心问题；分配失败错误边界待故障注入；测试清理读取未初始化 key。 |
| C# | 7.0 | SafeHandle、Nullable、不可变 Fields 和回调边界较规范；托管层范围相对集中。 | 提交后再次解码破坏错误原子性；Fields/回调存在重复分配；并发 Dispose 结果语义待验证。 |
| Lua | 7.0 | 规范源片段集中、脚本原子执行，写入前有较多严格校验；32 份副本可机器验证一致。 | Patch 总字段数量约束缺失，对所有 SDK 产生同一协议缺陷。 |
| Python | 6.5 | 已有跨语言、Sentinel、故障和长稳态测试组织；工具结果能落盘。 | 共享夹具所有权清理、peer 启动失败、进程管道和元数据可信度需要加固；WSL 与 VM 路径还需整理。 |
| PowerShell | 7.0 | Windows 工具探测与错误退出有明确流程；依赖策略和配置失败路径的 7 个离线场景通过。 | 单脚本较长，与 Bash 重复维护依赖/产物元数据；完整原生构建仍受 CMake 问题影响。 |
| Bash | 6.5 | 依赖查找和不隐式构建策略有 7 个离线场景覆盖；可用于独立 Linux 构建。 | 部分工具路径调用的引号和 JSON 转义有健壮性改进空间；与 PowerShell 大量重复。 |
| CMake | 6.5 | 依赖模块和离线测试入口已拆分；依赖策略的 4 个离线场景通过。 | Lua 文件未形成增量构建依赖；空运行时 DLL 列表导致 Windows POST_BUILD 失败。 |

**问题清单的阅读方式：**P1 应在下一次稳定性验收前优先修复；P2 随后修复并增加针对性覆盖；P3 为有明确退化条件的优化项。优先级不代表已发生生产事故。证据明确区分真实数据库、原实现隔离执行、API 行为验证和源码确认。

| 编号 | 优先级 | 问题 | 证据 |
|---|---|---|---|
| A01 | P1 | [Catalog Patch 可把合法 Map 写成无法读取的超限记录](<D:/projects/verdandi/lua/src/catalog/actions/patch.lua.inc:4>) | 真实 Redis 执行 |
| A02 | P1 | [Go Catalog 对空 Map 应用 Patch 会 panic](<D:/projects/verdandi/sdk/go/catalog/value.go:184>) | 原实现隔离执行 |
| A03 | P1 | [C++ 订阅队列满时会静默丢消息并保留 fence](<D:/projects/verdandi/sdk/cpp/src/driver.cpp:716>) | 实际函数原文隔离执行 |
| A04 | P1 | [C++ 待处理事件合并掩盖修订缺口并低估占用](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:534>) | 实际类原文隔离执行 |
| A05 | P1 | [C++ Catalog 初始同步转入精确修复后丢失就绪状态](<D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp:1092>) | 源码路径确认 |
| A06 | P1 | [C++ 将 SQLite 正常查询结束当作 checkpoint 故障](<D:/projects/verdandi/sdk/cpp/src/catalog_checkpoint.cpp:417>) | SQLite C API 行为验证＋源码定位 |
| A07 | P1 | [Rust 截断非 ASCII 错误详情会 panic](<D:/projects/verdandi/sdk/rust/src/error.rs:104>) | 原源文件执行 |
| A08 | P2 | [C++ 与 Rust 的 Choice 没有校验所属 Selector](<D:/projects/verdandi/sdk/cpp/include/verdandi/registration/selector.hpp:90>) | C++ 实际公开模板执行；Rust 源码确认 |
| A09 | P1 | [C++ Selector 提交遇到用户 Data 移动异常会部分成功](<D:/projects/verdandi/sdk/cpp/include/verdandi/registration/selector.hpp:526>) | 实际公开模板执行 |
| A10 | P1 | [C# 与 Legacy 在提交后解码返回值，失败不会撤销预测](<D:/projects/verdandi/sdk/csharp/src/Verdandi/Registration/Selector.cs:131>) | 源码路径确认 |
| A11 | P1 | [Go Selector 创建过程中关闭父客户端可能永久等待](<D:/projects/verdandi/sdk/go/registration/selector_core.go:145>) | 源码路径确认 |
| A12 | P2 | [Rust Catalog 构造 future 被取消时后台任务缺少取消守卫](<D:/projects/verdandi/sdk/rust/src/catalog/subscriber.rs:123>) | 源码路径确认 |
| A13 | P2 | [Rust 动态扩池期间取消会泄漏活跃命令计数](<D:/projects/verdandi/sdk/rust/src/client.rs:204>) | 源码路径确认 |
| A14 | P2 | [C++ Catalog 保留的空 Map/Array 在状态变化后消失](<D:/projects/verdandi/sdk/cpp/include/verdandi/catalog/subscriber.hpp:81>) | 实际公开模板执行 |
| A15 | P1 | [C++ Registration 移动赋值不会关闭被覆盖的注册](<D:/projects/verdandi/sdk/cpp/include/verdandi/registration/registration.hpp:83>) | 实际公开操作执行＋worker 所有权源码确认 |
| A16 | P2 | [三种原生 SDK 的 Patch 容量预估错误拒绝合法交换](<D:/projects/verdandi/sdk/go/catalog/publisher.go:221>) | 跨语言源码与算例确认 |
| A17 | P2 | [Go 严格配置解析仍接受错误大小写及折叠重名](<D:/projects/verdandi/sdk/go/configuration/json.go:55>) | 实际 JSON 解码实现隔离执行 |
| A18 | P2 | [C++ Selector 截止时间堆随续租次数增长](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:690>) | 源码复杂度确认 |
| A19 | P3 | [合法的 retry factor=1 导致退避计算随失败次数变慢](<D:/projects/verdandi/sdk/go/registration/selector_core.go:1370>) | 源码复杂度确认 |
| A20 | P2 | [仅修改 Lua 后增量构建 C++ 可能仍嵌入旧脚本](<D:/projects/verdandi/sdk/cpp/cmake/EmbedProtocol.cmake:11>) | 实际 CMake 模块执行 |
| A21 | P2 | [Windows 运行时 DLL 列表为空时构建后复制失败](<D:/projects/verdandi/sdk/cpp/CMakeLists.txt:127>) | 等价 CMake 命令执行 |
| A22 | P1 | [Sentinel 夹具部署拒绝后仍可能删除原有同名资源](<D:/projects/verdandi/testkit/sentinel/sentinel_test.py:190>) | 实际 Topology 类＋无副作用记录器执行 |
| A23 | P2 | [Python peer 启动失败存在未接管的子进程](<D:/projects/verdandi/testkit/catalog/interop_test.py:94>) | 源码路径确认 |
| A24 | P2 | [C ABI Redis 测试失败清理会读取未初始化 key](<D:/projects/verdandi/sdk/cpp/tests/c_abi_redis_test.c:86>) | 源码路径确认 |
| A25 | P2 | [Go 引用代码生成器把遮蔽内建名字的切片当成标量](<D:/projects/verdandi/sdk/go/cmd/verdandi-refgen/main.go:275>) | 实际生成器分类函数执行 |

**A01 · P1 · Catalog Patch 可把合法 Map 写成无法读取的超限记录**

位置：[lua/src/catalog/actions/patch.lua.inc:4](<D:/projects/verdandi/lua/src/catalog/actions/patch.lua.inc:4>)；[lua/src/catalog/actions/patch.lua.inc:42](<D:/projects/verdandi/lua/src/catalog/actions/patch.lua.inc:42>)；[lua/src/catalog/actions/read.lua.inc:127](<D:/projects/verdandi/lua/src/catalog/actions/read.lua.inc:127>)。

触发与结果：已有 65,536 个字段的合法 Map 再 Patch 一个新字段，记录总字节数仍小于 4 MiB。脚本只限制本次 set_count，没有限制合并后的总字段数。 实测 Replace 返回 revision=1，Patch 返回成功 revision=2，实际字段数变成 65,537，随后同一套 Read 脚本返回 corrupt。成功写入破坏了自身读取约束。

跨语言审查：Go、Rust、C++、C ABI、C#、Legacy 全部使用同源 Catalog Lua，因此全部受影响；32 份生成脚本与已审读源片段的一致性检查通过。

建议：在第一次写 Redis 之前计算 HLEN 去掉 4 个元字段后的数量，加上本次真正新增的字段数，并拒绝超过 maximum_fields 的结果。并发正确性由 Lua 保证，SDK 预检查只能辅助。

证据及回归要求：真实 Redis 执行。跨 SDK 加入 65,535→65,536 成功、65,536→65,537 拒绝且 revision/内容不变、同名字段更新成功的边界用例。


**A02 · P1 · Go Catalog 对空 Map 应用 Patch 会 panic**

位置：[sdk/go/catalog/value.go:184](<D:/projects/verdandi/sdk/go/catalog/value.go:184>)；[sdk/go/catalog/read.go:125](<D:/projects/verdandi/sdk/go/catalog/read.go:125>)；[sdk/go/catalog/subscriber.go:374](<D:/projects/verdandi/sdk/go/catalog/subscriber.go:374>)。

触发与结果：cloneFields 把空 Map 复制成 nil，增量读取或订阅更新随后直接向这个 map 赋值。空 Map 本身是合法状态。 使用实际读取解码代码的离线探针触发 assignment to entry in nil map。后台订阅应用更新处存在同一写法，可能导致进程崩溃。

跨语言审查：确认为 Go 特有的 nil map 写入问题。Rust 的 BTreeMap 和 C++ 的 std::map 空容器仍可写；C#、Legacy 经 C++ 实现不继承这一具体问题。

建议：区分只读复制与后续可写复制，写入前 make 非 nil map，并保留字段字节的深复制。仅换成 maps.Clone(nil) 仍不能修复。

证据及回归要求：原实现隔离执行。分别覆盖 Read 增量补全和实时订阅的空 Map→非空 Map；验证原快照不被改写。隔离探针替代了传输层与少量常量，不代表完整 Go SDK 集成通过。


**A03 · P1 · C++ 订阅队列满时会静默丢消息并保留 fence**

位置：[sdk/cpp/src/driver.cpp:716](<D:/projects/verdandi/sdk/cpp/src/driver.cpp:716>)；[sdk/cpp/src/driver.cpp:731](<D:/projects/verdandi/sdk/cpp/src/driver.cpp:731>)；[sdk/cpp/src/selector.cpp:1443](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:1443>)；[sdk/cpp/src/catalog_subscriber.cpp:1162](<D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp:1162>)。

触发与结果：队列已经装满 message，接着进入 fence 等控制事件。通用满队列分支直接 clear，仅 message 入队分支设置 lagged。 原文函数探针确认，两个消息被清空，队列只剩 fence，lagged=false。上层可能接受屏障并认为已对齐，但屏障之前的更新已经丢失；已有 lagged 标记也可能被控制事件清掉。

跨语言审查：C++ Registration 与 Catalog 共用 driver；C ABI、C#、Legacy 继承。Go 的同步 Receive/Pong 路径和 Rust 的显式 Lagged 处理未发现同形的无标记清队列行为。

建议：把丢失状态保存在不会被清队列抹掉的位置，在消费任何后续 fence 前先报告丢失并启动重同步；如拆分控制队列，仍须维护事件和屏障的顺序关系。

证据及回归要求：实际函数原文隔离执行。满队列+fence、已产生 lagged+fence、重连和关闭事件分别测试；再做真实高频发布下的全量状态一致性检查。当前探针未包含网络与订阅线程。


**A04 · P1 · C++ 待处理事件合并掩盖修订缺口并低估占用**

位置：[sdk/cpp/src/selector.cpp:534](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:534>)；[sdk/cpp/src/selector.cpp:571](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:571>)；[sdk/go/registration/pending.go:149](<D:/projects/verdandi/sdk/go/registration/pending.go:149>)；[sdk/rust/src/registration/pending.rs:158](<D:/projects/verdandi/sdk/rust/src/registration/pending.rs:158>)。

触发与结果：Register revision=1 后收到 base=2/revision=3 的 Update；另一个场景是两个相邻 Update 分别携带不同的大字段。 前一场景被合并成完整的 Register revision=3，缺少 revision=2 的变化却不要求修复。后一场景用 max(旧消息大小, 新消息大小) 计费；探针中实际字段数据 24,002 字节仍通过 15,000 字节预算。两个场景独立验证。

跨语言审查：C++ 及其 C ABI/C#/Legacy Registration 选择器受影响。Go、Rust 明确检查修订相邻性并维护合并后大小，是可借鉴的实现。Catalog 使用另一套 pending 逻辑，不能按同名结构直接认定传播。

建议：遇到修订缺口标记精确读取修复；对已有字段和版本信息实施与其他 SDK 一致的合并规则；按合并后保留的数据计算字节预算。

证据及回归要求：实际类原文隔离执行。把缺口、字段缺失、重叠/不重叠字段、版本更新、容量刚好达到上限的跨 SDK 向量放在共同语义测试中。


**A05 · P1 · C++ Catalog 初始同步转入精确修复后丢失就绪状态**

位置：[sdk/cpp/src/catalog_subscriber.cpp:1092](<D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp:1092>)；[sdk/cpp/src/catalog_subscriber.cpp:1104](<D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp:1104>)；[sdk/cpp/src/catalog_subscriber.cpp:1128](<D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp:1128>)。

触发与结果：首次 scope 同步处理缓冲事件时发现 stale，于是启动精确路径修复并 continue；这发生在 aligned=true 之前。 精确修复结束后 exact=true，不再设置 aligned；首次 ready 不会成功发送，后续消息持续缓冲，构造调用最终可能超时，虽然实际修复已经完成。

跨语言审查：C++ Catalog 及 C ABI/C#/Legacy 继承。Go、Rust 的 scope 同步等待者/批次上下文保留了初始就绪责任，未发现相同控制流缺口。

建议：将初始对齐的完成责任跨精确修复保留，在当前代所有必要修复完成后统一发布 aligned 与 ready。不要通过过早设置 aligned 跳过修复。

证据及回归要求：源码路径确认。用可控屏障在初始扫描与事件回放之间制造 stale，断言精确修复后构造成功且快照完整。本轮未执行该并发集成场景。


**A06 · P1 · C++ 将 SQLite 正常查询结束当作 checkpoint 故障**

位置：[sdk/cpp/src/catalog_checkpoint.cpp:417](<D:/projects/verdandi/sdk/cpp/src/catalog_checkpoint.cpp:417>)；[sdk/cpp/src/catalog_checkpoint.cpp:453](<D:/projects/verdandi/sdk/cpp/src/catalog_checkpoint.cpp:453>)；[sdk/cpp/tests/redis_integration_test.cpp:328](<D:/projects/verdandi/sdk/cpp/tests/redis_integration_test.cpp:328>)。

触发与结果：entries 查询遍历正常到达 SQLITE_DONE，但循环外没有保存 step，而是要求 sqlite3_errcode(database)==SQLITE_OK。 本机 SQLite 3.53.1 的真实 C API 返回 step=101、errcode=101，即 SQLITE_DONE；现有分支因此禁用健康存储。现有重开测试允许 Redis 在线，可能由 Redis 重同步补回数据而掩盖 checkpoint 未被读取。

跨语言审查：C++ SQLite 实现及 C ABI/C#/Legacy 继承。Go 使用 bbolt，Rust 使用 redb，不共享这段 SQLite 错误判断。

建议：保留最后一次 sqlite3_step 的返回值并要求 SQLITE_DONE；提交事务单独判断。用事务 RAII 统一回滚路径。

证据及回归要求：SQLite C API 行为验证＋源码定位。直接验证空和非空 checkpoint 的 load、存储未 disabled，以及 Redis 不可达时能得到已有保留数据。本轮验证的是 SQLite API 与现有分支，不是完整 C++ checkpoint 二进制测试。


**A07 · P1 · Rust 截断非 ASCII 错误详情会 panic**

位置：[sdk/rust/src/error.rs:104](<D:/projects/verdandi/sdk/rust/src/error.rs:104>)；[sdk/rust/src/error.rs:116](<D:/projects/verdandi/sdk/rust/src/error.rs:116>)；[sdk/rust/src/catalog/checkpoint.rs:340](<D:/projects/verdandi/sdk/rust/src/catalog/checkpoint.rs:340>)。

触发与结果：错误详情超过 512 字节，且第 512 字节处于 UTF-8 字符内部；例如连续 200 个汉字。 直接引入实际 error.rs 的 rustc 探针确认 driver 和 field_driver 两个错误构造器均 panic；checkpoint 错误详情具有同样 truncate(512) 写法。

跨语言审查：Rust 存在 panic。C++ 的按字节 resize 不会产生同种 panic，但可能输出被截坏的 UTF-8；C# Encoder.Convert 保持字符边界，Go 没有对应的 512 字节切割逻辑。

建议：提取统一的 UTF-8 有界详情函数：从上限回退到 is_char_boundary 再 truncate。保持 Rust 1.85 最低版本，不直接采用需要更高 MSRV 的边界 API。

证据及回归要求：原源文件执行。覆盖 ASCII、汉字、四字节字符和 511/512/513 字节边界；对所有错误入口及 checkpoint 使用同一组用例。


**A08 · P2 · C++ 与 Rust 的 Choice 没有校验所属 Selector**

位置：[sdk/cpp/include/verdandi/registration/selector.hpp:90](<D:/projects/verdandi/sdk/cpp/include/verdandi/registration/selector.hpp:90>)；[sdk/cpp/include/verdandi/registration/selector.hpp:492](<D:/projects/verdandi/sdk/cpp/include/verdandi/registration/selector.hpp:492>)；[sdk/rust/src/registration/selector.rs:256](<D:/projects/verdandi/sdk/rust/src/registration/selector.rs:256>)；[sdk/rust/src/registration/selector.rs:405](<D:/projects/verdandi/sdk/rust/src/registration/selector.rs:405>)。

触发与结果：两个 Selector 的本地事务 token 同为 1，把 A 的 Choice 交给 B。Choice 只携带 token/index，没有实例身份。 实际 C++ One 接受了外部 Choice 并选择 B 的同索引记录，没有拒绝跨实例身份。Rust 的结构与校验条件存在相同缺口。

跨语言审查：原生 C++ 和 Rust 受影响。Go 校验 transaction 指针和 token；C# 使用进程范围的事务编号。C ABI 不向托管层暴露此原生 Choice；Legacy 的公开 index API 应单独说明其较弱约束，不能自动等同为这一 token 碰撞。

建议：身份至少包含所属实例与事务代次，并考虑实例地址复用。One、Any、Mutate 统一校验同一个身份规则。

证据及回归要求：C++ 实际公开模板执行；Rust 源码确认。加入跨 Selector、旧事务、重复 Choice、索引越界和移动后的实例用例。C++ 探针替换了 Redis core 为固定合法候选视图，Rust 尚未执行完整用例。


**A09 · P1 · C++ Selector 提交遇到用户 Data 移动异常会部分成功**

位置：[sdk/cpp/include/verdandi/registration/selector.hpp:526](<D:/projects/verdandi/sdk/cpp/include/verdandi/registration/selector.hpp:526>)；[sdk/cpp/include/verdandi/registration/selector.hpp:292](<D:/projects/verdandi/sdk/cpp/include/verdandi/registration/selector.hpp:292>)。

触发与结果：事务修改两个候选，提交以 insert_or_assign 逐个写 overlay；应用 Data 的第二次移动抛出异常。该 Data 仍满足当前公开模板约束。 探针中第一项从 1 变成 11，第二项仍是 2，而 One 返回失败。局部预测不是原子提交。这里指出的是异常下部分提交；提交全部 staged 项本身是当前三种原生 SDK 的设计。

跨语言审查：原生 C++ 泛型 API 已复现。C#、Legacy 走原生 Fields，不具备这个自定义 Data 移动触发器；它们的提交后解码问题另列 A10。Go、Rust 没有这种可抛 C++ 移动构造语义。

建议：先构造完整可提交结果，再通过保证不抛的替换发布；同时审查返回结果的移动是否还能在发布后失败。不要仅捕获异常而留下已经改动的 overlay。

证据及回归要求：实际公开模板执行。给复制/移动/赋值和分配设置故障点，验证失败前后整个预测状态一致；成功时仍提交所有 staged 项。


**A10 · P1 · C# 与 Legacy 在提交后解码返回值，失败不会撤销预测**

位置：[sdk/csharp/src/Verdandi/Registration/Selector.cs:131](<D:/projects/verdandi/sdk/csharp/src/Verdandi/Registration/Selector.cs:131>)；[sdk/csharp/src/Verdandi/Registration/Selector.cs:155](<D:/projects/verdandi/sdk/csharp/src/Verdandi/Registration/Selector.cs:155>)；[sdk/csharp/src/Verdandi/Registration/Selector.cs:205](<D:/projects/verdandi/sdk/csharp/src/Verdandi/Registration/Selector.cs:205>)；[sdk/cpp/include/verdandi/legacy/selector.hpp:210](<D:/projects/verdandi/sdk/cpp/include/verdandi/legacy/selector.hpp:210>)；[sdk/cpp/src/c/selector.cpp:175](<D:/projects/verdandi/sdk/cpp/src/c/selector.cpp:175>)。

触发与结果：回调里的修改和编码成功，C ABI 返回时原生事务已提交；封装随后将拥有型 Fields 再次解码成应用类型，自定义解码器在此失败。 调用者得到失败 Result，实际预测却已更新，违背失败不提交的事务预期。Any 的后续元素失败也可造成同类结果。现有初次候选解码失败测试未覆盖提交后的解码。

跨语言审查：C# 与 C++ Legacy 包装层都有此路径。Go、Rust 和原生 C++ 都在提交前准备类型化返回值；原生 C++ 的异常原子性另见 A09。C ABI 本身只处理 Fields，不能替包装层验证应用解码器。

建议：在回调提交边界之前完成最终类型化验证并缓存结果，或设计准备/提交两阶段接口。要求应用编码器永不失败不能替代已公开的 Result 错误语义。

证据及回归要求：源码路径确认。让解码器仅对新修改值或 Any 第二项返回错误，确认公共 API 失败时 snapshot 保持原值。本轮没有执行完整 C#/Legacy 场景。


**A11 · P1 · Go Selector 创建过程中关闭父客户端可能永久等待**

位置：[sdk/go/registration/selector_core.go:145](<D:/projects/verdandi/sdk/go/registration/selector_core.go:145>)；[sdk/go/registration/selector_core.go:224](<D:/projects/verdandi/sdk/go/registration/selector_core.go:224>)；[sdk/go/registration/selector_core.go:247](<D:/projects/verdandi/sdk/go/registration/selector_core.go:247>)。

触发与结果：首次 ready 前父客户端关闭，worker 从 owner 取消、transport 关闭或重试等待路径直接 return。创建者传入 context.Background。 defer 关闭了 done，却没有发送 ready；创建者只 select ready 和调用者 context，可能永久挂起。函数末尾的首次 ready 发送无法覆盖中间的 return。

跨语言审查：Go Registration Selector 受影响。Go Registration 注册 worker、Go Catalog 的初始化上下文，以及 Rust 的最终 ready 完成路径未发现相同缺口；C++ 使用自身的 promise/同步超时路径。

建议：创建者监听 done/父生命周期完成，或保证 worker 的唯一退出路径完成首次 ready。避免在多个 return 旁手工重复补发通知。

证据及回归要求：源码路径确认。在首次订阅、扫描、重试等待三个屏障分别关闭父客户端，使用无截止时间的调用者 context，并断言有限时间内返回 Closed。


**A12 · P2 · Rust Catalog 构造 future 被取消时后台任务缺少取消守卫**

位置：[sdk/rust/src/catalog/subscriber.rs:123](<D:/projects/verdandi/sdk/rust/src/catalog/subscriber.rs:123>)；[sdk/rust/src/catalog/subscriber.rs:152](<D:/projects/verdandi/sdk/rust/src/catalog/subscriber.rs:152>)；[sdk/rust/src/catalog/subscriber.rs:160](<D:/projects/verdandi/sdk/rust/src/catalog/subscriber.rs:160>)。

触发与结果：new 已启动同步和读取任务，在等待 ready 的 await 处由外部 select/timeout 丢弃构造 future。 此时还没有 Subscriber 实例执行 Drop，worker 又持有 inner 的 Arc；现有显式错误分支中的 cancel 不会运行。订阅和父生命周期计数可继续占用，直到父客户端关闭。

跨语言审查：Rust Catalog 特有的构造期取消路径。Rust Registration 对 ready 接收端消失有清理处理；Go 通过显式 context 结束初始化，C++/C# 不使用这一被丢弃 future 的模型。

建议：从获取子取消令牌开始建立 Drop 取消守卫，只有成功交付 Subscriber 才解除守卫；保持后续显式关闭可以等待任务结束。

证据及回归要求：源码路径确认。在 ready 之前 abort 构造任务，检查订阅连接、worker 计数和父 admit 计数最终归零。本轮未运行 Tokio 集成验证。


**A13 · P2 · Rust 动态扩池期间取消会泄漏活跃命令计数**

位置：[sdk/rust/src/client.rs:204](<D:/projects/verdandi/sdk/rust/src/client.rs:204>)；[sdk/rust/src/client.rs:209](<D:/projects/verdandi/sdk/rust/src/client.rs:209>)；[sdk/rust/src/client.rs:213](<D:/projects/verdandi/sdk/rust/src/client.rs:213>)。

触发与结果：先 commands.fetch_add，再 await driver.scale，最后才构造负责归还计数的 CommandGuard。 future 在 await 时被取消，计数已增加但守卫尚不存在，之后池扩缩判断会使用失真的活跃计数。

跨语言审查：Rust 根客户端影响两个业务域。C++ 在 creating++/slot 标记之后的可抛路径也缺少资源归还守卫，属于相近的异常风险，未运行内存故障注入；Go 由驱动管理连接池，没有相同手动计数路径。C#/Legacy 继承 C++ 相关风险。

建议：增加计数后立刻构造 RAII 守卫，再进入任何 await。C++ 相应改用 slot lease 和 creating 计数守卫。

证据及回归要求：源码路径确认。可控地阻塞 scale 并取消命令，验证计数恢复、后续命令和关闭不受影响；另做 C++ 创建/请求分配失败注入。


**A14 · P2 · C++ Catalog 保留的空 Map/Array 在状态变化后消失**

位置：[sdk/cpp/include/verdandi/catalog/subscriber.hpp:81](<D:/projects/verdandi/sdk/cpp/include/verdandi/catalog/subscriber.hpp:81>)；[sdk/cpp/src/catalog_subscriber.cpp:835](<D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp:835>)。

触发与结果：已有合法空容器，状态切换为 unavailable/synchronizing/closed；load 用非空或 present 来判断是否存在保留值。 空容器和没有值被混为一谈。固定合法 retained state 探针中，同样的空 Map 在 Present 时有值、Closed 时返回无值。

跨语言审查：C++、C ABI、C#、Legacy Catalog 继承。Go 使用 kind 判定、Rust 使用 Option 保留存在性，未发现同一缺口。

建议：用明确的存在标记或满足契约的 replace_revision 判定保留值，不能用容器 empty 替代存在性。

证据及回归要求：实际公开模板执行。覆盖空 Map 和空 Array 的 Present→Unavailable→Synchronizing→Closed，并与从未存在/已删除状态区分。探针替换了状态提供者，没有模拟断网。


**A15 · P1 · C++ Registration 移动赋值不会关闭被覆盖的注册**

位置：[sdk/cpp/include/verdandi/registration/registration.hpp:83](<D:/projects/verdandi/sdk/cpp/include/verdandi/registration/registration.hpp:83>)；[sdk/cpp/src/registration.cpp:701](<D:/projects/verdandi/sdk/cpp/src/registration.cpp:701>)；[sdk/cpp/include/verdandi/legacy/client.hpp:36](<D:/projects/verdandi/sdk/cpp/include/verdandi/legacy/client.hpp:36>)。

触发与结果：两个活跃注册执行 a=std::move(b)。默认移动赋值覆盖 a.core_，而 a 原来的 worker 持有自己的 shared_ptr。 旧注册丢失公开句柄但仍可继续续租，直到父域关闭。探针确认原公开移动赋值没有调用旧 core 的 close；实际续租循环未在该探针中运行。

跨语言审查：原生 C++ 特有。Legacy owned_handle 移动赋值先释放旧句柄，C ABI 显式 release 走析构关闭，Rust 对被替换值执行 Drop；Go 显式生命周期模型没有此移动赋值接口。

建议：自定义移动赋值，先安全关闭旧注册，再转移新 core，并正确处理自移动。保持关闭/析构的异常约束。

证据及回归要求：实际公开操作执行＋worker 所有权源码确认。真实 Redis 下检查旧记录停止续租且按契约注销，新记录继续工作；同时覆盖自移动、空值和已经关闭的注册。


**A16 · P2 · 三种原生 SDK 的 Patch 容量预估错误拒绝合法交换**

位置：[sdk/go/catalog/publisher.go:221](<D:/projects/verdandi/sdk/go/catalog/publisher.go:221>)；[sdk/rust/src/catalog/publisher.rs:138](<D:/projects/verdandi/sdk/rust/src/catalog/publisher.rs:138>)；[sdk/cpp/src/catalog.cpp:426](<D:/projects/verdandi/sdk/cpp/src/catalog.cpp:426>)；[lua/src/catalog/actions/patch.lua.inc:48](<D:/projects/verdandi/lua/src/catalog/actions/patch.lua.inc:48>)。

触发与结果：最大 150 字节，原记录 a 空、b 为 100 字节，Patch 改为 a 为 100 字节、b 空。原值、补丁和最终结果都为 102 字节，但按 a→b 处理时中间投影为 202。 Go、Rust、C++ 在循环内比较上限，提前返回 Capacity，合法补丁无法提交。

跨语言审查：Go、Rust、C++ 都有，C#/Legacy 继承原生层。Lua 在完整增减后校验最终值，没有同样的中间值拒绝。

建议：汇总完整净变化再比较最终上限；保留整数溢出、损坏数据和数组不得新增索引的检查。可先累计删减量和增加量避免无符号下溢。

证据及回归要求：跨语言源码与算例确认。加入互换大小、多个先增后减、刚好满额和最终确实超限的共同向量。本轮算例与源码确认，未执行三种 SDK 的发布调用。


**A17 · P2 · Go 严格配置解析仍接受错误大小写及折叠重名**

位置：[sdk/go/configuration/json.go:55](<D:/projects/verdandi/sdk/go/configuration/json.go:55>)；[configuration.schema.json:1](<D:/projects/verdandi/configuration.schema.json:1>)。

触发与结果：encoding/json 的结构体字段匹配忽略大小写，而未知字段检查只调用 DisallowUnknownFields，重复键检查按原字符串识别。 实际解码器接受 Version、REDIS、MODE，以及 version 与 Version 同时出现，偏离 schema 中精确字段名和封闭对象的约束。

跨语言审查：Go 特有。Rust serde 和 C++ 的封闭绑定按精确名字匹配；C#/Legacy 配置走 C++。探针保留实际 json.go/config.go，但语义 check 被置为成功，仅证明解码边界，未冒充完整配置 SDK 测试。

建议：在结构体绑定之前精确校验对象键名和作用域内唯一性，保持 UTF-8、null、数值与单位校验。不要简单删掉现有多阶段校验来压缩行数。

证据及回归要求：实际 JSON 解码实现隔离执行。把错误大小写、折叠同名、嵌套对象同类输入加入共享 configuration corpus。


**A18 · P2 · C++ Selector 截止时间堆随续租次数增长**

位置：[sdk/cpp/src/selector.cpp:690](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:690>)；[sdk/cpp/src/selector.cpp:712](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:712>)；[sdk/cpp/src/selector.cpp:749](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:749>)。

触发与结果：每次续租/更新向 priority_queue 追加截止时间，删除记录不会移除旧堆项，旧项必须等到其原截止时间才弹出。 内存规模与 TTL 窗口内更新次数相关，而不只是活跃注册数；长 TTL 与高更新频率可积累大量陈旧堆项，突破对常驻视图规模的直觉。

跨语言审查：C++ Registration 及原生包装层受影响。Go、Rust 使用带索引的截止时间队列，每个 UUID 对应可更新条目。

建议：选择有索引的堆，或 std::set<(deadline,uuid)> 配合每 UUID 的旧迭代器/索引，更新时替换旧条目。后者能减少手写堆代码，但须实测常数开销。

证据及回归要求：源码复杂度确认。固定注册数和较长 TTL，持续续租，观测队列条目数与 RSS 是否稳定；比较吞吐与尾延迟。


**A19 · P3 · 合法的 retry factor=1 导致退避计算随失败次数变慢**

位置：[sdk/go/registration/selector_core.go:1370](<D:/projects/verdandi/sdk/go/registration/selector_core.go:1370>)；[sdk/rust/src/registration/selector.rs:2011](<D:/projects/verdandi/sdk/rust/src/registration/selector.rs:2011>)；[sdk/cpp/src/selector.cpp:1169](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:1169>)；[sdk/cpp/src/catalog_subscriber.cpp:770](<D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp:770>)。

触发与结果：重试倍数配置为 1，计算函数按累计失败次数循环，而 delay 不会增长到上限来提前退出。 单次计算退化为 O(failures)，长时间故障期间做无效重复计算。属于性能问题，不是当前正常配置下已经量化的性能回退。

跨语言审查：Go/Rust Selector 以及 C++ Selector、Catalog 有同形逻辑，C#/Legacy 继承。Go/Rust Catalog 已有 factor<=1 的直接路径，可统一语义。

建议：对 factor<=1 直接返回恒定基值后应用 jitter；共享各语言内部的有界退避实现，保留零、上限和溢出处理。

证据及回归要求：源码复杂度确认。比较 failures=0/1/大数且 factor=1 的结果一致性与执行开销，再覆盖默认 factor 和上限饱和。


**A20 · P2 · 仅修改 Lua 后增量构建 C++ 可能仍嵌入旧脚本**

位置：[sdk/cpp/cmake/EmbedProtocol.cmake:11](<D:/projects/verdandi/sdk/cpp/cmake/EmbedProtocol.cmake:11>)；[sdk/cpp/cmake/EmbedProtocol.cmake:18](<D:/projects/verdandi/sdk/cpp/cmake/EmbedProtocol.cmake:18>)。

触发与结果：配置时用 file(READ) 读 Lua，再由 configure_file 生成 protocol.cpp；Lua 文件自身没有加入配置重跑或构建依赖。 探针调用实际 EmbedProtocol.cmake，修改输入副本中的 Lua 后仅 cmake --build，生成的嵌入源码保持原样。开发者可能误以为新协议已被编入。

跨语言审查：C++ 及其二进制包装消费者受影响。Go embed 和 Rust include_str 由编译输入追踪；每次都主动重新 configure 的构建脚本也不会触发这个具体场景。

建议：把所有 Lua 输入纳入 CMAKE_CONFIGURE_DEPENDS，或用声明完整 DEPENDS 的生成命令构建 protocol.cpp。

证据及回归要求：实际 CMake 模块执行。配置/构建一次，只修改 Lua 再构建，断言嵌入内容和脚本哈希随之变化。探针始终修改输入副本，未改仓库协议。


**A21 · P2 · Windows 运行时 DLL 列表为空时构建后复制失败**

位置：[sdk/cpp/CMakeLists.txt:127](<D:/projects/verdandi/sdk/cpp/CMakeLists.txt:127>)。

触发与结果：TARGET_RUNTIME_DLLS 为空，例如外部依赖静态链接，或导入目标没有可发现的 SHARED DLL 元数据；仍无条件执行 copy_if_different。 COMMAND_EXPAND_LISTS 后只剩目标目录，参数数量不足，链接已成功的构建在 POST_BUILD 阶段失败。独立共享库探针的 MSBuild 返回 1，符合预期缺陷表现。

跨语言审查：Windows C++ 构建及 C#/Legacy 使用的原生 DLL 交付受影响；Linux 不进入 WIN32 分支。此结果不表示所有依赖组合都会失败。

建议：在 CMake 脚本内显式判断运行时 DLL 列表非空再复制；同时确认 imported target 的运行时位置元数据足以支持实际打包。

证据及回归要求：等价 CMake 命令执行。分别覆盖零个、一个、多个运行时 DLL，以及带空格目录。不能只在本机恰好有动态依赖的配置上验证。


**A22 · P1 · Sentinel 夹具部署拒绝后仍可能删除原有同名资源**

位置：[testkit/sentinel/sentinel_test.py:190](<D:/projects/verdandi/testkit/sentinel/sentinel_test.py:190>)；[testkit/sentinel/sentinel_test.py:232](<D:/projects/verdandi/testkit/sentinel/sentinel_test.py:232>)；[testkit/sentinel/sentinel_test.py:1035](<D:/projects/verdandi/testkit/sentinel/sentinel_test.py:1035>)。

触发与结果：deploy 预检查发现已有同名容器并拒绝运行，但 main 的 finally 仍调用 cleanup，后者按计划名称无条件 docker rm -f，并清理计划目录。 原文类的记录器探针确认：预检查报 collision 后仍生成删除既有容器的指令，共有 7 个删除请求。探针没有连接 Docker，也没有实际删除这些资源。

跨语言审查：这是共享测试基础设施问题，所有使用该 Sentinel Topology 的语言资格测试均受影响。Standalone Fixture 已有创建记录/所有权标签，不存在相同的无条件清理路径。

建议：仅清理本次成功创建并确认所有权的容器与目录，逐项取得资源后记账；预检查未通过时清理集必须为空。

证据及回归要求：实际 Topology 类＋无副作用记录器执行。使用假的 Remote 覆盖端口冲突、容器冲突、创建一半失败和重复 cleanup，再验证真实一次性夹具。修复前不运行这套破坏性清理流程。


**A23 · P2 · Python peer 启动失败存在未接管的子进程**

位置：[testkit/catalog/interop_test.py:94](<D:/projects/verdandi/testkit/catalog/interop_test.py:94>)；[testkit/sentinel/sentinel_test.py:832](<D:/projects/verdandi/testkit/sentinel/sentinel_test.py:832>)。

触发与结果：Catalog start_peers 在创建进程后等待 READY，失败时函数尚未返回，调用者还没有获得清理句柄；主 Sentinel 在进入 try 之前依次创建两个 peer，第二个创建失败也有窗口。 测试失败后可能遗留 peer 进程、连接和续租，使后续测试资源及结果不稳定。

跨语言审查：Catalog interop 与复用它的 Catalog Sentinel，以及主 Sentinel 进程启动逻辑受影响；各语言本身的 SDK 运行时代码不应因此扣成同一个生命周期 bug。

建议：用 contextlib.ExitStack，在每个 Popen 成功的下一步立即登记清理，全部 READY 后再转交所有权。

证据及回归要求：源码路径确认。注入第二个 Popen 失败、首个/第二个 READY 超时、错误协议和中断，断言没有遗留子进程。


**A24 · P2 · C ABI Redis 测试失败清理会读取未初始化 key**

位置：[sdk/cpp/tests/c_abi_redis_test.c:86](<D:/projects/verdandi/sdk/cpp/tests/c_abi_redis_test.c:86>)；[sdk/cpp/tests/c_abi_redis_test.c:136](<D:/projects/verdandi/sdk/cpp/tests/c_abi_redis_test.c:136>)；[sdk/cpp/tests/c_abi_redis_test.c:263](<D:/projects/verdandi/sdk/cpp/tests/c_abi_redis_test.c:263>)。

触发与结果：key/hash_key 局部字符数组未初始化；Ping 失败就跳到 cleanup，而 key 格式化尚未执行。中间失败也可能只初始化了其中一个。 erase_key 经 strlen 读取未初始化内存，产生未定义行为，并可能发出错误的清理请求；它发生在测试失败路径，容易掩盖真实故障。

跨语言审查：C ABI 的 C 测试特有。其他语言测试中的字符串已初始化，未发现同一未初始化读取。不能据此声称生产 C ABI 普通调用有相同问题。

建议：零初始化，并记录每个测试 key 是否已创建，只清理已取得的资源。

证据及回归要求：源码路径确认。分别模拟 Ping 失败、第一个写入失败及后续失败，用 sanitizers 或可控假传输检查 cleanup 不读取或删除未知 key。


**A25 · P2 · Go 引用代码生成器把遮蔽内建名字的切片当成标量**

位置：[sdk/go/cmd/verdandi-refgen/main.go:275](<D:/projects/verdandi/sdk/go/cmd/verdandi-refgen/main.go:275>)。

触发与结果：合法声明 type uintptr []byte。classifyType 在查本地类型声明之前按名字调用 isScalar，因此把 uintptr 当作不可变整数。 实际 main.go 的分类探针返回 fieldValue，而不是 fieldSlice；按该分类生成的视图/克隆代码不能维持切片所需的所有权隔离。没有把这一分类测试写成完整生成 SDK 已运行。

跨语言审查：Go refgen 特有。Rust derive 与 C++ 模板使用各自类型系统，C# 不使用这个 Go AST 生成器。

建议：优先解析作用域内声明，再识别预声明标量；也可以明确拒绝遮蔽名字。需要更完整类型解析时先考虑标准库 go/types，不必引入新依赖。

证据及回归要求：实际生成器分类函数执行。对所有预声明标量名字的本地遮蔽、别名链和循环引用生成代码并编译，验证 slice/map getter 与 CloneData 不泄漏可变别名。


以下风险有需要验证的具体代码路径，但没有足够证据计入已确认问题：

- **R01 内存分配失败下错误边界和连接池归还**：C ABI noexcept 的 bad_alloc 分支仍构造带分配的 error/detail；C# unmanaged 回调的错误编码也可能再分配。C++ driver 在 creating++ 后创建连接、在占用 slot 后构建请求的路径缺少全程守卫。需要故障注入，不能把尚未执行的极端分配失败写成已复现崩溃。 位置：[sdk/cpp/src/c/internal.hpp](<D:/projects/verdandi/sdk/cpp/src/c/internal.hpp:141>)、[sdk/cpp/src/driver.cpp](<D:/projects/verdandi/sdk/cpp/src/driver.cpp:453>)、[sdk/cpp/src/driver.cpp](<D:/projects/verdandi/sdk/cpp/src/driver.cpp:514>)、[sdk/csharp/src/Verdandi/Internal/NativeTypes.cs](<D:/projects/verdandi/sdk/csharp/src/Verdandi/Internal/NativeTypes.cs:202>)。
- **R02 C# 并发 Dispose 的 Result/异常边界**：IsUsable 检查与 SafeHandle 参与 P/Invoke 之间存在时间窗口，可能出现 ObjectDisposedException 而不是预期 Result。SafeHandle 提供内存生命周期保护；本轮没有证据认定 use-after-free。需要用同步屏障验证公开操作与 Dispose 的交错。 位置：[sdk/csharp/src/Verdandi/Client.cs](<D:/projects/verdandi/sdk/csharp/src/Verdandi/Client.cs:1>)、[sdk/csharp/src/Verdandi/Internal/SafeHandles.cs](<D:/projects/verdandi/sdk/csharp/src/Verdandi/Internal/SafeHandles.cs:1>)。
- **R03 Rust Subscriber 关闭完成条件发布顺序**：finish_worker 先把 workers 减到 0，再写 closed、标记 Entry Closed 和释放生命周期 guard；wait_finished 以 workers==0 返回。存在观察到计数归零但终态尚未发布的窗口，需要确定性并发测试，建议使用独立的完成标记而非工作计数承担两个含义。 位置：[sdk/rust/src/catalog/subscriber.rs](<D:/projects/verdandi/sdk/rust/src/catalog/subscriber.rs:700>)、[sdk/rust/src/catalog/subscriber.rs](<D:/projects/verdandi/sdk/rust/src/catalog/subscriber.rs:715>)。
- **R04 故障切换回滚与 checkpoint 修订单调性**：内存恢复可以发现服务端 revision 回退后重新同步，而持久化实现通常拒绝降低 cursor；C++ 的 cursor 也使用 max 累积。需要先明确主从切换丢失已确认写入时的 epoch/reset 契约，再用未受 WAIT 保护的故障场景验证，现有有 WAIT 的测试不能证明此情况。该项没有列入 25 个已定位缺陷。 位置：[sdk/go/catalog/store.go](<D:/projects/verdandi/sdk/go/catalog/store.go:1>)、[sdk/rust/src/catalog/checkpoint.rs](<D:/projects/verdandi/sdk/rust/src/catalog/checkpoint.rs:1>)、[sdk/cpp/src/catalog_subscriber.cpp](<D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp:1108>)。
- **R05 C++ 恢复任务的整体截止时间**：sync_timeout 明确用于构造和订阅确认，但恢复阶段的扫描/屏障等待还需验证是否始终受整体截止时间约束。Selector 并不存在所谓被忽略的 max_inflight_reads 配置，该猜测已排除；Catalog 才有对应配置。 位置：[sdk/cpp/src/selector.cpp](<D:/projects/verdandi/sdk/cpp/src/selector.cpp:1>)、[sdk/cpp/src/catalog_subscriber.cpp](<D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp:999>)。

精简应优先消除导致错误的重复责任，再减少机械样板。建议维持现有最低语言约束：Go 1.27、Rust 1.85/edition 2024、原生 C++23、Legacy C++11/14/17、C#14 与 net8.0/net10.0。不能为少写几行默默提高最低版本或引入新依赖。

| 范围 | 可以具体怎样精简 | 必须保留的语义 |
|---|---|---|
| Go 容器与解析 | `maps.EqualFunc` 配合 `bytes.Equal`；`slices.Sorted(maps.Keys(...))`；以索引循环的 `~string \| ~[]byte` 泛型共享规范十进制解析。 | slice/map 深复制、nil 与空值的差别、数值索引顺序、错误字段与溢出校验；不能机械使用浅层 Clone。 |
| Go 批次与测试 worker | 批次采用单一结构并整体交换零值；runBounded 在产生 goroutine 前限流，或固定 worker 数；移除 Go 1.22 之后多余的循环变量自赋值。 | 有界内存、取消传播、测试失败清理；不为了节省行数删除断言。 |
| Rust 配置与批次 | Config/RuntimeConfig 采用组合并保留归一化入口；Mailbox/Batch 共用数据结构，用 `mem::take` 取走；保留源字段到运行时字段的明确单位。 | 显式零值与默认值差别、检查顺序、关闭时等待者完成。 |
| Rust 错误与控制流 | 在 await 前创建 Drop 守卫；合并 Selector 中结构相同的 timeout/error 恢复分支；用 `collect::<Result<_>>()`、`transpose`、`Entry` 表达常见转换。 | 取消安全与错误上下文；分支合并不能抹掉 Ambiguous/Deadline 等语义。 |
| C++ 通用校验 | registration、selector、catalog_value、configuration 中的 UTF-8 处理提取共享底层验证，首尾码点/空白策略留给调用者；共享退避与等待工具。 | 四处校验的策略并不完全相同，不能直接只保留其中一个版本。 |
| C++ 生命周期与状态 | 引入轻量 slot lease、SQLite transaction guard；统一 fence gate/同步任务的资源封装；去掉先深复制 record 再重复复制 data 的路径。 | 同步代次、屏障顺序、事务强异常保证；避免把两个域的状态机硬合成大框架。 |
| C++/Rust 截止时间结构 | 比较现有索引堆与标准有序集合＋索引；后者可以减少手写堆维护。 | 每 UUID 有界条目数和 O(log n) 更新；必须比较吞吐/尾延迟，不能退回陈旧条目不断累积的裸堆。 |
| C# 封装 | SafeHandle 的公共释放框架放到基类；GCHandle 用作用域 lease；Fields 经验证后一次建连续存储；减少候选二次解码。 | SafeHandle 引用计数、终结器、strict UTF-8、回调不得逸出的异常边界；P/Invoke 声明保留准确可审查。 |
| Python 生成器与 peer | 两个 Lua 生成器共享 manifest/拼接/原子写入/--check 的标准库辅助函数；Peer 使用 contextmanager 与 ExitStack。 | 确定性输出、诊断、进程所有权；每个域的动作列表和协议规则仍清晰可见。 |
| 构建与执行目标 | 共享产物/依赖状态清单，PowerShell 与 Bash 保留平台适配；WSL、本机、SSH VM 由显式目标选择，避免散落路径判断。 | 系统→本机 vcpkg→build/deps 缓存的查找策略；缺失时提示外部构建，不能悄悄安装或拉依赖。 |
| Lua | 所有改动只落到规范片段并重新生成；把总字段数、总字节数等约束统一在第一次写入前校验。 | 脚本原子性、增量协议和失败不改写状态；生成副本不是手写膨胀。 |

可先形成三组小改动：生命周期/取消守卫修复，协议边界与事务修复，之后再做重复结构合并。每组使用对应的语言标准格式化工具，并检查同类错误在其他 SDK 的表现。UTF-8 与同步资源辅助代码重复明显，但本轮没有提交重构后的 diff，因此不承诺固定百分比或具体已减少行数。

测试工具还有几项独立改进空间：Remote.run 顺序读尽 stdout 再读 stderr 可能遇到管道反压；应并行排空并给进程/命令设置总超时。部分 benchmark 的源码指纹未包含所有传递输入，persistence 标签有硬编码，重置后计数差使用 max(0,current-previous) 会遗漏重置后的部分计数。Go soak 直接运行时默认时长与最低生命周期轮数/间隔也应统一。先修复测试可重复性，再比较优化前后的指标；不修改既有历史结果来“校正”过去证据。

本次实际执行记录如下。所有 SDK/探针都优先使用已有工具和缓存；本机与 VM 没有恢复 Go/Rust/C++ 第三方依赖。唯一新增下载是用户明确授权的 VM Docker 组件与 Redis 镜像。

| 验证 | 结果及边界 |
|---|---|
| 实际仓库 C++ 离线测试，MSVC Release | CTest 2/2 通过；notification 用例报告 66,973 checks。  |
| 实际仓库 Go 纯本地包 | refgen、internal/validate、internal/lifecycle 三个包通过。  |
| Registration/Catalog Lua 生成一致性 | 两个生成器 --check 通过，32 份生成脚本与规范源片段一致。  |
| 当前依赖查找策略离线测试 | 18 个场景通过：Bash 7、PowerShell 7、CMake 4；使用模拟依赖，不下载第三方包。  |
| 实际 sdk/rust/src/error.rs | driver_panicked=true field_driver_panicked=true 使用已安装 rustc 直接编译实际 error.rs，没有 Cargo 第三方依赖。 |
| 实际 C++ 公开 Selector/Entry/Registration 模板与操作 | 外部 Choice 被接受、用户 Data 移动异常造成部分提交、保留空 Map 丢失、移动赋值未关闭旧注册，四项均复现。 Redis-facing core 提供固定合法视图；Entry 状态由夹具给出；注册 worker 所有权用保留 shared_ptr 的桩模拟，未执行真实续租循环。 |
| 实际 enqueue/pending_events 源码原文 | 静默丢消息、修订缺口被合并、24,002 字节保留数据通过 15,000 字节预算，三项复现。 原文提取到 std-only 容器夹具；没有网络、Pub/Sub 线程或完整 Selector；修订缺口与容量场景分开构造。 |
| 实际 Go Catalog 读取解码与值实现 | assignment to entry in nil map 复制 10 个未修改的纯生产源文件，替换少量外部常量/辅助接口；不是整个 Go 模块集成。 |
| 实际 Go configuration/json.go 与 config.go | 错误大小写和大小写折叠重名均被解码接受。 Config.check 语义校验被桩替代，只证明解码器行为。 |
| 实际 Go refgen main.go 的分类函数 | type uintptr []byte 被分类为 fieldValue。 执行 AST 分类边界，未声称完成生成代码和整个 SDK 的编译。 |
| 本机 SQLite 3.53.1 真实 C API | 正常结束时 step=101、errcode=101；当前 C++ 的 SQLITE_OK 检查会拒绝。 验证 sqlite3_step/errcode 行为，并映射到当前 C++ 分支；没有链接完整 checkpoint 实现。 |
| 实际 EmbedProtocol.cmake | Lua 输入副本改变后仅 cmake --build，嵌入源码仍旧。 原 CMake 模块＋复制的协议输入，不修改工作树 Lua。 |
| 同等 Windows POST_BUILD DLL 复制命令 | 运行时 DLL 列表为空时 MSBuild exit=1，copy_if_different 参数不足。 独立共享库目标；这个预期失败是缺陷复现，不是测试套件全部通过。 |
| 实际 Python Topology AST | 容器冲突拒绝部署后，清理仍生成删除原有容器的请求；共记录 7 个删除指令。 Remote 仅记录指令，端口检查为桩；没有执行容器/目录删除。 |
| Ubuntu VM Redis 8.8.0＋仓库真实 Lua | Replace 65,536 字段成功，Patch 到 65,537 字段仍成功，随后 Read 返回 corrupt；本次测试键已清理。 经 SSH 本机回环隧道，Python 标准库 RESP 客户端直接执行脚本；未经过语言 SDK。仅清理本次生成的已知测试键。 |

探针和原始局部日志位于 [build/review-20260907](<D:/projects/verdandi/build/review-20260907>)，该目录被 Git 忽略；持久化到版本化目录的证据 JSON 保存执行范围、结果及源文件哈希。C++ 公共模板、原文摘录与 Go 解码探针都有明确的替代边界，它们不能代替线程、网络或完整绑定测试。

尚未执行的范围：完整 Go test/race、Rust cargo test/clippy、带第三方依赖的 C++ 核心与 SQLite/Redis/TLS 集成、C# 两个目标框架与 Legacy/C ABI 的完整原生集成，以及 Ubuntu 上各 SDK 的编译、跨语言互操作、Sentinel 切换和长稳态回归。工具链已存在不等于这些依赖已缓存；下一次完整运行仍需按用户授权补齐具体依赖。

Ubuntu VM 当前已具备独立数据库验证条件：`ubuntu@192.168.0.119`，Ubuntu 26.04.1，Docker CE/CLI 29.8.0、containerd 2.3.4；安装来自 Docker 官方 apt 仓库，daemon 使用 `http://192.168.0.25:7897` 代理。安装方式与代理配置依据 [Docker Ubuntu 安装说明](https://docs.docker.com/engine/install/ubuntu/) 和 [Docker daemon 代理说明](https://docs.docker.com/engine/daemon/proxy/)。未在文档或仓库中写入登录密码。

Redis 镜像为 `redis:8.8.0`，digest 为 `sha256:234c902a2db49461a129e2d4aeff85b28cf20187ed274a67f6e50995fa713c7b`；容器 `verdandi-audit-redis-20260907` 绑定 **VM 的 127.0.0.1:16379**，已返回 `PONG`。这是关闭 RDB/AOF、使用 tmpfs 的测试数据库，限制 512 MiB/2 CPU，Redis maxmemory 为 256 MiB，容器保留且按 unless-stopped 策略重启。Lua 验证结束只删除本次命名空间的已知键，最终 DBSize 为 0。

本轮临时 SSH 隧道已关闭，Redis 容器继续保留。从 Windows 使用时可显式建立隧道，应用连接本机 `127.0.0.1:16379`：

```powershell
ssh -N -L 127.0.0.1:16379:127.0.0.1:16379 -o ExitOnForwardFailure=yes ubuntu@192.168.0.119
```

此次数据库准备没有同步项目源码、安装 VM 语言工具链或自动运行原有 Sentinel 清理脚本。现有 GCC CMake preset 并未缺失；Selector 提交全部 staged 项也是有意契约，这两点已核对而未误列为缺陷。后续修改应先关闭 A01/A02/A03/A04/A05/A06/A07/A09/A10/A11/A15/A22 的正确性与资源问题，再推进精简和性能比较。

