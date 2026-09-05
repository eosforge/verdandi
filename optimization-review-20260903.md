# Verdandi 全项目优化、边界覆盖与发布工程复核

日期：2026-09-03

基线提交：`00842fbbe2233ff3136549f3811fded8f10332b4`

范围：Lua、Go、Rust、C++23、C ABI/C++11/14/17 Legacy、C#、共享配置、构建与测试工具

状态：本报告随 2026-09-05 已获授权的 Alpha 变更一并提交和推送

## 结论

当前实现继续满足 `0.1.x` Alpha 的公开审阅和受控服务接入要求。本轮以测量和静态证据为准，
没有为了减少源码行数合并应独立失败的协议边界，也没有把 Go、Rust、C++ 和 C# 机械改成同一
种并发或所有权形态。

本轮完成三项有直接收益的改进：

1. Go Catalog 的完整通知原先在解码后再次排序和遍历字段以校验形状、名称和容量；现在在一次
   有界解码/复制过程中完成这些校验，同时保留结构损坏优先于语义错误的诊断规则。512 字段
   `replace` 的 Linux 十轮中位数由 `85.87 us` 降到 `27.20 us`，下降 `68.33%`。
2. Go 的 Redis 固定宽度整数和协议安全整数统一使用无中间字符串转换的规范十进制解析原语。
   `int64`/`uint64` 解码分别下降 `42.86%`/`45.11%`，每次分配由 2 次降到 1 次；Registration
   完整 Hash 解析也减少 2 次分配。
3. Windows C++ 构建脚本修复 PowerShell 5.1 行为差异：系统 OpenSSL 探针的预期 stderr 不再
   被全局严格错误策略提前提升为异常，`auto` 模式能够按设计回退到现有 vcpkg。PowerShell
   5.1 和 7.6 均通过真实探针；脚本仍不安装工具链或 OpenSSL。

Go、Rust、C++、C ABI/Legacy 和 C# 的静态、离线、竞态或消毒器回归均通过。远端
`192.168.0.90` 在本轮不可达，Standalone 测试器在创建隔离容器之前因 SSH 超时退出，因此
本轮工作树没有新增 Redis 实机或长测资格；既有冻结源码的 Redis/Sentinel/TLS/12 小时结果
仍是历史证据，不能转移到当前提交的工作树。

## 代码改进

### Go Catalog 单遍通知解码

`decodeEventFields` 现在同时承担以下有界工作：

- 验证 MessagePack 字段数组数量、交替布局和完整消费；
- 验证 Map/Patch 的字节字典序，验证 Array 的连续规范十进制下标；
- 验证 Value 的唯一 `value` 字段、Map/Patch 的 UTF-8 与保留前缀规则；
- 在复制字段值时累计逻辑字节数，并在加法前执行上限检查；
- 对 Replace 核对发布的 `@encoded_bytes`，对 Patch 维持独立增量容量上限；
- 使用连续名称和值存储，且把每个暴露值的 capacity 截断到 length，防止调用方追加覆盖相邻值。

优化前的 `validateValue` 会对已经按协议排序的 512 字段重新构造名称切片、排序并遍历；CPU
profile 中 `sortedNames/pdqsort` 占主要成本。优化后 profile 中排序已经消失，剩余主要成本是
Hash map 插入/哈希、值复制、UTF-8 校验和 GC。这些都是当前拥有型 `Fields` 合同的真实成本；
若继续消除它们，就需要引入借用事件或自定义容器生命周期，当前没有足够证据支持扩大复杂度。

新增边界测试覆盖：精确容量、容量加一、空 Patch、保留字段名、非法 UTF-8、错误 Value
形状、规范 revision/整数、零值规则、前导零、符号、非数字和安全整数上限。

### Go 规范整数解析

内部 `validate.UintDecimal` 和 `validate.UintDecimalBytes` 采用乘加前溢出检查：

- 拒绝空输入、正负号、前导零和非数字；
- `allowZero` 只允许唯一形式 `"0"`；
- 调用方提供精确上限，包括 `2^53-1` 协议 revision、31 位配置值和各固定整数位宽；
- `int64` 最小值通过无溢出的绝对值边界处理；
- string 与 byte slice 保持两个具体循环，避免泛型字典和 `[]byte` 到 string 的转换成本。

