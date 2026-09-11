# Peer / Supervisor 服务基础规范

目标更新: Star/Planet 计划迁移到 C++26, Supervisor 保持 Go, 详见 [第一阶段骨架设计](peer/cpp26-skeleton-design.md).
独立 [peer-cpp/](peer-cpp/README.md) 已进入实现与验证. 以下 Rust 工具、入口与测试状态仍描述保留的 Rust 实现.
C++ 的逻辑会话、流控、解码和线程策略以骨架设计中已确认的取舍为准; 来源版本见
[C++ 依赖锁](peer/cpp26-dependencies.md), 不将目标规则当作当前 Rust 行为.

当前传输与准入契约已升级至 v4, 以 [gRPC 说明](peer/grpc-implementation.md) 为准.
新版已通过两端与混合组网验证, 见 [v4 报告](peer/grpc-validation-20260911.md).
最新结构整理、边界修复及补强测试见 [服务骨架报告](peer/service-hardening-20260911.md). 历史验证保留原时点范围.


本规范约束当前开源基础骨架. Rust Peer 负责认证网络会话, Go Supervisor 负责登记与管理入口.
这两项服务仍是 0.1.0 开发骨架, 已接入成员登记和持久成员表, 业务复制与生产容量认证仍未完成.
协议目标以 [Peer Core](peer/core-design.md) 和 [Supervisor 设计](peer/supervisor-design.md) 为准.
Galaxy / Peer 恒星 / 全量授权缓存 Relay 行星 / SDK 卫星的最新角色关系见 [星图结构修订](galaxy-architecture.md).
Star/Planet 角色准入、分组候选和单上游故障切换已实现, 见 [连接规则](peer/connection-rules.md).
可选存储模式、Star 混合持久确认与 Planet 业务重新登记仍为设计内容; 新进程仍须等待 Supervisor 鉴权.
Admin 的旧展示模型尚未替换.

## 组织与依赖方向

| 层 | Peer | Supervisor | 责任 |
| --- | --- | --- | --- |
| 可执行入口 | `star/src/main.rs`, `planet/src/main.rs` | `cmd/supervisor/` | 退出码, 版本与进程组合 |
| 进程组合 | 各角色 `src/app/`, 通用工具在 `common/src/app/` | `internal/app/` | 配置, 生命周期, 日志及有界关闭 |
| 网络/管理 | `common/src/connection.rs`, `rpc.rs` | `internal/admission/`, `internal/management/` | gRPC 会话、准入和 HTTP 管理, 不承担发布者业务 |
| 成员与连接 | Star topology 短锁索引, Planet 单上游任务 | membership bbolt 事务 | 进程代次和会话代次分离 |
| 共享契约 | `proto/` | 同一目录生成 Go 源码 | Protobuf 消息与 gRPC 服务, 不复制协议定义 |

Common `app/process.rs` 共用 Star/Planet 的参数入口、Tokio runtime 和退出码, 角色自己的运行循环保持独立.
Go `membership/validation.go` 统一持久记录与 RPC 的名称、地址、UUID 规则; 旧帧实现仅保留在测试文件.
Go/Rust 共同校验 `peer/tests/fixtures/admission-v4.json`, 不各自推导一套身份测试预期.

Peer 的 app 依赖网络库, 网络库不依赖 CLI、日志输出或进程信号. Go 管理 handler 不拥有 listener,
HTTP server 生命周期只由 app 负责. 不创建空的 repository/service/adapter 目录或只有一个实现的接口层.
新增模块必须对应真实所有权边界; 协议校验与进程运维策略不能混在一起.

## 运行契约

- 二进制为 `peer` (Star) / `planet` / `supervisor`, Windows 加 `.exe`. `--version` 可追溯包版本.
- 帮助与正常关闭退出码为 0, 启动/运行错误为 1, 参数错误为 2.
- 参数、日志和配置各自遵循语言惯例, 默认使用 `--name=value`. 不读取隐式全局配置.
- stdin 关闭不终止服务. Linux 响应 SIGINT/SIGTERM; Windows 响应 Ctrl+C, Peer 还支持 Ctrl+Break.
- Peer 默认 2 个 Tokio 工作线程, 可设置 1..64; 默认 5 秒关闭期限, 可设置 1..60 秒.
- Supervisor 默认 5 秒关闭期限和 128 个 HTTP 连接名额; 满额暂停接收, 空闲连接也占用名额.
- 根任务失败必须由进程观察到, 不能留下没有 listener 的假存活进程. 错误路径仍执行清理.
- 每个后台任务有唯一拥有者; 正常关闭等待任务回收, 超时走强制取消. Drop/abort 仅作为异常路径兜底.
- 运行日志写 stdout, 参数/终止错误写 stderr. 两端输出 JSON 运行日志, 不要求日志字段布局成为跨语言协议.
- Peer 诊断事件可能丢失, 状态快照必须直接查询索引. 不把日志、事件队列或 HTTP health 当成集群一致性证明.

