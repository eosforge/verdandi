# Verdandi 代码整理、离线优化与工程评估 — 2026-09-05

本轮基于 `alpha` 分支的 `81cb8b9`，遵守“不下载依赖”的要求。实际代码改动集中在
C++ Catalog；Go/Rust 的数组实现用于核对协议语义，C# 通过 C ABI 复用该 C++ 核心。
这是源码审查、局部实测和历史证据结合的评估，不是完整 SDK 或生产环境验收。
本轮修改尚未提交或推送。

## 已修复的问题

C++ `fields` 是按字符串字典序排列的 `std::map`。原来的 Catalog 数组校验却要求遍历
顺序中的下标严格逐次递增，因此 `0, 1, 10, 2, ...` 会在下标 `10` 处失败。离线编译
原始校验函数后，10 项数组通过，11 项和 100 项数组均返回 `contract: array`。

该函数被 Publisher、Subscriber 权威读取/通知处理和检查点校验复用，所以问题同时
影响原生 C++ 发布以及接收其他语言发布的合法数组；C ABI、Legacy 和 C# 也经过同一核心。

修复分别处理两个契约：

- **完整性**：每个字段名必须是 `[0,N)` 内的规范十进制下标。map 的键唯一，且规范表示
  唯一，因此 N 个这样的键必然连续覆盖整个数组，无须依赖遍历顺序或另外分配去重表。
- **写入顺序**：Replace 仍须按 `0, 1, 2, ...` 发送给 Lua。编码器按下标直接填入最终参数
  位置，Map 则继续使用字典序。没有修改 Lua、Redis key、公开 API 或 C ABI 签名。

代码入口是 [`catalog_value.cpp`](sdk/cpp/src/catalog_value.cpp) 和私有声明
[`catalog_value.hpp`](sdk/cpp/src/internal/catalog_value.hpp)。

## 后续复查：通知字段解码与跨语言影响

前一轮只修复值校验和发布参数，遗漏了 C++ Subscriber 的 `event_fields`：它把所有通知
字段都按字符串顺序检查，导致合法的 Array Replace 通知在 `9 → 10` 处被拒绝，并触发
订阅代次恢复。这个遗漏说明，局部校验测试不能替代生产端、消费端及绑定的完整审查。

现已将生产 MessagePack 游标和字段解码提取为私有标准库模块
[`catalog_event_fields.cpp`](sdk/cpp/src/catalog_event_fields.cpp) /
[`catalog_event_fields.hpp`](sdk/cpp/src/internal/catalog_event_fields.hpp)。Array Replace
按连续、无前导零的数值索引校验；Value/Map Replace 及稀疏 Array/Map Patch 仍按字典序
校验。Subscriber 根据通知操作和 kind 选择模式。Lua、公开 API 和 C ABI 签名没有改变。

| 语言或层 | 两处数组排序问题的审查结论 | 源码依据与验证边界 |
| --- | --- | --- |
| C++23 | 原值校验和通知字段解码均受影响，现已修复这两处 | 生产纯函数 Release/Debug 回归通过；完整 Subscriber/Redis 回归未执行 |
| C ABI | 经 `verdandi_catalog_replace` 和 Subscriber 调用共享核心，受影响 | 审查 `src/c/catalog.cpp`；需要重建核心并运行绑定回归 |
| Legacy C++ | 经同一 C ABI 受影响 | 审查 `include/verdandi/legacy/catalog.hpp`；未运行绑定回归 |
| C# | `CatalogPublisher.Replace` 经 P/Invoke 调用同一核心，受影响 | 审查 `CatalogPublisher.cs`、`NativeMethods.cs`；未运行托管回归 |
| Go | 已审查路径未发现这两处同类错误 | `value.go` 按数值位置生成 names，`publisher.go` 保留顺序，`event.go` 区分 Array Replace 和 Patch；本轮未运行 Go |
| Rust | 已审查路径未发现这两处同类错误 | `model.rs` 独立验证规范索引，`publisher.rs` 按索引填 names，`event.rs` 区分顺序；已有 512 项数组单元用例，本轮未运行 Rust |
| Lua | 已审查路径未发现同类错误 | Replace 用 `tostring(index)` 检查连续数值序；Patch 按字典序；权威读取的字段列表不等同于 Replace 通知顺序 |
| Python 测试工具 | 当前传参路径未发现同类重排错误，测试宽度有缺口 | `CatalogScripts.replace` 保留调用者的键值列表顺序；现有基础 Lua 集成用例仅 3 项，C# Array 用例仅 2 项 |
| PowerShell/Bash/CMake | 该缺陷不适用 | 构建编排不执行 Catalog 字段排序；正常与离线 CMake 均已接入新字段解码回归 |

