# Supervisor 登记与 Peer 认证组网验证

日期: 2026-09-10. 本文记录当前服务网络层, 不代表业务复制或生产容量认证.
实现完成后先整理代码和测试用例, 再执行以下检查. 未修改 Redis SDK 协议或 Admin 前端.

## 当前实现

- Protobuf 与自动 MessageID 的生成源码分别随 Peer/Supervisor 源码提交, 普通构建不调用 protoc.
- Supervisor 使用 mTLS 授权群组和公布 IP, bbolt 事务提交成员后返回完整快照和签名准入.
- Peer 每次启动生成 UUIDv4/进程密钥. 原始 CAS 基线跨登记重试固定, 旧请求不能覆盖新实例.
- Hello 验证证书指纹, 原始准入正文签名以及 TLS exporter 绑定的进程私钥证明.
- 完整名单一次安装后开始有界拨号. 旧 Peer 只学习直接连接者, 反向连接形成每对两条 TCP.
- Discover 和周期拓扑列表查询已删除. Supervisor 离线后现有进程继续保活和重连.

## 质量门槛

| 检查 | Windows x64 | Ubuntu x64 |
| --- | --- | --- |
| 协议生成无漂移, MessageID 兼容测试 | 通过, 4 项 | 通过, 4 项 |
| Rustfmt, Clippy -D warnings, 文档和 Release 构建 | 通过 | 通过 |
| Rust Peer 单元/真实 TCP/TLS/CLI | 通过, 29 项 | 通过, 29 项 |
| Go 格式, tidy -diff, mod verify, vet, 原生构建 | 通过 | 通过 |
| Go 随机顺序测试 | 通过, 24 个顶层组 | 通过, 24 个顶层组 |
| Go race | 未执行, 当前未配置 Windows cgo | 通过 |
| 真实 Go Supervisor + Rust Peer 进程回归 | 通过 | 通过 |
| Ctrl+Break / SIGTERM, stdin EOF, 进程退出与端口复用 | 通过 | 通过 |
| 本机故障循环 | 通过, 63.084 秒, 10 轮 | 通过, 60.739 秒, 8 轮 |
| Windows/Ubuntu 混合节点 | 通过, 两台各 2 Peer, 六组回归及 60 秒/13 轮故障循环 | 同一混合运行通过 |

测试配置: Rust 1.98.1, Go 1.27.1, protoc 36.1, protoc-gen-go 1.36.12.
最多两个编译任务; runtime 每 Peer 默认两个工作线程. 不使用 Redis 或 Docker.

## 覆盖范围

| 类别 | 实际断言 |
| --- | --- |
| 格式/兼容 | 固定 Ping 字节向量, 全部截断位置, 未知/退役 ID, 畸形消息, 超长头, 短写与零写 |
| 准入 | 不可信/过期/错误群组证书, 公布 IP 不在 SAN, 错误 UUID/公钥, 签名和正文篡改 |
| 会话身份 | 不同证书指纹, 不同 TLS exporter, 未持有进程密钥, 修改 Hello 协商字段均拒绝 |
| 名单 | 缺少自身, 重复 UUID/身份/地址, 错误群组, 非规范地址, 超容量, 截断响应均不部分初始化 |
| 持久登记 | 并发快照严格排序且完整, 重试幂等, 重开数据库, CAS 冲突, 旧请求迟到, 损坏记录失败 |
| 重连 | Supervisor 离线时切断透明 TCP 中继, 同 UUID 以新 session generation 恢复 |
| 进程替换 | 正常退出和强制终止后新 UUID, 相同部署名额复用, 旧任务/回调不删除新实例 |
| 保活 | 半帧跨越 Ping 定时器, 过期 Pong/对端 Ping 不延长本端期限, 错误不回声 |
| 有界资源 | 入站名额满时拒绝更多工作, 释放后恢复, 慢 TLS/半 Hello 不能阻塞关闭, 解码前限制成员数量与消息阶段 |
| 跨语言系统 | 并发三节点 mesh, Supervisor 离线时新节点等待, 重启后第四节点加入, 每节点三入三出 |
| 清理 | 只停止自有 PID/Job/进程组, join 输出线程, 验证端口重绑, 删除自有临时数据库目录 |

计数按测试函数/顶层组统计, 循环内边界和子用例未另行夸大计数.
实际状态从 Peer 索引周期读取, 测试要求读取新的状态样本, 不靠 Connected 日志累加推断全网.

## 发现并修正的问题

1. Go 持久成员校验曾允许 IPv6 zone 与 IPv4-mapped IPv6, Rust 会拒绝.
   统一为规范具体单播地址, 增加两端对应边界用例.
2. Supervisor 收到 ProtocolError 曾会回复另一个错误. 已与 Rust Peer 对齐为直接关闭, 增加无回声用例.
3. 初次 Rust 测试夹具以仅接受 PKCS#8 v2 的入口读取 v1 公钥缺省格式,
   导致签名失败并连带网络测试超时. 改为支持 v1 的入口且显式核对独立公钥;
   Go 侧已有私钥/公钥一致性核对. 生产 Rust 新进程密钥仍使用生成的 v2 格式.
