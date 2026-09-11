# Verdandi Peer

设计方向更新, 2026-09-11: 维护者选择将 Star/Planet 迁移到 C++26, 首阶段以 Linux/GCC 16.2 为目标,
Supervisor 保持 Go. C++ 骨架已获准在独立 [peer-cpp/](../peer-cpp/README.md) 实施, 依赖准备和独立验证正在进行.
下文仍描述现有 Rust 实现和有效构建入口, 不代表 C++ 已完成.
C++ 目标已确认采用逻辑会话、框架流控与标准 typed RPC 解码, 并记录 [依赖版本锁](cpp26-dependencies.md);
这些规则由新目录实现并独立验证, 下文仍保留 Rust 的物理连接/解码前限制.

Rust Star/Planet 共用 Common 网络核心, 实现角色准入, TLS 1.3 服务端验证, gRPC 账号准入与签名 bearer 凭证,
Star 双连接全互联, Planet 单上游和组内优先/跨组故障切换, Ping/Pong 与有界关闭.
二进制为 `peer.exe` / `peer` (Star) 和 `planet.exe` / `planet`.
完整的当前契约见 [基础连接规则](connection-rules.md).
它仍是 0.1.0 网络基础, Catalog/Registry 数据复制与业务持久恢复尚未实现.

## 启动

先由部署方准备身份目录: `ca.pem`, `cert.pem`, `key.pem`, `admission.pub`, `login.json`.
证书需要服务端用途和公布地址的 IP SAN, 不要求客户端用途或角色 URI.
`login.json` 仅含 `username` 和 `password`, 账号必须由 Supervisor 授权当前二进制角色.
`admission.pub` 是 Supervisor 的原始 32 字节 Ed25519 公钥.
所有节点的信任和群组策略一致, 每个部署使用独立证书和私钥.
身份目录不得使用仓库公开的测试私钥部署生产服务.

```powershell
./build/peer/target/release/peer.exe --listen=192.168.0.119:7443 --super=192.168.0.25:7442 --cluster=alpha --identity=identity
./build/peer/target/release/planet.exe --listen=192.168.0.119:7444 --super=192.168.0.25:7442 --cluster=alpha --group=east --identity=planet-identity
```

端口显式填写. 监听通配地址时增加 `--advertise=IP:PORT`. 同一账号可以运行多个端点, 同一账号及端点只保留一个当前进程.
`--id` 和 `--seed` 已移除. 每次 start 自动生成 UUIDv4, 重试和重连复用,
重启不从磁盘恢复旧 UUID. Linux SIGINT/SIGTERM 和 Windows Ctrl+C/Ctrl+Break 可正常关闭;
stdin EOF 不退出. JSON 日志输出 stdout. 正常/运行错误/参数错误退出码为 0/1/2.

## 加入与重连

1. 绑定 listener, 初始化期间拒绝互联.
2. 与 Supervisor 建立 TLS/gRPC, 账号登录取得端点代次, 提交 UUID 和公布地址.
3. Supervisor 原子提交成员并返回完整名单与签名准入正文. Peer 重试始终保留第一次 CAS 基线.
4. 收齐并验证名单后一次安装; Star 连接全部其他 Star, Planet 选择一个候选上游.
5. Hello 验证准入签名、角色和端点代次. 旧 Peer 只记录连接者自身并反向连接.
6. 每对 Star 保持两条 TLS/gRPC 会话. 断开不删除成员, 本地限速退避重连.

Star 没有 Discover, 第三方介绍, 周期拓扑查询或全局 revision. Supervisor 离线后已初始化进程继续互联;
新进程没有完整名单则等待. 每个进程重启仍要重新登记. 同一部署更高 epoch 取代旧实例,
取消其连接和拨号, 旧回调只删除自身 generation.
证书到期会限制新建 TLS 连接; 不承诺无限离线准入, 不包含在线吊销或自动轮换.
详细契约见 [协议](../proto/README.md) 与 [安全和测试边界](../service-admission-test-plan.md).

| 边界 | 默认值 |
| --- | --- |
| Star / Planet 成员上限 | 每种角色各 64, --max-peers 可设 1..4096; Star 含自身 |
| Planet 上游候选 / 活动连接 | 最多 8 / 1, 不随 --max-peers 扩大 |
| 工作线程 | 2, 可设 1..64 |
| TCP/DNS 连接期限 | 3 秒 |
| TLS + Hello 总期限 | 5 秒 |
| Ping 间隔 / Pong 总期限 | 10 秒 / 5 秒 |
| 全节点拨号节拍 / 并发 connect | 250 ms / 4 |
| 重连退避 | 100 ms..5 秒, 含抖动 |
| 入站预算 | 默认 128, 范围 1..65536; with_max_peers 设为上限的两倍 |
| Hello / 已建立控制消息上限 | 4096 字节 |
| 完整登记响应上限 | 2 MiB |

4096 是输入资源上限, 不是通过压力验证的推荐部署规模. 双连接 mesh 总数为 `N*(N-1)`.
`Peer::status()` 与 `connections()` 读取实际索引; `subscribe()` 是可丢失诊断事件.
`--status-interval-seconds=1` 可显式启用实际计数日志, 默认为零关闭.

## 构建与测试

普通构建只需已有 Rust 和锁定依赖, 不需要 protoc. 生成源码位于 `common/src/generated` 并随 schema 提交.
包装脚本将缓存限制在项目 `build/`, 不改变用户配置, 不自动下载.

```powershell
./peer/cargo.ps1 build --workspace --frozen --release --bins --jobs 2
./scripts/test-services.ps1
./scripts/test-services.ps1 -Mode soak -Duration 3600
```

Linux 使用 `bash peer/cargo.sh ...` 和 `bash scripts/test-services.sh --mode soak --duration 3600`.
项目内工具尚未进入 PATH 时, 只对本次命令指定
`env PATH="$PWD/build/tools/rust-1.98.1/bin:$PWD/build/tools/go-1.27.1/bin:$PATH" bash scripts/test-services.sh`.
一键入口先运行生成核对, 格式/静态检查, 单元与 TLS 网络测试和构建, 再运行真实 Go/Rust 进程矩阵.
`-SkipChecks` / `--skip-checks` 仅在已有检查与二进制仍匹配当前源码时使用.
当前整理与测试矩阵见 [骨架补强报告](service-hardening-20260911.md), 前一版 gRPC 结果见 [迁移验证](grpc-validation-20260911.md).

## 目录责任

- `common/src`: 通用配置, 身份, 登记与 gRPC 单连接生命周期; `app` 提供共用进程启动、CLI、日志和信号.
- `star/src`: Star 拓扑、拨号与根任务; `app` 组合 Star 进程.
- `planet/src`: 候选和唯一上游任务; `app` 组合 Planet 进程.
- 各 crate 的 `tests/unit`: 白盒与网络用例; `common/tests/support`: 共享网络夹具.
- `tests/fixtures`: 跨语言公开测试凭据和固定向量.

手写注释使用中文和 ASCII 标点, 关键代码块解释所有权与取消条件. 不创建空架构层.