Go 已有 512 项数组校验基准，但基准不会因普通单元测试命令而自动执行。Rust 的 512 项
单元用例也不能替代完整发布/通知/恢复验证。以上是对应路径的源码结论，不是对整种语言
实现“无 BUG”的保证。按维护者要求，每次 BUG 修复的跨语言传播审查已写入 `coding.md`
第 15 节和 `codex.md` 的持久决策。

补充验证：从修复前生产文件提取的游标和字段解码逻辑在新回归中产生 8 处失败，包括
11/12/100/101/1,024/65,536 项合法数组及错误接纳字典序 Array。修复后的同一生产字段
解码模块通过 MSVC `/W4 /WX` Release 和 Debug `/RTC1`；每次执行 66,973 个断言，涵盖
正确/错误顺序、截断、非法索引、字段数边界、二进制所有权及发布参数到通知字段的衔接。
这些断言不代表同等数量的独立场景。离线 CMake Release 的两个 CTest 目标均通过。

此次发现机器上已有 clang-format 22.1.3，已直接使用其对本任务修改的 11 个 C++ 文件
格式化并检查。未下载或安装工具。完整通知信封、Subscriber 生命周期、Redis、Sentinel、
TLS、检查点 I/O、C#/Legacy/C ABI 运行和 Go/Rust 测试仍未重新验证。新证据和当前源码
摘要见 [`catalog-array-propagation-20260905.json`](testkit/results/catalog-array-propagation-20260905.json)。
下方的微基准及其源码摘要属于首轮测量快照，后续格式化与通知修复不追溯改写原始证据。

## 整理和优化

1. 将纯字段校验和 Replace 参数编码移出 Redis 生命周期文件，形成可单独编译的内部模块。
   Subscriber 和检查点继续使用同一个校验入口，避免复制修复逻辑。
2. 成功的完整值/Patch 校验不再构造临时字段名 vector；共享单遍名称、UTF-8 和容量检查。
   容量累计保持在上限以内，再用减法检查剩余空间，避免整数溢出和下溢。
3. Replace 直接构造最终参数，Patch 的 HMGET、容量投影和参数编码复用同一只读 map 的顺序，
   去掉临时名称容器和逐字段重复树查找。版本核对、错误类别、锁与 Redis 原子操作保持原有契约。
4. 增加独立的纯 C++23 测试工程及可选微基准，不加载整个 SDK 或声明外部依赖。正常 SDK
   CTest 也运行同一回归测试源文件。运行方法见 [`BUILD.md`](sdk/cpp/BUILD.md)。

## 局部测量

环境：Windows x64、Intel Xeon E5-2680 v4、MSVC 19.44.35228、`/O2 /W4 /WX`。
7 组 before/after 交替顺序；before 使用从基准提交原样提取的校验函数。两侧逐项检查
计算结果一致，输入构造和 JSON 输出不计入测量。分配数记录实际 `operator new` 请求。

| 校验场景 | 原实现中位数 | 当前中位数 | 耗时下降 | 临时分配次数 |
| --- | ---: | ---: | ---: | ---: |
| Map，32 字段 | 1.111 μs | 0.615 μs | 44.6% | 1 → 0 |
| Map，512 字段 | 17.056 μs | 9.715 μs | 43.0% | 1 → 0 |
| Patch，512 字段 | 13.859 μs | 9.365 μs | 32.4% | 1 → 0 |
| Map，4,096 字段 | 164.054 μs | 90.036 μs | 45.1% | 1 → 0 |
| Map，65,536 字段 | 2.620 ms | 1.556 ms | 40.6% | 1 → 0 |

65,536 字段场景每次少一次约 1 MiB 的临时分配。此结论仅针对成功的校验函数；
字段本身、返回参数字符串、类型解码、网络和数据库仍有各自的内存成本。Windows
微基准是本机局部证据，不能换算为 SDK 吞吐量、Redis 延迟或 Linux 性能提升。

原始 7 组记录和源码摘要保存在
[`optimization-offline-20260905.json`](testkit/results/optimization-offline-20260905.json)。
当前微基准源文件为 [`catalog_value_benchmark.cpp`](sdk/cpp/tests/catalog_value_benchmark.cpp)。

## 验证结果与边界

