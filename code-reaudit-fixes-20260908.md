# 第二轮修复与短回归验证（2026-09-08）

修复前的逐行审查见 [code-reaudit-20260907.md](code-reaudit-20260907.md)。全部自有代码静态审查完成后才开始本轮修改和 SDK 测试。B01–B29 均已落实源码修复；Windows 与 Ubuntu 的下列短回归已通过。这不等于所有取消交错、网络故障或长时间负载都已验证。

## 修复、扩散复查与证据

| 问题 | 已落实的改动 | 本轮验证及边界 |
| --- | --- | --- |
| B01 | Go/Rust/C++ 在 Entry 插入锁内复核关闭状态，迟到插入只能产生 Closed；C ABI/Legacy/C# 继承 C++。 | Go 定向迟到插入回归通过；C++ 两平台各 8 轮 Find、创建和两次 Close 并发交错通过，每轮检查 128 个迟到路径。压力交错不能替代所有调度的确定性证明。 |
| B02/B03 | 三语言区分全空头部的 Stale 与部分损坏的 Corrupt；C++ 非法数组名返回 Contract、未知 kind 返回 Corrupt，保留 Value 转换的 Transition。 | Go/Rust 头部纯函数回归通过；C++ 两平台真实 Redis 删除后 Patch 返回 Stale。C++ 部分损坏头部的专门注入仍待补充。 |
| B04 | C++ Catalog/Registration 创建、首次发布及 driver 订阅创建纳入关闭准入；并发 Close 串行汇合。关闭先汇合准入，释放准入锁后等待子任务。 | C++ 两平台全量测试及 Catalog 并发创建/Close 回归通过；Registration 首次发布与根 Close 的每个交错尚未逐一注入。 |
| B05 | Rust 从 connect 返回起以 AbortOnDropHandle 拥有驱动任务，覆盖根池初始连接、Catalog、Registration Selector 的构造取消；显式关闭终止并等待任务。 | 真实 TCP 握手中取消、首次 poll 前取消回归在两平台通过；Redis 初始化/关闭集成通过。ACL 拒绝后多轮重试的连接计数仍待验证。 |
| B06 | Rust checkpoint 显式关闭 Database，在完成存储操作后释放文件锁；Catalog Close 等待该释放。 | 内部 checkpoint 与公开 Client/Subscriber 回归通过：保留旧句柄时重开相同文件，恢复到当前 revision，关闭后 Entry 为 Closed。两平台均通过。 |
| B07/B08 | C++ Read 严格检查成功回复字段数量和规范字节数；Rust 共用严格 RESP String/Bytes 转换，拒绝数组、Queued、Integer。 | Rust 类型拒绝回归通过；C++ 实际 Read/快照/恢复集成通过。C++ 异常服务器回复的独立注入仍待补充。 |
| B09 | Go/Rust 在任何默认值补写前校验 HMGET 形状和精确长度。 | Go 用 go-redis Hook 注入 0/1/7/9/16 项全空回复，均 Corrupt 且零写入；Rust 编译及配置集成通过，异常服务器注入待补充。 |
| B10 | Go/Rust 允许完整空 Data 的 no-op，继续经过关闭准入和 worker。 | 两平台公开 API 验证空 Data 不改变 revision/timestamp，Close 后调用返回 Closed。C++/绑定原有空 Data 语义经源码对照保留。 |
| B11/B15 | C++ 无 TTL 写入的邮箱批次后仍检查续租期限；每个 Update 在合并之前拒绝单字段超限，防止被后续覆盖掩盖。 | 两平台构建、Registration 更新/续租集成通过。持续占满邮箱的 no-op 与并发覆盖定向回归仍待补充。 |
| B12 | Rust 在启动 Registration worker 之前构造负责 Drop 关闭的 Core，正常返回只转移已有所有者。 | 两平台编译及生命周期/Redis 回归通过；ready 发送之后立即取消的精确交错待注入。 |
| B13/B14/B16/B18 | C++ Selector 自然到期变化统一安排发布；读取使用协议上限；旧同 revision Register 不回退 timestamp；忽略未知可选 @ Hash 字段。 | 两平台真实 Redis 回归通过：下调写上限后读取旧记录及额外可选元数据，在没有发布消息、没有续租时按时移出活动候选。旧事件逆序回放仍待专门注入。 |
| B17 | Rust 合并 Update 保留新旧最大 timestamp；Go/Rust 连续更新和万次合并测试补充时间断言。 | Go/Rust 两平台回归通过。 |
| B19 | C# 父 lease 链归 DependentSafeHandle 所有，真实 ReleaseHandle 后释放父引用；转移时取消 lease 的普通终结器。移除七处重复保管代码。 | .NET 8/10 均构建零警告、零错误；两框架的离线及真实 Redis 测试通过，包含仅叶对象存活、父 Dispose 的三级 GC/释放顺序回归。Linux 未安装 .NET，未声明 C# Linux 运行通过。 |
| B20 | C ABI 在 Selector 提交前构造输出容器；One 预构造候选槽，提交后仅 noexcept 移动赋值；Any 移动 vector 所有权。 | MSVC 揭示候选移动构造并非 noexcept，已改为预构造加移动赋值并保留静态断言。两平台编译、C ABI 真实 Redis 通过；未注入所有分配失败点。 |
| B21/B22 | C++ typed Key 与完整 Schema 编解码捕获应用构造、Codec、赋值/移动异常；统一 UTF-8 校验，Schema 支持合法中文字段。 | 两平台抛错 Codec/构造器、中文 Schema、非法 UTF-8 与类型边界测试通过。 |
| B23/B24 | Bash 工具路径正确加引号，并在任何 shell 设置前拒绝 source；PowerShell 在 finally 恢复编码和环境。补修 $null 被转换成空字符串、留下空 VSLANG 的问题。 | Linux 三个入口 source 拒绝后选项与语言环境不变，带空格 CMake/GCC 路径的版本探测通过。Windows 成功/失败路径均保留父环境与编码。Go/Rust PowerShell 入口仅设置子进程环境。 |
| B25/B26 | Go/Rust 拒绝已出现但非法的元数据；Catalog/Registration 分领域使用状态白名单；扩散复查同时修补 Go/C++ Read 入口。 | Go/Rust 异常回复回归通过，三语言真实 Redis 集成通过；C++ 异常回复矩阵待独立注入。 |
| B27/B28 | Go Lua 上限测试允许预期 Capacity 进入后续原子性断言；soak 监控启动后立即登记幂等停止和汇合 cleanup。 | Windows/Ubuntu 真实 Redis 验证 65,536 字段成功，第 65,537 个字段被拒绝且 Hash/ZSET/revision 不变，满容量覆盖仍可读取。长 soak 与其中途故障未执行。 |
| B29（旧 R05） | C++ Selector 整次同步使用绝对期限，扫描、fence、listener 共同检查，到期取消并等待当前任务后重试。 | 两平台完整构建和正常同步通过。慢扫描/丢失 fence 待定向验证；期限后的在途命令清理由普通传输超时约束。 |

