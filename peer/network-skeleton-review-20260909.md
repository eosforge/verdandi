# Peer 基础网络骨架审查

日期: 2026-09-09.

后续状态: 本文主体保留初次骨架的历史问题与复现. 同日接入 Protobuf 和自动发现时已修复 R1,
R2 的任务所有权与清理出口, R5 的封顶抖动, 以及 R6 的错误 cluster 断言和相关覆盖缺口.
R3 已在之前修复. 新版在 Windows / Ubuntu 各通过 31 项测试, 详见 [验证记录](protobuf-network-validation-20260909.md).
R4 的守护进程生命周期仍未实现; 这份历史评分不代表当前代码评分, 也没有把未测试的故障路径声称为已验证.

## 结论与范围

当前版本是可以继续演进的 Rust 网络原型. 代码风格总体规范, 模块职责较清晰, 但已实现的监听、连接、重连和关闭范围内仍有缺陷, 尚不能称为生产级网络骨架.

本次审查覆盖 `peer/src`, `peer/tests`, Cargo 声明、两端脚本与 README. 不把尚未授权实现的自动发现、状态复制、SDK 和持久化算作当前实现 BUG. 生产身份认证、心跳和服务托管属于后续发布能力缺口.

以下评分是针对当前工作树的工程判断, 不代表性能测试或正式认证.

| 维度 | 评分 / 10 | 判断 |
| --- | --- | --- |
| Rust 代码规范 | 7.5 | 命名、格式化、可见性、所有权和错误返回基本规范; 注释准确性与取消安全仍需改进 |
| 结构清晰度 | 8 | 模块按职责划分, 公开入口小, 库与 CLI 分离; `server.rs` 开始集中过多生命周期逻辑 |
| 测试充分性 | 4 | 3 项真实 TCP 测试覆盖基础连通, 缺少取消、容量、异常输入和任务失败覆盖 |
| 生产就绪度 | 4 | 关闭时仍能激活新会话, 服务退出机制不完整, Linux 脚本入口有实际错误 |

## 现有优点

- [src/lib.rs](src/lib.rs) 只导出启动配置、Peer 和诊断类型, 网络实现模块保持私有.
- [src/config.rs](src/config.rs) 把纯配置校验放在 bind 前. seed 数量有上限, `with_seeds` 对无限输入迭代器也只收集有限项.
- [src/protocol.rs](src/protocol.rs) 先验证远端长度再分配, 用检查加法和 `get` 处理切片, 拒绝截断、非法 UTF-8 和尾随字节.
- [src/connection.rs](src/connection.rs) 把解码结果与经过语义校验的描述分为两个类型, `Arc` 共享只读本地信息.
- [src/server.rs](src/server.rs) 使用子取消令牌、`JoinSet` 和 RAII permit 表达任务与容量归属, 没有引入额外抽象框架.
- Cargo 声明禁止 unsafe, 对 Clippy 和 unwrap/expect 设置严格 lint. 工作树有 MIT LICENSE、Cargo.lock 和项目内缓存入口.
- 中文与英文标点的详细注释符合当前维护者的阅读需要. 注释数量本身不作为生产质量指标.

## 已确认的问题

### R1. 握手不响应关闭, 停止请求后仍可激活会话

位置: [src/connection.rs](src/connection.rs), 第 130 行的握手等待和第 175 行的 Connected 发布.

握手只等待 `timeout(exchange_hello(...))`, 没有同时等待取消令牌. 取消分支直到 Connected 发布后才参与等待.

本次使用现有 Windows 可执行文件和 localhost TCP 连接实际复现:

| 场景 | 结果 |
| --- | --- |
| 已收到本地 Hello, 远端始终不回应, 此时输入停止信号 | 约 3010 ms 后退出, 记录握手 TimedOut |
| 已收到本地 Hello, 输入停止信号, 200 ms 后才补交有效远端 Hello | 仍产生 Connected, 随后才产生 Disconnected; 约 258 ms 后退出 |

需要把握手纳入取消等待, 并保证进入停止状态后不能发布新会话激活. 单纯给某个 `select!` 增加分支不足以替代对激活时序的定义.

### R2. 异常路径没有完整兑现任务等待契约

位置: [src/server.rs](src/server.rs), 第 104、190、235 行附近.

- 关闭循环遇到子任务错误时直接 `return Err`, 剩余任务由 `JoinSet::drop` 请求 abort, 没有继续逐个等待结束.
- 入站分配 generation 的 `?` 在计数耗尽时直接跳过公共清理段. 这是极低频边界, 不是当前普通连接的主要故障来源.
- `shutdown(self)` 在等待前用 `take()` 把根句柄移入局部变量. 若调用方取消这个 shutdown future, 局部 `JoinHandle` 被丢弃并失去 join 入口, Peer 的 Drop 此时也拿不到该句柄. 根取消已请求, 但任务结束和资源释放没有被等待确认.

已通过代码及项目缓存中的 Tokio 1.53.1 源码核对: JoinSet 的 Drop 会请求 abort; 等待取消完成还需要 join. JoinHandle 的 Drop 会 detach.

这些是静态确认的问题, 本次没有注入 supervisor panic 或 generation 耗尽. 不能把它们描述成正常运行必然泄漏, 但当前注释中的“全部结束后返回”表述过强.

改进方向是保留首个错误并继续回收任务, 同时明确 shutdown future 被取消时的所有权契约.

### R3. Linux Cargo 脚本的参数顺序错误

