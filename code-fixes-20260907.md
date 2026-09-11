# 2026-09-07 审计修复与验证记录

已对原审计 25 项中的 **23 项实施源码修复**；A22、A23 按用户要求保留 Python 待处理。另调整了风险 R03 的 Rust 关闭完成顺序。**这不是 23 项完整 SDK 集成验收通过的声明**；未执行的编译、取消与并发回归逐项列在下文。

原始 [code-audit-20260907.md](D:/projects/verdandi/code-audit-20260907.md) 与 [testkit/results/code-audit-20260907.json](D:/projects/verdandi/testkit/results/code-audit-20260907.json) 保留为修复前证据。结构化修复记录见 [testkit/results/code-fixes-20260907.json](D:/projects/verdandi/testkit/results/code-fixes-20260907.json)。本轮没有下载、安装、升级依赖，没有修改 Python 源码，没有提交或推送。

## 已运行验证

| 范围 | 结果 | 证据与边界 |
| --- | --- | --- |
| C++ | 通过 | 7/7 CTest：checkpoint、Legacy、Catalog 值与通知字段、运行时边界、Selector 事务、CMake 回归。 标准库与系统 SQLite 的离线工程；包含公共模板/C ABI 夹具，未链接完整 native 库。 |
| C / C++ 编译 | 通过 | 实际 catalog.cpp、selector.cpp 对象编译；C ABI C 测试文件 C11 /utf-8 /W4 /WX 编译。 编译通过不等于网络、包装层或集成运行通过。 |
| Go | 部分通过 | 实际源文件的 Read、Patch 容量、JSON 扫描边界 3 个测试通过；完整 refgen 包通过。 隔离目录中的生产源文件与仓库逐文件 SHA-256 一致；运输层与无关 Config.check 使用夹具。完整 SDK 缺少缓存依赖。 |
| Rust | 部分通过 | 实际 error.rs 以 rustc --test 独立编译并通过 UTF-8 边界测试；修改文件 rustfmt --check 通过。 cargo test --offline --locked --lib --no-default-features 因缺失 proc-macro2 等依赖未进入完整 crate 编译。 |
| C# | 编译通过 | 库与测试项目均通过 net8.0 / net10.0 Release 构建，0 警告、0 错误；格式检查通过。 使用清空包源的离线 restore 与已安装 targeting packs；没有下载。运行回归需要完整 native DLL。 |
| Lua / Redis | 通过 | 真实 Redis 上的聚合字段上限与原子拒绝、上限内覆盖、完整 Read 通过；全部自有测试键清理。 使用当前 canonical 生成脚本的独立协议探针；完整 SDK Lua 集成用例未执行。 |
| 格式 / 生成物 | 通过 | Go 13 文件、Rust 10 文件、C/C++ 31 文件及两个 C# 文件格式检查通过；Catalog Lua 生成物 freshness 通过。 包含本轮以前已存在的工作区改动，不把它们归成本轮新增修改。 |

C++ 的 7 套 CTest 串行执行且全部通过；正式测试源文件位于 sdk/cpp/tests。实际 SQLite 测试使用 Windows SDK 已提供的 winsqlite3；这不替代指定 SQLite 发行版本的打包验收。Legacy 测试在 MSVC 上采用项目 CXX_STANDARD=11 设置，但 MSVC 实际最低语言模式为 C++14，尚未宣称 Linux C++11 编译通过。

Go 的隔离测试不是完整 SDK 测试。Go 依赖解析已使用 GOPROXY=off、GOSUMDB=off、GOTOOLCHAIN=local；缺少 go-redis、bbolt、msgpack、goleak 缓存。Rust 使用 offline/locked，缺少 proc-macro2 等缓存。完整 C++ 缺少 Boost、OpenSSL、yyjson 等依赖，因此 C# 的 native 运行回归也未完成。Ubuntu 尚未发现 g++、clang++、cmake、Go、Rust 或 dotnet 可执行程序，本轮没有安装它们。

临时探针出现的两次问题均已区分并修正：C 编译命令补齐 /utf-8；Lua 探针按协议读取嵌套 &fields。它们不计为新增 SDK 缺陷，也不使用失败探针输出冒充通过。

## 逐项修复与跨语言复查

### A01 · P1 · Catalog Patch 可把合法 Map 写成无法读取的超限记录

