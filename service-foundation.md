# Star / Planet / Supervisor 服务基础规范

当前服务为 Linux/GCC 16.2 的 C++26 Star/Planet 和 Go Supervisor. 旧 Rust 服务已废弃,
不再生成、构建、回归或提供替代入口. Rust SDK 与独立协议生成工具仍按各自职责维护.
[身份与准入契约](cluster/identity-contract.md)定义当前命名、协议 v6 和升级边界.

## 组织与依赖方向

| 层 | C++ Star/Planet | Go Supervisor | 责任 |
| --- | --- | --- | --- |
| 入口 | `cluster-cpp/star/src/main.cpp`, `planet/src/main.cpp` | `supervisor/cmd/supervisor/` | 退出码、版本和进程组合 |
| 生命周期 | `common/src/runtime.cpp`, `process.*` | `internal/app/` | 监听、信号、回调排空和有界关闭 |
| 网络适配 | `admission.*`, `grpc_session.*`, `identity.*` | `internal/admission/` | TLS、单次登记、gRPC、原字节验签 |
| 成员与连接 | Star topology、Planet upstream、共享 Policy | `internal/membership/` | 部署代次、会话所有权和持久幂等 |
| 管理 | `admin/src/features/galaxy/` | `internal/management/` | 展示与 HTTP 管理, 不成为复制中转站 |
| 契约 | `proto/` | 同一 schema 生成 Go | 显式维护生成源码和兼容边界 |

C++ 共用代码位于 `verdandi::cluster`, 自身标识为不透明 `Id = std::string`.
Star 与 Planet 的选择策略放在各自 Policy, Common 不决定哪台 Star 作为 Planet 上游.
网络库不依赖 CLI、Vue 或应用信号, Vue 不直接管理 Three.js 资源生命周期.
不创建没有真实职责的 repository/service/adapter 层或只有一个实现的抽象接口.

## 运行与身份

- 进程名为 `star`、`planet`、`supervisor`. 帮助和正常退出为 0, 参数错误为 2, 运行错误为 1.
- 参数、默认值、单位和范围由 CLI 帮助明确提供. 不读取隐式全局配置, 不静默切换实现.
- C++ 使用控制循环与 gRPC worker, 旧 `--worker-threads` 被拒绝. 默认关闭预算为 5 秒, 范围 1..60 秒.
- stdin 关闭不结束服务. C++ Linux 响应 SIGINT/SIGTERM; Go Windows 支持其平台的退出信号.
- Supervisor 一次鉴权后持久登记并签发 id; 客户端仅保存随机启动请求键, 响应丢失时幂等重试.
- 只有已提交的 Hello 凭证签名, 不再签发启动票据. 原始 Protobuf 字节先验签, 再检查部署、角色、分组和代次绑定.
- 新启动进程需等待 Supervisor. 已准入进程在 Supervisor 离线时按已有授权继续连接和故障切换.
- `initialized` 只表示准入完成, `upstream` 只表示控制连接存在, 均不表示业务数据同步就绪.
- 异步回调拥有的请求、响应和取消状态必须活到完成事件, 关闭时排空本进程拥有的资源.
- 有界队列、消息容量、重试和固定错误码由实际配置落实; 日志不输出密码、票据、签名正文或私钥.

协议 v6 不兼容旧 Rust/v4, 旧 `peer_id` 成员库不会自动改写或清空.
开发验证使用独立数据库. Catalog/Registry 复制、业务恢复和 SDK 接入仍属于后续工作.

## 代码与依赖

遵循[编码规范](coding.md)和 [C++ 维护指南](cluster-cpp/CONTRIBUTING.md).
手写文件头描述功能; 配置每个变量说明含义、单位、默认值、边界与相互约束;
每个 enum 元素和函数声明说明契约, 函数实现对关键状态转换、错误与所有权块添加注释.
当前注释使用中文和 ASCII 标点, 修改后立即运行对应的 clang-format、gofmt 或 Rustfmt.
生成文件不手工编辑, 第三方许可证保留原文.

C++ 固定依赖见[版本锁](cluster-cpp/dependencies.lock.json)及[来源说明](cluster/cpp26-dependencies.md).
TLS 使用 gRPC 配套 BoringSSL, 不混用另一套加密运行库. Go 使用模块锁与本地缓存.
所有常规检查离线, 缺工具或依赖时失败, 不自动下载、安装、升级或切换版本.

## 验证入口

```powershell
./scripts/check-services.ps1 -Service supervisor
./scripts/generate-proto.ps1 -Check
```

```bash
bash scripts/check-services.sh
bash scripts/test-services.sh
python3 cluster-cpp/build.py regression --profile debug
```

服务检查包括格式、显式生成一致性、Go vet/模块验证/测试、C++ 编译与 CTest.
Linux 默认附带 Go race; 协议生成工具仍检查 fmt、Clippy 和编号分配测试.
回归运行本次持有的 Go Supervisor 与 C++ Star/Planet, 覆盖 TLS/RPC、注册、重启与故障切换.
测试最终回收自己的进程和临时目录并验证端口可重用, 不使用现有部署的数据库.
Python 服务夹具仅接受 `cpp`; SSH 配置中的 `star_implementation` 也只接受 `cpp`.

管理端在 `admin/` 使用已有 pnpm 执行 `pnpm check`, 覆盖格式、模块边界、行为测试和生产构建.
共享 Redis SDK 修改仍使用各 SDK 的验证入口, 不以服务骨架测试代替完整 SDK 回归.
性能、ASan/UBSan、TSan 和耐久测试按改动风险选择, 记录实际产物与平台;
旧 Rust、v4 和改名前的报告属于历史证据, 不转记为新协议的验收结论.
