# Verdandi 全项目优化与回归审计

日期：2026-09-01
范围：Lua、Go、Rust、C++23、C ABI/C++11 Legacy、C#、共享配置和测试工具
状态：当前未提交工作树完成短回归；未提交、未推送

## 结论

当前实现已经达到可公开审阅和用于 `0.1.0` Alpha 分布式开发的工程质量。
本轮没有为了减少行数而删除边界检查，也没有把不同语言机械改成相同形状；只接受能由
正确性、所有权、编译器诊断或测量证据支持的修改。

本轮发现并修正了五类实际问题：

1. C++ 最后一个 Driver 引用若恰好在 reactor 线程释放，原实现存在析构后
   `io_context::run()` 栈继续引用对象的生命周期风险；现在 reactor 拥有独立共享运行期。
2. C++ 命令在额外完成等待仍超时时，原连接不能先回到池中；现在先永久退役再异步取消，
   后续调用不会复用一条仍有未决 handler 的连接。
3. Rust Catalog 的部分锁中毒分支会静默丢弃同步请求或返回脱离权威视图的 Closed Entry；
   现在只对无用户回调、异常安全的内部临界区统一恢复，并有直接 poison 回归。
4. Rust Catalog 检查点恢复先构造完整受订阅范围覆盖的视图和总字节数，提交视图后最后发布
   cursor，避免观察到新 cursor 却仍看到旧视图的半提交窗口。
5. C# Fields 构造和原始 Key 写入存在可避免的逐字段对象、名称数组、值数组、HashSet 及整值
   再复制；现在使用值类型暂存、排序后相邻去重、一次连续载荷复制和同步 C ABI Span 固定。

另外，Windows shared C++ 构建现在自动把运行时 DLL 复制到目标目录，C# 独立测试明确选择
与宿主匹配的原生运行库。干净环境不再依赖调用者偶然配置的 `PATH`。

未发现仍需阻止 `0.1.0` Alpha 代码审阅的已知正确性缺陷。当前源码仍不能宣称已经通过
12 小时长测：既有 12 小时结果属于较早冻结提交，本轮修改后的工作树只完成本文记录的
静态、单元、竞态、消毒器、真实 Redis 8.8 和 Sentinel 故障回归。

## 审阅原则与代码体量

生产源码物理行数包含详细中文维护注释，不等同于圈复杂度或运行成本：

| 范围 | 文件数 | 物理行数 |
| --- | ---: | ---: |
| Go 生产源码（排除 `_test.go`） | 47 | 11,313 |
| Rust `src` | 33 | 9,880 |
| C++ 实现 | 16 | 8,510 |
| C/C++ 公共与内部头文件 | 27 | 4,191 |
| C# 生产源码 | 17 | 4,754 |
| Lua 规范片段与生成脚本 | 20 | 2,171 |

这些代码并非一套实现的简单多语言翻译。Go、Rust 各自拥有原生异步/并发模型；C++23 是
C ABI、Legacy 和 C# 共用的唯一原生运行时；Lua 是 Redis 内的最小原子粘合层。继续压缩
代码时应优先消除重复状态和重复所有权路径，而不是合并本来就应独立失败的协议边界。

## 分语言评分

评分只针对已经实现的 `0.1.0` Alpha 范围，不把明确不支持的 Cluster 当作缺陷。
Generic Campaign/Leader 已于 2026-09-02 从项目目标撤销，不再影响 `1.0.0`。

| 范围 | 评分 | 主要扣分点 |
| --- | ---: | --- |
| Lua 原子协议层 | **9.8/10** | Redis 运行期极晚 OOM/脚本运行错误无法做跨命令回滚；当前源码未重新做逐行统计基准或长测 |
| Go SDK | **9.7/10** | typed Selector 与 Catalog 大事件解码仍有可测分配；原始 Redis 逃生口由 ACL 约束而不受 Verdandi 不变量保护 |
| Rust SDK | **9.7/10** | 配置形态预检加类型解码有启动期双解析；尚无 Loom/Miri/分配统计与当前源码长测 |
| C++23 SDK | **9.6/10** | 同步 promise/future 命令边界尚无专项基准；缺直接原生两次切主、Clang 编译矩阵、安装导出和当前源码长测 |
| C ABI / C++11/14/17 Legacy | **9.6/10** | ABI 自动兼容检查和正式包布局未完成；同步 ABI 无取消感知异步接口 |
| C# SDK | **9.6/10** | 依赖原生资产分发；无真异步、NativeAOT/trimming/ARM64、分配基准、直接跨语言 peer 和当前源码长测 |
| 跨语言配置层 | **9.7/10** | 双向 TLS、自动打包和配置 fuzz 仍未完成；Rust 的 Fred 映射保持必要适配复杂度 |
| 测试与发布工程 | **9.6/10** | 覆盖广且有真实故障注入，但本轮源码尚无长测、C++ 直接两次切主、mTLS 和自动包消费门 |
| **当前已实现 Alpha 整体** | **9.7/10** | 可用于 `0.1.0` 受控接入，不构成生产承诺或 `1.0.0` 资格 |

