# 2026-09-07 第二轮代码审查（静态审查于 2026-09-08 完成）

本轮针对工作区当前内容重新审查，不能用上一轮审查或测试代替。用户已允许下载本项目的 Go、Rust、C++ 依赖，并要求完成本轮静态审查后再运行测试。Windows 使用已有工具；Ubuntu 获准安装的工具为 Go 1.27.1、Rust 1.98.1、build-essential、cmake、pkg-config。Python 修复与依赖下载继续暂缓。

## 覆盖与证据边界

- 当前状态：全部自有源码的静态审查完成，进入修复与验证阶段；此记录完成时尚未运行本轮 SDK 测试。
- 初始文件清单及 SHA-256：`build/reaudit-20260907/inventory.json`。
- 按文件、行范围记录的读取清单：`build/reaudit-20260907/read-ranges.jsonl`。读取记录仅证明取回过源文，不自动等于审查通过。
- 下列结论来自源码和契约对照；并发交错与网络故障仍需要完成审查后的定向回归。
- 原始 `code-audit-20260907.md` 与已有修复报告保留，不覆盖历史证据。
- 已完成手工逐段审阅：Go/Rust/C++/C#/C ABI/Legacy 生产实现、全部 SDK 测试、构建入口、CMake、testkit 及生成脚本，共 325 个源码文件、75,027 行；另有 32 个 Lua 生成副本、5,912 行，经两套生成器的 `--check` 验证与规范源一致。源码清单及修复前 SHA-256 保存在 `build/reaudit-20260907/reviewed-source-20260908.json`，全部手工审阅源码均有匹配当前哈希的完整行范围读取记录。
- 同时审查了配置 schema/example、共享 conformance 向量、项目文件和工具配置。锁文件用于锁定版本与完整性校验，不把第三方源码、证书数据及重复生成副本算作手工逐行审查。
- 生成器检查仅验证静态产物，两项均 exit 0；没有修改 Python，也不代表 SDK 构建或 Redis 场景已通过。发现问题的状态会在独立修复记录中跟踪，不抹除本表的修复前结论。

## 已确认的新问题（修复前记录）

