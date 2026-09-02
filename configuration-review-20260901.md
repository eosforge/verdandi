# Verdandi 配置层归一化审计

日期：2026-09-01
范围：共享 v1 JSON、Go、Rust、C++23、C ABI 与 C# 配置边界
状态：通过离线回归、真实 Redis 8.8 Sentinel TLS 与两次主库切换回归；工作区未提交、未推送

## 结论

当前配置层已经形成一套可维护的跨语言结构：

1. `configuration.schema.json` 只描述可由 JSON Schema 准确表达的结构约束；
2. `testkit/conformance/v1/configuration.json` 固定跨语言实际接受集合和稳定错误；
3. 各语言公开配置使用各自惯用类型，但外部 JSON 的默认值、范围、关系、错误字段和 Redis 行为保持一致；
4. C# 不复制一套 DTO 和校验器，而是通过 C ABI 复用 C++23 的同一解析核心；
5. 根 Redis 物理重连统一为固定 `delay_ms`，Selector/Catalog 的业务恢复仍保留指数退避；
6. 所有命令级重试保持关闭，失败是否重试仍由调用方显式决定。

这套实现适合作为 `0.1.0` Alpha 的配置层。真实 Redis 8.8 私有 CA
Sentinel TLS 已在 Windows x64 与 Linux x64 覆盖 Go/Rust 两次主库切换、C++23
全领域集成，以及 C# net8.0/net10.0 通过同一 C++ 核心的两次主库切换。它尚不
等于 `1.0.0` 的生产资格：自动打包、真实双向 TLS、TLS 长测和平台证书存储
集成仍未完成。macOS 明确不在支持范围内，也不是待补门禁。

## 归一化结构

| 层次 | 唯一职责 | 归一化方式 |
| --- | --- | --- |
| JSON Schema | 字段、对象、基础类型和可表达范围 | 结构伴随文件，不声称是完整执行规范 |
| 共享语料 | 实际接受/拒绝集合及稳定 `code/field` | 41 个语义文档与 6 个原始输入，共 47 项 |
| Go JSON | 严格读取并转换为 Go 原生配置 | 原生 `Standalone`/`Sentinel`、`time.Duration`、`tls.Config` |
| Rust JSON | 严格读取并转换为 Rust 原生配置 | 原生 `Duration`/`PathBuf`，Fred URL 仅留在驱动边界 |
| C++23 JSON | 严格读取并直接构造原生配置 | 编译期字段绑定、`chrono`、`filesystem::path` |
| C ABI | 提供稳定的离线校验入口 | 不暴露 C++ 对象布局，不创建 Client |
| C# | 严格 UTF-16/容量预检后调用 C ABI | 不维护重复 DTO，`Client.Open` 与 `Configuration.Validate` 共用编码路径 |

## 已修正的问题

- 删除根 Redis 重连中各驱动无法严格对应的指数、上限、倍率和抖动参数，改成默认 100ms、范围 10..30000ms 的固定等待。
- 保留 Selector/Catalog 的业务恢复退避，避免把“恢复本地权威视图”和“重新建立物理连接”混成同一个概念。
- 统一端点为精确 `host:port`：端口只接受 ASCII 十进制数字 1..65535，拒绝 `+6379`、边界空白、NUL、空 Host 和裸 IPv6。
- 统一 UTF-8、Unicode 边界空白、未知字段、重复字段、显式 null、尾随文档、错误对象形态、分数和非规范 `-0` 的处理。
- 统一缺失 `version`、`redis`、`redis.mode`、`redis.addresses` 的稳定错误字段。
- Rust 增加对象形态预检，堵住 Serde 结构体可能按数组位置解码的接受差异；第三方 URL 解析错误不再回显可能包含密码的 endpoint。
- Go 对地址切片、TLS 配置和证书池执行防御性复制，并固定 TLS 1.2 下限、禁止 `InsecureSkipVerify`。
- Rust/C++ TLS 文件采用 1MiB 有界读取；TLS 路径统一要求有效 UTF-8、最多 4096 字节且不得含 NUL。
- C++ 的 null 遍历改为显式工作栈，避免深层恶意输入消耗调用栈；配置字段分派继续使用编译期绑定而不引入动态查表。
- 新增 `verdandi_configuration_validate_json`，使 C/C++11 Legacy/C# 可在不连接 Redis、不读取 TLS 文件的前提下复用同一配置判定。
- C# 在分配 UTF-8 数组前检查无效 UTF-16 和 1MiB 上限；旧 ABI 缺少新增符号时返回明确 `incompatible/native_library`。
- Sentinel TLS 统一要求非空固定 `server_name`；该身份由所有 Sentinel、主节点和副本证书共同包含，不从动态发现地址推导。Go/Rust 拒绝错误身份，C++ 也在真实握手中拒绝错误身份。
- C++ 通过 SSL context 的握手回调把固定 DNS SNI 应用到 Boost.Redis 在 Sentinel 发现或重连时新建的每个 SSL 流；证书验证回调仍检查同一固定身份。
- Rust 复用 rustls 标准 WebPKI 校验器，仅把验证用身份固定为配置值；Fred Sentinel 模式禁用地址派生 SNI，因此部署不得依赖动态地址前的 SNI 虚拟主机路由。
- C ABI 增加字符串能力查询，C++11 Legacy 和 C# 均提供惯用包装；这属于本地运行库特性检测，不是 Redis 协议协商。
- Catalog 本地存储路径校验和 Redis 根配置分离，Registration/Selector/Catalog 的领域参数仍保持独立对象。