成功热路径不再执行 `ParseUint -> FormatUint -> 字符串比较`。失败路径仍调用标准解析器取得可用
Cause，不改变稳定 Verdandi code/field。反射写入泛型目标仍产生 8 B/1 allocation；继续消除它
需要绕过安全反射或扩大特化代码，收益不足以抵偿维护风险。

### Go 语言版本整理

两个既有错误链判断改用 Go 1.27 的 `errors.AsType`，去掉只为 `errors.As` 声明临时目标变量的
样板。Context、每 Registration 单 worker/单槽合并邮箱、每 Selector 单长期监听与至多一个
临时同步任务均保持不变；本轮没有修改其已验证的所有权模型。

### Windows C++ 构建入口

`Invoke-CMakeProbe` 现在只在执行可预期失败的原生命令期间把 PowerShell ErrorAction 降到
`Continue`，并始终在 `finally` 中恢复调用方的严格策略。探针仍以退出码裁决，将完整输出写入
隔离日志；PowerShell 自身错误仍会抛出。验证覆盖：

- PowerShell 7.6：系统 OpenSSL 不存在时回退现有 vcpkg，doctor 通过；
- Windows PowerShell 5.1：同一自动回退通过；
- Windows PowerShell 5.1 `-Dependencies system`：按设计失败，返回探针日志路径和稳定错误；
- Linux Bash doctor：GCC 13.3、CMake 4.3.2、Ninja、系统 OpenSSL 3.0+ 通过。

一次并行调用两个 Windows doctor 会竞争同一个探针目录；构建目录本就按配置唯一，不承诺同一
配置的并发构建。验收使用串行调用，未把该人为竞争产生的废弃结果计入产品失败。

## 性能证据

环境：WSL/Linux amd64、Go 1.27.0、Intel Core i7-13700F。每个对比各运行 10 次，使用
`benchstat`；数值为中位数。

| 基准 | 优化前 | 优化后 | 变化 |
| --- | ---: | ---: | ---: |
| Catalog DecodeReplaceEvent | 85.87 us/op | 27.20 us/op | -68.33% |
| Catalog DecodeReplaceEvent bytes | 91,485 B/op | 82,008 B/op | -10.36% |
| Catalog DecodeReplaceEvent allocs | 8 | 6 | -25.00% |
| Redis int64 decode | 96.94 ns/op | 55.38 ns/op | -42.86% |
| Redis uint64 decode | 102.35 ns/op | 56.19 ns/op | -45.11% |
| Redis integer decode bytes | 32 B/op | 8 B/op | -75.00% |
| Redis integer decode allocs | 2 | 1 | -50.00% |
| Registration ParseStoredRecord | 6.351 us/op | 6.161 us/op | -2.99% |
| Registration ParseStoredRecord bytes | 8,789 B/op | 8,768 B/op | -0.24% |
| Registration ParseStoredRecord allocs | 67 | 65 | -2.99% |

本轮全量 Go benchmark 冒烟还确认：Registration 规范上限记录校验为 0 allocation，普通 pending
应用约 19.74 ns/0 allocation，RedisClock 约 23.50 ns/0 allocation，引用型 `Any(8/500)` 为
0 allocation。不同执行轮次的绝对时间不用于跨机器比较。

## 短回归结果

| 范围 | 本轮结果 |
| --- | --- |
| Go 格式、模块校验、vet、全部包 | 通过 |
| Go 10 次随机测试顺序 | 通过 |
| Go WSL/Linux race | 通过 |
| Go 新边界用例 100 次重复 | 通过 |
| Go 配置/Catalog/Registration fuzz，各 15 秒 | 通过；约 1,098,567 / 208,118 / 3,854,238 次执行 |
| Go benchmark 十轮对比与全量冒烟 | 通过 |
| Rust stable fmt/test/strict Clippy/rustdoc | 通过；73 library + 4 offline external tests |
| Rust 1.85 MSRV check/test | 通过；同一 77 个 endpoint-independent tests |
| C++ Linux/Windows shared Release build + CTest | 通过；各 6 个离线项通过、3 个端点项按契约跳过 |
| C++ clang-format / clang-tidy | 通过 |
| C++ GCC ASan/UBSan/Leak CTest | 通过；6 个离线项通过、3 个端点项跳过 |
| C ABI v1 / C++11/14/17 Legacy | Linux、Windows 和消毒器树均通过 |
| C# .NET 8/10 format/analyzers/Release | 通过，0 warning / 0 error |
| C# .NET 8/10 原生 DLL 离线回归 | 通过 |
| Registration/Catalog Lua 生成一致性 | 通过 |
| Python 测试工具语法 | 17 个文件通过 |
| Redis 8 本轮实机回归 | 未执行：远端 SSH/ICMP 超时，fixture 创建前退出 |

