# Star / Planet 基础连接规则

日期: 2026-09-11. 本文描述当前实现, 业务复制仍按 [星图设计](../galaxy-architecture.md) 后续推进.

C++26 目标按 [骨架设计](cpp26-skeleton-design.md) 在独立 [peer-cpp/](../peer-cpp/README.md) 实施和验证. 本文的角色、身份和连接规则作为迁移基准;
Cargo/Rust 路径与运行时说明仍是当前实现事实, 不能直接当作未来 C++ 构建方法.
C++ 迁移已获准用逻辑会话管理身份与代次, 底层连接交由 gRPC 复用/重建; 下文物理连接计数不再作为
C++ 验收公式. 对应流控与解码策略差异以骨架设计第 13.1 节为准, 本文保留当前 Rust 行为记录.

## 组织与角色

`peer/` 是一个 Cargo workspace, 共用锁文件和项目内构建目录:

| 目录 | 所有者 | 责任 |
| --- | --- | --- |
| `common/src` | `verdandi-peer-common` | 配置, 身份, gRPC, 准入, TLS/Hello, 保活, 退避及通用进程工具 |
| `star/src` | `verdandi-peer` | Star 成员索引, 全互联拨号, 入站会话, `peer` 可执行程序 |
| `planet/src` | `verdandi-planet` | 候选队列, 唯一活动上游, 故障切换, `planet` 可执行程序 |
| 各 crate 的 `tests/unit` | 对应 crate | 白盒用例, 不为测试扩大私有状态的可见性 |
| `common/tests/support` | 测试工具 | 有界 TLS 夹具, 不进入发布库 |
| `tests/fixtures` | 跨语言测试 | Go/Rust 共用公开身份和固定测试向量 |

依赖方向为 Star/Planet -> Common. Common 不认识具体拓扑, 会话完成认证后通过回调安装角色自有的 RAII 会话记录.
不复制 TLS、Protobuf 或保活实现; 不提前创建空的存储抽象层.

## 准入和配置

两种角色都需要 `--listen=IP:PORT --super=HOST:PORT --cluster=NAME --identity=DIR`.
通配监听另配 `--advertise=IP:PORT`. `--group=NAME` 默认 `default`, 长度 1..64,
允许 ASCII 字母、数字、下划线、连字符、点. Group 只表示入口偏好, 不表示业务 Zone 或权限.

角色由二进制决定, 必须属于 Supervisor 账号允许的角色. 节点在 `login.json` 配置账号密码,
Supervisor 在 `accounts.json` 保存加盐摘要和角色范围. 证书只承担 TLS 服务端身份验证.
同账号允许多节点, 不按用户名替换其他端点. 新进程生成 UUIDv4,
必须等待 Supervisor 准入; 磁盘中存在旧记录不允许复用旧进程身份.
登记首次取得的 CAS 基线在整个进程中保持不变, 丢失响应后的重试和候选刷新不递增准入 epoch.

线协议主版本为 4. 签名绑定 Galaxy、UUID、账号/端点指纹、地址、epoch、role 和 group.
凭证是可跨连接复用的 bearer credential, 不包含 TLS exporter 或额外进程密钥.
同账号同端点使用 CAS 替换重启实例, 不同端点互相独立. 旧 v3 成员库包含已删除字段,
新版拒绝读取, 不自动覆盖. 部署需指定新成员库文件, 保留旧库备份.
凭证无独立 TTL 或在线吊销; 账号密码更改不会撤销已签发凭证. 完整边界见 [gRPC 契约](grpc-implementation.md).

## Star 互联

Star 登记收到完整 Star 名单, 必须包含自身. 每个 Star 主动连接其他 Star,
每对节点最终保留两条 TCP 会话. 已认证的新 Star 到达后, 接收端只安装其自身记录并反向连接.
没有第三方介绍、周期对账或从 Planet 扩展 Star 名单.

| 本端 | 对端 | 本端方向 | 结果 |
| --- | --- | --- | --- |
| Star | Star | 入站或出站 | 接受 |
| Star | Planet | 入站 | 接受, 不反向拨号 |
| Planet | Star | 出站 | 接受, 必须匹配授权候选部署和地址 |
| Star | Planet | 出站 | 拒绝 |
| Planet | Star | 入站 | 拒绝 |
| Planet | Planet | 任意 | 拒绝 |