| 编号 | 严重度 | 问题与触发条件 | 扩散范围 | 建议修复和后续验证 |
| --- | --- | --- | --- | --- |
| B01 | P1 | Catalog `Find` 在检查未关闭之后暂停；另一个线程完成 `Close` 的最终 Entry 扫描；`Find` 随后插入初始状态为 Absent 的新 Entry。关闭完成后仍可得到非 Closed 的 Entry。 | Go、Rust、C++；C ABI、Legacy、C# 继承 C++ 状态机。 | 在 Entry 插入与最终关闭状态之间建立同一锁下的线性化规则；用可控同步点验证该交错。 |
| B02 | P2 | 对已经删除或不存在的目标执行 Patch，前置 HMGET 得到全空头部，却返回 Corrupt。合法删除属于基线失效，Lua 和 Patch 契约预期为 Stale。 | Go、Rust、C++ 及 C++ 绑定。 | 区分全部头部缺失与部分头部损坏；验证删除后 Patch、从未存在的目标及部分损坏三种情况。 |
| B03 | P2 | C++ Catalog 错误分类顺序与 Go/Rust 不同：非法数组索引名先返回 Invalid，其他实现返回 Contract；未知 `@kind` 返回 Transition，其他实现返回 Corrupt。 | C++、C ABI、Legacy、C#；Go/Rust 提供对照。 | 统一可观察的错误语义，保留 Value 到 Map 的合法 Transition 分类，并增加同输入对照。 |
| B04 | P1 | C++ Catalog 创建 Subscriber 的准入未与 Client Close 串行化。Close 可以在 `open()` 检查与 `add()` 之间扫完子对象，随后 `store_.reset()` 与构造函数的 `restore()`/`store()` 复制同一个 shared_ptr 发生数据竞争；第二个 Close 也可能在第一个清理完前返回。 | C++ 及其绑定；Go 的 gate.Track 和 Rust Activity Guard 提供关闭等待。 | 在同一关闭准入机制中登记构造全过程，阻止关闭后新增子对象，统一 store 访问和并发 Close 汇合。 |
| B05 | P1 | Rust Catalog/Selector 在 Fred init 成功、SUBSCRIBE 失败或构造 future 被取消时，没有负责退出连接的早期守卫。Fred connect 任务自己持有 ClientInner，丢弃 JoinHandle 会分离任务，不会自动 QUIT；Selector 自动重试可重复遗留连接。 | 已确认 Rust Catalog 和 Registration Selector；根 DynamicPool init 也存在取消前所有权窗口，继续对照；Go/C++ 对应清理继续复查。 | 在第一次启动连接前建立取消清理所有权，并在正常生命周期转移该责任；验证订阅 ACL 拒绝、init 超时、构造取消及重复重试后的连接/任务回收。 |
| B06 | P2 | Rust Catalog Client Close 只等待 Activity，未释放或关闭 ClientInner 中的 redb checkpoint。关闭对象及 Subscriber/Publisher 句柄仍存活时，数据库文件锁继续被保留，无法像 Go/C++ 一样关闭后重开同一路径。 | Rust；Go 显式关闭 bbolt，C++ 在汇合后 reset store。 | 将 checkpoint 的关闭责任纳入显式 Close；保留旧公开句柄的同时重开相同文件，确认关闭后不会继续 I/O。 |
| B07 | P2 | C++ Catalog Read 回复解析不拒绝额外字段，并接受 `@encoded_bytes` 的前导零；Go/Rust 的同一成功回复要求字段集合和规范数字。 | C++ 及其绑定。 | 对 absent/deleted/present 要求精确字段集合，统一使用规范整数解析；保留有效空 Map/Array 的零字节值。 |
| B08 | P2 | Rust Catalog 的 `value_string` 使用 Fred `into_owned_bytes`，后者会递归拆开单元素 Array，并把 Integer/Queued 转成字节。异常嵌套回复可绕过 SDK 注释承诺的字符串/字节类型边界。 | Rust Catalog；Registration 中相同转换继续检查。Go/C++ 的对应文本读取拒绝嵌套数组。 | 在协议边界显式匹配允许的 RESP 类型，增加嵌套数组、Queued、整数与非法 UTF-8 对照。 |
| B09 | P1 | Registration 初始化收到超过八项、且额外位置为 nil/Null 的 HMGET 回复时，Go/Rust 在确认长度前按返回下标访问八项固定配置数组，触发 panic。 | Go、Rust；C++ read_policy 在迭代前检查类型与精确长度，绑定继承该保护。 | 在补默认值和任何写操作前校验回复类型与长度；异常长度必须返回 Corrupt，且不得写入部分默认值。 |
| B10 | P2 | 已注册的 Data 字段集合为空时，`Update(empty)` 是合法的无变化请求，但 Go/Rust 的内部邮箱校验将其与没有请求内容混淆，返回 Contract；C++ 返回成功。 | Go、Rust；C++ 及其绑定作为对照。 | 在公开完整 Data 更新与内部增量请求之间保留语义，空结构 no-op 仍检查生命周期；验证不推进 revision、不刷新 TTL。 |
| B11 | P1 | C++ Registration worker 只要邮箱持续有请求就 `continue`，即使本批是 no-op 或失败、没有刷新 TTL，也不执行已到期的自动续租。持续提交相同值可使租约过期。 | C++ 及其绑定；Rust 显式检查 renewal.is_elapsed，Go 的到期计时器路径在无写入批次后执行续租。 | 每批无 TTL 写入后检查到期续租，避免邮箱流量饿死定时工作；验证持续 no-op 下租约仍有效、revision 不变。 |
| B12 | P1 | Rust Registration 构造等待 ready 时，worker 成功发送就绪结果后、调用方再次 poll 前取消构造 future，send 已成功而 RegistrationCore 尚未产生，缺少 Drop 关闭责任。worker 自己持有 Shared 和唤醒 Sender，可能持续自动续租一个调用方从未得到的注册。 | Rust Registration；Go 没有可直接丢弃栈帧的 future，C++ 同步构造等待 ready。 | 在启动 worker 前建立构造取消守卫，返回 Core 时转交清理责任；覆盖 ready 发送后取消并验证注册、任务和许可回收。 |
| B13 | P1 | C++ Selector 在租约/retained 到期时只设置 dirty，没有设置 publish_at。默认非零发布间隔下，publish_at 仍为 time_point::max；如果之后没有新消息，过期记录继续留在公开活动视图。 | C++ 及其绑定；Go expiry 调用 markDirty，Rust expiry 调用 mark_dirty，都会安排发布。 | 为所有视图变化统一安排发布截止时间；验证停止续租且不产生其他消息时，活动候选按期移除、retained 随后删除。 |
| B14 | P1 | C++ Selector 用可下调的 Zone 写入策略校验扫描/订阅记录；管理员降低单字段大小后，原先合法且仍在续租的记录可使 Selector 持续恢复失败。 | C++ 及其绑定；Go/Rust 使用固定协议上限。codex.md 2026-08-23 决策明确要求下调策略后旧记录仍可发现。 | 读取/事件验证使用协议上限，新的内容写入与本地预测继续按当前策略验证；覆盖下调策略前后重连和存量记录读取。 |
| B15 | P2 | C++ Registration 在合并邮箱前未检查当前单字段大小限制。先提交超限值、再提交合法值并在同批覆盖时，超限调用会随最终合法状态收到成功；Go/Rust 在合并前拒绝。 | C++ 及其绑定。 | 将每个请求独立可判定的字段容量检查放在合并前；完整合并记录限制仍由 worker 校验。验证同字段并发覆盖与不同字段超限。 |
| B16 | P1 | C++ Selector 回放与当前记录相同 revision、内容相同但较旧的 Register 事件时，直接采用事件 timestamp。扫描已取得更新的租约时间，也会被旧缓冲事件覆盖，造成提前过期。 | C++ 及其绑定；Go/Rust 在同 revision 路径保留较大 timestamp。 | 在确认同 revision 内容相同后保留最大已观察时间；覆盖扫描结果与缓冲旧 Register 交错。 |
| B17 | P1 | Rust PendingChanges 连续合并 Update 时覆盖字段和 revision，却未读入 incoming.timestamp，最终 max 比较的是同一个旧时间。合并后的状态可能按早期租约提前过期。 | Rust；Go pending.go:204、C++ registration_pending.hpp:126 保留新旧最大 timestamp。 | 在移动 incoming 字段前保存新时间并合并；验证多个连续 Update、Renew/Update 混合及真实过期判断。 |
| B18 | P2 | C++ Selector 把 Registration Hash 中未知的可选 `@` 字段当作 Corrupt，阻断有效记录同步。protocol.md 明确要求忽略未知可选 Hash 字段。 | C++ 及其绑定；Go/Rust 的对应读取会忽略未知 `@` 字段，仍拒绝 `&` 字段。 | 统一向前兼容规则，保留已知元数据的重复、类型和数值校验；加入带未知元数据的扫描/修复对照。 |
| B19 | P1 | C# 子对象的 SafeHandleLease 只保留直接父 SafeHandle；父包装拥有的上一级 lease 不在此引用链中。只保留 Registration/Selector/Publisher/Entry、让父包装被 GC 回收时，上一级 lease 可以终结，提前关闭仍被叶对象使用的根传输或领域。 | C# 的多层句柄生命周期；原生 C++/Rust/Go 的运行时对象本身持有父运行时。Legacy 的 root_state → domain state → subscriber state 引用链完整，没有此缺口。 | 让原生句柄释放所有者持有完整父依赖链，直到真实 ReleaseHandle 后再归还；验证仅叶对象存活时强制 GC，及显式关闭与最终释放顺序。现有终结器用例仅丢弃叶 Registration 并保留 Client。 |
| B20 | P1 | C ABI One/Any 在原生 Selector 已提交本地预测后才分配输出句柄；One 还在此时扩容结果 vector。分配失败会返回 Capacity，但预测已经生效，违反失败整体回滚。 | C ABI、Legacy、C#；Go/Rust 和 C++ 模板在提交前准备结果，但不能替 ABI 层保护后续分配。 | 在提交之前准备结果句柄和所需容量，成功后仅无异常转移所有权；注入各分配点失败并检查整个 overlay 不变。 |
| B21 | P2 | C++ 根 Key 的 typed get/set 直接调用应用 field_codec，绕过现有 invoke_application。应用 Codec 抛出异常时，声明返回 expected 的根操作仍向调用方抛出；get 构造 optional 的应用移动也在保护之外。 | C++ 原生 typed Key；C ABI 根操作使用原始 bytes，Legacy 根操作也不调用此 typed Codec；C# 有自己的 Codec 捕获。Go/Rust 的 panic 策略须按各自回调契约对照，不能机械移植 C++ catch。 | 在完整 typed 边界复用已有静态异常转换，并复查 typed Hash 默认构造、赋值和返回移动；验证抛错 Codec 不进行写入，读取返回稳定错误。 |
| B22 | P2 | C++ Schema 的 consteval 字段名检查只接受可打印 ASCII，拒绝协议允许的合法 UTF-8 字段名，因而同一模型可用 raw Fields，却不能使用官方强类型 Schema。 | C++23 Schema；Go/Rust、Legacy 和 C# 字段接口没有这个 ASCII 限制。 | 统一编译期与运行时的 UTF-8/保留前缀规则；保留非法 UTF-8、重复和长度检查，以中文字段名做编译及互读回归。 |
| B23 | P2 | Linux 构建入口用未加引号的 $CMAKE、$CXX_PATH、$CC_PATH 执行版本查询，路径包含空格时被拆成多个参数，已存在的工具也无法通过诊断。 | build.sh；PowerShell 对应位置用调用运算符和独立参数，无相同拆词。 | 对工具路径统一引用；使用含空格的已存在工具路径验证诊断和参数传递，无需新下载。 |
| B24 | P2 | build.sh 在拒绝 source 前已经 set shell 选项并 export LANG/LC_ALL，拒绝后仍污染调用 shell；build.ps1 改变 Console.OutputEncoding 后只恢复 VSLANG，没有恢复控制台编码。 | Linux/Windows 原生入口；Go/Rust 入口通过子进程或先拒绝 source 隔离。 | Bash 在任何调用方状态修改之前拒绝 source；PowerShell 保存并在 finally 恢复控制台状态，保持项目内、子进程限定的配置约定。 |
| B25 | P2 | Go/Rust Registration 回复解析把已出现但格式错误的 `@revision`、`&field` 当作缺失；Rust 对 `@timestamp` 也这样处理。错误回复如 stale 携带非法 revision 时仍按有效 Stale 返回，丢掉损坏证据；Rust 的正整数 helper 还接受字符串零，与整数零的行为不同。 | Go、Rust Registration；C++ 对已出现的已知字段立即校验。Catalog 的对应已知字段有独立校验，不直接扩散同一遗漏。 | 区分字段不存在和字段存在但解析失败；统一正整数范围及规范十进制。成功 Register/Update/Renew 的 worker 已额外验证 revision/timestamp，不能误报为它们会直接接受零值确认。补充错误回复、Unregister 回复及不同 RESP 数值类型的用例。 |
| B26 | P2 | Rust Registration 直接使用完整 SDK Code 表解析 Lua status，接受只应由本地生命周期/传输产生的 closed、deadline、ambiguous、unavailable；Go/C++ 对同输入返回 Corrupt。Rust Catalog 同样直接复用此表，例如 closed 被当作正常错误，而 Go/C++ 将其判作未知协议状态。 | Rust 两个领域；C++/Go 各领域已有不同状态白名单，Catalog 的 unavailable 是合法状态，不能套用 Registration 白名单。 | 依据每个领域的 Lua 状态契约限定允许值，保留各自的未知状态错误语义；以同一组异常回复对照各语言。 |
| B27 | P2 | Go Catalog 聚合字段上限回归的 eval helper 对 parseScriptReply 返回的任何 error 都调用 t.Fatal；合法拒绝第 65,537 个字段时，该解析器必然同时返回 Capacity error，测试提前终止，无法执行拒绝原子性、满容量覆盖及可读性断言。 | Go 测试夹具；Python Catalog 夹具保留原始 error 回复供 expect_status 检查，Rust/C++ 的负向用例显式检查错误。 | 让 helper 区分预期领域拒绝、异常协议和传输错误；完整运行此现有负向用例并检查拒绝后的存储状态，不把提前退出视为业务缺陷。 |
| B28 | P2 | Go Registration soak 启动 runtimeMonitor 后，只在正常结束路径关闭 runtimeStop；任一中途 t.Fatal（例如 update loop 失败）跳过该关闭。监控循环只等待 ticker/stop，不监听已取消的 ctx，因此遗留协程，给真实失败叠加 goleak 错误。 | Go Registration 测试工具；Catalog 的监控使用父 ctx，父取消可退出；Rust/C++/C# 对应 SDK 测试没有这份独立 stop 通道监控。 | 启动监控时立即登记幂等停止并等待的 cleanup，正常结束复用；注入业务失败验证监控退出，保留原始错误诊断。 |