Linux 可用 systemd 托管前台程序. 当前没有 daemonize、Windows SCM 服务注册或自动部署动作;
启动失败的重启退避应由外部进程管理器设置. 服务账户与目录权限由部署环境负责.

## 编码与审查

执行 [coding.md](coding.md): 中文解释配 ASCII 标点, 关键块解释意图与所有权, 160 列上限,
Rustfmt/gofmt 在每次源文件修改后立即执行. Rust 禁止 unsafe、unwrap/expect, 公共声明缺少文档或文档链接损坏会失败;
测试代码可使用断言验证不变量. Go 使用标准 vet 和 race. 不为减少行数牺牲错误、取消和资源边界.

修复缺陷时逐一审查 Peer、Supervisor 和相关 SDK 是否存在同类路径, 记录哪些需要修复、哪些已有防护.
测试覆盖可观察行为, 不机械复制实现. 线程、连接、消息长度、定时器、重试和缓存都有明确预算.
对未认证地址的校验只能证明格式, 不能声明已认证或合法加入生产群组.

## 构建、依赖与协议生成

所有缓存、依赖和编译产物都在被 Git 忽略的 `build/` 下; 协议生成源码随 `.proto` 提交到服务的 generated 目录.
不设置全局环境变量, 不使用 `go env -w`,
不在普通构建中下载工具、升级依赖或自动改变协议清单. Cargo.lock、go.mod、go.sum 与 message-ids.lock 必须审查并跟随源码提交.
当前只正式验证 Windows x64 和 Ubuntu x64; Go 1.27.1、Rust 1.98.1、protoc 36.1 是已验证工具组合.
Tonic 0.14.6 要求 Rust 1.88, 服务与生成器声明相应下限; 实际使用 Rust 1.98.1, 未独立验证 1.88.