**源码已修改**。Patch 在第一次写入前检查现有字段数与真正新增字段数之和，更新四份生成副本。

实现：[lua/src/catalog/actions/patch.lua.inc](D:/projects/verdandi/lua/src/catalog/actions/patch.lua.inc)。

传播复查：Go、Rust、C++、C ABI、C#、Legacy 全部使用同源 Catalog Lua，因此全部受影响；32 份生成脚本与已审读源片段的一致性检查通过。

验证：真实 Redis：65,535→65,536 成功；65,537 拒绝且 revision、字段与字段修订索引不变；上限内覆盖与完整 Read 成功。

待验证/限制：各完整 SDK 的客户端集成矩阵未运行。

回归入口：[sdk/go/catalog/lua_limits_integration_test.go](D:/projects/verdandi/sdk/go/catalog/lua_limits_integration_test.go)。

### A02 · P1 · Go Catalog 对空 Map 应用 Patch 会 panic

**源码已修改**。空字段集合复制后保持可写，并继续复制字节内容；排序改用 maps.Keys 与 slices.Sorted。

实现：[sdk/go/catalog/value.go](D:/projects/verdandi/sdk/go/catalog/value.go)。

传播复查：确认为 Go 特有的 nil map 写入问题。Rust 的 BTreeMap 和 C++ 的 std::map 空容器仍可写；C#、Legacy 经 C++ 实现不继承这一具体问题。

验证：实际 Read 代码的空 Map→首次 Patch 回归先红后绿，原快照保持不变。

待验证/限制：Subscriber 正式回归已加入，完整包因缺失依赖未执行。

回归入口：[sdk/go/catalog/read_regression_test.go](D:/projects/verdandi/sdk/go/catalog/read_regression_test.go)、[sdk/go/catalog/subscriber_regression_test.go](D:/projects/verdandi/sdk/go/catalog/subscriber_regression_test.go)。

### A03 · P1 · C++ 订阅队列满时会静默丢消息并保留 fence

**源码已修改**。将 lagged 状态独立于可清空的队列保存，优先于后续控制事件交付。

实现：[sdk/cpp/src/internal/subscription_queue.hpp](D:/projects/verdandi/sdk/cpp/src/internal/subscription_queue.hpp)、[sdk/cpp/src/driver.cpp](D:/projects/verdandi/sdk/cpp/src/driver.cpp)。

传播复查：C++ Registration 与 Catalog 共用 driver；C ABI、C#、Legacy 继承。Go 的同步 Receive/Pong 路径和 Rust 的显式 Lagged 处理未发现同形的无标记清队列行为。

验证：实际队列实现覆盖满队列后的 fence、重连、失败、关闭及已有 lagged，全部通过。

待验证/限制：完整 driver 网络线程和高频 Redis 订阅回归未运行。

回归入口：[sdk/cpp/tests/runtime_boundaries_test.cpp](D:/projects/verdandi/sdk/cpp/tests/runtime_boundaries_test.cpp)。

### A04 · P1 · C++ 待处理事件合并掩盖修订缺口并低估占用

**源码已修改**。只合并修订连续且字段结构兼容的事件；重新计算合并后占用，接近容量时先准备副本，失败保留旧事件。

实现：[sdk/cpp/src/internal/registration_pending.hpp](D:/projects/verdandi/sdk/cpp/src/internal/registration_pending.hpp)、[sdk/cpp/src/selector.cpp](D:/projects/verdandi/sdk/cpp/src/selector.cpp)。

传播复查：C++ 及其 C ABI/C#/Legacy Registration 选择器受影响。Go、Rust 明确检查修订相邻性并维护合并后大小，是可借鉴的实现。Catalog 使用另一套 pending 逻辑，不能按同名结构直接认定传播。

验证：修订缺口、未知字段、连续合并及不重叠大字段超限回滚通过；实际 selector.cpp 通过编译。

待验证/限制：完整订阅恢复集成未运行。

回归入口：[sdk/cpp/tests/runtime_boundaries_test.cpp](D:/projects/verdandi/sdk/cpp/tests/runtime_boundaries_test.cpp)。

### A05 · P1 · C++ Catalog 初始同步转入精确修复后丢失就绪状态

**源码已修改**。scope 初始同步完成后、转入 exact repair 之前保留 aligned 状态；仍在修复全部完成后发送 ready。