定位：

- B01：`sdk/go/catalog/subscriber.go` 的 Find、getOrCreate、markScope、finishWorker；`sdk/rust/src/catalog/subscriber.rs` 的 find、entry、mark_scope、finish_worker；`sdk/cpp/src/catalog_subscriber.cpp` 的 find、get_or_create、mark_all、close。
- B02：`sdk/go/catalog/projection.go`、`sdk/rust/src/catalog/publisher.rs`、`sdk/cpp/src/catalog.cpp`；对照 `lua/src/catalog/actions/patch.lua.inc` 和 `catalog/api.md` 的 Patch 契约。
- B03：`sdk/cpp/src/catalog_value.cpp` 的数组验证顺序、`sdk/cpp/src/catalog.cpp` 的 Patch kind 解析；对照 Go value/projection、Rust model/publisher。
- B04：`sdk/cpp/src/catalog.cpp:253`、`:313`、`:317`；`sdk/cpp/src/catalog_subscriber.cpp:875`、`:1201`。
- B05：`sdk/rust/src/catalog/subscriber.rs:104`、`:110`、`:145`；`sdk/rust/src/registration/selector.rs:1598`、`:1601`。第三方依据为锁定的 Fred 10.1.0 `src/runtime/_tokio.rs` 的 connect/init 和 `src/modules/inner.rs` 的所有权；依赖源码仅用于核实接口行为，不计作项目审查覆盖。
- B06：`sdk/rust/src/catalog/client.rs:26`、`:77`、`:114`；checkpoint 中的 Database 仅通过对象析构释放。
- B07：`sdk/cpp/src/catalog_subscriber.cpp:536`、`:564`、`:605`；对照 `sdk/go/catalog/read.go` 和 `sdk/rust/src/catalog/subscriber_read.rs`。
- B08：`sdk/rust/src/catalog/scripts.rs:137`；锁定 Fred 10.1.0 `src/types/args.rs:1125`。这里只使用依赖源码验证转换语义，未运行测试。
- B09：`sdk/go/registration/client.go:240`、`sdk/rust/src/registration/client.rs:234`；C++ 对照 `sdk/cpp/src/registration.cpp:407`。
- B10：Go `registration.go:253`、`registration_core.go:287`；Rust `registration/mod.rs:329`、`:678`；C++ `registration.cpp:717`、`:962`。
- B11：`sdk/cpp/src/registration.cpp:826`；Rust 对照 `registration/mod.rs:576`，Go 对照 `registration_core.go:428`。
- B12：`sdk/rust/src/registration/mod.rs:399`、`:404`、`:541`；返回 `RegistrationCore` 前没有具备关闭责任的守卫。
- B13：`sdk/cpp/src/selector.cpp:1135`、`:1140`、`:1181`；Go `selector_core.go:374`、Rust `selector.rs:990` 提供对照。
- B14：`sdk/cpp/src/selector.cpp:680`、`:1102`、`:1149`；Go `selector_core.go:599`、Rust `selector.rs:1052`；依据 `codex.md` 的 2026-08-23 Zone limits 决策。
- B15：`sdk/cpp/src/registration.cpp:717`、`:747`、`:990`；Go `registration_core.go:287`、Rust `registration/mod.rs:678`。
- B16：`sdk/cpp/src/selector.cpp:826`、`:834`；Go `selector_core.go:996`、Rust `selector.rs:1425`。
- B17：`sdk/rust/src/registration/pending.rs:194`、`:205`；Go `pending.go:204`、C++ `registration_pending.hpp:126`。
- B18：`sdk/cpp/src/selector.cpp:612`；Rust `registration/event.rs:276`；依据 `protocol.md:160`。
- B19：C# `Internal/SafeHandles.cs:263`、`:294`；`Registration/Registration.cs:14`、`:40`；`Registration/RegistrationClient.cs:11`；`Catalog/CatalogSubscriber.cs:219`。现有测试对照 `tests/Verdandi.Tests/Program.cs:701`。
- B08 的扩散复查：Rust Registration `script.rs:211`、`event.rs:369/375`、`config.rs:447` 以及 Selector 的订阅/PONG 路径都存在同类宽松转换；修复须覆盖这些协议入口。
- B20：`sdk/cpp/src/c/selector.cpp:186`、`:217`；对照 C++ 模板与 Go/Rust 的提交顺序，以及已修复 A09/A10 的包装返回边界。
- B21：`sdk/cpp/include/verdandi/client.hpp:75`、`:95`、`:108`；已有异常转换在 `schema.hpp:103`，C++ 规则依据 `coding.md:390`。
- B22：`sdk/cpp/include/verdandi/schema.hpp:64`；协议依据 `protocol.md:799`，运行时配置中已有独立 UTF-8 校验可供整理。
- B23：`sdk/cpp/build.sh:349`、`:383`、`:624`。
- B24：`sdk/cpp/build.sh:23`、`:27`、`:32`；`sdk/cpp/build.ps1:95`、`:1032`。
- B25：Go `registration/script_reply.go:67`、`:82`；Rust `registration/script.rs:186`、`:187`、`:188`、`:218`；C++ 对照 `registration.cpp:229`、`:238`、`:252`、`:259`。Rust 既有 `tests/internal/registration/script.rs:86` 甚至明确接受前导零，后续应修正测试预期而非只新增一份相反测试。
- B26：Rust `registration/script.rs:199`、`catalog/scripts.rs:81`、`error.rs:28`；Go 对照 `registration/script_reply.go:84`、`catalog/protocol.go:77`；C++ 对照 `registration.cpp:225`、`catalog.cpp:112`。