`--max-peers=N` 在 Supervisor 和 Star 中分别限制 N 个 Star 和 N 个 Planet.
Star 额度包含自身, Planet 不占用 Star mesh 名额; 总入站预算为 2N.
离线成员保留, 当前没有自动回收部署名额或管理删除接口.
Star 状态的 `members/inbound/outbound` 只统计 Star, `planet_inbound` 单独统计活动 Planet 会话.

## Planet 候选和换绑

两种角色共用 gRPC `RegistrationResponse`: Star 是完整 Star 名单, Planet 最多 8 个候选.
Prost 分配前按协议硬上限检查 repeated 数量, 解码后检查当前角色及配置预算,
并校验 Galaxy、角色、排序和 UUID/部署/地址唯一性.

Supervisor 尽量返回 4 个本组、4 个跨组候选, 不足时互相补足. 候选按进程 UUID 的哈希分散起点,
`candidate_round` 每轮向前移动 4 个位置; 最终应答仍按本组优先、UUID 排序.
轮次仅用于轮换候选, 不代表成员 revision、权限版本或业务顺序. 名单不是实时健康证明.
当前管理端仍从有界成员事务快照挑选候选, 不是按组建立专用索引; 小响应不等于常数时间查询.
大规模部署和集中故障下的管理端吞吐尚未压测, 不能将 4096 的配置上限当成推荐规模.

Planet 只有一个串行上游任务:

1. 优先尝试当前可拨号的本组候选, 无可用本组候选时尝试跨组入口; 同级顺序按进程加盐分散.
2. 每轮候选最多尝试一次, 防止慢失败入口饿死后面的候选. 每次拨号至少间隔 250 ms.
3. TLS/Hello 成功后才安装唯一活动上游. 旧流退出并释放记录后才拨号下一台.
4. 每个候选单独退避并加抖动; 稳定窗口从完成认证开始计算. 非法身份或协议错误隔离该候选.
5. 没有候选或当前候选全部尝试失败后, 最多每 5 秒尝试刷新下一批, 单次刷新总等待最多 5 秒.
6. Supervisor 暂时不可达时保留当前批候选并继续重试; 健康上游期间不查询新名单、不主动迁移.

刷新保持当前进程准入不变. 直接握手已观察到更高 Star epoch 时, 旧名单不得使其回退.
同一候选地址可以接受相同部署的更高已签名 epoch; 相同 epoch 必须逐字段匹配, 更低 epoch 拒绝.
已隔离记录不因原样刷新解除隔离, 更新的合法记录可以重新尝试.
离线故障恢复只覆盖已知且授权有效的候选. 未知地址、所有已知候选均失效或证书过期不保证恢复.

## 保活、边界和未实现部分

节点间使用 `PeerTransport.OpenSession` 双向 gRPC 流, 每个方向传输 `SessionPacket` oneof.
HTTP/2 分帧由框架负责. Star/Planet 复用 TLS、Ping/Pong 绝对期限、有界队列和取消清理.
每会话队列 4 条, 控制消息最多 4096 字节. 入队不表示送达, 匹配 Pong 才完成本轮保活.
正常关闭 join 所有自有任务; Drop/超时强制终止仅作为兜底. 测试进程与临时目录由 testkit 拥有并清理.

`initialized` 只代表完成准入, Planet `upstream` 只代表认证连接已经建立.
网络分区下旧 Star 可能暂时仍观察到旧半连接; 本轮没有实现业务归属 fencing 或全网即时撤销.
Planet listener 当前关闭不支持的入站流. 尚未实现 SDK 接入、请求转发、全量状态缓存、数据同步、
内存/落盘模式、Registry 重新登记及 Catalog 业务恢复, 不提供伪造的成功 ACK 或业务 Ready 状态.

## 检查入口

Windows: `scripts/test-services.ps1`; Linux: `bash scripts/test-services.sh`.
长时故障循环使用 `-Mode soak -Duration 3600` / `--mode soak --duration 3600`.
先执行全 workspace 格式、Clippy、单元/网络测试、文档和 release 构建, 再运行真实 Go Supervisor 与 Rust Star/Planet.
Linux 默认包含 Go race; Windows 未默认启用 race. 一键服务测试复用已有工具和依赖, 不下载软件.
旧 v3 结果保留在 [历史验证](star-planet-validation-20260910.md); 当前实现见 [gRPC 说明](grpc-implementation.md), 执行结果见 [v4 验证报告](grpc-validation-20260911.md).