实现：[sdk/cpp/src/catalog_subscriber.cpp](D:/projects/verdandi/sdk/cpp/src/catalog_subscriber.cpp)。

传播复查：C++ Catalog 及 C ABI/C#/Legacy 继承。Go、Rust 的 scope 同步等待者/批次上下文保留了初始就绪责任，未发现相同控制流缺口。

验证：完成控制流与其他原生 SDK 的源码对照。

待验证/限制：缺少完整 C++ 依赖，Subscriber 编译与异步集成回归尚未完成。

### A06 · P1 · C++ 将 SQLite 正常查询结束当作 checkpoint 故障

**源码已修改**。以 sqlite3_step 返回 SQLITE_DONE 判定正常结束；其他结束状态回滚并禁用存储。提取既有 Path 实现以链接实际 checkpoint 测试。

实现：[sdk/cpp/src/catalog_checkpoint.cpp](D:/projects/verdandi/sdk/cpp/src/catalog_checkpoint.cpp)、[sdk/cpp/src/catalog_path.cpp](D:/projects/verdandi/sdk/cpp/src/catalog_path.cpp)。

传播复查：C++ SQLite 实现及 C ABI/C#/Legacy 继承。Go 使用 bbolt，Rust 使用 redb，不共享这段 SQLite 错误判断。

验证：生产 load 实现配合系统 SQLite：正常双记录读取通过；第一行前和第一行后注入 SQLITE_IOERR 均返回失败并禁用存储。

待验证/限制：完整 SDK 链接、指定发行依赖版本和 Linux SQLite 测试未运行。

回归入口：[sdk/cpp/tests/checkpoint_failure_test.cpp](D:/projects/verdandi/sdk/cpp/tests/checkpoint_failure_test.cpp)。

### A07 · P1 · Rust 截断非 ASCII 错误详情会 panic

**源码已修改**。Rust 共用 UTF-8 边界截断函数，checkpoint 复用；C++ 截断时退回完整码点边界。

实现：[sdk/rust/src/error.rs](D:/projects/verdandi/sdk/rust/src/error.rs)、[sdk/rust/src/catalog/checkpoint.rs](D:/projects/verdandi/sdk/rust/src/catalog/checkpoint.rs)、[sdk/cpp/src/error.cpp](D:/projects/verdandi/sdk/cpp/src/error.cpp)。

传播复查：Rust 存在 panic。C++ 的按字节 resize 不会产生同种 panic，但可能输出被截坏的 UTF-8；C# Encoder.Convert 保持字符边界，Go 没有对应的 512 字节切割逻辑。

验证：Rust 实际 error.rs 独立测试覆盖 ASCII 边界、中文和 emoji；C++ UTF-8 边界回归通过。

待验证/限制：完整 Rust crate 与 checkpoint 正式测试因依赖缺失未执行。

回归入口：[sdk/rust/tests/internal/error.rs](D:/projects/verdandi/sdk/rust/tests/internal/error.rs)、[sdk/rust/tests/internal/catalog/checkpoint.rs](D:/projects/verdandi/sdk/rust/tests/internal/catalog/checkpoint.rs)、[sdk/cpp/tests/runtime_boundaries_test.cpp](D:/projects/verdandi/sdk/cpp/tests/runtime_boundaries_test.cpp)。

### A08 · P2 · C++ 与 Rust 的 Choice 没有校验所属 Selector

**源码已修改**。Choice 加入所属 Selector 的进程内唯一标识；Rust 用 valid_for 统一 owner、token 和索引校验。

实现：[sdk/cpp/include/verdandi/registration/selector.hpp](D:/projects/verdandi/sdk/cpp/include/verdandi/registration/selector.hpp)、[sdk/rust/src/registration/selector.rs](D:/projects/verdandi/sdk/rust/src/registration/selector.rs)。

传播复查：原生 C++ 和 Rust 受影响。Go 校验 transaction 指针和 token；C# 使用进程范围的事务编号。C ABI 不向托管层暴露此原生 Choice；Legacy 的公开 index API 应单独说明其较弱约束，不能自动等同为这一 token 碰撞。

验证：实际 C++ 公共模板拒绝来自其他 Selector 的 Choice。

待验证/限制：Rust 对应正式用例已加入，完整 crate 未执行。