## 已核对的测试覆盖缺口

- B27：`sdk/go/catalog/lua_limits_integration_test.go:46`、`:65`；返回语义见 `sdk/go/catalog/protocol.go:68`，Python 对照 `testkit/lua/catalog_test.py:108`、`:124`、`:359`。
- B28：`sdk/go/registration/soak_test.go:311`、`:398`、`:510`、`:1290`。不把夹具泄漏误归因于 SDK worker。
- Rust `tests/catalog_v2.rs` 的 checkpoint 用例只重新创建 Subscriber，仍使用同一个 Catalog Client，没有覆盖 B06 所需的“关闭 Client 后保留旧句柄并重开同一文件”；C++ 的对应用例实际重开了 Client。
- Rust `tests/internal/registration/pending.rs:42` 使用 timestamp 50/60 连续 Update，却只断言字段、base/latest revision；后面的 10,000 次合并测试也没有断言最终 timestamp，因此不能检出 B17。Go `registration/pending_test.go:57`、`:148` 有同样断言缺口，虽然当前 Go 实现正确。应补充时间与实际过期行为断言。
- Go `registration/integration_test.go:925` 发布 revision gap 后等待的谓词在发布前已经成立，可以在 listener 处理事件之前通过；需要等待定向修复读取完成的可观测同步点，再断言内容与 generation，不能靠增加 sleep 代替。
- C++ `runtime_boundaries_test.cpp` 的到期测试直接操作内部 state，没有运行 listener 的 dirty/publish_at 调度，因此不能检出 B13。
- C++ `selector_transaction_test.cpp` 和 Legacy/C# 的回滚测试注入应用移动或 Codec 失败，没有覆盖原生提交后的 ABI 输出分配失败，因此不能检出 B20。
- C# `Program.cs:675` 的终结器压力测试只丢弃 Registration，父 Client/RegistrationClient 仍由调用方保留；`RunHandleLifetime` 测的是显式 Dispose。两者都没有覆盖 B19 所需的“仅叶对象存活并强制 GC”。
- C++ Redis 根测试使用固定键 `verdandi:test:cpp:root/hash`；多份测试并行访问同一实例会相互覆盖。其他 C++ 领域及 C ABI/Legacy/C# 使用生成的 Zone。后续测试必须隔离实例或修正这两个测试键，失败路径的清理也需与 Go/Rust 夹具继续对照。
- C# Python 夹具仍直接调用 CMake preset、dotnet restore/publish，并保留 WSL 运行分支；它没有实施本轮的 VM 源码同步和项目入口约定。Python 修改继续暂缓，不能把直接运行旧夹具当作当前授权工具流程已迁移。