## 评分

| 部分 | 评分 | 结论 |
| --- | ---: | --- |
| 共享外部契约 | 9.7/10 | Schema、执行校验和同一语料职责清楚，实际接受集合已跨语言锁定 |
| Go | 9.6/10 | API 最符合 Go 惯例，驱动映射薄，输入所有权和 TLS 安全边界严谨 |
| Rust | 9.4/10 | 类型表达和错误安全优秀，Fred 适配集中，但存在一次结构预检加一次类型解析 |
| C++23 | 9.7/10 | 编译期绑定、严格解析、有界 I/O、真实 Sentinel TLS 和静态/共享/消毒器证据完整 |
| C ABI / Legacy | 9.7/10 | 边界小且稳定，字符串能力查询可检测加法模块，低标准包装不复制运行时 |
| C# | 9.6/10 | 无配置规则漂移、能力查询和 Windows/Linux 双目标 TLS 主库切换均通过；代价是依赖原生库及其正确分发 |
| 配置层整体 | **9.7/10** | 可作为 Alpha 发布配置层，尚不宣称全平台、双向 TLS 或自动打包资格 |

## 各语言优缺点

### Go

优点：

- `Config`、`Standalone`、`Sentinel`、`time.Duration` 和 `tls.Config` 都符合 Go 使用习惯；外部 JSON 只负责转换。
- `Client` 保持为 go-redis 的薄封装，运行时展开值集中于私有 `runtimeConfig` 和 `driver.go`。
- 地址切片、TLS 容器及证书池不会继续引用调用方可变容器。
- 普通命令重试明确关闭，服务端错误和传输错误分类集中。
- 全包测试、vet 和 WSL/Linux race 均通过。

缺点：

- 为得到重复字段、原始数值词法和稳定错误，JSON 加载需要多阶段检查；这增加一次启动期解析成本，但输入上限只有 1MiB。
- `tls.Config` 内部回调、私钥对象等引用无法安全深拷贝，仍要求调用方在 Client 生命周期内保持不可变。
- 原生配置与外部 DTO 是两套类型，维护时必须继续依靠共享语料防止转换漂移。

### Rust

优点：

- `Duration`、`PathBuf`、`Option` 和 RAII 保留了 Rust 的类型优势。
- Fred URL、Sentinel IPv6 适配、TLS 连接器和重连策略都收敛在驱动边界。
- 配置诊断不会泄漏包含凭据的原始 URL。
- 固定重连显式清除 Fred 默认抖动，语义与 Go/C++ 一致。
- 当前工具链、严格 Clippy 和声明的 Rust 1.85 最低工具链均通过。

缺点：

- 为拒绝 Serde 的位置式结构输入，先读 `Value` 做形态检查、再做强类型解码，启动期会解析两次。
- 语言原生根配置仍以 Fred URL 表示拓扑，与共享 JSON 的 `mode/addresses` 外形不同；这是驱动适配成本，不应泄漏到外部配置。
- Fred 无法把固定 SNI 传播到动态发现节点；当前实现关闭 Sentinel 的地址派生 SNI，但仍以固定身份执行完整 WebPKI 验证。Redis/Sentinel 直连成立，依赖 SNI 路由的代理部署不在承诺内。

### C++23

优点：

- 字段名和解析闭包由非类型模板参数及折叠表达式展开，已知字段分派没有堆分配表。
- 原生结构直接使用 `chrono`、`filesystem` 和明确整数宽度，不保留第二套运行时 DTO。
- JSON、TLS 文件和遍历深度均有明确资源边界；null 检查不依赖递归调用栈。
- 同一核心同时服务 C++23、C ABI、C++11/14/17 Legacy 与 C#。
- Debug 静态、Release 共享、ASan/UBSan、clang-format 和 clang-tidy 全部通过。

缺点：

- 严格 UTF-8、文件系统路径和 JSON 错误归一由项目自己承担，跨平台维护负担高于 Go/Rust。
- OpenSSL context 回调依赖握手开始时机为新 SSL 流注入固定 SNI；Windows/Linux 直接握手和两平台 C# 两次主库切换均已通过，未来 Boost/OpenSSL 升级仍需保留该回归。
- `load_json` 为最多 1MiB 的启动配置保留完整缓冲；这很安全但不是流式解析。

### C ABI / C++11 Legacy

优点：