回归入口：[sdk/cpp/tests/selector_transaction_test.cpp](D:/projects/verdandi/sdk/cpp/tests/selector_transaction_test.cpp)、[sdk/rust/tests/internal/registration/selector.rs](D:/projects/verdandi/sdk/rust/tests/internal/registration/selector.rs)。

### A09 · P1 · C++ Selector 提交遇到用户 Data 移动异常会部分成功

**源码已修改**。先准备所有 staged 节点与返回对象，仅在返回构造成功后以节点转移提交；异常时不发布任何预测。

实现：[sdk/cpp/include/verdandi/registration/selector.hpp](D:/projects/verdandi/sdk/cpp/include/verdandi/registration/selector.hpp)。

传播复查：原生 C++ 泛型 API 已复现。C#、Legacy 走原生 Fields，不具备这个自定义 Data 移动触发器；它们的提交后解码问题另列 A10。Go、Rust 没有这种可抛 C++ 移动构造语义。

验证：实际公共模板的 One/Any 在不同用户 Data 移动位置抛异常，20 个故障预算场景保持全提交或全回滚。

待验证/限制：完整 native 库集成及分配失败注入未运行。

回归入口：[sdk/cpp/tests/selector_transaction_test.cpp](D:/projects/verdandi/sdk/cpp/tests/selector_transaction_test.cpp)。

### A10 · P1 · C# 与 Legacy 在提交后解码返回值，失败不会撤销预测

**源码已修改**。C# 与 Legacy 均在事务回调内完成返回候选项解码；删除提交后重复解码。Legacy 候选项使用可无异常移动的拥有型状态。

实现：[sdk/csharp/src/Verdandi/Registration/Selector.cs](D:/projects/verdandi/sdk/csharp/src/Verdandi/Registration/Selector.cs)、[sdk/cpp/include/verdandi/legacy/selector.hpp](D:/projects/verdandi/sdk/cpp/include/verdandi/legacy/selector.hpp)。

传播复查：C# 与 C++ Legacy 包装层都有此路径。Go、Rust 和原生 C++ 都在提交前准备类型化返回值；原生 C++ 的异常原子性另见 A09。C ABI 本身只处理 Fields，不能替包装层验证应用解码器。

验证：Legacy 公共 facade 的解码失败回滚及提交后用户 Data 移动异常场景通过；C# 库与正式回归项目均通过 net8.0/net10.0 编译。

待验证/限制：C# 返回解码回滚正式用例未运行；Legacy 使用 C ABI 夹具，尚非完整 native/Redis 集成。

回归入口：[sdk/cpp/tests/legacy_transaction_test.cpp](D:/projects/verdandi/sdk/cpp/tests/legacy_transaction_test.cpp)、[sdk/csharp/tests/Verdandi.Tests/Program.cs](D:/projects/verdandi/sdk/csharp/tests/Verdandi.Tests/Program.cs)。

### A11 · P1 · Go Selector 创建过程中关闭父客户端可能永久等待

**源码已修改**。首次就绪前的所有退出路径由 defer 发送 Closed，消除父客户端关闭时的启动等待缺口。

实现：[sdk/go/registration/selector_core.go](D:/projects/verdandi/sdk/go/registration/selector_core.go)。

传播复查：Go Registration Selector 受影响。Go Registration 注册 worker、Go Catalog 的初始化上下文，以及 Rust 的最终 ready 完成路径未发现相同缺口；C++ 使用自身的 promise/同步超时路径。

验证：完成 ready 发送路径及跨语言生命周期源码复查。

待验证/限制：拨号期间关闭父客户端的正式回归已加入，完整 Go 包因依赖缺失未执行。

回归入口：[sdk/go/registration/selector_startup_test.go](D:/projects/verdandi/sdk/go/registration/selector_startup_test.go)。

### A12 · P2 · Rust Catalog 构造 future 被取消时后台任务缺少取消守卫

**源码已修改**。Catalog 构造期使用 CancellationToken 的 drop_guard，成功交付后 disarm；Registration 复用同一现有运行库机制以去掉自定义 guard。

实现：[sdk/rust/src/catalog/subscriber.rs](D:/projects/verdandi/sdk/rust/src/catalog/subscriber.rs)、[sdk/rust/src/registration/selector.rs](D:/projects/verdandi/sdk/rust/src/registration/selector.rs)。