## 待核实的问题

- C++ Catalog Open 额外执行 INFO SERVER；Go/Rust 只加载域脚本。同一 Redis 8 ACL 禁止 INFO 时，三个 SDK 的可用性可能不同。需要结合公开 ACL/版本契约决定删除探测还是明确约束。
- Rust 根 DynamicPool 的 init future 取消也可能保留驱动：连接任务持有 DynamicPoolInner，池 Drop 清理不能仅靠外部句柄被释放触发。与 B05 一起继续核查。
- B04 的关闭准入缺口也出现在 C++ Registration 的 add/create 和并发 Client Close；私有 driver subscribe 与 close 同样分开检查/登记。继续整理各调用链的实际影响，统一修复准入而非只保护 Catalog 的 store。
- Rust Registration 显式刷新与后台刷新缺少 Go configMu 的串行化，较早读取的策略可能较晚发布；需结合策略刷新契约决定并发发布规则。
- 旧 R05 在 C++ Selector 源码中已确认：同步扫描与 fence 等待没有整个 generation 的 sync_timeout，只有构造等待和订阅确认期限；Go/Rust 对完整同步任务设置整体期限。后续补慢扫描/丢 fence 的回归。
- Go 打开 checkpoint 不创建缺失父目录，Rust/C++ 会创建；需明确是否把这种可观察差异统一为同一项目契约。
- 旧风险 R04 仍存在：checkpoint 的单调 revision 策略与 Redis 故障切换丢失写入后的权威回退，需要跨语言的 epoch/reset 规则及定向故障测试。
- C# IsUsable 检查与 P/Invoke 的 SafeHandle 准入之间存在 Dispose 交错窗口；源生成封送可能抛出 ObjectDisposedException，而非包装声明的 Result。需要纳入并发关闭回归，并核对公开线程安全范围。
- Windows Ninja 模式实际使用 PATH 中的 cl，但报告及缓存标签取自 vswhere 的最新安装；多个 MSVC 环境并存时可能不一致。需固定实际编译器并核对 CMake generator instance。
- Linux check 的 run-clang-tidy 命令没有传入脚本 --jobs；后续应把静态分析并发也纳入内存预算，不能只限制普通编译。

