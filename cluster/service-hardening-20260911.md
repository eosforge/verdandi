# 服务骨架整理与测试补强

日期: 2026-09-11. 范围为 Rust Common/Star/Planet、Go Supervisor、服务检查入口和测试编排器.
本轮保持 gRPC v4 与既有组网规则, 不扩展 Catalog/Registry 业务协议、凭证寿命策略或 SDK 接入.
Windows、Ubuntu 原生与两端直连的回归及故障循环全部通过. 详细计数、验证边界与证据见下文.

## 结构与代码整理

| 改动 | 目的与边界 |
| --- | --- |
| Common `app/process.rs` 共用参数处理、runtime 创建和退出码 | Star/Planet 只传自己的 async 运行函数. 使用 `AsyncFnOnce` 静态调度, 不引入通用节点 trait 或装箱 future |
| Go `membership/validation.go` 统一名称、UUID 和规范地址校验 | RPC 与持久成员不再各维护一份地址规则 |
| 共用退避与代次测试归 Common | 不在 Star 中重复测试已移到 Common 的纯计算逻辑 |
| Go 旧帧读写实现改为 `legacy_test.go` | 历史向量保留, 旧 TCP 实现只参与测试, 不提供生产备用路径 |
| 成员写事务复用已校验的独立快照 | 每次新登记省去第二次全表读取、JSON 解码和校验. 保留 bbolt 同步提交、排序、容量检查和返回值独立所有权 |
| 修订帮助与源码注释 | 去掉进程私钥、自定义帧等旧说明; 中文注释使用 ASCII 标点, 解释取消与资源边界 |

未引入新第三方依赖, 未调整依赖版本, 未修改全局环境或自动下载策略.
事务整理减少了明确可见的重复工作, 本轮没有据此宣称吞吐提升百分比.

## 发现的问题与修正

| 类别 | 原问题 | 修正与验证 |
| --- | --- | --- |
| 配置边界 | Rust 部分时间与容量只校验非零, `Duration::MAX`、`usize::MAX` 等可能通过后触发 Instant/channel/semaphore panic | 所有网络时间统一限制 1 ms..24 h; 入站/诊断容量 1..65536, 并发拨号 1..4096. 参数在绑定与创建任务前验证 |
| 恢复策略 | gRPC 读取错误统一变为 InvalidData, Planet 可能隔离临时中断的候选; 登记取消状态也可能被当作永久失败 | 共用状态映射, 区分连接中断、取消、超时、限额、权限与协议错误, 丢弃远端正文与 details |
| 本地身份 | Supervisor 只核对证书与私钥是否匹配, 过期或不被自身信任根认可的证书仍可启动 | 启动时验证证书链、有效期和服务端用途. 目标主机名仍由客户端验证, 不用通配 bind 地址冒充 SAN |
| 取消与计算 | Go 账号计算名额空闲时, 已取消请求可能进入 KDF 并返回错误的认证分类 | 排队前后及固定 KDF 完成后检查取消, 不增加专用 goroutine |
| 输入处理 | Go JSON 对原始非法 UTF-8 可进行替换, 离线账号生成可能改变密码输入 | 先检查原始 UTF-8; 无效账号名称/角色在 KDF 前拒绝, 生成失败不输出部分账号 |
| 一致性防御 | 接收层未显式禁止同 principal 的更高 epoch 改换角色或端点, 可能破坏容量和部署身份约束 | Rust 拓扑与 Go 存储同时约束槽位角色/地址不可变. 正常账号登记已通过端点指纹约束地址, 这项是对内部与跨实现边界的补强 |

同类审查包括 Go Supervisor 的时间/连接上界, Rust Star 与 Planet 的公共取消路径, Go/Rust 的角色和代次约束.
也只读检查了 Redis SDK 的对应配置与错误路径: Go/Rust SDK 已有时间上下界, C# 经 C ABI 复用 C++ 配置验证;
这些 SDK 尚未接入本次服务 gRPC, 不存在同一 gRPC 错误分类路径. 本轮不修改或重跑 SDK.

## 新增与归位的测试

