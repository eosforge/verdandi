# Orbit / Astra / Comet v1 回归复验

2026-09-15, 对当前未提交的命名、协议及构建入口修改执行复验. 所有已运行检查通过, 本轮无需修改生产代码.

## 测试结果

| 范围 | 结果 |
| --- | --- |
| Linux C++ Debug | 10 项 CTest、6 项 TLS/RPC、13 项进程场景全部通过 |
| Linux C++ Release | 10 项 CTest、6 项 TLS/RPC、13 项进程场景全部通过 |
| Linux C++ ASan/UBSan | 同上, 无内存或未定义行为诊断 |
| Linux C++ TSan | 同上, 无数据竞争诊断 |
| Windows / Linux Go Supervisor | 格式、模块检查、vet、测试及构建通过; Linux race 通过 |
| Windows / Linux 协议生成器 | 生成一致性、fmt、Clippy 及各 4 项测试通过 |
| Windows Python | 构建入口 6 项、测试器与清理边界 38 项, 共 44 项通过 |
| Windows Admin | 格式、架构边界、53 项测试、类型检查及生产构建通过 |
| C++ / Proto / Python 格式 | clang-format、Black 检查通过; git diff --check 通过 |

C++ 每个配置运行 `bash astra/build.sh regression --profile <profile>`, Debug 另加 `--benchmarks` 验证隔离探针编译.
Go 使用项目 `scripts/check-services` 入口, Linux 启用 `--race`.
Admin 使用已有本地工具执行各项检查, Node 为 24.19.0, 满足 package.json 的 24.x 要求;
本轮没有改动或安装 README 固定的 24.21.0. 未执行浏览器 WebGL 人工验收.

## 覆盖与边界

- 协议包名、完整 RPC 路径、Orbit Member 归属、签名域、主版本 1 接受及非 v1 拒绝.
- 内部 Store、分配失败注入、配置与契约、会话及生命周期.
- TLS 1.3、HTTP/2、重复会话、握手超时、空闲连接关闭与资源回收.
- 三 Star 互联、Supervisor 故障与持久恢复、Star 重启、错误身份和权限拒绝、现有 Planet 故障切换.
- ASan/UBSan 覆盖本项目手写目标; TSan 同时使用生成消息与已有第三方插桩依赖. 不声称覆盖系统运行库或第三方汇编实现.

SyncTransport 仍为未注册草案, Comet SDK 尚未实现. 这些通过结果不代表业务同步接口已可用.
本轮没有吞吐基准、长期故障测试或无限时后台任务.

## 可追溯性与清理

- Windows / Linux 的 338 个相关文件内容一致, 测试前后均核对.
- 704 个冻结文件未变; 3420 个第三方安装文件的大小和修改时间未变.
- 四个配置的测试产物 SHA-256 与 RPC 报告匹配.
- 每组进程回归均确认进程退出、端口可复用、临时目录移除; 最终未发现残留测试服务进程.
- 未下载依赖、重编第三方库、commit 或 push.

结构化报告: [protocol-v1-recheck-20260915.json](protocol-v1-recheck-20260915.json).
Linux 原始日志保留在 `/home/ubuntu/verdandi/build/protocol-v1-recheck/`.