## 可精简和优化的位置

| 范围 | 建议 | 必须保留的性质 |
| --- | --- | --- |
| Go 长测统计 | Registration 的两组逐次延迟数组为 `500 × seconds × 8 × 2` 字节；24 小时仅此约 659 MiB，另有各 Selector 的样本与最终合并副本。可像 Catalog 一样采用固定大小直方图，精确记录 count/sum/max 和阈值超限数，分位数说明桶误差。`runBounded` 改为固定 worker 数，避免先创建全部任务协程再限流。 | 测试时长、吞吐与尾延迟资格门槛保持明确；不能将上述静态预算当作此前 VM OOM 的已证实根因。 |
| Go Catalog Patch | 字段数量/大小的拒绝性校验尽可能早于完整克隆，避免无效的大请求先分配。 | 不保留调用方可变缓冲区，不引入检查与使用之间的可变共享。 |
| Go Subscription | 评估用 `maps.Keys`、`slices.Sorted` 简化取键排序，避免排序比较器重复拼接路径。 | 线上的规范字节顺序及 checkpoint 顺序不变。 |
| Rust Catalog Publisher | 对已经取得所有权的 Fields 尽量移动数据，减少临时名称数组、二次查找和 payload 克隆。 | 数组按数字索引编码、请求持有完整数据。 |
| C++ Catalog | 合并私有 key builder 与重复的外部转发 helper。 | 键格式和公开边界不变。 |
| C# Fields 互操作 | 评估从 native 字段结果直接构造最终拥有数据的 Fields，避免两次 payload 拷贝。 | SafeHandle 生命周期、不可变性、排序和大小限制保持正确。 |
| C++ Registration | update_state 在判断 no-op 前深拷贝整个 current.data，可延迟至确有变化时；移除 same_bytes 这种仅转发 operator== 的私有函数。 | 无变化请求仍返回成功，不发生写入，确定失败不修改 desired state。 |
| Rust Registration | 合并 Mailbox/Batch 的重复字段结构，用 mem::take 转交所有权；首次 Register 的 data_shape 从字段名建立，避免为提取字段名先克隆全部 payload。 | 许可跟随待处理请求，取消等待者不能提前归还容量。 |
| C ABI 字段输入 | read_fields 在完整克隆前应用所属操作的数量、名称和字节上限；erase/subscription 的 reserve 也尽可能放在数量检查之后。 | 按所属域使用真实上限，不用 Registration 上限误拒绝 Catalog；不改变调用方缓冲区。 |
| C++ Selector | 删除更新路径上重复的 Data 拷贝，为已知数量的视图数组预留空间；统一私有 UTF-8 校验。 | 公开快照保持独立所有权，字段语法与错误类别不变。 |
| C# SafeHandle | 把重复的句柄构造、父依赖与释放顺序集中到窄基类，可用已支持的主构造函数减少模板式代码。 | 真正 ReleaseHandle 前完整父链存活，不能通过简化丢失 B19 的所有权。 |
| C++ Legacy | 保留 C++11 的 optional/result 必要兼容实现；优先合并重复回调错误复制，简化一次性状态转发。 | 不升级消费端标准，不让 STL 或异常越过 C ABI。 |
| 构建脚本 | 合并只调用一个函数的 stage 转发；对版本、参数和路径处理使用小范围共用辅助逻辑。 | 独立命令、准确退出码、下载边界和 child-process 配置语义不变。 |