传播复查：Rust Catalog 特有的构造期取消路径。Rust Registration 对 ready 接收端消失有清理处理；Go 通过显式 context 结束初始化，C++/C# 不使用这一被丢弃 future 的模型。

验证：完成取消路径源码复查，并核验所用 tokio-util DropGuard API。

待验证/限制：完整 crate 类型检查与构造 future 取消的生命周期回归未运行。

### A13 · P2 · Rust 动态扩池期间取消会泄漏活跃命令计数

**源码已修改**。增加活跃命令计数后立即创建 CommandGuard，再进入可能等待的动态扩池路径。

实现：[sdk/rust/src/client.rs](D:/projects/verdandi/sdk/rust/src/client.rs)。

传播复查：Rust 根客户端影响两个业务域。C++ 在 creating++/slot 标记之后的可抛路径也缺少资源归还守卫，属于相近的异常风险，未运行内存故障注入；Go 由驱动管理连接池，没有相同手动计数路径。C#/Legacy 继承 C++ 相关风险。

验证：完成 guard 创建、取消与 Drop 归还路径源码复查。

待验证/限制：完整 crate 及确定性取消测试未运行；C++ 相近的分配异常归还风险 R01 仍未关闭。

### A14 · P2 · C++ Catalog 保留的空 Map/Array 在状态变化后消失

**源码已修改**。保留值存在性改用 replace_revision，避免把合法空 Map/Array 当成不存在。

实现：[sdk/cpp/include/verdandi/catalog/subscriber.hpp](D:/projects/verdandi/sdk/cpp/include/verdandi/catalog/subscriber.hpp)。

传播复查：C++、C ABI、C#、Legacy Catalog 继承。Go 使用 kind 判定、Rust 使用 Option 保留存在性，未发现同一缺口。

验证：空 Map/Array 在 present、unavailable、synchronizing、closed 的保留语义及真正 absent/deleted 场景通过。

待验证/限制：C ABI、C#、Legacy 的完整运行矩阵未运行。

回归入口：[sdk/cpp/tests/selector_transaction_test.cpp](D:/projects/verdandi/sdk/cpp/tests/selector_transaction_test.cpp)。

### A15 · P1 · C++ Registration 移动赋值不会关闭被覆盖的注册

**源码已修改**。移动赋值先交由临时拥有者接管，再交换 core，使旧注册确定执行关闭；保留自赋值保护。

实现：[sdk/cpp/include/verdandi/registration/registration.hpp](D:/projects/verdandi/sdk/cpp/include/verdandi/registration/registration.hpp)。

传播复查：原生 C++ 特有。Legacy owned_handle 移动赋值先释放旧句柄，C ABI 显式 release 走析构关闭，Rust 对被替换值执行 Drop；Go 显式生命周期模型没有此移动赋值接口。

验证：实际公共 Registration 模板验证旧 owner 已关闭、新 owner 仍有效。

待验证/限制：网络侧注销与租约回收集成未运行。

回归入口：[sdk/cpp/tests/selector_transaction_test.cpp](D:/projects/verdandi/sdk/cpp/tests/selector_transaction_test.cpp)。

### A16 · P2 · 三种原生 SDK 的 Patch 容量预估错误拒绝合法交换

**源码已修改**。三种原生 SDK 均按完整增减后的最终容量判断，避免中间增长误拒绝；Go 把纯容量计算与 I/O 分开。

实现：[sdk/go/catalog/projection.go](D:/projects/verdandi/sdk/go/catalog/projection.go)、[sdk/go/catalog/publisher.go](D:/projects/verdandi/sdk/go/catalog/publisher.go)、[sdk/rust/src/catalog/publisher.rs](D:/projects/verdandi/sdk/rust/src/catalog/publisher.rs)、[sdk/cpp/src/catalog.cpp](D:/projects/verdandi/sdk/cpp/src/catalog.cpp)。

传播复查：Go、Rust、C++ 都有，C#/Legacy 继承原生层。Lua 在完整增减后校验最终值，没有同样的中间值拒绝。

验证：Go 实际纯计算覆盖 Map/Array、两种字段顺序、合法交换、真实超限和错误旧字节数；C++ catalog.cpp 编译通过。

待验证/限制：Rust 正式回归已加入但未运行；各 SDK Publisher 集成及 C++ 此计算的直接运行回归未完成。