## 逐语言优点与剩余问题

### Lua

优点：

- Registration 与 Catalog 各自使用四个按操作生成的专用脚本；SHA 缓存不会重复解析源码。
- 控制参数使用固定位置，动态 Attr/Data/Catalog 字段保持命名形式，Lua 不承担应用结构校验。
- Revision、timestamp、TTL、Hash/ZSET、membership 和 Pub/Sub 在一个 Redis 原子执行中更新。
- 生成文件逐字节检查，Redis 8.8 实测覆盖 `HSETEX`、`SCRIPT FLUSH`、安全整数上限、精确 base
  竞争、4 MiB Catalog、TTL 与 Delete。

缺点与方向：

- 源片段和生成结果是两套物理文件，但生成一致性门已经把漂移变成构建失败；不建议重新合成
  一个巨型通用脚本。
- Redis 脚本原子性防止并发穿插，却不能撤销一个命令成功后才发生的 Redis 运行期错误。应继续
  保持脚本短小，并用真实 Redis 故障测试而不是在 Lua 中复制 SDK 校验。
- 没有证据支持继续缓存 `@revision` 等极短常量或改写当前 append 形态；本轮因此未做推测性微调。

### Go

优点：

- 根 Client 保持 go-redis 薄封装；Zone、Registration、Selector 和 Catalog 生命周期归领域所有。
- 每个 Registration 一条单槽 Fields 合并邮箱和一个长期 worker；每个 Selector 一个长期监听
  加至多一个临时同步任务，符合已确认的所有权模型。
- 泛型只出现在强类型输入输出边界；内部使用拥有型 Fields，应用 Codec 不被 SDK 代码生成绑架。
- Context 作为每次操作参数传播，不存入长生命周期 Client；关闭、结果 waiter 与后台 worker
  有明确所有者。
- `gofmt`、vet、全包测试、WSL/Linux race、Redis 8.8、跨语言和两次 TLS 切主全部通过。

缺点与方向：

- 当前 500 候选 `One` 约 2,225 B/28 alloc，`Any(8)` 约 8,065 B/97 alloc。它们包含强类型
  候选物化和安全脱离视图的成本；应先用真实服务策略 profile，再决定是否增加借用型高级 API。
- Catalog Replace 事件解码约 91 KiB/8 alloc，主要是完整值所有权，而非每字段对象爆炸；只有在
  大 Catalog 更新成为实际 CPU/GC 热点后才值得引入更复杂的共享缓冲生命周期。
- `Client.Redis()` 是明确的 ACL 逃生口。它是运维和高级命令能力，不保证 Verdandi 的 Revision、
  TTL 或广播不变量；文档必须持续说明这一点。

### Rust

优点：

- RAII admission guard、Tokio cancellation、Arc/ArcSwap 和 awaited shutdown 保持 Rust 原生所有权，
  没有复制 Go 的 channel/context 形状。
- 生产 crate 无 `unsafe`、无 `unwrap/expect` 热点，严格 Clippy、warning-denied rustdoc 和 Rust 1.85
  最低工具链均通过。
- Catalog 仍是一个长期 listener 加至多一个临时同步 task；锁只保护极短的请求合并、视图索引和
  字节预算，不跨 Redis I/O 或用户回调。
- 本轮 poison 恢复只用于内部异常安全临界区，防止一次内部 panic 永久静默丢请求；测试会主动
  poison Mutex/RwLock 并验证继续服务。
- 检查点恢复的 view、byte count、cursor 现在按完整提交顺序发布。

缺点与方向：

- 为拒绝 Serde 的位置式对象输入，外部配置先检查 JSON 形态再做强类型解码；1 MiB 启动配置内
  可以接受，但不是最低解析成本。
- Fred 的 URL、TLS、Sentinel 与 reconnect 适配集中在私有驱动，维护复杂度仍高于 Go；不能为了
  表面简洁把 Fred 类型暴露到领域 API。
- 当前没有 Loom 模型测试和 allocator-counted Criterion 基线。只在并发协议进一步复杂或引入
  `unsafe` 时增加相应工具，避免为测试而扩大生产复杂度。

### C++23

优点：

