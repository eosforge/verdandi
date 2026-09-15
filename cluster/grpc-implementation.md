# gRPC 服务骨架与账号准入

> 当前实现以 [Orbit/Astra/Comet v1](../protocol-v1.md) 和 [身份契约](identity-contract.md) 为准. 下文保留设计演进记录, 不作为旧协议兼容要求.

> 2026-09-12: 旧 Rust 服务已废弃, 当前仅维护 C++ Star/Planet + Go Supervisor. 协议 v5、Supervisor 签发不透明 id 和重试规则以[身份与准入契约](identity-contract.md)为准. 下文旧 UUID/v4/Rust 对照描述保留为设计演进记录, 不再是当前实现要求.

日期: 2026-09-11. 功能已接入; 初次迁移通过两端与混合组网回归及 60 秒故障循环.
后续整理与测试补强单独记录在 [服务骨架报告](service-hardening-20260911.md), 不覆盖初次迁移证据.
本文件替代旧 v3 文档中的自定义 TCP 帧、mTLS 账号身份和 TLS exporter 规则.

后续 C++26 迁移已按 [设计稿](cpp26-skeleton-design.md) 在独立 [astra/](../astra/README.md) 实施和验证, 保留本文的 Rust 实现作对照.
新骨架以本文 v4 为兼容基准; C++ 的生命周期与资源约束需重新验证, 不沿用 Rust 测试成绩.
C++ 已确认调整物理连接映射、固定 64 KiB 窗口和名单解码前扫描要求, 详见设计稿第 13.1 节;
v4 字段与认证规则保持兼容, 本文的 Rust 实现及历史测试结论不因目标设计变更而改写.
本轮新增跨实现修复: 入站 OpenSession 在验证拨号方 Hello 之前不发送自身 bearer 凭证, 字段与 RPC 不变.
历史测试未覆盖匿名调用方获取服务端 Hello 的情况, 此项新增验证单独记入 C++ 迁移报告.

## 当前实现

| 部分 | 实现与责任 |
| --- | --- |
| Supervisor | Go gRPC `Admission.Challenge/Register`, 账号验证, bbolt 成员事务和 Ed25519 凭证签发 |
| Star / Planet | Rust Tonic `StarTransport.OpenSession`, 每条连接一个双向消息流 |
| Common | TLS 配置, 凭证验证, 有界 RPC, Ping/Pong, 取消和资源释放 |
| Star | 成员索引、每对 Star 两条连接、新成员反向连接 |
| Planet | 最多 8 个候选、一个活动上游、本组优先和跨组故障切换 |
| 生成源码 | `.proto` 显式生成到各服务 `src/generated` / `internal/generated`; 普通构建不调用生成器 |

gRPC 处理 HTTP/2 分帧、消息流、协议状态、流控和 Protobuf 编解码. 业务层仍管理连接生命周期、
准入、拓扑、截止时间及容量. Rust 保留一个小型 TLS 适配器以强制 TLS 1.3、h2 和取消实际 socket;
这不要求其他语言实现 TLS exporter 或直接操纵 HTTP/2.

生产 Star 已移除自定义 FrameReader/FrameWriter. 旧 MessageID 清单继续作为历史保留,
v4 不使用它调度. 旧 TCP 比较器留在隔离性能工具中, 不作为第二个生产后端.

## 登录与加入流程

1. 每次进程启动生成新 UUIDv4. 本地核对证书链、有效期、服务端用途和公布 IP SAN.
2. 通过 TLS 连接 Supervisor, 验证其证书和目标地址, 使用 `login.json` 的账号密码请求 Challenge.
3. Supervisor 验证密码, 返回这个账号及端点的当前 epoch. 本进程只保存第一次基线.
4. Register 再次提交账号密码、UUID、地址、role/group 和原 CAS 基线.
5. Supervisor 校验 Galaxy、角色、规范地址, 在一次 bbolt 事务中提交登记并取得完整快照.
6. 返回独立签名凭证. Star 同时收到完整 Star 名单; Planet 收到最多 8 个授权候选.
7. 节点先验证整份名单和自己的凭证, 再开始互联. Hello 携带原始 Member 编码和签名.
8. 对端验证签名、Galaxy、角色、期望候选和实例代次; 验证通过后才安装活动会话.

账号允许的角色来自 Supervisor `accounts.json`, 不再读取证书 URI 作为角色授权.
密码只提交给 Supervisor, 不进入成员表、Hello、公开状态或日志. Supervisor 只持久配置密码摘要,
使用 PBKDF2-HMAC-SHA256、600000 次迭代、16 字节随机盐和 32 字节摘要. KDF 最多并发 4 个,
未知账号也走相同 KDF. 此限制控制工作量, 不等于完整的账号锁定、审计或防爆破服务.

## 多节点与重启规则

账号不是节点 ID. 每个进程持有独立 UUID 和签名凭证, 同一账号的不同端点互不替换.
成员槽位的 `principal` 为 `SHA256(username + NUL + cluster_id + NUL + canonical_advertise)`.
用户名与 Galaxy 名不允许 NUL, 地址必须规范化, 避免组合编码歧义.

同账号、同 Galaxy、同端点的新 UUID 使用 CAS 递增 epoch 并替换旧进程; 原请求重试保持幂等.
旧请求不得重新获取更高基线抢回身份. 已观察到新 epoch 的节点拒绝更旧身份和过期退出回调.
同一端点不允许其他账号占用; 同一槽位也不能原地改变角色. 地址或用户名改变会创建新槽位,
旧槽位仍保留, 当前没有自动退休或删除接口. 这些属于当前实现策略, 并不赋予账号下节点相同业务所有权.

