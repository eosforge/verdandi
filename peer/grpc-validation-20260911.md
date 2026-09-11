# gRPC 服务迁移验证报告

日期: 2026-09-11. 结论: Supervisor、Star 和 Planet 的 gRPC 连接与账号准入骨架已完成, Windows、Ubuntu 和两端混合组网回归通过.
这项结论限于当前连接与准入功能, 不代表业务复制、长期耐久性或生产容量已经认证.

## 实现结果

- Supervisor 使用 Go gRPC `Admission.Challenge/Register`. 节点通过 TLS 提交账号密码, 获取独立签名的 bearer 凭证.
- 同一账号允许多个节点. 各进程有独立 UUID; 不同端点互不替换, 同端点重启通过 CAS 更新 epoch, 重试保持幂等.
- Star/Planet 使用 Rust Tonic `PeerTransport.OpenSession` 双向流. 生产路径移除自定义 TCP 分帧及 TLS exporter、进程签名私钥证明.
- 每对 Star 保留两条连接; Planet 保留一个活动上游, 最多 8 个候选, 本组优先、故障时允许跨组切换.
- Supervisor 离线时, 已运行节点使用已有凭证与名单继续工作; 新进程等待 Supervisor 鉴权.
- 保留 TLS 1.3、服务端证书验证、角色/Galaxy 校验、消息限额、队列背压、Ping/Pong 期限和自有连接清理.
- Protobuf 生成源码保存在服务源码目录. 普通构建离线使用生成结果, 显式生成检查负责发现漂移.

完整边界与账号配置见 [实现说明](grpc-implementation.md)、[连接规则](connection-rules.md) 和 [Supervisor 使用说明](../supervisor/README.md).

## 验证环境与结果

Windows 项目: `D:\projects\verdandi`. Ubuntu 项目: `/home/ubuntu/verdandi`, 地址 `192.168.0.119`.
混合测试直接连接 Windows `192.168.0.25` 与 Ubuntu. 本次不修改防火墙、系统环境或全局工具配置.
新增 Go 模块与生成工具按授权放入两端项目 `build/deps/go` 和 `build/tools/`; 其他工具优先复用已有缓存.
Rust 实际验证版本为 1.98.1, 项目声明最低 1.88. 最低版本本身未另行安装验证.

| 检查 | Windows | Ubuntu |
| --- | --- | --- |
| Rust 服务测试 | 36 通过: Star 20、Common 12、Planet 4 | 同左 |
| Go 服务测试 | 32 个顶层测试函数通过 | 同左 |
| 协议生成器测试 | 4 通过 | 4 通过 |
| 隔离传输探针测试 | 14 通过 | 14 通过 |
| Python 测试编排单测 | 24 通过 | 23 通过, 1 项平台跳过 |
| 格式、生成一致性、Clippy、Go vet、模块检查 | 通过 | 通过 |
| Release 服务构建 | 通过 | 通过 |
| Go race | 未执行, 当前未配置所需 cgo 环境 | 通过 |

上述测试数量分层列出, 不将 Go 子测试展开后与其他语言相加. Python 用于进程编排、同步和资源清理验证, 不是服务运行依赖.

| 部署方式 | 独立回归 | 回归耗时 | 故障循环实际时长 | 重启循环 | 循环期间最低可用内存 |
| --- | --- | --- | --- | --- | --- |
| Windows 原生 | 11/11 通过 | 28.284 s | 62.978 s | 9 | 18253 MiB |
| Ubuntu 原生 | 11/11 通过 | 21.866 s | 61.278 s | 9 | 923 MiB |
| Windows + Ubuntu | 11/11 通过 | 28.458 s | 63.536 s | 9 | 910 MiB |

每次故障循环运行 4 个 Star、2 个 Planet, 请求时长为 60 秒. 每次 soak 调用也先执行 11 组回归, 表中故障循环时长不包含前置回归.
内存为测试器采样到的系统可用量, 不是服务 RSS 或容量上限. Ubuntu 使用动态内存; 收尾时本次开机累计 `oom_kill` 为 0.

### 真实进程覆盖的 11 组场景

1. 三个 Star 并发启动并建立完整互联.
2. Supervisor 离线后已有网络继续工作, 新 Star 等待.
3. Supervisor 使用持久成员库重启, 等待节点随后加入.
4. Star 正常退出、强制退出及原端点重启.
5. 非法身份和证书输入拒绝加入.
6. Star/Planet 账号角色越权拒绝加入.
7. Planet 优先本组, 不加入 Star 全互联网络.
8. Supervisor 离线时 Planet 跨组切换.
9. 新 Planet 等待 Supervisor; 健康上游不因候选变化而切换.
10. 一个 Planet 仅保留一个活动上游.
11. 退出信号、进程回收和监听端口复用.

