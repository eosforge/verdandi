# Star / Planet C++26 骨架设计与迁移计划

> 2026-09-12: 旧 Rust 服务已废弃, 当前仅维护 C++ Star/Planet + Go Supervisor. 协议 v6、单次 Register、Supervisor 签发不透明 id 和持久幂等规则以[身份与准入契约](identity-contract.md)为准. 下文旧 UUID/v4/Rust 对照和 Challenge/expected_epoch 描述保留为设计演进记录, 不再是当前实现要求.

日期: 2026-09-11. 状态: 独立 cluster-cpp/ 连接回归及 TSan 已通过; 其余设计验收按维护者要求暂停.

维护者选择将 Star/Planet 从 Rust 迁移到 C++26, Supervisor 保持 Go, 暂不要求 MSVC 支持.
文档完成后维护者已明确要求开始实现. Rust/Tonic 保留为对照, 其测试成绩不能转记为 C++ 成绩.
第一阶段完成连接与准入骨架, 不同时推进 Catalog/Registry 业务复制、存储引擎或 SDK 迁移.

本文拥有 C++26 目标结构与迁移门槛. 当前线上含义、字段编号和错误码仍以
[gRPC v4](grpc-implementation.md)、[连接规则](connection-rules.md) 和 `proto/` 为准.
逻辑会话、流控参数和解码检查采用本次已确认的简化, 详见第 13.1 节; 它们取代旧实现的对应迁移要求.
其他差异仍须记录并解决, 不能以“交给 gRPC”代替身份正确性、资源测量和生命周期验证.

## 1. 目标、依据与边界

### 1.1 已确定的方向

| 项目 | 目标 |
| --- | --- |
| Star / Planet | C++26, 首个构建与运行验证平台为 Linux x64 / GCC 16.2.0 |
| Supervisor | 保持现有 Go gRPC 服务、账号配置与 bbolt 成员表 |
| 网络协议 | 保持 v4, `Admission.Challenge/Register` 与 `StarTransport.OpenSession` |
| 目录职责 | 在 `cluster-cpp/common`, `cluster-cpp/star`, `cluster-cpp/planet` 保持责任划分 |
| 产物 | Star 为 `star`, Planet 为 `planet`, 第一阶段不产出 Windows 二进制 |
| 设计目标 | 减少重复声明、包装层、分配与拷贝, 同时保持明确的资源和并发所有权 |
| 实施授权 | 已允许新建目录实施; 所列 C++ 依赖另已明确获准在 Ubuntu 项目内获取和构建 |

不把 MSVC 的新特性支持缺口等同于整个编译器不稳定. 此次选 GCC 是功能与维护者偏好的选择.
暂不承诺 MinGW、Clang、跨平台 C++26 二进制 ABI, 不因换语言改变现有 C++23 SDK / C ABI.

### 1.2 第一阶段交付范围

完成配置、身份加载、新进程 UUID、Supervisor 登录与幂等登记、TLS/gRPC 会话、Hello 验证、
Ping/Pong、Star 两方向逻辑会话 mesh、Planet 候选与单上游切换、状态查询、日志、信号退出和一键测试.
保留当前业务未就绪语义: `initialized` 只表示完成准入, `upstream` 只表示认证连接已建立.

以下不进入第一阶段实现: 数据落盘/恢复, 全量与增量业务同步, Publisher/Registry 转发,
Planet 换绑时业务重新登记, SDK Bind, 分布式持久 ACK, Supervisor HA, 在线吊销, Admin 3D API 与虫洞.
后续 Star/Planet 仍共用状态合并与存储规则; 现在不创建空的 Store、Repository 或 SyncEngine 接口.

### 1.3 工具链实测依据

2026-09-11 对相同源码做了 15 项功能探测及一项宏采集, GCC 16.2.0 通过 11 项功能,
MSVC 19.51.36257 通过 4 项. 这是新特性取样, 不是语言评分或完整符合性认证.
GCC 来自官方发布源与校验通过的 GMP/MPFR/MPC, 安装在 Ubuntu 项目 `build/tools/gcc-16.2.0`.
未替换系统 GCC; 完整 GCC 自测与 bootstrap 未执行.

| 特性 | GCC 16.2 实测 | 本阶段处置 |
| --- | --- | --- |
| 静态反射、反射注解 | 基础用例通过, 需要 `-freflection` | 用于私有配置描述与有限枚举映射, 扩展用例仍需测试 |
| `template for`、包索引 | 通过 | 优先使用展开语句; 仅有真实异构索引需求时使用包索引 |
| C++26 契约 | 正例和前置条件违约终止通过 | 只用于内部不变量, 显式启用 enforce |
| `inplace_vector` | 插入与满容量行为通过 | Planet 最多八项的候选容器 |
| 编译期异常 | 通过 | 可用于复杂元数据的编译期诊断, 简单检查优先 static_assert |
| `flat_map`, `jthread`, `expected`, ranges | 选定用例通过 | 可用, 注意它们不是全部来自 C++26 |
| `hive`, constexpr `unordered_map`, inplace stop token, 标准 sender/receiver | 未通过 | 不进入基础依赖, 不自行仿制标准库补齐 |
| 显式 STL 越界检查 | 正例与越界终止通过 | 作为独立构建配置及负例测试 |