回归入口：[sdk/go/catalog/projection_test.go](D:/projects/verdandi/sdk/go/catalog/projection_test.go)、[sdk/rust/tests/internal/catalog/publisher.rs](D:/projects/verdandi/sdk/rust/tests/internal/catalog/publisher.rs)。

### A17 · P2 · Go 严格配置解析仍接受错误大小写及折叠重名

**源码已修改**。对象成员名限定为契约使用的小写 ASCII、数字与下划线，阻断大小写和 Unicode 折叠匹配；字符串值仍允许 Unicode。

实现：[sdk/go/configuration/json.go](D:/projects/verdandi/sdk/go/configuration/json.go)。

传播复查：Go 特有。Rust serde 和 C++ 的封闭绑定按精确名字匹配；C#/Legacy 配置走 C++。探针保留实际 json.go/config.go，但语义 check 被置为成功，仅证明解码边界，未冒充完整配置 SDK 测试。

验证：实际 json.go 扫描器的合法成员、错误大小写、ſ/K 折叠与 Unicode 值回归通过。

待验证/限制：隔离包的无关 Config.check 使用夹具；这不代表完整配置语义或 SDK 通过。

回归入口：[sdk/go/configuration/json_boundaries_test.go](D:/projects/verdandi/sdk/go/configuration/json_boundaries_test.go)。

### A18 · P2 · C++ Selector 截止时间堆随续租次数增长

**源码已修改**。截止索引改为按时间与 UUID 排序、可删除旧条目的 std::set；更新、移除与保留迁移同步维护。

实现：[sdk/cpp/src/internal/selector_state.hpp](D:/projects/verdandi/sdk/cpp/src/internal/selector_state.hpp)、[sdk/cpp/src/selector.cpp](D:/projects/verdandi/sdk/cpp/src/selector.cpp)。

传播复查：C++ Registration 及原生包装层受影响。Go、Rust 使用带索引的截止时间队列，每个 UUID 对应可更新条目。

验证：10,000 次续租仍只保留一项；同时间不同 UUID、到期迁移和移除回归通过；实际 selector.cpp 编译通过。

待验证/限制：完整运行时长期内存曲线未测。

回归入口：[sdk/cpp/tests/runtime_boundaries_test.cpp](D:/projects/verdandi/sdk/cpp/tests/runtime_boundaries_test.cpp)。

### A19 · P3 · 合法的 retry factor=1 导致退避计算随失败次数变慢

**源码已修改**。factor=1 直接返回固定延迟；C++ Selector/Catalog 复用同一个退避与等待实现。

实现：[sdk/cpp/src/internal/retry.hpp](D:/projects/verdandi/sdk/cpp/src/internal/retry.hpp)、[sdk/go/registration/selector_core.go](D:/projects/verdandi/sdk/go/registration/selector_core.go)、[sdk/rust/src/registration/selector.rs](D:/projects/verdandi/sdk/rust/src/registration/selector.rs)。

传播复查：Go/Rust Selector 以及 C++ Selector、Catalog 有同形逻辑，C#/Legacy 继承。Go/Rust Catalog 已有 factor<=1 的直接路径，可统一语义。

验证：C++ 最大 size_t 失败计数下 factor=1、指数增长和上限回归通过。

待验证/限制：Go/Rust 对应正式用例已加入但完整包未执行；其 Catalog 已有直接返回路径。

回归入口：[sdk/cpp/tests/runtime_boundaries_test.cpp](D:/projects/verdandi/sdk/cpp/tests/runtime_boundaries_test.cpp)、[sdk/go/registration/selector_startup_test.go](D:/projects/verdandi/sdk/go/registration/selector_startup_test.go)、[sdk/rust/tests/internal/registration/selector.rs](D:/projects/verdandi/sdk/rust/tests/internal/registration/selector.rs)。

### A20 · P2 · 仅修改 Lua 后增量构建 C++ 可能仍嵌入旧脚本

**源码已修改**。将八个被嵌入 Lua 输入加入 CMAKE_CONFIGURE_DEPENDS。

实现：[sdk/cpp/cmake/EmbedProtocol.cmake](D:/projects/verdandi/sdk/cpp/cmake/EmbedProtocol.cmake)。

传播复查：C++ 及其二进制包装消费者受影响。Go embed 和 Rust include_str 由编译输入追踪；每次都主动重新 configure 的构建脚本也不会触发这个具体场景。