## 已落实的简化

- C++ 四份 UTF-8 实现合并为一个 constexpr helper，供 Schema、Catalog、Registration、Selector 和配置共用。
- C++ Registration 先判断变化再复制完整 Data，no-op 不复制；移除只转发字节比较的 helper。
- Go 负载测试使用固定 worker 数，避免为全部任务先建立协程，错误缓冲按 worker 数分配。
- Rust 统一 RESP 类型边界、Catalog 状态解析，构造取消复用已有 Core 的 Drop。
- C# 七处父链保存与释放集中到一个 SafeHandle 基类。

长测延迟样本仍随时间占用内存，24 小时两组更新样本约 659 MiB；固定大小统计方案保留在审查报告中。本轮没有证实该预算就是先前 VM OOM 的原因。

## 测试矩阵

| 范围 | Windows | Ubuntu 26.04 |
| --- | --- | --- |
| Go | 全包单元及 Redis integration 通过；公开空 Data 回归、含 integration 的 go vet 通过 | 全包单元与 Redis integration 均在 -race 下通过，含新增空 Data 回归 |
| Rust | 84 项 library 及完整 workspace 测试通过；10 项真实 Redis 集成通过；Clippy workspace/all-targets -D warnings 通过 | 85 项 library 测试通过；10 项真实 Redis 集成通过；新增 checkpoint/空 Data 所在 8 项再次通过 |
| C++23 / C ABI / Legacy | shared Debug 全量构建；16/16 CTest 通过；新增关闭交错回归通过；全量 clang-format 检查通过 | GCC 15.2 shared Debug 全量构建；16/16 CTest 通过，包含相同关闭交错、到期及策略回归 |
| C# | net8.0/net10.0 构建及离线、Redis 测试均通过 | 本轮未运行 C# |
| 互操作 | Go ↔ Rust 注册记录与 Catalog 写入、读取、删除全部通过，双方正常 STOP，专属测试 Zone 清理完成 | 使用 VM 内现有隔离 Redis；未搭建 Sentinel 故障拓扑 |

结构化结果见 [code-reaudit-20260908.json](testkit/results/code-reaudit-20260908.json)。本地日志、同步哈希和运行入口位于 `build/reaudit-20260907/`。测试执行限定项目缓存、锁文件及已授权依赖；SDK 编译/测试使用 offline 或 GOPROXY=off、GOTOOLCHAIN=local。互操作 Rust peer 的独立锁文件另补齐 8 个锁定 crate 到 `build/deps/cargo`，随后离线构建，未改锁文件。没有安装 Python 包，互操作使用项目内临时 PowerShell 驱动调用既有 peer。

Ubuntu 修复源码通过 65、5、2 个文件的三个增量批次同步，测试结束后另同步最终报告。每次先核验压缩包和全部旧/新文件 SHA-256 再写入，不传 `.git` 或缓存。初始快照及各次清单保留，`source-current-manifest.json` 记录当前已同步源码。Linux OpenSSL 仅补齐预编译开发包内两个架构头文件的相对链接，没有源码构建或系统 OpenSSL 修改。

VM 编译每次一个任务，Go GOMAXPROCS=2、测试并发 2；观测物理内存 7422 MiB、可用约 6.5 GiB，Swap 使用始终为 0。本轮未复现 OOM；没有据此推断历史 OOM 的原因。

## 尚未覆盖

Python A22/A23 修复与下载继续按用户要求暂缓，不运行已知不安全的 Sentinel 清理夹具。慢网络/ACL/精确取消交错等注入缺口见上表；本轮不声称已完成新源码的 Sentinel 故障切换、TLS/mTLS、长 soak、ASan/UBSan 或所有平台静态分析资格。旧报告里的对应结果属于旧源码，不挪作本轮证据。