普通 `go test -cover ./...` 的合计 statement coverage 是 `47.4%`；分包为 root 53.1%、Catalog
33.3%、Registration 44.0%、configuration 78.4%、lifecycle 80.4%、validate 100%。该数字
未启用 Redis/Sentinel/load/soak 标签，因此不能直接代表协议测试深度，也不能用补无价值分支
的方式追逐。需要持续关注的是状态转换、失败分类和资源所有权，而不是单独设定行覆盖率目标。

## 边界覆盖复核

| 边界类别 | 现有覆盖 | 审计结论 |
| --- | --- | --- |
| 标识符与字段 | Zone/Type/UUID、UTF-8、保留前缀、重复、排序、字段数/长度 | 充分 |
| 数值协议 | 零、符号、前导零、非数字、各位宽上下界、`2^53-1` | 本轮补强后充分 |
| 二进制协议 | MessagePack 类型、长度头、截断、尾随数据、声明数量炸弹 | Go/Rust 直接覆盖充分 |
| Catalog 值 | Value/Array/Map 形状、Replace/Patch/Delete、base gap、容量 | Go/Rust 充分；C++ 缺直接畸形通知注入 |
| Registration 状态 | Register/Update/Renew/Unregister、版本、TTL、pending 合并 | 充分 |
| 并发与关闭 | Go race/goleak/取消，Rust poison/shutdown，C++ RAII/消毒器，C# disposal/finalizer | 充分 |
| 所有权 | Fields 深拷贝、切片追加隔离、detached/reference view、C ABI handle | 充分 |
| 配置 | 1 MiB 上限、重复/未知字段、默认/显式零、关系、41+6 共享 corpus | 充分；属性 fuzz 目前只有 Go |
| Redis 故障 | 断连、脚本缓存、丢通知、切主、Sentinel 丢失、错误 TLS 身份 | 历史冻结证据充分；当前 diff 未重跑 |
| 持久化 | bbolt/redb/SQLite 恢复、损坏/过期 checkpoint、清理 | 已有覆盖 |
| 跨语言 | Go/Rust/Lua、C++/C ABI/Legacy、C# 共用核心 | 功能覆盖广；正式包消费矩阵未完成 |
| 构建工具 | Windows/Linux doctor、离线缓存、错误依赖、PowerShell 5.1/7 | 本轮修复后满足 Alpha |

没有测试集合能证明所有可能输入；这里的“充分”表示已覆盖当前协议中可枚举的上下界、主要状态
转换和已知故障类别，并有 fuzz/race/sanitizer 补充组合空间。

## 仍需补充的测试与发布门

以下不阻止 `0.1.x` Alpha，但不能在 `1.0.0` 时继续留空：

1. 为当前最终提交重新执行隔离 Redis 8、Sentinel TLS 和精确源码指纹长测；旧冻结结果不能继承。
2. 增加 C++ Catalog/Registration 通知解析器的直接畸形 payload 表驱动或原生 fuzz，避免仅依赖
   canonical live 通知和共享核心的上层测试。
3. 增加 C ABI 导出符号快照、结构 size/alignment、旧头+新库和新头+旧库兼容门，并完成
   CMake install/export 与真实下游 `find_package` 消费。
4. 为 C# 完成 RID 原生资产打包、NativeAOT/trimming、分配基准和发布包内加载测试。
5. 在需求确定后补 live mutual TLS；当前已验证的是私有 CA 的服务端认证和错误身份拒绝。
6. Rust/C++ 若解析器继续演进，再增加属性 fuzz/allocator 基线；当前没有 `unsafe` 或复杂 lock-free
   算法，不建议仅为工具清单引入 Loom/Miri。
7. 增加直接 C++23 常驻 peer 的完整两次 Sentinel promotion；当前直接 C++ smoke 覆盖 TLS 全领域，
   两次 promotion 由使用同一 C++ 核心的 C# 覆盖。