验证：隔离 CMake 工程只修改 Lua 后执行 cmake --build，即触发重新配置并更新嵌入内容。

待验证/限制：完整依赖组合未运行。

回归入口：[sdk/cpp/tests/cmake_regression_test.cmake](D:/projects/verdandi/sdk/cpp/tests/cmake_regression_test.cmake)。

### A21 · P2 · Windows 运行时 DLL 列表为空时构建后复制失败

**源码已修改**。运行时 DLL 复制脚本显式处理空列表，并保持列表参数与含空格路径。

实现：[sdk/cpp/cmake/CopyRuntimeDlls.cmake](D:/projects/verdandi/sdk/cpp/cmake/CopyRuntimeDlls.cmake)、[sdk/cpp/CMakeLists.txt](D:/projects/verdandi/sdk/cpp/CMakeLists.txt)。

传播复查：Windows C++ 构建及 C#/Legacy 使用的原生 DLL 交付受影响；Linux 不进入 WIN32 分支。此结果不表示所有依赖组合都会失败。

验证：空列表、单 DLL、多 DLL 与含空格路径的复制及内容比较全部通过。

待验证/限制：各发行 DLL 依赖组合与打包测试未运行。

回归入口：[sdk/cpp/tests/cmake_regression_test.cmake](D:/projects/verdandi/sdk/cpp/tests/cmake_regression_test.cmake)。

### A22 · P1 · Sentinel 夹具部署拒绝后仍可能删除原有同名资源

**用户指定暂缓**。按用户要求暂缓 Python 修改与 Black 下载。

传播复查：这是共享测试基础设施问题，所有使用该 Sentinel Topology 的语言资格测试均受影响。Standalone Fixture 已有创建记录/所有权标签，不存在相同的无条件清理路径。

验证：未修改原 Python 夹具。

待验证/限制：Sentinel 清理所有权缺陷仍存在；未执行有风险的 Sentinel 夹具。

### A23 · P2 · Python peer 启动失败存在未接管的子进程

**用户指定暂缓**。按用户要求暂缓 Python 修改与 Black 下载。

传播复查：Catalog interop 与复用它的 Catalog Sentinel，以及主 Sentinel 进程启动逻辑受影响；各语言本身的 SDK 运行时代码不应因此扣成同一个生命周期 bug。

验证：未修改原 Python 夹具。

待验证/限制：peer 启动失败后子进程接管与清理问题仍待修复。

### A24 · P2 · C ABI Redis 测试失败清理会读取未初始化 key

**源码已修改**。清理涉及的 key 数组零初始化，erase_key 仅在 client 有效且 key 非空时执行。

实现：[sdk/cpp/tests/c_abi_redis_test.c](D:/projects/verdandi/sdk/cpp/tests/c_abi_redis_test.c)。

传播复查：C ABI 的 C 测试特有。其他语言测试中的字符串已初始化，未发现同一未初始化读取。不能据此声称生产 C ABI 普通调用有相同问题。

验证：实际 C 测试文件通过 MSVC C11 /utf-8 /W4 /WX 编译。

待验证/限制：连接建立中途失败的真实 C ABI/Redis 回归未执行。

回归入口：[sdk/cpp/tests/c_abi_redis_test.c](D:/projects/verdandi/sdk/cpp/tests/c_abi_redis_test.c)。

### A25 · P2 · Go 引用代码生成器把遮蔽内建名字的切片当成标量

**源码已修改**。解析标量前先检查本地类型声明，尊重对预声明标识符的合法遮蔽。

实现：[sdk/go/cmd/verdandi-refgen/main.go](D:/projects/verdandi/sdk/go/cmd/verdandi-refgen/main.go)。

传播复查：Go refgen 特有。Rust derive 与 C++ 模板使用各自类型系统，C# 不使用这个 Go AST 生成器。

验证：完整 refgen 包的 5 个顶层测试及子场景通过；包含 type uintptr []byte 生成切片视图与 Clone 的回归。

待验证/限制：无本项额外依赖阻塞；不代表完整 Go SDK 通过。

回归入口：[sdk/go/cmd/verdandi-refgen/main_test.go](D:/projects/verdandi/sdk/go/cmd/verdandi-refgen/main_test.go)。