| 检查 | 本轮结果 |
| --- | --- |
| 原始代码中的 10/11/100 项数组 | 成功复现 11、100 项误拒绝 |
| 新回归的 MSVC Release 和 Debug `/RTC1` | 通过，严格警告作为错误 |
| 独立 CMake Release 构建与 CTest | 通过，不需要外部 SDK 依赖 |
| 数组 0/1/10/11/12/100/101/1,024/65,536 项 | 通过；逐个核对数值顺序及参数 |
| 数组缺口、非规范下标、整数溢出、字段数上界 | 通过 |
| 空值、精确容量、超过容量、4 MiB 边界、UTF-8 | 通过 |
| 二进制字段名/值、零字节、输出所有权 | 通过 |
| `catalog.cpp`、C ABI Catalog 与 Redis 集成测试编译单元 | MSVC `/c /W4 /WX` 通过；不是完整链接或执行 |
| Registration/Catalog Lua 生成一致性 | 通过，脚本未变 |
| 完整原生库、C#/Legacy 运行时回归 | 未执行，现有外部依赖不齐 |
| C++ Subscriber 完整编译 | 未完成，缺少 `openssl/evp.h` |
| Redis、Sentinel、TLS、实际检查点 I/O | 未执行 |
| Go/Rust/.NET SDK 验证、Linux、sanitizers | 未执行，本机工具链/依赖不齐 |
| clang-format、clang-tidy | 首轮均未执行；后续已用现有 clang-format 22.1.3 完成修改文件检查，clang-tidy 仍未执行 |

实机测试已加入 12 项数组 Replace、同时 Patch 下标 `2`/`10`、订阅观察及重开恢复用例，
并通过编译检查；尚未执行，不计入通过的运行时证据。历史实机或 12 小时结果不继承给当前改动。
没有为了通过验证安装工具链、恢复包或连接远端测试主机。

## 评分

**当前已实现 Alpha 范围：8.0/10。** 这是工程判断，采用下表权重，四舍五入到一位小数；
不是覆盖率、可靠性概率或生产就绪率。旧报告的 9.7 分采用不同评审口径，本次不沿用其尺度。
本次无法复跑的部分降低结论置信度，不将本机缺依赖本身判为产品缺陷。

| 维度 | 权重 | 评分 | 依据 |
| --- | ---: | ---: | --- |
| 架构与职责边界 | 20% | 9.0 | 协议和应用解耦，领域生命周期清晰，绑定复用原生核心 |
| 正确性与边界防护 | 25% | 8.0 | 恢复和资源约束扎实，但本轮发现数组十进制位数边界漏测 |
| 性能与资源使用 | 15% | 8.0 | mailbox、局部视图和增量同步合理；有实测，但原生整体基线不足 |
| 可维护性 | 15% | 8.0 | 私有边界清晰；多语言状态机、绑定和大量历史文档增加维护成本 |
| 测试与证据完整性 | 15% | 7.5 | 历史矩阵广；C++ 原生畸形通知和跨语言边界向量仍有缺口 |
| 构建与交付成熟度 | 10% | 6.5 | 源码构建入口已具备；正式包消费、ABI 兼容和 RID 分发尚未完成 |

### 优点

- Registration 的独立写入所有者、合并 mailbox 和明确关闭路径，有利于控制并发和资源生命周期。
- Selector 本地视图与订阅前置、版本修复、PING/PONG 对齐，既保留选择热路径性能，也明确了恢复语义。
- Lua 负责原子状态转换，应用字段语义由 SDK/应用负责；Go、Rust 和 C++ 使用各自的语言机制。
- C ABI、Legacy 和 C# 共用一个运行时，避免重复实现协议。可丢弃检查点也没有冒充权威数据源。
- 有真实故障、互操作、race/fuzz、sanitizer 和历史长测记录，且可以按源码版本区分证据。

### 缺点与取舍

- **跨语言测试覆盖不均衡。** Go/Rust 的 Catalog 行为不能自动证明 C++ 正确；本轮问题说明，
  仅覆盖小数组会遗漏字符串/数值排序差异，应扩展跨语言 Array 边界和直接畸形通知用例。
- **验证与交付尚未闭环。** 当前源码需要完整原生及绑定回归；CMake install/export、ABI
  自动兼容检查、NuGet RID、NativeAOT/trimming 等不能由源码构建成功替代。
- **数据所有权有成本。** 完整 Catalog 内存视图、detached 值和类型解码带来复制及分配。
  当前 Selector 策略和完整快照的 O(N) 成本也需要按真实消费者规模测量。
- **维护面较大。** 独立原生 SDK 必须保持协议一致；历史决策、当前状态与旧评分分散在大量
  文档中，接手者容易混用旧结论。应继续把当前契约与历史证据明确分开。
- **后端能力有明确取舍。** 依赖 Redis 8 单主语义，Sentinel 异步复制可能丢失已确认写入；
  需要应用理解这一契约。Redis Cluster、macOS、通用 Leader 不属于已承诺范围，不按遗漏功能扣分。

后续验证应先运行当前源码的 C++ 数组实机用例及 C ABI/C# 绑定回归，再补通知解析边界和
最终源码的 Redis/Sentinel 测试。依赖恢复仍以维护者后续安排为准。