单元与网络测试另覆盖密码错误、同账号多端点、CAS 竞争、响应丢失、旧请求重试、凭证篡改、重复会话、消息和对象数量限制、过期 Pong 及取消.

## 审查与测试发现的修正

| 问题 | 修正与验证 |
| --- | --- |
| Go gRPC 停机时, 尚未完成 TLS 的连接不能仅靠 `Stop` 及时结束 | listener 跟踪原始 socket, 停机先关闭自有连接, 再等待 gRPC 和 handler. 慢 TLS 停机测试与真实进程清理通过. 同类检查确认 Rust 已有 I/O 取消所有者与任务回收; SDK 不走此 gRPC 路径 |
| 从 mTLS 改为 bearer 后, 节点自己的服务端证书不再通过旧客户端认证流程被提前验证 | 启动时显式验证证书链、有效期、用途和公布 IP SAN, 防止先登记再因不可用证书无法互联 |
| 仅限制响应字节数仍允许大量空 Member 导致对象分配放大 | Prost 解码前扫描并限制为 4096 个 Member, 解码后继续检查配置容量和 Planet 候选上限 |
| Linux race 下账号 KDF 放大测试耗时, 原先 4 次 RPC 共用 3 秒场景预算不足 | 场景预算调整为 15 秒, race 包超时改为 120 秒, 随后 Linux 完整检查与 race 通过. 生产每 RPC 5 秒期限未放宽 |

整理同时移除了生产自定义帧读写器, 将 TLS/gRPC 适配与取消集中在 Common, 通过生成服务契约减少手工调度逻辑.
历史 MessageID 表保留为兼容记录, 不参与 v4 消息分派.

## 清理结果与证据

六份真实进程报告全部记录: 进程已等待退出、端口可复用、自有临时目录已移除.
收尾再检查两端: 项目服务/探针进程为 0, `build/testkit/tmp` 自有临时目录为 0.
保留依赖缓存、构建产物、日志和报告用于复查, 不将它们当作测试资源误删.

- [Windows 回归原始结果](../testkit/results/service-grpc-windows-regression-20260911.json), [Windows 故障循环](../testkit/results/service-grpc-windows-soak-20260911.json).
- [Ubuntu 回归原始结果](../testkit/results/service-grpc-linux-regression-20260911.json), [Ubuntu 故障循环](../testkit/results/service-grpc-linux-soak-20260911.json).
- [混合回归原始结果](../testkit/results/service-grpc-mixed-regression-20260911.json), [混合故障循环](../testkit/results/service-grpc-mixed-soak-20260911.json).
- [汇总结果及清理快照](../testkit/results/service-grpc-20260911.json).

汇总中的相关源码清单指纹为 `208511f65c89c5dbdf7ef7d82293d09f3f3dd780b1761cbb34a3ecb8a2a38236`.
这是所列服务/协议/脚本范围的清单指纹, 不是 Git 提交号或整个仓库指纹. 工作区原有 SDK 改动保留, 本轮未扩展为 SDK 迁移或测试.
详细 Linux 日志保留在项目 `build/grpc-linux-check.log`、`build/grpc-linux-check-initial.log` 和 `build/grpc-linux-final.log`.

## 兼容性与明确限制

- **协议 v4 与 v3 不兼容.** Supervisor、Star、Planet 必须一起升级. 旧成员库严格拒绝读取; 为新版指定新库并重新登记, 保留旧库备份. 未认证将 v4 库交给旧版本回退的行为.
- bearer 凭证可由持有者重复使用. 当前没有独立 TTL 或在线吊销; 修改密码、删除账号不会立即撤销已签发凭证.
- 账号授权尚未包含每账号 IP 白名单或反向地址验证. 新进程的本地证书检查不是恶意客户端的地址所有权证明.
- Catalog/Registry 存储复制、Planet 业务转发、SDK Bind、Supervisor HA 尚未实现.
- 60 秒故障循环属于短期故障回归, 不能替代小时/天级长期测试. 测试入口继续支持配置时长.
- 本轮未重跑历史 660 组 v3 容量矩阵. [旧性能报告](grpc-benchmark-results.md) 的原始成功与失败结果保留, 不能作为当前 v4 的新吞吐或尾延迟结论.