原始本机证据在 [探测报告](../build/cpp26-support-20260911/README.md) 与其 results.json;
这些是忽略的本地产物, 新克隆应重新生成, 不把链接存在作为通过证明.
GCC 官方也明确列出反射与其他 C++26 功能及所需选项.
[GCC 16 变更](https://gcc.gnu.org/gcc-16/changes.html), [GCC C++ 状态](https://gcc.gnu.org/projects/cxx-status.html).

## 2. C++26 应如何减少复杂度

### 2.1 配置只维护一份字段声明

当前 Rust CLI 分别维护合法选项集合、分派、默认值装配与帮助文字. C++ 目标是在私有
`NativeOptions` 字段上描述显式 CLI 名、单位、上下限、是否必填和帮助说明, 默认值留在字段初始化器.
用反射和 `template for` 得到解析分派、单字段检查与帮助表; 不再维护平行字段名单.

采用很小的封闭元数据集合, 仅支持当前需要的整数、时长、字符串、地址与路径类型:

| 信息 | 唯一所有者 | 自动得到的内容 |
| --- | --- | --- |
| `--pong-timeout-ms` 等外部名称 | 字段注解中的显式名称 | 选项查找与帮助, 重命名 C++ 成员不会改变 CLI |
| 默认值 | 配置字段初始化器 | 实例默认值与帮助中的默认值 |
| 数值范围与单位 | 字段注解 | 单字段边界检查和说明 |
| 必填与重复输入 | 描述表与每次解析的 seen 位集 | 缺失/重复错误, 不用字符串集合重复存储 |
| 跨字段关系 | `validate()` 中的普通命名检查 | 通配监听需 advertise、退避下限不超过上限等 |
| 敏感信息 | Identity 私有字段与显式输出白名单 | 不自动展开密码、私钥、完整凭证; 不增加无行为的 Secret 包装 |

编译期拒绝重复 CLI 名、缺失描述、非法范围、缺少解析器的字段类型. 默认值也必须经过同一规则检查.
运行期仍处理未知选项、重复选项、数字溢出和格式错误, 以结构化错误返回.
配置解析先完成再读取身份文件或联网, 防止先产生副作用后报参数错误.

不为两种角色创建不同的解析器; role 由二进制入口给定, 不能通过 `--role` 提升权限.
`--name=value` 与当前已接受的 `--name value` 都保留. `--identity` 省略时仍使用 `identity`.

反射局限在私有头文件和少量编译单元. 不生成第二套业务 schema, 不为每个连接重新构造字段表,
不把未经审核的对象自动转成日志或 JSON. login.json 的严格解析使用独立的小型边界.
现有 SDK 的配置/codec 规则保持不变; 此处不是重新引入 SDK 级配置 DSL 或业务类型生成器.

### 2.2 枚举、元编程与类型边界

对内部阶段和错误类别可从枚举反射生成诊断名称与覆盖检查. 协议错误码和 Protobuf oneof
使用生成的枚举与访问器, 不根据 C++ 枚举声明顺序分配线上编号.
有意稳定的外部日志值显式标注, 不随成员重命名变化. 未知协议枚举仍按协议拒绝.

使用 concepts 约束确有多个类型的解析/描述操作, 用 `template for` 展开异构字段.
优先普通函数、具体类型和清楚的 switch; 不为“使用包索引”创造 tuple 中转层.
不引入 CRTP 角色框架、递归继承访问器、全局类型注册中心或通用反射 RPC 总线.

`Id`, `Principal`, `SessionGeneration` 与 `MemberEpoch` 分离, 防止整数和字符串混用.
这些类型只提供验证/比较等必要行为, 不为每个标量套一层转发类.
类型只在输入验证后构造. bytes/string 的拥有和借用区别通过值、`span`、`string_view` 及接口注释表达.

### 2.3 容器按访问模式选择

| 场景 | 首选 | 理由与限制 |
| --- | --- | --- |
| Planet 候选 | `std::inplace_vector<Candidate, 8>` | 避免容器自身的独立堆分配; Candidate 内部字符串仍可能分配 |
| 控制发送的四个待发送槽 | `std::array<std::optional<PendingPacket>, 4>` 与环形下标 | 容量固定、O(1) 入出队, 不用 vector 头删和移位 |
| 单个待匹配 Ping | `std::optional<PendingPing>` | 类型直接表达最多一个, 不维护 request_id 到 timer 的 map |
| 按 principal 寻址的 Star 索引 | `std::map<Principal, Entry>` | 当前索引只在建流、重试和状态快照使用, 有序遍历使拨号顺序确定; 后续业务热路径另测 |
| 排序后的名单和状态输出 | 连续 vector, 按需快照排序 | 排序是低频路径, 不为每次 Ping 复制/排序全表 |
| 少量、读多写少的静态描述 | constexpr array; 必要时 flat_map | 不把 flat_map 机械替换全部 map, 插入会移动元素 |

Map 中不向异步操作借出会失效的迭代器. Session 需要稳定地址时独立拥有对象;
成员替换使用不可变的成员值/受控句柄, 更新完成才发布.
优先把候选中的标识转成定长值以进一步减少分配, 但原始凭证字节必须独立保留供验签.

`try_push_back` 的较新实现返回 `optional<T&>`, 不应继续按旧指针返回值与 nullptr 比较.
容量失败应返回错误而非依靠异常正常控制流程.
[返回值变更说明 P3981R1](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p3981r1.html).

### 2.4 契约和硬化的位置

契约表达已经由内部算法保证的条件, 如槽位计数合法、关闭后不再发布新会话.
网络输入、签名、未知枚举、超限和用户配置必须先做普通条件检查并返回错误.
禁止直接把这些外部检查改成会终止服务的 `pre` 或 `contract_assert`.

契约表达式必须无副作用, 在必要的锁内读取稳定状态, 不分配、不联网、不记日志.
不在每条 Ping 上扫描整个拓扑作为后置条件. 全表一致性检查放入单测/诊断门槛.
启用或禁用诊断配置都不能改变成功状态转移、授权判断或返回值.

首阶段项目代码启用 `-fcontracts -fcontract-evaluation-semantic=enforce` 与 `_GLIBCXX_ASSERTIONS=1`.
第三方目标不被全局注入项目契约编译选项. 不使用 assume 语义, 不将 `_GLIBCXX_DEBUG` 混入不匹配的依赖 ABI.
负例独立进程运行, 禁止 core dump, 报告预期终止, 不弹错误窗口或把它记成回归失败.

### 2.5 暂不使用的特性

标准 sender/receiver 尚未通过本机测试; 不为它自建执行器生态.
gRPC 回调能够表达本阶段工作, 因此先不引入协程库、手写 task/promise、Asio 适配层或线程一连接模型.
C++ modules / `import std` 不进入首阶段, 避免同时增加生成头、依赖构建与分析工具兼容变量.
编译期异常仅在诊断确实更清楚时使用. constexpr 容器不是运行期哈希表加速开关.

## 3. 目标目录与依赖方向

以下为首版实际源码布局. 验证是否完成以 `cluster-cpp/validation.md` 的证据为准.

```text
cluster-cpp/
    CMakeLists.txt                 # 服务构建根, 不移到仓库根
    .clang-format
    build.py / build.sh / build.ps1
    prepare_dependencies.py        # 独立显式准备, 普通构建不获取依赖
    dependencies.lock.json / licenses/
    test_processes.py             # 真实 RPC 与 Rust/C++ 混合进程测试
    common/
        include/verdandi/cluster/     # 两个角色实际共用的少量声明
        src/
            config.cpp / options.hpp
            identity.cpp / admission.cpp
            grpc_session.cpp
            runtime.cpp           # 生命周期协调和控制循环
            process.cpp / process.hpp # 私有信号、唤醒与有限结构化日志
            rpc_status.hpp        # Supervisor 和会话共用的状态码分类
            generated/            # 显式生成的 .pb.* 与 .grpc.pb.*
        tests/                    # 纯逻辑, 解码, TLS/RPC, 生命周期
    star/
        src/main.cpp / topology.cpp
    planet/
        src/main.cpp / upstream.cpp
```

`star_star` / `star_planet` 只依赖 `star_core`. 入口再组合角色与 `star_runtime`, 后者依赖 `star_wire` / gRPC / crypto.
公开测试凭据复用 `cluster/tests/fixtures`, 进程故障循环复用 `testkit/services.py`, 不复制两份状态机和夹具.
Common 不包含 Star mesh 策略或 Planet 选址策略; 两个 main 只组合实际对象与退出码.
协议生成目标与手写目标分别设置警告选项. 生成类型只穿过协议适配边界, 不遍布配置与拓扑算法.
两个程序各自编译一套普通角色实现, 不把 entire runtime 模板化为 `Node<Role, Transport, Store, ...>`.

维护者随后允许新建目录, 实施采用独立 `cluster-cpp/`, 当前 Rust 仍位于 `cluster/`, 两者使用独立构建输出.
第一阶段验收前保留 Rust 作为行为对照; C++ 验收通过后再整理 Rust 运行入口与服务专用依赖.
SDK Rust 和 `proto/generator` 的后续去留独立处理, 不能误删它们仍使用的 Cargo 工作流.
不长期维护两个生产 transport 后端, 也不提前移动/删除现有实现来制造不可回退状态.

## 4. gRPC 与并发模型

### 4.1 首选官方 Callback API

首选生成的 callback client/server reactor 实现长期 OpenSession; 准入 RPC 使用异步 unary 调用.
不使用每连接一个阻塞读线程与一个阻塞写线程. gRPC 官方说明回调 API 比传统 CompletionQueue API 更易使用;
它不是性能自动优于其他 API 的承诺.
[官方回调教程](https://grpc.io/docs/languages/cpp/callback/).

每个进程拥有一个控制调度循环, 负责重试、心跳/握手期限和诊断快照的调度. 默认不创建每会话线程.
调度等待使用 steady_clock 和可中断等待; jthread 的自动 join 不能替代主动取消 gRPC.
gRPC 自己拥有 I/O/回调工作线程, 必须独立记录实际数量和峰值.

已确认由 gRPC 管理 I/O worker, 应用控制循环首版固定为一个. 不机械映射 Tokio 的 worker 数量.
C++ 入口对旧 `--worker-threads` 显式报不支持并说明原因; 只有 M0 证明某项 gRPC 预算有明确作用时,
才以独立名称公开该配置并更新帮助. 不能静默接受无效参数, 也不宣称它等于进程总线程数.

CompletionQueue 只作为 callback 在前置验证中无法满足必要契约时的替代评估.
若需要替换, 先更新此设计, 不同时提供两套可配置的生产异步后端.

### 4.2 对象与锁

| 对象 | 拥有什么 | 保护方式 |
| --- | --- | --- |
| Runtime | listener/server、控制线程、Identity、角色对象和在途会话集合 | 关闭状态与实际完成登记, 统一等待 |
| Identity | TLS/crypto 句柄、账号材料、公钥 | 初始化后只读共享, 禁止默认格式化 |
| AdmissionAttempt | 首次 CAS 基线、请求/响应缓冲、context 和完成状态 | 进程生命周期持有, 回调结束才释放 |
| Session | context/reactor、收发消息、握手状态、Ping 状态与 session generation | 会话短 mutex, 原子操作只用于独立标志 |
| StarTopology | Star/Planet 槽位、epoch 和两方向会话句柄 | 一个短 mutex 起步 |
| PlanetUpstream | 至多八个候选与唯一 active 会话 | 控制循环串行决策, 完成结果带 generation |
| TimerSlots | 每个活动对象固定数量的可取消期限 | 到期索引有界, 更新替换原槽 |

不嵌套持有 topology 与 session 的锁. 在锁中取得有所有权的局部句柄/动作, 解锁后调用 gRPC、
取消其他会话、记录日志或唤醒控制循环. 使用代次在重新取得锁时确认动作仍属于当前对象.
不在锁内 join、读身份文件、验整份名单或构造大量状态输出.

控制通知优先采用每对象 pending 标志和一次合并唤醒; 不将正确性事件写进可丢日志队列.
若必须排队, 队列有配置总预算, 满额进入显式关闭/失败路径, 不新增无限 deque.
计时器取消必须能回收槽位, 不能仅靠不断追加“稍后发现过期”的 weak_ptr 条目保持表面简单.

### 4.3 Reactor 生命周期硬约束

同一 stream 至多一个读操作和一个写操作在途, 读写完成可能并发. callback 必须短小, 不阻塞等另一个 callback.
发送消息在写完成之前保持有效且不可修改; 读消息在下次 StartRead 前消费/转移完成.
客户端从 reaction 外发起操作需要正确持有/释放 holds, 最终由 OnDone 收尾.
服务端取消仍需完成 Finish, 正在写入时不能直接 Finish; OnDone 才是最终回调.
[gRPC C++ 生命周期规则](https://grpc.io/docs/languages/cpp/best_practices/).

实施时用明确的 `Open -> Closing -> Done` 状态和 `read_inflight/write_inflight` 表达约束.
外部计时器只持有安全的会话控制句柄, 在 OnDone 后不得取得裸 reactor 指针发起操作.
所有 Start*/Finish 的许可与关闭状态统一串行化; server 侧不套用 client holds 规则.
不从多个回调同时 `delete this`, 不靠 shared_ptr 环维持生命周期; 唯一最终释放点必须在测试中可观察.
C++ 异常不得穿过 gRPC callback. 边界捕获并转成有限错误/取消; 内存耗尽不承诺服务能无限继续运行.

### 4.4 关闭流程

1. SIGINT/SIGTERM 只通知正常线程, 信号处理器不加锁、不分配、不调用 gRPC 或打印 JSON.
2. 标记 Stopping, 停止新准入、候选刷新与拨号, 注销计时器, 禁止安装新活动会话.
3. 取消在途 admission 与客户端 RPC, 请求 server 有界 Shutdown, 推进服务端 Finish.
4. 等待每个拥有的 RPC 到达最终完成, 再释放对应请求、Arena、凭证引用与会话记录.
5. 回收 listener、控制循环、日志输出和临时资源. 正常退出 0, 运行错误 1, 参数错误 2.

退出预算默认 5 秒, 范围 1..60 秒. 超时是失败而不是假装优雅退出成功.
最终进程级终止可以作为兜底, 但必须在报告中区分正常释放和强制终止; 不 detach 仍借用运行状态的线程.
stdin EOF 不触发退出. 状态日志只查询真实索引, 诊断丢失不改变拓扑正确性.

## 5. 必须保持的协议与网络行为

### 5.0 Supervisor 签发不透明 Id 的目标规则

2026-09-12 维护者确认: 进程 Id 的生成与格式由 Supervisor 独占决定,
Star/Planet 只认证携带该 ID 的准入凭证确实由受信 Supervisor 签发, 不复制 ID 生成规则.
本节取代下文关于客户端生成 UUIDv4、固定 16 字节和 32 位小写十六进制校验的目标要求.
下文这些描述仍记录当前 v4 实现; 本次仅记录设计, 尚未修改 Schema、生成代码和运行时.

- `Id` 使用不透明 `string`, 不承诺 UUID 版本、variant、十六进制字符集、前缀或固定字节数.
  Supervisor 可在稳定的传输类型与长度上限内改变新 ID 的生成方式, 不要求 Star/Planet 同步升级格式解析器.
- Star/Planet 原样保存 ID, 仅进行相等比较、全值散列及需要的确定性字节排序.
  不从 ID 推断时间、角色、部署、成员代次或业务版本; 不做大小写折叠、裁剪、Unicode 归一化或重新编码后再比较.
  不同字节序列就是不同 ID, 排序只用于稳定遍历和名单验证, 不代表新旧顺序.
- Supervisor 负责签发域内的唯一性及登记结果稳定性. 一次进程准入对应的 ID 在重试、候选刷新和重连中保持不变;
  新进程重新准入取得新 ID. 修改生成方式不改写已签发身份, 旧格式与新格式可同时存在.
- 继续使用受信 Supervisor 公钥验证带协议域隔离的准入正文原始字节. 签名覆盖完整 Member,
  包括 ID、Galaxy、部署指纹、端点、角色、group 和成员 epoch, 不缩减成仅对 ID 签名.
  ID 的签发真实性统一在准入边界确认, 内部角色逻辑不重复执行 ID 格式验证.
- 外层仍检查消息与 ID 的长度边界、解码成功和必要字段存在, 防止无界分配或空身份.
  这些是稳定的传输约束, 不是 Supervisor 内部的生成格式. 准入仍检查集群、角色、身份绑定及成员代次;
  签发证明不单独提供在线吊销、实时健康或防止 bearer 凭证被复制的能力.

实施时让 `Id` 脱离当前 `HexId<16>`, 不影响仍有固定算法含义的 `Principal`.
维护者选择沿用 Protobuf 的 `string id`, C++ 使用拥有数据的 `std::string` 保存原值, 不引入十六进制到固定数组的转换.
传输文本遵循 Protobuf string 的 UTF-8 约束, 并检查非空和按 UTF-8 字节计的长度上限; 上限在迁移时冻结,
不因内部生成算法变化修改它. 日志及 UI 使用标准转义, 不直接把 ID 拼入 JSON、路径或格式串.
保留 string 字段类型不表示身份语义自动兼容: 旧节点仍会拒绝非 UUIDv4 格式, 签发迁移及混合版本行为需配套验证.

当前 Register 用客户端生成的 id 识别响应丢失后的重试. 改为服务端签发时, 应由独立登记票据关联同一次尝试,
在持久登记中保证同票据重试返回同一 ID 和 epoch, 不为重试增加成员代次. 票据绑定、并发新进程和旧票据重放的具体流程
还需与现有 expected_epoch 防旧实例覆盖规则一起实现; 不能通过省略 ID 格式检查顺带删除这些生命周期约束.

### 5.1 Supervisor 登录

新进程生成一次 UUIDv4, 用规范的 32 个小写十六进制字符表示. 重试与重连不换 UUID.
本地先校验身份文件、证书链/期限/服务端用途、私钥匹配与公布地址 IP SAN.
TLS 验证 Supervisor 名称或 IP, Challenge/Register 均提交账号密码. 不恢复旧进程身份.

登录、签发与验签的职责保持现有 v4 规则:

| 环节 | 执行方 | 使用的材料 |
| --- | --- | --- |
| 登录与角色授权 | Go Supervisor | 校验 username/password 与账号角色权限 |
| 签发独立节点凭证 | Go Supervisor | 用 Supervisor 的 Ed25519 私钥签名 Member 原始字节 |
| 节点间身份验证 | Star/Planet | 用 Supervisor 公钥验证对方提交的凭证, C++ 侧使用 BoringSSL |

账号密码用于取得签发资格, 不是凭证签名密钥. Star/Planet 不需要额外的进程签名私钥,
账号密码不发给其他节点, TLS 证书私钥与 Supervisor 凭证签名私钥也各自独立.
首版跨语言 TLS 配置使用 ECDSA P-256 / SHA-256 证书. 锁定的 BoringSSL 默认 TLS 验证算法
不包含 Ed25519, 因而不能复用旧 Ed25519 TLS 测试证书. 准入正文仍使用 Ed25519 签名,
不修改协议或 BoringSSL 私有实现来消除这一算法配置差异.
已签发凭证仍是 bearer credential, 没有独立 TTL 或在线吊销; 改密码不撤销已有凭证.
OpenSession 的拨号方先提交 Hello, 接收方验证准入后才返回自己的 Hello, 避免匿名调用方直接取得服务端 bearer 凭证.
gRPC 响应头可以提前发送以支持流式客户端, 但响应头不携带凭证. 此顺序同样适用于保留的 Rust 对照实现.
Supervisor 离线时已获准进程可验证已有凭证并重连, 新进程仍须等待登录与完整准入.

第一次成功 Challenge 的 expected_epoch 在整个进程中保持不变. Register 响应丢失后使用原请求身份及原基线重试,
不会重新取得更大基线覆盖竞争的新进程. 同账号不同端点独立; 同账号同端点重启 CAS 替换.
具体 principal 输入为 `username + NUL + cluster_id + NUL + canonical_advertise`, 使用 SHA-256.

先校验整个响应再原子安装. Star 收到严格排序、包含自己的完整 Star 名单; Planet 至多八个授权 Star,
本组优先、组内按 UUID 排序. 检查角色、Galaxy、UUID/principal/地址唯一性和自身凭证一致性.
名称与地址规范化必须复用 [v4 身份向量](tests/fixtures/admission-v4.json), 特别检查 IPv6 别名与 scope.

### 5.2 字节签名和分层输入限制

Ed25519 验证输入必须是 `verdandi-admission-v4\0` 加 admission 原始字节, 正文最多 1024 字节,
签名必须 64 字节. 不能 decode 再 serialize 后验签, 不能把“确定性 Protobuf”误认为跨语言规范编码.
未知/保留字段与重复字段按现有协议和回归处理, 不使用 C++ 结构体内存布局作为协议格式.

按维护者对内部服务的取舍, 第一阶段直接使用生成的 typed unary stub, 取消为 Register 单独设计的
ByteBuffer adapter 和 Member 解码前扫描. 不在标准解析器之外重复遍历 wire 格式.

1. 配置并验证 gRPC 接收字节上限: Register 响应最多 2 MiB, 请求保持 4096 字节预算.
2. 由生成的 Protobuf 解析器处理字段和格式错误, 按正常 RPC 错误路径结束失败调用.
3. 解码后先检查 members 至多 4096, Planet 候选至多八个, 再做名单唯一性、身份、代次和签名检查.
4. 只在整份响应合法后原子发布状态; 超限或失败响应不进入长期成员表.

认证、签名、消息大小、角色授权和代次检查直接关系到连接正确性, 继续保留. 不因“内部服务”
省略错误处理、异步所有权检查或关闭清理; 已验证且不可变的进程内对象可复用验证结果.

这项选择减少适配代码与重复扫描, 尚无实测性能收益结论. 2 MiB 编码预算不能保证解析前最多分配
4096 个对象; 大量小 Member 仍可能放大临时分配. 第一阶段接受这个边界, 通过实际 RSS/分配测量记录成本,
不再把无 Member 分配的前置检查设为迁移门槛. 若测量暴露正常规模下的资源问题, 再针对证据评估改进.

### 5.3 Star 与 Planet

| 关系 | 允许方向 | 行为 |
| --- | --- | --- |
| Star - Star | 双方主动建立各自出站会话 | 每对两个有方向的逻辑 OpenSession, 新认证成员触发一次有界反向建流 |
| Planet - Star | Planet 出站, Star 入站 | Planet 仅安装一个活动上游, Star 不反向拨号 Planet |
| Planet - Planet 或 Star 主动拨号 Planet | 不允许 | 关闭并给出有限错误 |

N 个 Star 稳态逻辑流总量为 `N*(N-1)`, 单 Star 持有 `2*(N-1)` 个流端点, 再加其 Planet 入站.
每条逻辑流连接两个节点, 全网统计只计一次. TCP/HTTP2 连接由 gRPC 复用或重建, 不要求物理连接数等于此公式.
保留两个方向的逻辑会话, 不引入 gossip/revision 对账. 大规模 mesh 的二次增长不会被 C++26 消除.
`max_members` 每角色分别计数, Star 包含自身, Planet 不占 Star mesh 名额, 离线记录不自动删除.

Star 槽位安装按 principal + member epoch 比较; 更高代次不能改变既定角色/地址.
成员 epoch 表示 Supervisor 签发的进程身份代次, 本地 session generation 表示一次逻辑会话尝试,
二者不能混用. 每个成员身份/方向只安装一个当前有效会话, 更高 epoch 替换仍遵循现有规则.
旧会话退出只删除自己的 session generation, 不能删除替换后的新会话. 出站会话核对预期目标.
每次 OpenSession 均重新完成 Hello 和身份验证; 已结束的流允许在同一 TCP 上重新开流.
即使来自不同 TCP, 重复活动身份/方向也按会话索引拒绝, 不能靠物理连接唯一性代替判断.
准入后未知但签名合法的新 Star 可以被接纳, 不要求它已经出现在本机旧名单中.

Planet 本组优先, 故障时用已授权跨组候选. 每轮每候选最多尝试一次, 每次拨号至少间隔 250 ms,
每候选独立退避抖动, 稳定窗口从认证成功开始. 旧流完成释放后才安装下一活动流.
传输中断/超时/限额不作为永久身份隔离; 身份/协议拒绝单独隔离.
原样刷新不能解除隔离, 合法新代次可重新尝试; 旧名单不覆盖握手中已观察到的新 epoch.
候选耗尽才限速刷新, 健康上游期间不刷新、不主动迁移. Supervisor 离线保留已知候选继续尝试.

### 5.4 心跳和背压

Ping 只用作当前会话保活, 不携带拓扑 revision. 每方向最多一个等待编号.
从本端安排 Ping 起建立绝对截止时间, 覆盖入队、实际发送和匹配 Pong. 旧 Pong、对端 Ping 与其他消息不续期.
request_id 耗尽时受控关闭会话, 不发生整数回绕后匹配旧响应.

控制消息最多 4096 字节, 应用待发送队列保持四条; 另有一个在途写缓冲.
排队预算与在途预算分别计数, 不把 gRPC 内部缓存当成零.
队列满时不能等待阻塞 callback; 本阶段控制流无法按期限入队则关闭并退避恢复, 不静默丢弃 Ping/Pong.
收到 ProtocolError 后关闭, 不产生错误回声. 尝试发送错误不构成必达承诺.

64 KiB 不作为协议常量或跨实现互操作前提. 首版从所选 gRPC 的流控默认值起步, 记录有效参数与 BDP 行为,
再按慢读者、消息规模和 RSS 实测调整. HTTP2 窗口不能代替上述应用队列和消息预算.

## 6. gRPC 迁移前置验证, 不可跳过

这些是依赖适配问题, 不能用一份空 Hello demo 代替验证. M0 未通过时保留 Rust 生产入口.

| 编号 | 当前必须保留的能力 | C++ 验证方式与失败处置 |
| --- | --- | --- |
| G1 | TLS 1.3、h2、根与目标名称验证 | TLS1.2-only、错误 SAN/根、过期证书负例; 只能使用不绕过验证的配置 |
| G2 | 慢 TLS / 未发 HTTP2 请求也有期限和连接预算 | 原始 socket 慢握手压测并 SIGTERM, 核对 FD/RSS/退出时间; 只有 RPC 计数不合格 |
| G3 | 身份、成员 epoch 与逻辑会话 generation 正确隔离 | 同身份/方向的重复活动流被拒绝; 旧流结束后重用 TCP 的新流须重新认证; 跨 TCP 重复与旧回调同样受约束 |
| G4 | Star 两方向逻辑流与 Planet 唯一活动上游 | 按认证角色索引核对, TCP 重建/复用不改变逻辑拓扑; 物理连接与逻辑流分开统计 |
| G5 | 有限消息、应用队列和可解释的流控资源 | 记录框架窗口/BDP 配置, 压慢读者并核对队列、RSS 和回落; 不要求固定 64 KiB |
| G6 | 标准解析与解码后名单限制 | 超字节上限、超成员数、空 Member、截断与非法字段测试; 记录解析峰值, 不宣称解析前对象数受限 |
| G7 | 准入总期限和取消 | DNS、TCP、TLS、Challenge、Register 分别阻塞, 验证端到端截止及回调释放 |
| G8 | Reactor 外部计时器与关闭安全 | OnCancel/OnDone/写完成/定时到期并发, ASan/TSan 与确定性调度用例 |

gRPC 的 TLS 版本设置目前暴露在 experimental 命名空间, 需要局限于 identity 适配文件并锁定依赖版本.
不能因 SslCredentials 默认能够握手就声称只启用了 TLS1.3.
[TLS options 源码](https://raw.githubusercontent.com/grpc/grpc/v1.84.0/include/grpcpp/security/tls_credentials_options.h).

G2 优先使用 gRPC 的公开握手期限与资源能力, 先验证效果再决定是否需要很小的适配.
G3/G4 在逻辑会话和角色索引中实现, 不为恢复旧物理连接限制引入底层钩子.
按客户端 IP:PORT 字符串计数、在 handler 才计数、或设置 ResourceQuota 都不能直接证明所有握手资源受控.
若必须维护大块 gRPC 私有代码/自定义 acceptor, 先报告成本与可替代规则, 不偷偷削弱契约或引入第二套网络栈.
窗口/重试/消息选项必须对应所选 tag 的头文件, 并进行行为测试, 不使用网上过时的字符串参数假定生效.
[gRPC 参数定义](https://raw.githubusercontent.com/grpc/grpc/v1.84.0/include/grpc/impl/channel_arg_names.h).

## 7. 分配、拷贝与缓存策略

优先顺序为避免不必要工作、转移所有权、复用有界缓冲, 最后才是内存池.
Hello 与已验证 admission 是会话间可共享的不可变数据; 每次重连仍校验新的连接身份, 不缓存“永久认证成功”.
小 Ping/Pong 使用固定的在途消息对象, callback 完成前不 Clear/重写. 列表只在登记/刷新时构建.

第一阶段默认使用普通生成消息、有界缓冲复用和适合小集合的 inplace_vector.
Protobuf Arena 仅作为后续测量候选, 例如一次登记响应解析; 如采用, 验证后把必要状态移入长期成员值,
禁止从已释放 Arena 留下指针. 长期流不共用一个永不 Reset 的 Arena, 不为几十字节 Ping 每次创建 Arena.

Arena 的创建分配可以并发, 但这不使消息修改线程安全, Reset/销毁也需要所有使用者结束.
跨 Arena 的转移可能复制, 不假定 std::move 总是无拷贝. 不使用 UnsafeArena* 绕过所有权约束.
[Protobuf Arena 指南](https://protobuf.dev/reference/cpp/arenas/).

`shared_ptr<const Payload>` 可用于未来扇出共享, 引用计数有成本, 慢读者也会延长保留时间.
第一阶段性能夹具可对比“一次序列化 + 共享 grpc::Slice/ByteBuffer”, 但不改变生产 oneof 协议.
这一对比只度量应用层减少的拷贝; Protobuf 解析、HTTP2、TLS 加密和内核传输仍可能复制.
不宣称端到端零拷贝, 不缓存无限历史或为未来业务提前搭建对象池层级.

用预算表达最低可解释的应用内存:

```text
单会话应用内存 = 身份引用 + 一个接收对象 + 一个在途写 + 四个排队槽 + 有界计时状态
进程应用内存 = 有界成员索引 + 所有会话 + 最多一个准入响应 + 单条日志缓冲
实测总 RSS    = 上述内存 + protobuf 对象开销 + gRPC/TLS/线程/分配器/系统缓存
```

2 MiB 是编码响应预算, 不是解析对象的精确 RSS 上限. `Clear()` 后保留容量、连接反复建立后的池驻留,
以及 allocator high-water mark 均需在 soak 中测量. 不能用 encoded bytes 冒充实际内存用量.

## 8. 工具、依赖和生成管理

### 8.1 已锁定版本, 整体兼容性待验证

按维护者决定, 以 2026-09-11 核对时的最新稳定发布锁定新 C++ 服务依赖, 不使用浮动 latest.
gRPC C++ 当日发布了 v1.84.0, 替代初稿的 v1.83.1 候选. Go gRPC 属于独立实现, 不随此清单升级.
精确 tag、commit、来源与上游子模块差异见 [C++ 依赖版本锁定](cpp26-dependencies.md).
版本选择已确定, 不代表这套组合已通过 GCC 16.2 的构建和运行验证.

| 组件 | 选择原则 | 本地位置 / 待完成事项 |
| --- | --- | --- |
| GCC / libstdc++ | 使用已验证的 16.2.0 组合 | `build/tools/gcc-16.2.0`, 不修改系统工具链 |
| gRPC C++ / grpc_cpp_plugin | 1.84.0, 插件使用同一份源码 | `build/deps/cluster-cpp`, 生成工具放 `build/tools` |
| Protobuf / protoc | 36.1, C++ 生成代码与 runtime 精确匹配 | 生成器放 `build/tools`, 运行库放依赖前缀 |
| Abseil、c-ares、RE2、zlib | 20260817.0 / 1.34.8 / 2025-11-05 / 1.3.2 | commit 见版本锁; 与 gRPC 上游子模块不同, M0 验证组合 |
| TLS 与 Ed25519/SHA256/RNG | 使用所选 gRPC 配套的 BoringSSL, TLS 与本地凭证验签共用同一份构建 | 锁定 gRPC 所引用的 BoringSSL commit, 放项目依赖前缀 |
| JSON | yyjson 0.13.0, 严格解析 login.json; 日志只输出已限定字符集的字段 | 匹配版本的已有缓存优先; 直接链接库, 不依赖整个 Redis SDK |
| CMake / Make或Ninja / clang-format | 优先已有工具, 逐项验证 C++26 配置及语法支持 | 缺失工具如实报告, 不自动安装 |
| 测试 | 先用 CTest、小型现有断言设施及 Python 编排器 | 不因惯例自动添加 GoogleTest/benchmark 或新的包管理器 |

根据维护者后续讨论, 新骨架改为采用 gRPC 配套 BoringSSL, 替代本设计初稿的 OpenSSL 优先建议.
gRPC 源码构建设置 `gRPC_SSL_PROVIDER=module`, 使用其锁定子模块而非 BoringSSL HEAD.
TLS 握手与记录传输由 gRPC 负责; 私有 identity 适配器使用同一 BoringSSL crypto 构建进行
Ed25519 验签、SHA-256、随机数与本地证书检查. 不因独立凭证验签再引入 OpenSSL 或第二套 crypto.
这些本地调用仍需在所选 commit 上编译并通过 Go/Rust 公共向量, 不是只验证 TLS 能连接.
[gRPC SSL provider 实现](https://raw.githubusercontent.com/grpc/grpc/v1.84.0/cmake/ssl.cmake).

BoringSSL 不保证稳定 API/ABI. 只在服务私有适配器使用它, 不导出其类型, 不独立替换动态库版本.
依赖锁同时记录 gRPC/BoringSSL commit、编译选项和静态/动态链接方式, 升级整套重建与回归.
“配套”不表示发行源码压缩包必然含完整子模块, 也不表示系统安装的任意 gRPC 包使用 BoringSSL.
已准备包须核对实际 TLS provider; 不匹配时报告缺口, 不自动退回 OpenSSL.
[BoringSSL 兼容边界](https://boringssl.googlesource.com/boringssl/+/HEAD/README.md),
[Ed25519 公共接口](https://boringssl.googlesource.com/boringssl/+/HEAD/include/openssl/curve25519.h).

既有 Redis C++ SDK 的 OpenSSL 搜索与外部构建规则保持不变. 本轮 C++ gRPC/BoringSSL 来源集合
已另行取得 Ubuntu 项目内下载和构建授权, 具体版本与归档摘要见依赖锁.

C++ Protobuf 要求生成代码与 runtime 精确匹配, 不承诺跨版本 ABI.
当前已有 protoc 36.1 与所选版本号一致, 仍需核对实际生成器来源、C++ runtime 和生成插件的组合后复用.
[Protobuf 运行时兼容保证](https://protobuf.dev/support/cross-version-runtime-guarantee/).

### 8.2 构建隔离与离线规则

首版依赖产物放 `build/deps/cluster-cpp/linux-gcc16/install`, 版本由来源锁固定,
对象与二进制放 `build/cluster-cpp/<profile>`, core-only 使用独立的 `core-<profile>`.
依赖锁记录 archive/commit 摘要、provider、C++ ABI、编译器、链接方式与许可证; 不只记录“系统中找到”.
现有系统 C++ 二进制依赖要通过 ABI/编译链接探测, 失败则复用源码在项目内另建, 不强行混合 GCC 15/16 的运行库.

正常配置、构建、检查、测试一律离线. 禁止隐式 FetchContent 下载、git submodule 拉取、vcpkg bootstrap/install
或 protoc 下载. CMake 禁止标准自动降级, 必需能力缺失要在 configure 期间明确失败.
使用项目 wrapper 仅给子进程设置 PATH、依赖搜索路径和临时目录. 支持直接 CMake, 但必须显式选择相同工具链和前缀.

Linux 发布产物最终需要声明 libstdc++/libgcc/glibc 与实际链接的 gRPC/BoringSSL 运行依赖及加载策略.
开发构建可以使用项目 RPATH; 发布不能嵌入 `/home/ubuntu/verdandi` 这样的开发机绝对路径.
首阶段做可重定位暂存目录运行检查, 不承诺任意旧发行版能运行 Ubuntu 构建产物.

### 8.3 协议生成

`.proto` 是唯一线上契约. C++ `.pb.h/.pb.cc/.grpc.pb.h/.grpc.pb.cc` 显式生成并作为源码放在
`cluster-cpp/common/src/generated`; 普通编译不运行 protoc. 保留字段编号和 reserved 项, 不复活 MessageID 分派.
校验命令在项目临时目录重新生成后逐字节比较, 不在检查阶段修改源码.
锁定 protoc、插件及参数, 去掉时间/机器路径等不稳定生成输入.

`proto/star_transport.proto` 的 Supervisor 旧帧协议注释已修正, 并明确逻辑会话与物理连接的区别;
Go/Rust 生成源已同步更新注释, 字段和 RPC 未变化.
生成逻辑不移入 C++ 反射: `.proto` 解决跨语言 schema, 静态反射解决本地声明重复, 两者各自负责一层.

## 9. 阶段计划与退出条件

| 阶段 | 可审核成果 | 进入下一阶段的条件 |
| --- | --- | --- |
| M0 工具与能力适配 | C++26 feature probes、锁定版本的构建验证与产物清单、TLS/RPC 最小实证、G1-G8 结果 | 下载另行获准, 按本轮确认的逻辑会话/资源规则验收; 不修改生产默认入口 |
| M1 本地基础 | 目录/CMake、反射配置、有限错误、身份/名单验证、离线单测 | 参数/恶意输入失败可解释, 生成一致, 所有权清楚 |
| M2 单会话 | Go Supervisor 准入、Hello、控制流、保活、取消和释放 | C++-Rust 两方向兼容, 新旧 epoch 与超时/限额测试通过 |
| M3 角色骨架 | Star mesh, Planet 单上游/刷新/故障切换 | 现有 13 组进程回归逐项映射并通过, 不提供业务假 ACK |
| M4 整理与质量门槛 | 删除重复路径, 注释与错误传播审查, sanitizer、长时/性能报告 | 正确性/资源/复杂度均有证据, 仅此时切换 C++ 默认服务入口 |

每阶段先完成代码及测试用例, 再做静态整理与跨层审查, 最后执行该阶段验证.
M4 前只允许对照测试选择 Rust 或 C++ 二进制, 不把所有现有脚本直接替换成未验收实现.
不设置为了赶进度的完成百分比; 按未通过门槛和待验收行为报告状态.

## 10. 测试矩阵

### 10.1 单元、编译期与解码测试

| 层 | 必须覆盖 |
| --- | --- |
| C++26 元数据 | 字段枚举/注解取值、默认值、名称唯一性、未支持类型编译失败、稳定帮助输出、敏感类型不可反射输出 |
| 配置 | 沿用的 CLI 选项与旧 worker 参数显式拒绝, 空/缺失/重复/未知值, 数值溢出, 时长上限, role 注入, wildcard/advertise, 无 I/O 副作用 |
| 身份 | UUID版本/variant/长度, principal 分隔, Unicode/NUL, IPv4/IPv6规范化, 公共 Go/Rust 身份向量 |
| 凭证 | 原始字节签名、错误公钥/签名/域、超长正文、同 epoch 内容冲突、高 epoch 非法角色/端点变化 |
| 解码与限制 | gRPC 字节上限, 解码后 Member 上限, 空 Member, 错误 wire type, 长度/varint溢出, 截断, unknown/reserved/重复字段, 失败不安装状态 |
| 纯状态转移 | Hello之前错误消息、重复Hello、空oneof、旧Pong、不匹配编号、心跳溢出、队列满与取消 |
| 成员/候选 | 原子名单安装, 更高代次替换, 旧callback不删新记录, 容量包含自身, 隔离与刷新, 本组/跨组排序 |
| 并发所有权 | 读写/取消/定时器并发, OnDone后回调访问, 写buffer过早复用, shutdown同时拨号, admission响应丢失 |

编译失败用例由独立目标运行, 只检查对应的错误条件, 不能把头文件找不到当成预期类型诊断通过.
契约关闭的对照构建用于证明外部验证和语义不依赖契约执行, 不作为新的发行模式承诺.

### 10.2 真实进程与兼容测试

复用现有 13 组: CLI、并发加入、Supervisor 离线、Supervisor 恢复、Star 重启、非法身份、非法登录、
角色越权、组内候选、跨组切换、新 Planet 等待、单上游和清理. 记录每组与具体新测试的映射.
旧 Rust 的物理连接、固定窗口及前置扫描断言保留为历史实现验证; C++ 用本次已确认的规则替换对应断言,
单独记录差异, 不将差异标为未经解释的测试跳过. v4 字段与认证向量仍须互通.

增加或明确:

- Linux C++ Star - C++ Star, C++ Planet - C++ Star; 所有方向分别验收.
- Linux C++ 与保留的 Rust Star/Planet 双向交互, 使用原 Go Supervisor 和相同 v4 测试向量.
- 同端点旧进程延迟响应/断开, 新进程 epoch 不能被回滚; 数据库无需为同一 v4 语言迁移清空.
- 慢 TLS、HTTP2 无请求、同 TCP 上旧流结束后的新流重新认证、跨 TCP 重复活动会话拒绝、慢读者和队列饱和.
- stdin EOF、SIGINT/SIGTERM、取消全部 socket/RPC、端口重绑和重复启动后 FD/线程回落.

第一阶段强制 Linux 原生与同机跨语言测试. Windows Rust - Linux C++ 作为有条件补充,
不因此要求 Windows C++26. 跨主机时必须明确源版本与防火墙授权, 不自动修改系统规则.
SDK 不在本轮回归范围, 除非实际修改了其共享构建工具或契约; 这时只运行受影响范围并说明原因.

### 10.3 Sanitizer 与长时测试

Debug/Release 均跑功能测试. ASan+UBSan 与 TSan 分开构建, 支持时启用泄漏检测.
gRPC/crypto 未插桩部分必须标记, 不把依赖内部未覆盖的代码宣称为无竞争.
依赖噪音抑制必须精确到已核实问题, 不使用通配 suppressions 隐藏项目栈.
clang-tidy 若不能解析选用反射语法, 如实列缺口, 不靠修改特性宏假装完成 C++26 分析.

建议先跑短回归及 60 秒故障预检, 再完成至少一次 3600 秒长时测试作为第一阶段候选门槛.
一键长时入口允许显式 duration, 必须记录有效运行时间, 提前结束不能判通过.
无需启动 Redis/Docker. 复用 Go Supervisor 与公开隔离测试身份, 测试只清理自己的进程、端口、临时目录.
虚拟机单任务编译起步; 运行前确认可用内存, 低于安全线时停测并记为中断, 不依靠动态内存必然及时扩展.

## 11. 性能和复杂度验收

### 11.1 骨架可以测什么

真实控制面测试: 并发加入速率、完成全互联时间、重连/切换恢复时间、保活延迟、CPU、RSS、FD、线程数、
应用分配次数与字节数、队列峰值、取消到 OnDone 和关闭耗时. 空闲 RSS 与重连后稳态 RSS 都要记录.
相同 Galaxy 下以 2/4/8/16 个 Star、0/1/8/32 个 Planet 为建议阶梯, 达到资源阈值就停止扩大并记录缺口.
报告分别列逻辑活动流数、每节点扇出与实际 TCP/HTTP2 连接数, 不以 Channel 对象数替代 socket 观测.
拓扑展示的边表示认证逻辑关系, 物理连接复用/重建只作为诊断信息; 当前 Admin 实现不在本轮修改范围.

### 11.2 推流夹具, 不伪造业务完成

本阶段生产 SessionPacket 没有业务更新消息. 大量小 Catalog/Registry 更新吞吐只能先用隔离 benchmark schema
承载 opaque payload, 不向生产 `.proto` 临时塞测试 bytes, 不把结果称为 Registry/Catalog 一致性通过.

建议矩阵分层执行, 不直接启动全部笛卡尔积:

| 维度 | 建议取值 |
| --- | --- |
| 模拟 Registry 身份数量 | 100, 1000, 10000, 只表示夹具的逻辑对象数量 |
| 小更新 payload | 64, 256, 1024 bytes, 另列实际编码字节 |
| 接收端扇出 | 1, 4, 16 |
| 更新分布 | 均匀与热点键; 稳态推送与短突发 |
| 下游速度 | 正常, 降速, 暂停直到触发预算 |
| 实现对照 | 当前 Rust gRPC v4 等价夹具, C++ 普通对象, C++ 有界复用/共享编码 |

至少五组交错重复, 预热与正式采样分开. 比较同机 CPU预算、相同TLS/压缩/消息/队列设置、相同负载与接收确认点.
同时记录窗口与 BDP 配置; 框架默认值不同的结果单列为默认配置对照, 不把它当成相同传输参数下的语言差异.
报告 offered/accepted/received 速率、丢弃/拒绝/超时数、p50/p95/p99及样本数, 不只统计成功且快的消息.
有节拍的开放负载记录计划发送时间以暴露排队; 闭环最大吞吐单独报告, 防止协调遗漏掩盖尾延迟.
跨主机时使用发送者时钟上的往返确认或独立校准, 不直接相减两台机器的时间戳.

普通对象与 Arena/共享编码必须保持相同所有权与错误路径. 分配仪表会影响性能, 分配计数和无仪表计时分开运行.
缓存总预算满时有明确失败/背压, 吞吐下降不能通过无限排队隐藏.

### 11.3 怎样认定“优化有效”

维护性先看删除了几处重复字段表、角色重复逻辑和生命周期分支, 再统计手写生产行数、模板行数、
编译时长和二进制大小. 生成代码、测试与中文注释单列, 不用删除注释/测试来制造代码量下降.
没有时间预算、对象所有者或停止路径的“短代码”不被视作简化.

反射配置属于启动冷路径, 首要收益是减少维护点, 不宣传它提高消息吞吐.
容器内联只承诺去掉可观察到的容器分配, Arena/序列化共享仅在相同负载实测后接受为默认.
建议把跨重复测试持续出现的吞吐或 p99 约 5% 以上回退作为调查触发线, 不是已确认的性能 SLA.
所有正确性和内存有界测试必须通过; 无证据的优化不因“C++26”标签自动进入实现.

## 12. 编码、注释与可维护性规则

继续使用中文源码注释和 ASCII 标点, 公共函数说明参数、结果、错误与所有权, 关键块说明取消和代次条件.
160 列是上限, 不压成难读单行. 代码写入后立即 clang-format; 反射语法若超出现有工具能力,
只对必要语法片段作有说明的局部保护, 并登记格式化缺口, 不整文件禁用或默默破坏语法.

公开失败返回优先 `std::expected<T, Error>`. Error 使用有限枚举与必要上下文, 不把远端正文当错误分类.
进程根与 gRPC 边界负责异常转换, 析构不抛异常. 不把 every call 包装成相同的 try/catch 工具层.
使用 `unique_ptr`/值所有权为默认, 仅跨回调或共享不可变数据确有需求时使用 shared_ptr.
`string_view`/`span` 不隐式跨异步边界, 存在回调使用者时不得释放/Reset 底层容器或 Arena.

修复任何缺陷都审查 C++、保留 Rust、Go Supervisor 与相关 Python 工具是否存在同类路径.
SDK 仅在对应路径适用时进入审查, 记录“不适用”的依据, 不以语言不同推断无缺陷.
库版本升级需要 feature probe、生成一致性、协议/回调生命周期和资源回归, 不只跑 hello-world.

## 13. 已确认的取舍与待验证事项

### 13.1 维护者已确认的七项决定

以下决定已同步应用到第 4..11 节, 不再作为待确认选项. 初稿要求的“一条 TCP 一生只允许一个会话”、
固定 64 KiB 窗口和 Member 解码前扫描已被本次决定取代. 当前 Rust 源码及其历史测试结果没有随文档改变.

| 编号 | 已确认取舍 | 实施边界 |
| --- | --- | --- |
| 1 | 身份和代次由逻辑会话管理, gRPC 可以复用或重建底层连接 | 每流认证, 按身份/方向防重复, epoch 与本地 generation 分开; Star 两方向和 Planet 单上游保留 |
| 2 | 64 KiB 不固定为协议要求 | 用框架默认流控起步, 记录并测量配置; 应用消息、队列和在途缓冲仍有预算 |
| 3 | 优先采用 gRPC 公开资源与生命周期能力 | 先验证慢握手、取消和关闭; 仅在证据表明必要时评估小型适配 |
| 4 | 内部服务暂不增加显著影响性能的冗余验证 | 本轮落实为取消 Register 解码前重复扫描; 保留认证、字节上限、解码后数量及状态正确性检查 |
| 5 | gRPC 管理 I/O worker, 应用控制循环保持简单 | 旧 worker 参数显式报不支持; 经验证有作用的预算才以独立名称公开 |
| 6 | 普通对象和有界复用先行 | Arena、共享序列化和额外池化有对照收益再引入 |
| 7 | 锁定核对时的最新稳定版本 | 精确版本和 commit 已记录; BoringSSL 跟随 gRPC 的固定子模块, 不跟随 HEAD |

第 4 项是当前范围内的工程取舍, 不表示所有验证都已测出性能问题.
此时不承诺解码前对象数量硬上限; 这一差异在资源测试与报告中明确记录.

### 13.2 实施阶段需要回答的工程问题

| 项目 | 当前方向 | 所需证据 |
| --- | --- | --- |
| 完整依赖组合 | 按 [版本锁](cpp26-dependencies.md) 准备, 使用 gRPC 配套 BoringSSL | GCC 16.2 编译链接、生成一致性、Go/Rust 互操作; 最新稳定版组合不同于 gRPC 上游子模块组合 |
| 未认证连接资源 | 使用公开握手期限与资源 API | 慢 TLS/HTTP2 无请求期间的 FD、RSS、期限和停止清理, 不能只统计 handler |
| 流控与线程预算 | 默认配置基线, 有证据再调优 | 慢读者、线程峰值、队列和重连后 RSS; 参数值未测定前不宣称总资源硬上限 |
| Reactor 生命周期 | 一个状态所有者和唯一最终完成点 | 读写、Pong、超时、取消及关闭的并发排列, 旧回调不影响新会话 |
| Arena / 共享编码 | 非默认测量候选 | 同条件 CPU、分配、RSS、吞吐和尾延迟; 收益不足就保持简单基线 |
| 长时与性能门槛 | 3600 秒候选门槛, 5% 回退调查线 | 实施测试计划中的负载、主机条件和有效时长, 不当作已承诺 SLA |

`ResourceQuota` 只约束附着到它的 gRPC 实体, 不能直接解释为完整进程 RSS/线程硬上限,
预算下调也没有固定完成时间. 应用自己的成员、队列和计时器仍要管理.
[ResourceQuota 接口说明](https://raw.githubusercontent.com/grpc/grpc/v1.84.0/include/grpcpp/resource_quota.h).
同样, HTTP2 流控协调发送与接收, 不会自动限制业务自行积累的缓存.
[gRPC 流控说明](https://grpc.io/docs/guides/flow-control/).

### 13.3 后续顺序与当前完成边界

先在获准的实施阶段验证锁定依赖与 G1..G8, 再完成连接骨架, 整理所有权与重复逻辑后进行功能和长时测试.
优化实验独立于正确性验收; 后续业务复制与存储仍按 Galaxy 设计另行推进.

独立 `cluster-cpp/` 已实现连接骨架, 获准的依赖已在 Ubuntu 项目内构建, Debug/Release、ASan/UBSan、
跨语言互通和修复后一小时故障循环已有实测证据. 完整 TSan 已通过, 2026-09-12 按维护者指示恢复剩余验收.
编译期负例、契约关闭对照、16 Star / 32 Planet 规模与分配测试已通过; 推流完成五轮 225 组对照,
40 组独立分配对照及新增 sanitizer 检查. 默认 Linux C++ 入口完整回归通过, M0..M4 连接范围验收完成.
本轮细项及容量拒绝、分析器和性能限制见 [补充报告](../cluster-cpp/qualification-20260912.md), 不扩大为业务或生产容量认证.
实际证据和缺口以 [C++ 验证记录](../cluster-cpp/validation.md) 为准, 不把设计文本当作已实现或已通过的证明.