- 离线校验只传字节视图和固定错误，不暴露 C++ ABI 或配置对象布局。
- 新符号是加法式扩展，旧调用不受影响；共享库当前导出 90 个 `verdandi_*` 符号。
- C++11/14/17 继续链接同一 C++23 核心，不产生第二套配置语义。

缺点：

- 能力名称必须保持稳定且只做加法；较旧 ABI v1 若连查询符号都没有，绑定仍只能把缺失入口报告为不兼容。
- 固定错误缓冲和边界复制是稳定 ABI 的成本。

### C#

优点：

- 不定义重复配置 DTO、默认值和范围，彻底避免托管规则与原生核心漂移。
- `Configuration.Validate` 和 `Client.Open` 使用同一严格编码及 1MiB 限制。
- 无效代理项在托管侧拒绝，不会被默认 UTF-8 替换字符悄悄改变。
- net8.0/net10.0 都以最新共享库通过同一配置语料。

缺点：

- 离线配置校验也要求原生库可加载；部署比纯托管解析器更严格。
- 当前只提供验证和打开能力，没有托管强类型配置编辑器；若以后增加 Builder，应只生成 JSON，不能再复制校验规则。
- NuGet/RID 自动打包仍未实现；当前能力查询能诊断“加载了旧库”，但不能代替正确的原生资产选择和签名发布。
- 托管 `string` 无法表示非法原始 UTF-8，因此这类语料由 C/C++ 边界覆盖，C# 只覆盖非法 UTF-16。

## 回归结果

- Go：`go test ./...`、`go vet ./...`、WSL/Linux `go test -race ./...` 通过。
- Rust：格式检查、`cargo test --all-targets`、全目标全特性严格 Clippy、Rust 1.85 最低工具链测试通过；12 个需要外部 Redis/负载环境的用例保持显式 ignored。
- C++：Debug 静态、Release 共享、ASan/UBSan 三套均为 9 个 CTest 中 6 个离线通过、3 个 Redis 用例按无地址配置正确跳过；格式和 clang-tidy 通过。
- C#：格式检查通过，Release net8.0/net10.0 构建为 0 warning/0 error；两个 Linux self-contained 测试程序均通过最新 `libverdandi_cpp.so`。
- 共享配置语料：Go/Rust/C++ 执行 41 个语义用例加 6 个原始输入，共 47 个；C# 执行全部 41 个语义与 5 个可由 `string` 表示的原始输入，并另测非法 UTF-16，非法原始 UTF-8 由原生边界覆盖。
- 共享 C ABI：Release 库导出 90 个 `verdandi_*` 符号，包含 `verdandi_c_has_capability`；C11、C++11 Legacy 和 C# 已覆盖已知、未知、空值及无效视图。
- Go/Rust Sentinel TLS：Windows x64 与 WSL/Linux x64 均完成私有 CA、证书 SAN 仅含 `verdandi.test`、错误固定身份拒绝、独立 Redis/Sentinel ACL、两次主库切换、确认写丢失修复、`SCRIPT FLUSH`、全部 Sentinel 中断与恢复；两端 Selector generation 均为 `1 -> 2 -> 3`。
- C++23 Sentinel TLS：Windows MSVC shared Release 与 Linux GCC shared Release 均通过 root、Registration、Selector、Catalog 和 checkpoint 集成；错误证书身份被拒绝，结束后 `DBSIZE=0`。直接 C++23 两次主库切换仍是独立待补门禁。
- C# Sentinel TLS：Windows x64 与 Linux x64 的 net8.0/net10.0 均通过同一 C++ shared Release 核心完成两次主库切换，Selector generation 均为 `1 -> 2 -> 3`，最终 `DBSIZE=0`。
- Windows x64：Visual Studio 2026/MSVC 19.51、Windows SDK 26100、CMake 4.4 和 vcpkg OpenSSL 3.6.0 完成静态 Debug 与共享 Release 严格构建；两套 CTest 均为 9/9 接受，C# net8.0/net10.0 直接加载生成的 DLL 后离线及完整 Sentinel TLS 测试通过。

配置归一化证据见 `testkit/results/configuration-normalization-20260901.json`。当前跨平台 TLS 证据为 `sentinel-tls-go-rust-{linux,windows}-20260901.json`、`sentinel-tls-cpp-{linux,windows}-20260901.json` 和 `sentinel-tls-csharp-{linux,windows}-20260901.json`；不带平台后缀的三个文件仅保留为较早的 Linux 历史结果。本轮没有运行新的长测或双向 TLS。

## 发布判断

配置层和固定身份 Sentinel TLS 可以随 `0.1.0` Alpha 提供给分布式开发和服务接入。部署必须保证所有 Sentinel/数据节点证书共享 `server_name`，且不得在 Rust Sentinel 路径依赖 SNI 虚拟主机路由。自动打包按本次决策延期。正式 `1.0.0` 前仍应补齐：真实双向 TLS、自动包与 ABI 检查、TLS 长测、配置模糊测试，以及标准英文注释；这些属于发布资格，不是本次配置契约缺口。macOS 明确不受支持，不属于未来发布承诺。