Redis Cluster、macOS、ARM64 和 generic Leader 不在已承诺范围；其中前两项与 Leader 是明确不
支持或已撤销的目标，不作为当前缺陷计分。ARM64 若未来进入支持矩阵，必须重新完成 ABI、原生包、
性能和故障资格。

## 分语言评分

评分针对已经实现的 `0.1.x` Alpha 范围，不等同于 `1.0.0` 发布承诺。

| 范围 | 评分 | 主要扣分点 |
| --- | ---: | --- |
| Lua 原子协议层 | **9.8/10** | Redis 运行期极晚失败不能跨命令回滚；当前 diff 无新实机/长测 |
| Go SDK | **9.8/10** | 拥有型大型 Catalog 事件仍有 map/copy 成本；原始 Redis 逃生口不受协议不变量保护 |
| Rust SDK | **9.7/10** | 配置启动期双解析；尚无属性 fuzz、分配基线和当前源码长测 |
| C++23 SDK | **9.6/10** | 缺直接畸形通知/fuzz、专项性能基线、完整两次切主和 install/export |
| C ABI / C++11/14/17 Legacy | **9.6/10** | 缺自动 ABI 兼容矩阵和正式包布局；同步 ABI 无异步取消协议 |
| C# SDK | **9.6/10** | 依赖原生资产分发；NativeAOT/trimming/RID 包和分配基准未完成 |
| 跨语言配置层 | **9.8/10** | Go 已有属性 fuzz，其余语言仍以共享 corpus 为主；mTLS 未验证 |
| 构建、测试与发布工程 | **9.6/10** | 覆盖广，但本轮远端不可达，当前 diff 无实机、长测和自动包消费门 |
| **当前已实现 Alpha 整体** | **9.7/10** | 适合 `0.1.x` 受控接入；不构成生产承诺或 `1.0.0` 资格 |

## 主要优点

- Lua 只承担 Redis 原子粘合；SDK 负责应用结构校验和解析，职责边界清晰。
- Go、Rust 和 C++ 使用各自语言的 Context/RAII/cancellation/expected/所有权机制，没有形状同步
  带来的最低公分母设计。
- Registration 和 Selector 的 worker/task 所有权符合已确认模型，不按进程建立全局队列，也不
  为每次全量同步永久增加线程。
- Catalog 有完整最新状态、增量通知、gap repair 和可选 checkpoint，而不伪装成审计日志。
- C++23、C ABI、Legacy 和 C# 共用一个原生状态机，避免四套协议实现漂移。
- 配置默认值、范围、显式零和关系检查在各语言有同一外部 JSON 语义，内部保持语言原生结构。
- 测试覆盖静态、单元、fuzz、race、sanitizer、MSRV、跨语言、Redis、Sentinel、TLS 和历史长测；
  失败环境与产品失败在证据中分开记录。

## 主要缺点与取舍

- 多语言完整 SDK 的生产源码体量仍大；主要来自独立所有权/并发实现、详细中文维护注释和原生
  绑定层，而不是可以安全删除的重复业务状态机。
- Go `Fields` 与安全 detached view 以拥有数据换取简单生命周期，大对象仍会产生 map 与复制成本。
- Rust 为严格拒绝位置式/重复 JSON 输入保留形态预检和 Serde 类型解析两阶段，启动成本不是最低。
- C++ 同步命令边界经 reactor、连接池和 promise/future，正确性清晰，但缺独立性能基线。
- C ABI/C# 发布体验尚依赖调用者正确放置 DLL/SO；源码可构建不等于包管理已经完成。
- 当前源码的联网与耐久资格不完整；任何正式生产声明都必须以最终提交重新冻结和运行。

## 最终判断

本轮没有发现需要阻止 `0.1.x` 继续分布式开发的已知正确性问题。已修改的 Go 热路径有显著、
可重复且不扩大公开 API 的收益；PowerShell 修复消除了真实跨版本构建失败。当前提交适合交给
维护者继续 code review；远端恢复后仍应补跑实机回归，之后的提交或推送仍需维护者明确授权。
它尚不满足 `1.0.0` 的最终注释、包发布、ABI 消费、最终源码故障回归和耐久资格。