- 配置: 所有定时参数的零值、下界、上界、极端值, 容量构造边界和退避关系.
- 传输: 真实 TLS/HTTP2 中客户端或服务端所有者释放, 保留外层句柄也不能保住失去所有者的 I/O; 同一 socket 第二次 OpenSession 必须拒绝.
- 协议: 全部错误分类及敏感 details 丢弃; 未知 Protobuf 字段、畸形 varint/长度、深层 group 和 repeated 对象预算.
- 身份: Go/Rust 共同消费固定向量, 比较名称、IPv4/IPv6 规范地址、UUID 和 principal 摘要; 过期、异链、错误端点证书.
- 生命周期: Go Accept/Close 交错与并发 Close; Planet 被取消的 wait 不丢失 shutdown 能力; 根任务失败可观察; 旧租约不能清空新上游.
- 存储: 更高 epoch 不改变槽位角色/地址, 替换后排序及重开一致性, epoch 耗尽、单写者锁和返回快照所有权.
- 账号: 取消时名额空闲/已满, 非法配置、非法 UTF-8、输入/输出故障和不输出半条账号记录.
- 编排器: 临时身份副本隔离、进程名称不被覆盖、非法场景不启动程序、CLI 错误退出码与过时帮助信息拒绝.
- 覆盖引导 fuzz: Go 账号文件解析和持久 Member 解码. 常规 `go test` 执行种子; 有限时长 fuzz 由检查入口显式启用.

真实进程回归由 11 组扩为 13 组, 增加三种服务的 help/version/非法参数, 以及错误密码和非法 login.json.
每组都继续核对状态、失败退出和端口回收. 一键服务测试默认先运行 Python 编排器单测.

## 验证结果

两端使用同一份服务与测试源码, 186 个相关文件逐字节一致, 包含相关 schema、生成源码、锁文件和公开夹具,
不包含 Markdown、缓存或结果文件. 精确文件清单保存在本轮机器报告中.

功能测试源码集合 SHA-256: `15eb485f16d5bebf54fc2d4d4ffd70a5ccb99af93e55d1c86153a56bca9b4b28`.
最终格式版本 SHA-256: `b1cb51aa47500c13cc6528b3346256f09ffceddbbf41120e0855fc022085a5f8`.
两者仅在两个 Go 调用和三个 Rust 宏代码块的折行上不同, 去掉空白后逐字节一致,
Go scanner 另核对包含注释、字符串和自动分号的 token 序列一致. 格式版本重新核对两端一致并构建,
没有将同一测试结果伪装成第二轮测试. 原始/最终文件哈希均写入机器报告.

| 检查 | Windows x64 | Ubuntu x64 |
| --- | --- | --- |
| Rust 服务测试 | 53 通过: Common 26, Star 19, Planet 8 | 同样通过 |
| Go Supervisor | 44 个普通顶层测试通过, 另含 2 个 fuzz 入口的种子 | 同样通过 |
| 协议生成器 | 4 通过, 已提交生成源码无漂移 | 同样通过 |
| 隔离比较工具 | 14 通过 | 14 通过 |
| Python 编排器 | 28 通过 | 27 通过, 1 项 Windows 平台测试跳过 |
| 格式与静态检查 | Rustfmt、Clippy、Go tidy-diff/verify/vet、Rust 文档通过 | 同样通过 |
| Release 构建 | star / planet / supervisor 通过 | 同样通过 |
| Go race | 未运行, 未配置本轮 Windows cgo 环境 | 通过 |
| Python Black | 修改的源码检查通过 | 本机未安装, 同步源码由 Windows 检查并核对哈希 |

相较初次 gRPC 迁移, Rust 测试从 36 增至 53, Go 普通顶层测试从 32 增至 44, Python 从 24 增至 28.
Go 子用例未展开累加, 不把 fuzz 执行次数计作新增独立测试数量.

| Go fuzz, 每入口请求 10 秒 / 2 worker | Windows 执行次数 | Ubuntu 执行次数 |
| --- | --- | --- |
| 账号配置解析 | 85,013 | 39,962 |
| 持久 Member 解码与 round trip | 201,322 | 110,583 |

两端均无失败输入. 这些数值说明有限探索工作量, 不是输入空间覆盖率, 也不用于比较平台性能.
Rust 的未知字段与畸形编码 corpus 是确定性回归, 没有声称进行了覆盖引导 Rust fuzz.

| 真实进程部署 | 13 组回归耗时 | 故障循环请求 / 实际时长 | 完成重启轮数 |
| --- | --- | --- | --- |
| Windows 原生 | 30.592 秒 | 120 / 120.384 秒 | 17 |
| Ubuntu 原生 | 23.359 秒 | 120 / 124.197 秒 | 18 |
| Windows + Ubuntu 直连 | 28.810 秒 | 300 / 303.735 秒 | 42 |

