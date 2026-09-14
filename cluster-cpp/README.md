# C++26 Star / Planet

Linux x64 / GCC 16.2.0 的 C++26 连接骨架, 与 Go Supervisor 通过 gRPC/TLS 协议 v6 协作.
当前运行范围是准入、互联、心跳和有界资源清理, 尚无 Catalog/Registry 业务接口或 SDK 接入.
新增内部 SyncStore 和同步协议草案, 本轮整理后按维护者要求暂停编译与测试.
审核入口见 [SyncStore 整理说明](sync-foundation-review-20260914.md), 不沿用旧版本的验证结论.

首次阅读从本文的结构和启动方式开始, 然后查看[维护指南](CONTRIBUTING.md)与[身份契约](../cluster/identity-contract.md).
本轮类型、状态与入口精简见[review 说明](minimal-review-20260914.md).
既有验收历史保留在[验证记录](validation.md), 不将旧协议长测成绩转记给当前源码.

目录职责:

- `common`: 配置、身份、准入、逻辑会话、gRPC 与进程生命周期.
- `star`: Star 成员表和两方向会话拓扑.
- `planet`: 至多八个候选、单活动上游与故障切换.
- `common/src/generated`: 从仓库 `proto/` 显式生成并保留的 C++ 源码.
- `common/src/process.*`: 私有信号、唤醒与 JSON 日志, 与连接生命周期分开维护.
- `bench`: 隔离推流夹具及其生成协议, 不链接进两个服务程序.
- `test_build.py`, `test_language.py`, `test_scale.py`, `test_push.py`: 构建入口、编译诊断、真实组网规模和推流对照.

已实现的边界是连接骨架: 账号登录和 Supervisor 签发准入, Supervisor 签发的进程 id, TLS 1.3, 双向 gRPC 流,
Star 两方向逻辑会话互联, Planet 本组优先/跨组故障切换, Ping/Pong, 有界重试和 SIGINT/SIGTERM 清理.
Star 与 Planet 的差异留在各自的 Policy 中, Common 不判断哪一台 Star 应成为 Planet 的上游.

Planet 业务推进暂停, 下一阶段优先完成 Star 与第一版 SDK 的直接接入闭环.
本次[准入精简](admission-simplification-20260912.md)不实现业务同步.

此阶段没有业务存储、Catalog/Registry 同步、Planet 换绑重新登记或 SDK 接入.
`initialized` 表示完成准入, `upstream` 表示认证连接存在, 均不表示业务数据就绪.

下一阶段的数据方向见[按 Key 对账与流式同步设计](../cluster/key-stream-sync-design.md):
发布者维护每 Key 版本, SDK 对账后接入有序推送, 历史不足时恢复相关 Key, 删除与未知分别处理.
该文尚未转为业务实现, 不属于以上连接测试的覆盖范围.

身份已迁移为 [Supervisor 签发不透明 id](../cluster/identity-contract.md). Star/Planet 不生成或解析 UUID, 登记通过一个 RPC 完成, 重试复用随机请求键, 幂等和代次由 Supervisor 管理. 控制协议主版本为 6, 旧 Rust 服务已废弃. 上述历史性能与耐久报告不自动覆盖本次协议迁移.

## 构建与生成

首版构建平台为 Linux x64 / GCC 16.2.0. 源码、工具和依赖产物使用项目目录.
从仓库根目录运行下列命令. 普通构建不下载依赖、不运行 protoc, 缺失时直接失败.

```bash
bash cluster-cpp/build.sh build --profile debug
bash cluster-cpp/build.sh test --profile debug
bash cluster-cpp/build.sh build --profile release
```

构建输出为 `build/cluster-cpp/<profile>/star` 和 `planet`. Debug 开发产物携带项目 GCC 运行库搜索路径.
Windows 可编辑源码, `build.ps1` 会明确报告当前平台不受支持, 不偷偷切换编译器或远程主机.

依赖是独立的显式准备动作, 需要 Python 3.12+, 应先按项目规则取得下载授权. 本轮批准的是 Ubuntu 项目目录.
已有工具和源码缓存优先复用. 服务和依赖的 `build` 阶段均离线并单任务编译;
依赖准备脚本另对普通依赖构建子进程施加 2 GiB 地址空间上限.
完整 TSan 使用独立的 `linux-gcc16-tsan` 前缀重新插桩依赖, 显式执行
`python3 cluster-cpp/prepare_dependencies.py build --profile tsan`. 它复用已下载源码, 不覆盖普通运行库或生成工具.
TSan 生成器需要很大的虚拟 shadow 地址空间, 这项配置不套用 2 GiB 虚拟地址限制, 仍保持单任务.

```bash
python3 cluster-cpp/prepare_dependencies.py fetch
python3 cluster-cpp/prepare_dependencies.py build
bash cluster-cpp/build.sh generate
bash cluster-cpp/build.sh check-generated
```

固定版本、源码 commit 和归档 SHA-256 见 [dependencies.lock.json](dependencies.lock.json).
源码和静态依赖前缀在 `build/deps/cluster-cpp`, protoc/plugin 在 `build/tools`.
`generate` 显式更新 `common/src/generated` 和 `bench/generated`, `check-generated` 只逐字节比较. 两者都不下载工具.
gRPC 使用其配套 BoringSSL, 不另找系统 OpenSSL. 第三方授权原文保存在 [licenses/](licenses/README.md).

## 启动