## 已排除的疑点

- Go Replace 的编码字段所有权由 Encoder 契约转移；不能仅因未二次克隆就断言保留了调用方可变别名。
- C# UTF-16 ordinal 与 native UTF-8 排序不同，但复制结果会由 Fields.Create 重新排序，当前路径没有证据表明查找因此错误。
- Rust 当前所审 API 和已下载依赖的最低版本要求尚未证明违反声明的 Rust 1.85 最低版本。
- C++ 无符号协议数字解析已经检查安全整数上限；该路径不能据此认定存在溢出。
- Rust Fred 的 ZSCAN page 在 Drop 时会调度下一页；当前循环没有显式调用 page.next() 本身不是漏页缺陷。
- 普通 One/Any 在有效非空选择后提交本次全部 staged 修改，是现有跨语言契约；Go ReferenceSelector 明确采用仅提交最终选中项的另一契约，不能把两者直接判作漏掉选中项过滤。
- 已读取锁定 Boost 1.92.0 发布归档目录：包含 boost/redis.hpp，但没有根 CMakeLists.txt；不能认定当前 FetchContent_MakeAvailable 会因此自动构建整个 Boost。

## 准备状态（测试开始前记录）

修复阶段的源码改动、扩散复查和逐项验证状态见 [code-reaudit-fixes-20260908.md](code-reaudit-fixes-20260908.md)。原 B25 的 Catalog 对照仍有遗漏：Go Read 错误分支也会忽略已出现但类型非法的元数据，已在扩散复查中补修；B26 同时覆盖 Catalog Read。原待核实 R05 已作为 B29 落实绝对同步期限，定向网络回归尚待执行。