- `std::expected`、RAII、`std::jthread`/stop token、compile-time Schema 展开和拥有型快照构成窄
  公共 API；Boost.Redis、OpenSSL、yyjson、SQLite 都不泄漏到业务头文件。
- 一个编译核心同时服务 C++23、C ABI、C++11/14/17 Legacy 和 C#，没有多套状态机。
- 私有 CA Sentinel TLS 在 Windows/MSVC 与 Linux/GCC 均验证固定身份并拒绝错误身份。
- Driver 的 reactor 运行期现在独立共享；即使最后一个引用在 I/O handler 内释放，析构也不会
  让仍在执行的 `io_context::run()` 引用已销毁 implementation。
- 超时连接在取消前从池中退役；模糊写入不会污染下一次命令。
- GCC Debug、GCC ASan/UBSan、MSVC Debug、MSVC shared Release、clang-format、clang-tidy 和
  C++11/14/17 consumer 均通过。

缺点与方向：

- 同步公共命令会经过 post、promise/future 和连接池；正确性清晰，但还没有独立的 update、renew、
  One/Any、Catalog decode 基准来量化相对 Go/Rust 的成本。
- 当前直接 C++ TLS smoke 覆盖所有领域和错误身份，完整两次切主则由使用同一核心的 C# peer
  覆盖。正式发布前仍应增加直接 C++23 常驻 peer，消除包装层可能掩盖的差异。
- CMake 尚无正式 install/export/version package 与下游 `find_package` 消费矩阵；Windows 运行时
  DLL 自动复制只解决本地运行，不等于发布打包。
- Linux Clang 编译、原生 fuzz、mTLS 与本轮源码长测仍未完成。macOS 是明确不支持的平台，不列为
  待补承诺。

### C ABI 与 Legacy

优点：

- C ABI 只暴露固定宽度结构、字节视图、opaque handle、稳定字符串错误与明确释放函数。
- C++11/14/17 头部包装提供 RAII 和类型化 API，但不复制 C++23 runtime。
- capability 查询是无分配字符串入口，可让 C#/Legacy 检测加法模块而不猜导出符号集合。

缺点与方向：

- 需要自动导出符号快照、结构大小/对齐、旧头+新库和新头+旧库兼容门；现有单元覆盖还不是完整
  ABI 发布系统。
- 同步 C ABI 适合当前核心，但未来真异步必须设计完成回调/取消/所有权协议，不能在上层用线程池
  假装异步。

### C#

优点：

- `LibraryImport`、SafeHandle、`ref struct` Candidate、强类型泛型和显式 Result 隐藏全部 C 布局。
- Fields 在一个连续数组内拥有 UTF-8 名称和值；本轮删除逐字段 class、名称数组、值数组和 HashSet，
  排序后相邻检查重复并只复制一次最终载荷。
- 同步 Key store 直接固定借用的 `ReadOnlySpan<byte>`；原生函数返回前完成参数复制，避免每次整值
  `ToArray()`。空值的 null 指针/零长度路径有真实 Redis 回归。
- 独立 harness 明确选择宿主原生库，并验证 net8/net10、Windows/Linux、自包含发布、显式路径和
  应用目录两种加载方式。
- Windows 与 Linux 的 net8/net10 peer 均通过私有 CA TLS、错误身份拒绝和两次切主，generation
  均为 `1 -> 2 -> 3`。

缺点与方向：

- C# 仍是同步绑定；不提供会占用线程池伪装的 `Task.Run` API。高并发服务需要等待原生真异步 ABI。
- NuGet RID 资产、符号、许可证、NativeAOT、trimming、single-file、ARM64 和签名发布未形成自动门。
- Selector 候选首次访问会穿越 ABI 并构造托管 Attr/Data；同一 callback 内缓存，但尚无 BenchmarkDotNet
  分配/延迟基线。
- 没有 C# 与 Go/Rust 的直接 peer、mTLS、取消风暴或本轮源码长测。

## 回归结果

### 静态、单元与工具链

- Go：`gofmt` 无输出；`go vet ./...`、`go test ./...`、WSL/Linux `go test -race ./...` 通过。
- Rust：`cargo fmt --check`、全目标/全特性 Clippy `-D warnings`、warning-denied rustdoc、当前工具链
  测试和 `cargo +1.85.0 check --all-features` 通过；库单元 73/73，外部离线用例 6/6。
- C++：GCC Debug、GCC ASan/UBSan、MSVC Debug、MSVC shared Release 均接受 9/9 CTest；其中
  6 个离线测试通过，3 个需要 Redis URL 的用例按契约跳过。clang-format 和项目 clang-tidy 通过。
