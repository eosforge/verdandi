# Verdandi Supervisor

当前准入为协议 v6, 单次 Register 完成账号认证和持久幂等登记, Supervisor 签发不透明 `id` 与 Hello 准入凭证. 不再使用启动票据, 详见[身份与准入契约](../cluster/identity-contract.md).

Go Supervisor 提供管理 HTTP 和独立 gRPC/TLS 准入入口, 通过账号密码授权节点,
用 bbolt 持久保存成员, 签发每进程独立的 Ed25519 bearer 凭证. Star 获得完整 Star 名单,
Planet 获得最多 8 个候选. 同账号可运行多节点, 不承担节点间数据转发.
完整规则见 [gRPC 契约](../cluster/grpc-implementation.md) 和 [连接规则](../cluster/connection-rules.md).

## 构建与启动

```powershell
./supervisor/go.ps1 build -trimpath -p 2 -o ../build/supervisor/supervisor.exe ./cmd/supervisor
./build/supervisor/supervisor.exe --listen=127.0.0.1:8080 --star-listen=192.168.0.25:7442 --cluster=alpha --identity=identity --members=build/supervisor/members-v6.db
```

Linux 使用 `bash supervisor/go.sh ...` 和无 `.exe` 的二进制. 数据库父目录需预先存在.
普通命令离线使用项目 `build/deps/go`, 不改变全局设置. Go 下限 1.27.0.

身份目录包含 `ca.pem`, `cert.pem`, `key.pem`, `admission.key`, `admission.pub`, `accounts.json`.
TLS 证书具备服务端用途, SAN 匹配节点配置的 Supervisor 主机名或 IP; TLS 1.3/h2,
无需客户端证书. 准入私钥为 Ed25519 PKCS#8 PEM, 公钥为原始 32 字节, 每份配置最多 16 KiB.
与 C++ gRPC/BoringSSL 互通时, TLS 证书采用 ECDSA P-256 / SHA-256; 准入签名仍使用 Ed25519.

## 账号配置

`accounts.json` 是最多 64 项的数组. 每项包含 `username`, `salt`, `hash`, `roles`.
roles 可为 `["star"]`, `["planet"]` 或两者. 同一 Supervisor 只授权启动参数指定的 Galaxy,
group 仅是入口偏好, 不授予业务权限. 不使用证书 URI 绑定账号或角色.

用 `supervisor --make-account` 从 stdin 读取以下结构, 向 stdout 输出可加入数组的一项:

```json
{"username":"service-nodes","password":"replace-with-a-deployment-secret","roles":["star","planet"]}
```

此模式不监听端口、不写账号文件. 部署时通过受保护的 stdin 管道提供输入, 不将真实密码放进
命令参数或 shell 历史. 输出采用 PBKDF2-HMAC-SHA256/600000 次迭代和随机 16 字节盐,
不包含明文密码. 节点身份目录的 `login.json` 保存相应 username/password.
配置、密钥和目录权限由部署账户管理. 仓库夹具中的所有密码和私钥都公开, 只能测试.

## 参数与边界

| 参数 | 默认值 | 作用 |
| --- | --- | --- |
| `--listen` | `127.0.0.1:8080` | 管理 HTTP |
| `--star-listen` | 空 | gRPC/TLS 登记地址, 空为仅管理模式 |
| `--cluster`, `--identity`, `--members` | 空 | 登记启用时全部必填 |
| `--max-members` | 64 | 每种角色各自的持久成员上限, 1..4096 |
| `--max-startups` | 1,048,576 | 每 Galaxy 累计启动记录预算, 1..16,777,216; 满时拒绝新启动, 不驱逐旧记录 |
| `--max-connections` | 128 | HTTP 与登记端口各自的连接上限 |
| `--shutdown-timeout` | 5s | 管理 HTTP 排空期限 |
| `--log-level` | INFO | slog 等级 |

请求最多 4096 字节, 响应最多 2 MiB, 每连接最多一个并发 RPC. 建链期限 5 秒, 每 RPC 5 秒,
空闲期限 5 秒, 最大连接年龄 15 秒加 1 秒宽限. 密码计算最多并发 4 个.
停机关闭所有已接受的原始 socket, 包括尚未完成 TLS 的连接, 再等待 gRPC 和 handler.
已经进行的数据库 fsync 不能被网络超时强行中止.

## 身份、恢复与兼容性

成员 principal 由已认证账号、Galaxy 和规范端点构造, 同账号不同端点独立.
同端点的全新启动按事务顺序替换并递增 epoch; 原请求被替换后永久拒绝.
客户端不提交 CAS 基线, 幂等记录由 Supervisor 持久维护, 重启后仍有效.
断线不删除成员. 用户名/地址改变会创建新槽位, 当前没有在线退役和删除接口.

v6 不兼容 v5 两阶段准入及更早协议. 现有 v5 的 id 成员记录可保留, 新登记创建私有启动索引;
索引创建后不能降级给旧 Supervisor. 含旧 peer_id/public_key 的数据库仍拒绝, 不自动删除旧库.
一个库由文件锁限制为一个 Supervisor 写入. 启动索引满后可提高 --max-startups, 不自动回收历史请求.
账号删除或密码变更只影响后续登录, 不会即时撤销已签名 bearer 凭证.
凭证无独立 TTL, 不提供在线吊销、轮换或 Supervisor HA; 业务数据恢复尚未实现.

`GET /healthz` 只报告管理 HTTP 可响应, 不是全群 Ready. `--super` 应连接登记端口.
Admin 拓扑接口、Catalog Publisher 和业务持久化仍待实现.

## 组织与验证

`cmd/supervisor` 管 CLI/信号; `app` 管生命周期; `admission` 管 gRPC、账号和签名;
`membership` 管事务; `generated` 保存禁止手改的生成源码; `management` 管 HTTP.
`membership/validation.go` 由持久层和 RPC 共用, 避免身份输入规则漂移.
旧 `internal/protocol` 全部为 `_test.go`, 仅保留历史帧向量测试.
依赖锁在 go.mod/go.sum, 包括 grpc、protobuf、bbolt 和 x/net. 新下载仍需具体授权.

Windows 检查 Go: `scripts/check-services.ps1 -Service supervisor`; Linux Go/C++ 回归: `bash scripts/test-services.sh`. 旧 Rust 服务已废弃.
可选长时模式 `-Mode soak -Duration 3600` / `--mode soak --duration 3600`, 自动清理资源.
Linux 检查默认包含 Go race, Windows race 需要已有 cgo 编译器和显式开关.
有限 fuzz: `scripts/check-services.ps1 -Service supervisor -FuzzSeconds 10` 或 `bash scripts/check-services.sh --fuzz-seconds=10`.
当前整理和完整测试范围见 [骨架补强报告](../cluster/service-hardening-20260911.md).