4. Windows 编辑后的 Bash wrapper 曾带 CRLF. 已恢复 LF, Linux 后续门槛通过.
5. Windows 信号测试助手初次附着控制台被拒绝. 现由独立助手脱离自身控制台后,
   只附着当前测试子进程的独占隐藏控制台发送 Ctrl+Break. 真实两个服务均通过正常退出.
6. 生成器不再忽略 schema 目录读取错误, 并拒绝生成目录中多余旧文件.
   Supervisor 关闭成员库的失败也向根调用者返回, 不静默吞掉.
7. 仅限制 Protobuf 帧长度仍可能解码出大量空成员对象. Peer 现在先扫描顶层成员字段,
   在对象分配前检查数量; Supervisor 在读取正文前拒绝当前阶段不允许的消息编号.
   两端新增对应回归, 修改后完整一键检查及真实进程回归再次通过.

这些问题审查了 Rust/Go 对应路径. Redis SDK 尚未接入服务端新协议, 没有相同消息分派或成员表路径.

## 限制

- 成员默认 64, 最大允许 4096; 本次真实进程矩阵只运行 4 Peer, 不是 4096 节点压力资格.
- 准入无独立期限; 新 TLS 连接仍受证书有效期约束. 不实现在线吊销或自动证书轮换.
- 部署身份是叶证书指纹; 证书轮换需要显式迁移/退役机制, 当前不自动回收名额.
- 更高 epoch 隔离已观察到的新实例, 不是跨分区即时全局撤销旧进程权限.
- Supervisor 是单写成员库, 不包含 HA 共识或回滚旧备份后的安全恢复.
- Peer 全群重启且 Supervisor 离线时不能重新登记. Catalog/Registry 业务复制和权威持久恢复未实现.
- 签名证明进程获准登记, 不证明恶意客户端实际保存了全份名单.
- 网络总期限不能强行取消操作系统中已经发生的数据库 fsync.

## 复现入口

Windows: `scripts/test-services.ps1`; 长时: `scripts/test-services.ps1 -Mode soak -Duration 3600`.
Linux: `bash scripts/test-services.sh`; 长时加 `--mode soak --duration 3600`.
两个入口默认先检查并构建, 不下载依赖. 已验证源码和二进制未变化时可显式跳过检查.
混合节点: `scripts/test-services.ps1 -Address 192.168.0.25 -RemoteConfig build/testkit/services-remote.json`.
SSH 凭据仅在忽略目录中, 不随本文发布. 远端先准备同一版本的源码、工具和二进制.
结构化结果保存在 `build/testkit/results/services-*.json`; 执行失败和通过均保留记录.

## 运行证据

可提交的脱敏汇总见 [结构化验证记录](testkit/results/service-admission-20260910.json).

| 场景 | 结果文件 | 观察 |
| --- | --- | --- |
| Windows 最终回归 | `build/testkit/results/services-1788974957473047500.json` | 14.312 秒, 六组场景通过 |
| Ubuntu 最终回归 | `build/testkit/results/linux-services-1788975014155870225.json` | 10.504 秒, 六组场景通过, 从 VM 复制的原始结果 |
| Windows 故障循环 | `build/testkit/results/services-1788973898255982800.json` | 循环 63.084 秒, 10 轮, 最低可用内存 12344 MiB |
| Ubuntu 故障循环 | `build/testkit/results/linux-services-1788973900754705859.json` | 循环 60.739 秒, 8 轮, 最低可用内存 6753 MiB |
| 混合直连初次尝试 | `build/testkit/results/services-1788974036571231900.json` | Windows 单节点已登记, Ubuntu 未连接到登记端口, 测试停止并清理 |
| 混合直连最终回归与故障循环 | `build/testkit/results/services-1789007545576233300.json` | 六组回归通过, 循环 60.000 秒/13 轮, 两端最低可用内存 6753 MiB, 总计 75.955 秒 |

Duration 限定故障循环窗口, 已开始的一轮收敛会完成后再停止, 初始化和清理另计时间.
两次各自主机的故障循环在最后的解码数量/消息阶段限制之前执行; 修改后两端重新通过完整检查和进程回归,
并在取得临时入站规则授权后通过上述混合回归和 60 秒故障循环. 完整检查日志位于
`build/testkit/windows-services-final.log` 与 `build/testkit/linux-services-final.log`.
混合运行日志为 `build/testkit/mixed-services-final.log`. 测试通过端口重绑断言,
最终独立检查未发现两端残留测试服务或自有临时目录. Ubuntu 可用内存 6747 MiB, swap 使用为 0.

维护者已明确授权本次临时入站规则. 管理员助手 `scripts/test-firewall.ps1` 仅为两个构建程序放行
本机 192.168.0.25 与远端 192.168.0.119 之间的 TCP 入站. 测试结束后删除两条自有规则,
并独立查询 ActiveStore 确认无自有规则残留. 回执 `build/testkit/firewall-ab5d4456.json`
状态为 `removed`, `remaining_rules` 为空, `error` 为 null. 助手也已退出.
未关闭防火墙或修改已有规则. 此助手不属于默认一键测试自动操作, 本次授权不扩展到未来系统改动.
