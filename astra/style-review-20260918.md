# C++ 规范整理及静态审阅

日期: 2026-09-18.

## 范围与验证边界

本轮整理 `astra/common/`, `astra/star/`, `astra/pulsar/` 下的全部 68 个手写 C++ 文件, 包含头文件, 模板, 生产实现及测试. 16 个生成文件按修改前 SHA-256 核对, 内容保持不变. 第三方代码未修改.

`planet/src/upstream.cpp`, `planet/src/upstream.hpp`, `bench/store.cpp` 作为共用类型的消费者, 只同步类型名称. 对这三个文件进行了重命名归一后的 token 比较, 未引入其他代码变更. 本轮未推进 Planet 功能.

依据为根目录 [C++ 编码规范](../cpp-coding.md). 已同步 [贡献指南](CONTRIBUTING.md), [Pulsar 设计](pulsar/design.md) 和 [纪元时钟设计](pulsar/epoch-clock-design.md) 的当前类型名称. 既往审计和测试记录保留当时名称.

本轮仅完成源码格式化, 引用检索, 差异审阅及静态检查. **没有运行构建, 单元测试, 回归, Sanitizer 或性能测试, 也没有执行 Linux 同步或下载依赖.** 依照 [AGENTS.md](../AGENTS.md), 本轮修改需要得到新的测试授权后才能验证. 历史测试结果不作为当前改动通过的证据.

## 类型命名与作用域

直接修改实际定义与调用点, 不保留旧名称别名.

| 原名称 | 当前名称 | 职责或归属 |
| --- | --- | --- |
| `StarTopology` | `Star` | Star 角色的拓扑策略 |
| `PulsarServer` | `Server` | Pulsar 的监听和资源组合 |
| `PulsarAuthority` | `Authority` | 账号校验与身份签发 |
| `MembershipLedger` | `Ledger` | 持久成员及启动请求账本 |
| `EpochClock` | `Clock` | 单调不减的业务 Unix 纳秒时钟 |
| 原 `Clock` 别名 | `Steady` | `std::chrono::steady_clock`, 用于本地调度和历史年龄 |
| `PhysicalClock` | `Source` | Pulsar 的物理参考采样与连续走时 |
| `PulseClient` | `Sampler` | Star 的 gRPC 对时采样线程 |
| `ClockFilter` | `Filter` | 单批四时间戳过滤 |
| `RpcSession` | `Session` | 逻辑会话状态与回调交接 |
| `AcceptedSession` / `ClientSession` | `Inbound` / `Outbound` | 入站与出站传输适配 |
| `RejectedSession` | `Rejection` | 未接纳流的立即结束路径 |
| `SnapshotIndex` | `Index` | Store 的共享分页快照索引 |
| `MemberEpoch` | `Member::Epoch` | 明确属于成员的远端实例代次 |
| `SessionGeneration` | `Generation` | 策略与会话共用的本地流代次 |
| `Policy::NetworkStatus` | `Policy::State` | 网络策略的诊断快照, 避免与错误 `Status` 混淆 |
| `EpochClock::DeadlineError` / `UnixEpoch` | `Clock::Error` / `Clock::Origin` | 期限构造错误与 chrono 坐标标签 |
| `Admission::RegistrationCall` | `Admission::Call` | 单次登记请求的私有异步状态 |
| `RpcSession::PendingPacket` | `Session::Pending` | 私有待发送项 |
| `PulseReactor` / `ExpiryTarget` | 匿名 namespace 的 `Reactor` / 私有 `Reactor::Expiry` | 流处理器及其独立超时寿命状态 |

测试夹具也进行了同类收敛: `Recorder`, `Manual`, `Concurrent`, `Pause`, `Watch`, `Failure`, `Pipe`, `Limit`, `Idle`. 翻译单元内的测试辅助函数和类型进入匿名 namespace; `main` 和标准要求位于全局的替换型 `operator new/delete` 仍留在全局.

原 `system_time_sample()` 改为 `Source::sample()`, 由实际拥有该职责的类提供静态入口. 采样实现和默认 Provider 行为不变.

生成消息的名称, gRPC 的固定虚函数名称, 协议字段和签名域未重命名. `Config`, `Id` 等既有通用术语继续使用. 文件路径暂时保留, 避免把类型整理扩展成构建目录迁移. 这是手写 C++ 源码接口调整, 仓库内引用已同步; 不承诺仓库外代码无需修改.

## 注释与排版

- 补充函数契约, 参数与变量用途, 时间单位, 默认值与初始状态, 锁边界, 资源所有权及异常提交顺序. 测试中的线程交接, 分配注入, RPC 截止和资源清理也纳入整理.
- 手写注释使用中文及英文标点. 修正旧注释中已经失效的 `max` 无限期哨兵说明, 时钟参考系描述和 BoringSSL 依赖说明.
- 函数头及普通表达式取消固定列宽换行; 保留函数体和 lambda 体的结构. 长构造 lambda 的工作循环抽入 `Concurrent::run`, 保持原捕获所有权及交付顺序.
- 多阶段函数开头及阶段之间保留空行. `.clang-format` 保留块首空行, 不重排注释文字; `.editorconfig` 对 Astra C++ 取消旧列宽限制.
- 8 处手写枚举均逐项检查: 首项从下一行开始, 每项有对应中文说明, 结束括号单独起行.