`proto/message-ids.lock` 默认只读. 新增消息后用 [显式生成流程](proto/README.md) 自动追加编号,
再审查差异; 原有编号和删除消息的占位都不得复用. 普通编译不调用 protoc,
一键检查的只读生成核对发现源码或清单过期时必须失败.
依赖获取需要维护者针对具体包明确同意. 已获准的 gRPC、Protobuf、TLS、bbolt 与测试工具沿用项目内缓存,
精确版本以 [gRPC 依赖说明](peer/grpc-implementation.md#依赖与工具) 和锁文件为准.
Ubuntu 的 Rust 1.98.1 rustfmt/Clippy 位于 `build/tools/rust-1.98.1`; 组件包和官方 manifest 留在 `build/deps/linux/x64`.
本次结构整理没有新增依赖、下载或全局安装.

早期骨架基础依赖的来源与许可证元数据 (不含后续 gRPC 等完整清单):

| 服务 | 依赖 | 来源 | 许可证 |
| --- | --- | --- | --- |
| Peer | tokio / tokio-util | crates.io, Tokio 项目 | MIT |
| Peer | prost / prost-build / prost-types | crates.io, Tokio Prost 项目 | Apache-2.0 |
| Peer | serde_json | crates.io, Serde 项目 | MIT OR Apache-2.0 |
| Peer 信号路径 | signal-hook-registry | crates.io, signal-hook 项目 | MIT OR Apache-2.0 |
| Supervisor | golang.org/x/net | Go 官方扩展库 | BSD-3-Clause |

精确版本和传递依赖以各服务锁文件为准, 上表不替代发布包的完整依赖清单.

## 一键检查

Windows, 从任意目录执行脚本的实际路径:

```powershell
.\scripts\check-services.ps1
.\scripts\check-services.ps1 -Service peer
.\scripts\check-services.ps1 -Service supervisor -Race
.\scripts\check-services.ps1 -FuzzSeconds 10
```

Linux, 将已有项目工具临时加入本次命令 PATH:

```bash
env PATH="$PWD/build/tools/rust-1.98.1/bin:$PWD/build/tools/go-1.27.1/bin:$PATH" bash scripts/check-services.sh
# 显式有限时长 fuzz, 每个入口 10 秒, 默认两个 worker.
env PATH="$PWD/build/tools/rust-1.98.1/bin:$PWD/build/tools/go-1.27.1/bin:$PATH" bash scripts/check-services.sh --jobs=1 --fuzz-seconds=10
```

两端默认最多 2 个编译任务, 无需测试数据库. 检查包括格式、只读依赖/编号、静态检查、单元/网络测试、
Rust 文档和原生 Release 构建. Linux 默认执行 Go race; Windows 只有显式 `-Race` 时执行并要求已有 C 编译器.
退出码非零表示失败, 不静默跳过缺少的工具. 脚本可供未来 CI 调用, 当前没有配置云端流水线或发布任务.
Go fuzz 默认只执行种子, 显式时长范围为 0..300 秒. 有限时长 fuzz 不能替代长期故障测试或外部安全审计.

## 历史骨架验证, 2026-09-09 (旧协议)

| 检查 | Windows x64 | Ubuntu x64 |
| --- | --- | --- |
| 格式、Clippy、Rust 文档、冻结构建 | 通过 | 补齐已授权组件后通过 |
| Peer 回归 | 35 项通过 | 35 项通过 |
| Go tidy/vet、Supervisor 回归、构建 | 8 个顶层测试组通过, 包含子用例 | 同样通过 |
| Go race | 未执行, 当前没有配置 Windows cgo 编译器 | 通过 |
| 原生进程信号与端口释放 | 未执行实际控制台信号注入 | 两个程序均通过 SIGTERM、stdin EOF、JSON 日志和端口释放 |
| Peer 日志输出断开 | 通过代码检查 | BrokenPipe 返回失败, 无 panic 或挂起 |

Peer 根任务终止和 Supervisor listener 失败均有立即传播的回归. 编号清单测试在断言失败时也由 RAII 删除临时文件.
普通构建前后 `peer.proto` 和 `message-ids.lock` 逐字节一致. Go/Rust/C++/C# 生产入口搜索未发现同类 stdin 控制路径;
Go Supervisor 已主动观察 Serve 结果, C++ 协议生成位于 binary build 目录, Go 生成器是显式命令.
这次修复不改变 Redis SDK 协议, 未重复启动数据库回归. 所有测试服务、连接和临时文件均已回收.

## 当前实现与剩余边界

| 项目 | 当前状态 |
| --- | --- |
| Supervisor 完整名单, 进程 UUID 与 epoch 替换 | 已实现, 事务提交后返回名单 |
| gRPC/TLS, 账号角色授权, 签名 bearer 准入 | v4 已实现, 不使用 exporter 或额外进程密钥 |
| Peer 双连接 mesh 与离线重连 | 已实现, Discover 已删除 |
| bbolt 持久成员 | 已实现, 独占单写, 损坏记录拒绝启动 |
| 业务复制与权威 Catalog 持久恢复 | 未实现 |
| 证书轮换, 在线吊销, 成员退役与 Supervisor HA | 未实现 |
| Admin API, 权限与 3D 拓扑 | 未接入 |
| 生产容量/延迟与长时资格 | 需按部署规模测量, 短时故障测试不能代替 |

新一键入口为 `scripts/test-services.ps1` 和 `scripts/test-services.sh`.
默认先运行完整本机质量门槛、构建和 Python 编排器单测, 再启动真实 Go/Rust 进程完成 13 组故障回归.
长时模式使用 `-Mode soak -Duration 3600` 或 `--mode soak --duration 3600`.
两端依赖准备不在脚本中自动执行. 结果在 `build/testkit/results/services-*.json`.
跨主机运行可传 `-Address <本机可达IP> -RemoteConfig <项目build内配置>`;
对应 Bash 参数为 `--address` 和 `--remote-config`. 配置含 host, username, password,
project; 不提交凭据. 两端必须预先构建同一版服务并信任已核对的 SSH 主机密钥.

每次测试独占进程, 端口和项目临时目录. 正常/失败路径均 join 子进程和日志读取线程;
Windows 子进程使用独占隐藏控制台和 Job, 不触碰用户终端. SSH agent 在控制通道 EOF 时清理.
测试结果持久保留, 临时数据库和进程日志目录清理; 失败结果保留有限日志尾部.
硬断电或主机内核崩溃不能由普通 finally 保证清理, 不承诺覆盖这类情况.

详见 [本轮验证矩阵](service-admission-test-plan.md).