Supervisor 不在线时, 已取得名单与凭证的进程可继续互联; Planet 可以使用已知候选切换.
新进程仍须等待 Supervisor. 凭证没有独立 TTL 和在线吊销功能, 也没有持有者私钥证明:
**拿到完整 bearer 凭证的人可以使用它**, TLS 只保护传输. 修改账号密码或删除账号只阻止后续登录,
不会即时撤销已签发凭证. TLS 证书过期阻止新建连接, 不会自动中断已建立流.

账号授权不证明公布地址的所有权, 当前没有每账号 IP 白名单或反向地址验证.
完整名单门槛由正常节点启动流程保证, 凭证不能证明恶意客户端实际保存了名单.
Member 凭证绑定身份字段, Hello 的协议版本和容量是逐连接校验的协商字段, 不声称有额外签名覆盖.

## 边界与清理

| 资源 | 当前约束 |
| --- | --- |
| Star/Planet 控制消息 | 硬上限 4096 字节; 协商预算还会再次检查 |
| 应用发送队列 | 每会话 4 条; 入队成功不等于对端收到 |
| HTTP/2 接收窗口 | 固定 64 KiB 流窗口和连接窗口, 不开启自动增大 |
| 节点间 RPC | 每物理连接只允许一个 OpenSession |
| Supervisor 请求 / 响应 | 4096 字节 / 2 MiB |
| 响应对象预算 | Prost 分配前扫描并限制为 4096 个 Member; 解码后检查配置上限或 Planet 的 8 个候选 |
| Supervisor 连接 | 可配置总连接数; 每连接最多一个并发 RPC |
| Supervisor 时限 | TLS/HTTP2 建链 5 秒, 每 RPC 5 秒, 空闲 5 秒, 连接年龄 15 秒加 1 秒宽限 |
| Star 登记 | 本地总期限覆盖连接、Challenge 和 Register; 失败重试保留首次 CAS 基线 |
| 心跳 | 单方向最多一个待匹配编号; 绝对期限覆盖入队、实际传输和匹配 Pong |
| Star 时间配置 | 网络时间统一为 1 ms..24 h, 退避最小值不得大于最大值; 创建任务前拒绝极端输入 |
| Star 并发配置 | 入站连接 1..65536, 并发拨号 1..4096, 诊断事件队列 1..65536; 配置上界不是容量认证 |

普通输入、旧 Pong 和对端 Ping 不延长本端等待期限. 错误通知尽力发送, 随后关闭流,
不承诺关闭前一定刷到网络, 不对远端错误产生回声. 连接中断后由角色层重新拨号并验证身份.
gRPC 状态在 Common 统一映射: 中断、取消、超时与限额保持可重试类别, 权限和协议拒绝单独处理.
远端错误正文与 details 不进入本地诊断, Planet 不因临时传输错误永久隔离候选.

每条 Rust RPC 的实际 TLS I/O 有取消所有者. Go Supervisor 同时跟踪尚未完成 TLS 的原始 socket,
停机先关闭它们, 再等待 gRPC 和 handler. 测试证实仅调用 gRPC Stop 不足以立即结束慢 TLS 握手.
磁盘 fsync 仍受操作系统影响, RPC 截止时间不能强行中止已经进入内核的持久提交.

## 依赖与工具

- Rust: Tonic / tonic-prost / tonic-prost-build 0.14.6. 兼容下限调整为 Rust 1.88, 实际测试使用 1.98.1.
- Go: grpc 1.83.2, protobuf 1.36.12; 生成插件 protoc-gen-go-grpc 1.6.2, protoc-gen-go 1.36.12.
- protoc: 36.1, 仅显式生成与生成一致性检查需要.
- 全部缓存和新增工具都放两端项目 `build/`, 不修改用户或系统配置. 普通 wrapper 离线运行.
- C++/SDK 不在本轮变更范围. gRPC C++ 可选择 BoringSSL, 是否需要外部 OpenSSL 由构建方式决定.

## 兼容性和未实现范围

v4 的传输和签名域均改变, 不兼容 v3. 必须同时升级 Supervisor、Star 和 Planet.
旧证书指纹成员库包含被删除的 `public_key` 字段, 新版严格拒绝读取, 不自动覆盖或清空旧库.
部署时为新版指定新的成员库文件并让节点重新登记; 旧库备份保留供回退.
当前成员库不包含 Catalog/Registry 业务数据, 不声称这是未来业务持久化迁移方案.

Catalog/Registry 存储复制、SDK Bind、Planet 业务请求转发、在线撤销、自动证书/密码轮换、
Supervisor HA 和生产耐久性资格仍未实现. 此次完成的是连接与准入骨架.

## 验证

当前测试覆盖同账号多端点、密码错误、角色越权、非法地址、本机证书失效、凭证篡改、
CAS 竞争、响应丢失、重复会话、消息/对象/连接限额、过期 Pong、离线重连、候选切换和停机清理.
旧 v3 的 660 组性能报告保留为历史证据, 不计作新版认证的通过结果, 也没有重新跑一遍来夸大验证范围.
初次迁移明细见 [迁移验证报告](grpc-validation-20260911.md), 最新补强结果见 [服务骨架报告](service-hardening-20260911.md).