注释不替代类型约束和代码校验, 静态扫描也不等同于编译器或并发检测器验证.

## 已实施的修正与精简

### Star 初始化的异常提交边界

旧逻辑先替换 `members_`, 再复制 `local` 到 `local_`. 后一步复制成员字符串时可能抛出 `bad_alloc`, 留下成员表已经替换但本地身份尚未完整安装的状态.

当前先在临时对象中准备本地身份, 并用 `static_assert` 约束其移动赋值不抛异常. 取得状态锁后, 通过 Map 交换和已准备身份的移动完成提交. 不再在替换成员表之后执行本地身份字符串分配.

### Star 名单校验

- 前一 ID 改为调用期间的 `string_view`, 避免每项都复制 ID 字符串.
- 远端主体在构建临时 Map 时直接检查插入结果, 删除另一份主体数组及其排序查重.
- 本地主体重复仍通过完整身份比较或严格 ID 顺序拒绝; 地址的独立查重保持原语义.
- 在 `core_test.cpp` 新增本地主体重复和远端主体重复两个场景, 均核对拒绝后状态为空以及后续合法初始化可成功. 用例已编写, 尚未执行.

这里减少的是静态可识别的复制, 分配及重复排序, 没有提供未经测量的加速比例. 本地身份分配失败的精确注入仍应作为后续验证补充, 新增的重复主体用例不能替代它.

### 字符串返回路径

`json_string()` 和 `Config::help()` 先在已有 `result` 中追加末尾文本, 再返回结果, 消除左值 `operator+` 产生的整段额外复制. 输出字面量和拼接顺序保持一致.

## 继续优化前需要证据的部分

| 位置 | 静态观察 | 下一步依据 |
| --- | --- | --- |
| `Store::extract` | 预算检查和 Delta/Key 复制仍占用状态锁, 大批历史可能影响写入尾延迟 | 测量历史量, 键长和并发写入; 若确为热点, 再评估固定不可变批次寿命后在锁外复制 |
| `Store::tick` | 按用户确认语义完整追平时间, 大跨度包含逐拍处理空拍的成本 | 测量挂起恢复与不同过期密度; 优化必须证明等价, 不恢复截断追赶的 `max_ticks` |
| `Index::View` 遍历 | 叶页固定检查 64 个槽, 稀疏时存在空项扫描 | 比较稀疏与密集分布后再考虑位图遍历, 不能只以少循环次数判断收益 |
| Runtime 旧会话取消 | 会话遍历和按代次查找仍可能重复扫描 | 测量实际成员规模和替换频率, 再决定额外索引是否值得维护 |

当前快照已经在短状态锁内捕获共享视图, 随后在锁外复制 Map/Key; 本轮没有再引入持久化容器, 新缓存层或无锁状态机. 未为少量 padding 使用 packing, 也未降低密码派生或身份校验的既有强度.

## 同类问题的扩散检查

- C++ Planet 的 `initialize()` 仍存在先替换 `candidates_` 再复制 `local_` 的相似异常窗口, 位于 `planet/src/upstream.cpp`. 本轮只同步该冻结组件的共用类型名称, 没有修改其逻辑; 后续恢复 Planet 工作时应优先处理并增加分配失败用例.
- 旧 Rust Star 的对应初始化先构造完整临时表, 最后移动安装并设置初始化标志; 未发现同样的提交后身份字符串复制步骤. 这不表示旧实现已完成全量复审, 也不把 Rust 的 OOM 行为等同于 C++ 异常回滚.
- JSON 返回路径的同形字符串复制在当前三个目录中复查, 同时处理了配置帮助返回路径.
- Clock/Steady 改名保持业务绝对期限, 本地经过时间和历史保留时间的原有强类型边界; 不改变 500 ms 质量门槛, 对时调速或 TTL 规则.

## 已完成的静态核对与建议测试

已核对源码格式, `git diff --check`, 8 处枚举, 生成文件哈希, 旧类名引用和源码 token 差异. 原有字符串字面量未被注释规范化误改. 字面量差异仅涉及新增用例的 Galaxy 值及重复主体诊断在代码中的位置变化.

获得本轮授权后, 建议执行:

1. 完整 Astra 构建, 覆盖 Star, Pulsar, 共同库及冻结 Planet 的接口适配; 启用基准目标时另外编译 `bench/store.cpp` 的类型引用.
2. CTest 回归, 重点包括核心拓扑, 配置反射, Protobuf 代理, 会话并发交接, Store 故障注入, Wheel, 时钟和 Pulsar RPC/重启.
3. ASan/UBSan 与 TSan, 覆盖测试辅助函数作用域调整和 `Concurrent::run` 的线程寿命, 以及既有快照与取消场景.

本轮没有形成需要两小时或持续压力测试才能判断的性能结论. 待先取得构建与回归结果, 再按具体热点决定基准范围.
