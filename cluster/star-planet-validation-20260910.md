# Star / Planet 基础骨架验证

日期: 2026-09-10. 结论: 本轮连接功能的 Windows、Ubuntu 和两端混合回归通过.
这不是业务数据复制或生产容量认证. 当前规则见 [连接规则](connection-rules.md),
机器可读证据见 [JSON](../testkit/results/star-planet-foundation-20260910.json).

## 本轮实现

- `cluster/common`, `cluster/star`, `cluster/planet` 共用 Cargo workspace、锁文件和帧/身份/保活实现.
- Star 保持双连接全互联; Planet 独立准入, 只保留一个活动上游, 不扩展 Star mesh.
- 协议主版本 3, 签名绑定 role/group, 独立候选响应 MessageID 10; 既有消息编号不变.
- 最多 8 个候选, 本组优先、跨组备用、失败后轮换, 没有健康连接时才刷新.
- Supervisor 离线时已有 Planet 继续切换; 新进程等待准入, 每次重启生成新 UUID.
- PS/Bash 一键检查现在覆盖整个 Rust workspace 和两个 release 可执行程序.

共用层不依赖任何具体拓扑. Star 用短锁索引, Planet 用一个串行上游任务和每候选少量退避状态,
不为每个 Planet 候选创建独立拨号任务. 不提前实现业务存储接口或独立转发状态机.

## 执行结果

| 验证 | Windows x64 | Ubuntu x64 |
| --- | --- | --- |
| Protobuf 生成新鲜度, 4 项 MessageID 生成器测试 | 通过 | 通过 |
| Rustfmt, 全 workspace Clippy, 文档及 release 构建 | 通过 | 通过 |
| Rust 37 项测试: Star 23, Common 10, Planet 4 | 通过 | 通过 |
| Go 格式、模块核验、tidy diff、vet 和各测试包 | 通过 | 通过 |
| Go race | 未执行 | 通过 |
| Python Black 与语法检查 | 通过 | 同一份已核验源码 |

Windows 在完整检查后增加慢候选用例, 再运行全 workspace Clippy 与全部 Planet 测试;
最终 Supervisor 修改后重跑其完整检查. Linux 最后执行完整 `check-services.sh --jobs=1`.
全部构建使用 frozen/offline 项目缓存, 没有新增下载、全局工具配置或防火墙改动.

真实进程矩阵每次使用 4 个 Star、2 个 Planet 和独立 Supervisor, 公共测试凭据不安装到系统信任库:

| 环境 | 最终回归耗时 | 60 秒故障循环实际时长 | 重启轮数 |
| --- | ---: | ---: | ---: |
| Windows 本机 | 22.748 秒 | 61.064 秒 | 10 |
| Ubuntu 本机 | 18.866 秒 | 60.000 秒 | 10 |
| Windows / Ubuntu 混合 | 22.414 秒 | 63.042 秒 | 10 |

每种循环此前都完成同一套回归; 表中的最终回归在最后一处旧格式解码修复后重新执行.
循环按完整故障周期收尾, 因而实际时长可以略超过配置值. Linux/混合循环最低可用内存为 1146 MiB.
三种环境均确认进程退出、端口复用和自有临时目录清理. 60 秒只是短时故障循环, 不代表数小时或生产长期稳定性.

核心场景覆盖:

- 并发 Star 准入、完整网格、Supervisor 持久成员表重启, 旧成员重连和新成员等待.
- Star 正常/强制退出与新进程替代, 保持每对两条连接.
- 无效、过期、错误 Galaxy 和角色冒充证书拒绝.
- Planet 本组选择、Supervisor 与主用 Star 同时离线后的跨组切换.
- 新 Planet 在 Supervisor 离线时等待, 管理端恢复后加入; 健康上游不会因本组恢复被迁走.
- Star 实际入站索引与 Planet 单上游状态交叉核验.
- 空候选等待并刷新, 两个慢 TLS 失败候选不能饿死跨组可用 Star.
- 候选轮次最终覆盖测试集合, 候选数量/顺序/身份唯一性, repeated 对象解码前限额.
- 当前候选接受同部署的更高已签名 epoch; 旧会话证明不能跨 TLS 连接重放.
- 截断帧、未知/退役 MessageID、超限、半帧读取、Ping/Pong 绝对期限和取消清理.

## 修复及跨语言传播审查

| 问题 | 修复与验证 | 传播范围 |
| --- | --- | --- |
| TLS 握手耗时被计入稳定连接窗口 | Star/Planet 均在认证并安装会话后开始计时; 连接/退避和故障测试通过 | Rust 两种角色已修正. Go Supervisor 只接收入站且有固定总期限, 无该稳定窗口 |
| 慢本组候选不断变为可重试, 饿死跨组候选 | 每轮每候选只尝试一次; 真实 TCP 黑洞用例通过 | Planet 特有. Star 各候选独立任务, Go Supervisor 只选择名单 |
| 双端登记异步完成导致测试误判 | 测试等待网格和两端实际连接索引, 不以一端 start/握手完成代替收敛 | Rust 夹具已修正; Python 矩阵同样检查最新状态与两端计数 |
| 空值/null 被误当作旧成员格式 | Go 只在 role/group 字段真正同时缺失时迁移为 Star/default; 缺一项、空值/null、未知值都拒绝 | 仅 Supervisor bbolt JSON 读取. Rust 不读取该库, Member 校验已拒绝未指定角色/空组 |

审查路径包括 `cluster/star/src/server.rs`, `cluster/planet/src/server.rs`, `cluster/common/src/{connection,registration,identity}.rs`,
`supervisor/internal/{admission,membership}`, 以及 `testkit/services.py`.
对现有 Redis SDK, 核对了 Rust `sdk/rust/src/redis.rs` 的命令期限、Go Registration 执行路径、
C++ `src/driver.cpp` / `src/internal/retry.hpp` 和 C# `Internal/SafeHandles.cs` 的共享核心边界.
这些路径不实现 Star/Planet 候选或 Supervisor 成员数据库迁移, 无需复制本轮修复.
C ABI、Legacy、C# 仍复用 C++ 核心; Lua 不实现连接调度. 本轮没有重新执行整套 Redis SDK 回归.

## 同步与证据边界

Ubuntu 先前部分服务源码通过独立快照部署, 未在统一同步记录内. 本轮先核对旧实现并保存完整备份,
再纳入哈希差异同步; 修改和移动的旧文件仍保存在项目 `build/testkit/sync-history`.
最终服务范围共 121 个非 Markdown 源码/配置/夹具文件, SHA-256 聚合指纹:

`bc0220e58bc2cb5be8dd4f154ea1f63906d1ac9b7b8eb0d87f325f029031da26`.

待后续设计和实现: SDK 接入、Publisher/Registry 请求转发、缓存与数据同步、业务持久化、
换绑重新登记及旧业务归属 fencing、混合持久确认、实时吊销/轮换、生产负载和长时压力认证.
Planet 的 `initialized/upstream` 仅描述准入和连接, 不表示业务 Ready.