- Rust Windows 目标的锁定依赖已下载至项目 `build/deps/cargo`，尚未编译或运行测试。
- Go 锁定依赖已通过项目入口下载到 `build/deps/go/pkg/mod`，使用本次进程的 goproxy.cn 并保留 go.sum 校验；没有修改全局配置。
- SQLite、yyjson、Boost 锁定源码包与 Windows OpenSSL 3.5.8 预编译包在 Windows、Ubuntu 项目缓存中均已通过锁定哈希。Windows OpenSSL 的 x64 头文件、库和 DLL 已解包到项目指定位置，尚未链接验证。
- Ubuntu Go 1.27.1、Rust/Cargo 1.98.1 已校验官方 SHA-256 并分别安装在项目 `build/tools/go-1.27.1`、`build/tools/rust-1.98.1`。GCC/G++ 15.2.0、CMake 4.2.3、pkg-config 2.5.1 已从 Ubuntu 官方 HTTPS 软件源安装；临时源列表在项目 build/tools 内，没有改写系统源配置。
- Ubuntu OpenSSL 3.5.5 开发包及运行库已从官方源下载并解包到项目 `build/deps/openssl/linux/x64`；头文件与库的项目内布局已准备完成，尚未链接验证，没有编译 OpenSSL 源码。
- Ubuntu 的 Go 锁定模块和 Rust Linux 目标锁定 crates 已通过项目入口下载完成；Windows 上那次 Linux 目标额外下载的 TLS 失败不影响 Ubuntu 缓存，但不会记为 Windows 成功。
- 2026-09-08 首份源码快照共 708 个文件，按 Git 跟踪和未忽略文件清单打包，文本按仓库 LF 规则归一化；快照 SHA-256 为 `970e9c3fdf02bb379fc489701fc4a447d1c1c5651c8c135974b21d1f7bc44b99`。已验证后解包至 `/home/ubuntu/verdandi`，未同步 `.git` 或 `build`。Windows/Linux 文件哈希映射在 `build/reaudit-20260907/source-20260908-manifest.json`，后续修复仍需增量同步。
- Ubuntu 只准备已授权工具与依赖；后续编译限制并发，避免再次耗尽虚拟机内存。