循环每轮强制终止当前上游 Star, 观察 Planet 切换, 重启 Star 并等待全互联,
再强制终止/重启 Supervisor. 每次活动循环包含 4 个 Star 和 2 个 Planet.
循环时长不包含前置回归和最后清理, 因完成正在执行的一轮可能略超请求时长.
三种部署共完成 77 轮. Ubuntu 原生循环最低可用内存为 1052 MiB, 混合循环两端最低值为 1086 MiB,
均高于测试器 384 MiB 停止阈值; Windows 原生最低为 18329 MiB. 这是主机可用内存, 不是服务 RSS.

13 组进程场景如下, soak 模式也执行这些前置/收尾断言:

| 场景 | 核对内容 |
| --- | --- |
| CLI | 三个二进制的 help/version、非法参数及退出码 |
| 并发加入 | 三个 Star 同时加入后成员数与双连接索引吻合 |
| Supervisor 离线 | 已入网 Star 保持通信, 新进程等待 |
| Supervisor 恢复 | 持久成员重开, 等待进程完成登记并加入 |
| Star 重启 | 正常与强制退出后使用新 UUID/epoch 恢复 |
| 非法身份 | 不可信、过期或不匹配的身份不能初始化 |
| 非法登录 | 错误密码与畸形 login.json 拒绝启动, 日志不泄露测试密码 |
| 角色越权 | Star/Planet 不能以请求字段提升账号角色 |
| 组内候选 | Planet 优先本组, 不加入 Star 全互联 |
| 跨组切换 | Supervisor 离线时使用已有候选切换 |
| 新 Planet 等待 | 等待 Supervisor; 已连接健康上游的 Planet 保持当前会话 |
| 单上游 | Planet 状态与所有 Star 的实际入站计数交叉核对 |
| 清理 | 信号退出、进程回收、端口重新绑定和测试临时目录回收 |

本轮遇到的验证问题也保留边界: 新增 Python 用例最初错误地期待测试目录完全为空,
忽略已有 `.owner` 标记; 已将断言改为比较原目录快照后重跑通过.
Ubuntu 补充执行 Black 时报告模块不存在, 未下载工具; Rust/Go 完整检查此前已通过,
Linux Python 单测随后独立通过, 源码格式在 Windows 检查并通过两端哈希核对.

## 原始证据与清理

- [完整机器报告及源码清单](../testkit/results/service-hardening-20260911.json).
- Windows: [13 组回归](../testkit/results/service-hardening-windows-regression-20260911.json), [120 秒故障循环](../testkit/results/service-hardening-windows-soak-20260911.json).
- Ubuntu: [13 组回归](../testkit/results/service-hardening-linux-regression-20260911.json), [120 秒故障循环](../testkit/results/service-hardening-linux-soak-20260911.json).
- 跨主机: [13 组回归](../testkit/results/service-hardening-mixed-regression-20260911.json), [300 秒故障循环](../testkit/results/service-hardening-mixed-soak-20260911.json).

每份场景报告都记录进程回收、端口可重新绑定和自有临时目录移除.
最后只读核对确认两端没有残留的测试服务进程或测试临时目录, Ubuntu 本次启动以来的 `oom_kill` 为 0.
结果记录在机器报告的 `cleanup` 字段. 只回收测试自己创建的资源, 构建缓存和历史报告保留.
格式/静态检查、构建、race 和 fuzz 的完整本机日志保存在 `build/services-polish-*.log`.
既有 `service-grpc-20260911.json` 与 v3 性能记录保留, 本轮没有覆盖旧证据.

## 维护入口

```powershell
./scripts/check-services.ps1 -FuzzSeconds 10
./scripts/test-services.ps1
./scripts/test-services.ps1 -Mode soak -Duration 3600
```

```bash
bash scripts/check-services.sh --jobs=1 --fuzz-seconds=10
bash scripts/test-services.sh
bash scripts/test-services.sh --mode soak --duration 3600
```

Go fuzz 每入口时长允许 0..300 秒, 默认 0 只执行种子, 两个 fuzz worker 的缓存留在项目 GOCACHE.
Linux 默认包含 Go race. Windows 的 race 仍需已准备的 cgo 编译器和显式 `-Race`.
这些入口只用已经准备的依赖, 不进行下载. 工具在项目内时按各 README 给出的单次命令 PATH 使用.

当前仍是可测试的服务骨架. bearer 的过期/吊销、业务复制、数据持久恢复、Supervisor HA 和生产容量认证仍是明确的后续范围.
有限时长 fuzz 与分钟级故障循环不能证明不存在所有缺陷或已完成长期耐久性认证.