位置: [cargo.sh](cargo.sh), 第 16 行.

当前执行形式为 `cargo --manifest-path ... test ...`, 但 `--manifest-path` 属于相关子命令的参数. 使用现有 Cargo 以相同顺序执行, 得到:

```text
error: unexpected argument '--manifest-path' found
tip: 'test --manifest-path' exists
```

本次验证的是实际 Cargo 参数解析, 没有声称已在 Ubuntu 执行 Bash 脚本. 参数问题已经足以使 README 提供的 Linux 入口失败.

简单改法是让脚本自身进入 crate 目录, 然后原样转发 Cargo 参数. 现有 `sdk/rust/cargo.sh` 已采用这个方式; 全仓脚本检索未发现其他同样的 `cargo --manifest-path` 调用.

后续状态: 在同日的 `verdandi` 可执行文件命名调整中, R3 已按此方式修复. 下述审查结论及其他问题仍对应原审查范围.

### R4. CLI 仍是交互式示例的生命周期

位置: [src/main.rs](src/main.rs), 第 60 行附近; [src/server.rs](src/server.rs), 第 38 行附近.

当前通过一行标准输入或 EOF 停止服务. 本次关闭进程标准输入后, 服务约 6 ms 即正常退出. README 对此有说明, 因而这是明确的原型行为, 但不适合直接作为守护进程的默认入口.

此外, CLI 等待 stdin 时没有同时观察根网络任务的终止结果. 根任务发生致命错误时, 进程可能仍停留在输入等待中. 当前也没有应用层的 Ctrl+C/SIGTERM 有序关闭路径.

发布前需要建立可等待的网络结束状态, 让 CLI 同时处理停止信号和致命网络错误, 并在所有出口执行清理.

### R5. 退避达到上限后抖动消失

位置: [src/server.rs](src/server.rs), 第 354-373 行.

先把 base 截断为 maximum, 再加非负 jitter, 最后再次截断为 maximum. 当 base 已达到 maximum 时, 所有 jitter 都被丢弃. 默认配置下连续失败达到第 7 次后, 所有后续间隔均固定为 5 秒.

因此不能声称长期故障下重试间隔仍保持节点间抖动. 应在上限内选择有效抖动区间, 并测试到达上限后的多个输入.

### R6. 现有测试有覆盖缺口, 且一个断言弱于测试名称

位置: [tests/network.rs](tests/network.rs), 第 44、117 行附近.

`mismatched_cluster_never_becomes_connected` 只等待 ConnectionRejected. `next_rejected` 会跳过其他所有事件, 包括本不该出现的 Connected. 如果实现先发 Connected 再发 Rejected, 这个测试仍可能通过.

应明确拒绝意外 Connected, 并补充以下行为测试:

- Hello 等待期间关闭, 以及关闭请求后的迟到 Hello.
- 帧长上限、截断、非法 UTF-8 和未协商正文.
- 入站容量耗尽和连接结束后的 permit 归还.
- 退避等待中的关闭、重连 generation 和连续启停后的端口释放.
- 根任务失败、调用方取消 shutdown future 后的任务归属.
- 两端启动脚本与非交互运行场景.

## 注释准确性与结构改进

详细中文注释应保留, 但需纠正超过实现保证的陈述:

- Hello 检查证明格式和集群声明一致, 无法认证真实身份. `validate_remote` 的“可信远端身份”容易让读者误认为已有认证.
- generation 当前由每个 Peer 实例自己的计数器分配, 并非所有 Peer 实例共享的进程全局计数. 尚无 session registry 来落实旧 generation 结果屏蔽.
- `JoinSet` 保证任务有归属, 不代表每条异常返回路径都已等待清理完成.
- “先编码完整帧”只保证本地校验失败前没有发送. TCP 写入中途仍可能失败, 不应解释为网络发送具有整帧原子性.
- 按 seed 地址串行拨号只保证同一 seed supervisor 不并发拨号, 不能代替按远端 peer ID 去重.

现有模块划分无需推倒:

| 模块 | 保留的职责 |
| --- | --- |
| main | 参数、进程退出和日志输出 |
| config | 静态配置和资源上限 |
| protocol | 帧编码与边界检查 |
| connection | 单条会话握手、读取与关闭 |
| server | 监听和任务监督 |

`server.rs` 同时容纳退避算法和 boot ID 生成, 但当前规模仍可阅读. 可以在后续独立测试或扩展时抽出纯退避函数, 无需立即增加 Transport trait、通用事件总线或多层 Manager.

## 证据与后续门槛

- 上轮已完成 fmt、严格 Clippy 和 3 项 localhost TCP 测试; 本轮未修改 Rust 源码, 未重复运行这些已通过的基线.
- 本轮新增验证为 Linux 脚本对应的 Cargo 参数解析、3 个有界进程/TCP 探针, 以及 Tokio 官方缓存源码核对. 所有探针进程和 socket 均已关闭.
- 本次只记录审查结果, 没有修复生产代码, 没有下载依赖.
- 仓库存在 MIT LICENSE, 但本次未找到常见平台的仓库 CI 配置. 当前证据不足以宣称 Peer 已通过 Windows/Linux、MSRV 与故障场景的自动化发布门禁.
- 未实现的身份认证、心跳、会话去重和协议冻结应按既定阶段推进. 当前应先关闭 R1-R6 中基础网络的问题, 再扩展数据同步, 避免把生命周期缺陷带入后续状态机.