## 简化效果与代价

- C++ 两处 retry/wait 实现合并；拆出队列、pending 与状态索引的实际实现供独立验证。
- Rust 删除手写启动取消守卫，复用 CancellationToken::drop_guard；Choice 校验集中到 valid_for。
- C# 与 Legacy 在提交前准备返回值，删除提交后的重复解码路径。
- Go 用 maps.Keys + slices.Sorted 生成有序字段名，把容量计算从发布 I/O 中拆出。

按原审计快照的相同口径，加上本轮新增源文件：332→353 个文件，79,711→80,895 个物理行，净增 1,184 行。统计排除 build 临时探针与文档，不把修复前的未提交改动当成本轮变动。

| 类别 | 原审计物理行 | 当前物理行 | 变化 |
| --- | ---: | ---: | ---: |
| 构建与开发工具 | 2852 | 2891 | +39 |
| 生成代码 | 6040 | 6060 | +20 |
| 手写运行时代码 | 44834 | 44985 | +151 |
| 测试与夹具 | 25985 | 26959 | +974 |

- Legacy 每个 detached candidate 新增一个拥有型状态分配，以保证提交后移动返回不调用可能抛异常的应用 Data 移动构造。
- C++ 新截止索引使存储与条目数绑定；未运行新的全 SDK 吞吐或延迟基准。
- 本轮优先正确性与必要回归；手写运行时代码净增 151 行，测试净增 974 行，不宣称已经显著缩小项目。

原审计评分不因这些局部测试自动上调；完整 SDK 和并发资格测试补齐后再评价。

## 未关闭的风险与验证工作

- **R01 内存分配失败下错误边界和连接池归还**：C ABI noexcept 的 bad_alloc 分支仍构造带分配的 error/detail；C# unmanaged 回调的错误编码也可能再分配。C++ driver 在 creating++ 后创建连接、在占用 slot 后构建请求的路径缺少全程守卫。需要故障注入，不能把尚未执行的极端分配失败写成已复现崩溃。
- **R02 C# 并发 Dispose 的 Result/异常边界**：IsUsable 检查与 SafeHandle 参与 P/Invoke 之间存在时间窗口，可能出现 ObjectDisposedException 而不是预期 Result。SafeHandle 提供内存生命周期保护；本轮没有证据认定 use-after-free。需要用同步屏障验证公开操作与 Dispose 的交错。
- **R03 Rust Subscriber 关闭完成条件发布顺序**：mark_scope(Closed) 与 Client Guard 释放完成后发布 closed；wait_finished 等待该完成标记，避免把 workers==0 当成清理完成。 源码已调整，仍缺少确定性并发验证。
- **R04 故障切换回滚与 checkpoint 修订单调性**：内存恢复可以发现服务端 revision 回退后重新同步，而持久化实现通常拒绝降低 cursor；C++ 的 cursor 也使用 max 累积。需要先明确主从切换丢失已确认写入时的 epoch/reset 契约，再用未受 WAIT 保护的故障场景验证，现有有 WAIT 的测试不能证明此情况。该项没有列入 25 个已定位缺陷。
- **R05 C++ 恢复任务的整体截止时间**：sync_timeout 明确用于构造和订阅确认，但恢复阶段的扫描/屏障等待还需验证是否始终受整体截止时间约束。Selector 并不存在所谓被忽略的 max_inflight_reads 配置，该猜测已排除；Catalog 才有对应配置。

A22 Sentinel 资源所有权修复完成前，不运行受影响的共享 Sentinel 夹具。A23 是 peer 启动失败后的子进程生命周期问题，仍未修复。

## Ubuntu 与测试清理

用户关闭 Hyper-V 动态内存并重启后，虚拟机报告总内存 7,422 MiB、最后可用 6,837 MiB、Swap 使用 0，最近读取的 Balloon 为 0。本次真实 Lua 边界回归通过。此前 OOM 的详细内核根因未定案，短时测试通过不等于系统长期稳定性资格通过。

Redis 最后健康检查时间 2026-09-07T16:27:31.4953508+08:00：PING=PONG、DBSIZE=0；进程 RSS 45101056 字节，Redis 记录的 used_memory_peak 36909768 字节。仅清理本次随机 Zone 的已知键，未执行全库删除；SSH 会话及临时转发已关闭，原有测试 Redis 容器保留。