两个入口使用同一组选项, 角色由可执行文件决定. 账号、TLS 和准入公钥放在各自的身份目录中:
`ca.pem`, `cert.pem`, `key.pem`, `admission.pub`, `login.json`.
`login.json` 包含 `username` 和 `password`, 不通过命令行或日志传递密码.
跨语言部署的 TLS 证书使用 ECDSA P-256 / SHA-256. 当前 BoringSSL 默认 TLS 验证算法不包含 Ed25519,
这与 Supervisor 使用 Ed25519 签署准入正文是两件事; 不修改 TLS 私有配置来绕过限制.
部署身份的准备规则沿用 [服务基础说明](../service-foundation.md).
仓库公开测试私钥仅供测试夹具使用.

```bash
build/cluster-cpp/debug/star --listen=192.168.1.10:7443 --super=supervisor.example:7440 \
    --cluster=example --group=east --identity=/path/to/star-identity --status-interval-seconds=5
build/cluster-cpp/debug/planet --listen=192.168.1.11:7443 --super=supervisor.example:7440 \
    --cluster=example --group=east --identity=/path/to/planet-identity --status-interval-seconds=5
```

`--help` 显示完整选项、默认值和范围. `--worker-threads` 被明确拒绝: 应用控制循环固定一个,
gRPC 独立管理 I/O worker. Supervisor 暂时不可用时, 已准入进程按已有授权继续通信/切换;
新进程仍需重新登录. Planet 此阶段不接受下游业务连接.

## 测试

公共一键入口默认选择 C++: `bash scripts/test-services.sh`, 长时加 `--mode soak --duration 3600`.
旧 Rust 服务已废弃, 不再提供实现切换选项. Windows 可运行 `scripts/check-services.ps1 -Service supervisor` 检查 Go.
独立 C++ 入口用于切换下面的构建和诊断配置.

```bash
bash cluster-cpp/build.sh regression --profile debug
bash cluster-cpp/build.sh soak --profile release --duration=3600
bash cluster-cpp/build.sh regression --profile asan
bash cluster-cpp/build.sh regression --profile tsan
bash cluster-cpp/build.sh regression --profile release --contracts ignore
bash cluster-cpp/build.sh scale --profile release
bash cluster-cpp/build.sh scale --profile release --measure-allocations
```

`regression` 包含 CTest、生成一致性、真实 TLS/RPC 边界、Go Supervisor/C++ 组网和现有进程回归.
`soak` 先执行同样的前置检查, 再按指定秒数运行故障循环, 收集 RSS/线程/FD 的初始、峰值和末值.
两者需要项目内已有 Python 测试依赖、Go Supervisor 及 C++ Star/Planet 产物, 不自动安装或下载它们.
测试创建自己持有的服务与临时目录, 结束和异常时均清理, 报告保留在 `build/testkit/results/`.
不复用或清理部署中的进程和数据库.

`scale` 阶梯为 2/4/8/16 Star 和 0/1/8/32 Planet, 每组包含加入、故障切换、重启和清理.
旧 `benchmark` 命令及 `--benchmark-smoke` 选项已移除; 它们原本就因 Rust v4 接收器废弃而不可执行.
`bench/` 和历史测量脚本保留为隔离实验材料, `--benchmarks` 仍可显式编译实验目标, 不属于生产服务或当前性能验收.
容量拒绝保留为失败样本, 报告 `completed_with_capacity_rejections` 表示完成矩阵, 不代表所有负载通过.
分配统计输出到独立 `release-allocations` 目录, 只计 C++ new 的累计请求, 不用于正常性能排名.
关闭契约的对照输出到 `release-contracts-ignore`, 不覆盖默认 enforce 产物.

`test --core-only` 仅运行无 gRPC 的配置/拓扑测试, 不产生服务, 不能用来声称网络验收通过.
构建入口主动恢复 `BUILD_TESTING=ON`, CTest 未发现测试时失败; 不兼容的诊断选项组合也会明确拒绝.
ASan 配置同时启用 UBSan, 发现诊断立即失败. ASan 插桩覆盖本项目手写目标;
完整 TSan 还要求生成消息与独立前缀的第三方 C/C++ 库一同插桩, 避免缺失内部同步信息.
两者均不声称覆盖系统运行库或第三方汇编实现.
当前整理后的实际结果和限制以 [review 说明](minimal-review-20260914.md) 为准.

## 维护约定

配置选项使用 C++26 反射注解和 `template for` 生成解析、范围检查与帮助, 默认值只写一处.
Planet 使用 `inplace_vector` 存放至多八个候选, 控制消息使用四个固定槽位.
`expected` 表达可恢复错误, 契约只检查内部不变量, 不因不可信网络输入触发契约终止.
角色索引使用普通锁, gRPC 回调只交接完成状态, 控制循环统一推进协议和生命周期.

生产源码采用中文注释和 ASCII 标点. 每次写入后执行本目录 `.clang-format`; 仅现有格式化器不能解析的反射语法
使用局部保护. 测试不依赖 `assert` 在 Release 中是否开启.
文件头写当前功能说明, 配置字段和每个枚举元素逐项注释, 函数声明与关键实现块说明契约和原因;
详细要求见 [文件职责与注释](CONTRIBUTING.md#文件职责与注释).
安装阶段复制两个程序及许可证, 不保留开发机 RPATH. 正式分发仍需匹配 GCC/libstdc++ 与系统运行库并完成部署验证,
本骨架不承诺跨发行版二进制兼容.