- C#：格式/分析器门通过；net8.0/net10.0 Release 均为 0 warning、0 error；Python harness 语法通过。
- 工作树 `git diff --check` 通过。

### 真实 Redis 8.8

- 完整 Standalone 矩阵通过 Lua Registration/Catalog、Go、Go race、Rust 所有 live 领域、根 Redis API
  和 Go/Rust Registration/Catalog 互操作。Redis 处理 4,228 条命令。
- C# 独立 Standalone 矩阵通过 11 个 build/analyzer/offline/self-contained/live suite，覆盖 net8/net10、
  两种原生加载路径、ACL、空值、强类型边界、并发 Dispose、Registration/Selector 和 Catalog。
- C++ Windows 与 Linux shared Release TLS smoke 均通过 Root、Registration、Selector、Catalog、
  checkpoint、错误身份拒绝和零 key 清理。
- C# Windows 与 Linux net8/net10 TLS 两次切主均通过，包含确认写丢失、脚本缓存清空、全部 Sentinel
  不可用、主库丢失、恢复和第二次切主。
- Go/Rust Linux TLS 两次切主通过，两个 UUID 保持不变，两个 Selector generation 均为
  `1 -> 2 -> 3`，并完成跨语言收敛。
- Catalog Go/Rust Sentinel 独立矩阵通过两次切主、脚本清空、最终 revision 10、Delete 和零 key。

结构化证据：

- `testkit/results/full-project-regression-20260901.json`
- `testkit/results/csharp-standalone-full-review-20260901.json`
- `testkit/results/cpp-sentinel-tls-windows-full-review-20260901.json`
- `testkit/results/cpp-sentinel-tls-linux-full-review-20260901.json`
- `testkit/results/csharp-sentinel-tls-windows-full-review-20260901.json`
- `testkit/results/csharp-sentinel-tls-linux-full-review-20260901.json`
- `testkit/results/sentinel-tls-go-rust-linux-final-review-20260901.json`
- `testkit/results/catalog-sentinel-final-review-20260901.json`
- `testkit/results/optimization-regression-20260901.json`

### Go Linux 基准（3 次）

| 基准 | 当前范围 | 分配 |
| --- | ---: | ---: |
| Registration event decode | 801.7..806.6 ns | 896 B / 12 alloc |
| Registration construction | 138.3..139.9 ns | 240 B / 3 alloc |
| 32 次 pending 合并 | 5.447..5.462 us | 112 B / 1 alloc |
| 单次 pending drain | 144.0..144.7 ns | 112 B / 1 alloc |
| 默认最大 Registration 校验 | 1.021..1.052 us | 0 B / 0 alloc |
| 发布 500 条 Selector view | 47.056..47.914 us | 31,528 B / 6 alloc |
| RedisClock upper-now | 21.76..21.88 ns | 0 B / 0 alloc |
| 应用 Registration Update | 1.168..1.227 us | 2,888 B / 5 alloc |
| 应用 Registration Renew | 1.116..1.141 us | 2,888 B / 5 alloc |
| 无 repair 的 pending 判定 | 18.45..18.69 ns | 0 B / 0 alloc |
| typed Selector One / 500 | 11.641..11.774 us | 2,225 B / 28 alloc |
| typed Selector Any(8) / 500 | 12.593..12.693 us | 8,065 B / 97 alloc |
| Catalog 256-field array 校验 | 5.881..5.977 us | 9,472 B / 1 alloc |
| Catalog Replace event decode | 72.240..74.186 us | 91,486 B / 8 alloc |

这些是当前机器上的绝对基线，不与不同源码、不同操作系统或不同编译器结果直接比较。本轮 Go
没有发现新的 profile 证据支持修改；因此保留当前实现，并把 Selector/Catalog 分配列为可验证的
后续优化目标。

## 发布判断与剩余门禁

当前工作树适合继续 Maintainer code review，并可在明确 `0.1.0 Alpha`、不承诺生产稳定性的前提下
用于分布式开发和受控服务接入。正式进入可发布源码前仍应先由 Maintainer 审核本轮未提交差异。

正式 `1.0.0` 至少仍需要：

1. 把临时中文生产注释统一为标准英文并重跑全部门禁；
2. 对最终源码重新做长测，不能继承旧冻结指纹；
3. 补齐 mTLS、C++ 直接两次 Sentinel 主库切换、正式原生/NuGet 包消费和 ABI 兼容自动门；
4. 对承诺的平台/架构完成发布矩阵。Redis Cluster、generic Campaign/Leader 与 macOS
   仍是明确不支持，不应写成隐含承诺。

本轮没有创建或修改 Git commit，也没有 push。
