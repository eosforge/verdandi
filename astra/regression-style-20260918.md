# C++ 规范整理及类型收敛回归

日期: 2026-09-18. 对应改动说明: [规范整理及静态审阅](style-review-20260918.md).

## 结论

Debug, Release, ASan/UBSan, TSan 四种配置的完整回归均通过. 每种配置执行 21 项 CTest, 6 个 TLS/RPC 场景及 13 个拓扑与恢复场景. 没有跳过测试, 未报告 Sanitizer 错误, 无需修改生产代码或用例来使本轮通过.

本轮以用户明确要求的“回归测试”为授权. 未下载或安装工具和依赖, 未进行长期测试或性能基准, 未提交代码.

## 被测源码与环境

- 基准提交: `3b104bde7643ffe2887974b688be63a704659ce0`, 分支 `alpha`. 实际被测对象包含工作区尚未提交的规范整理及类名修改, 不能仅用该提交号代替被测源码身份.
- 源码及夹具清单共 299 个文件, 测试前记录 SHA-256, 测试后核对 Windows 工作区和 Ubuntu 副本一致. 本报告在最终核对完成后新增, 不属于被测清单.
- 清单 `build/style-regression-20260918/source.json` 的 SHA-256: `a267c65e47728d440029d95814067ff886d6197b185dad042fd21a72c954eb18`.
- Ubuntu 虚拟机 `192.168.0.119`, 项目 `/home/ubuntu/verdandi`, 内核 `Linux 7.0.0-31-generic x86_64`, 项目内 GCC 16.2.0, C++26, 契约 `enforce`.
- 复用项目内 gRPC 1.84.0, Protobuf/protoc 36.1 及匹配生成工具. TSan 使用既有独立插桩依赖前缀. 构建串行执行, `--parallel 1`.
- 当前 C++ 运行目标只支持 Linux/GCC. Windows 执行了源码格式和静态核对, 没有进行 MSVC 构建; 旧 SDK 不属于本轮回归范围.

## 结果

| 配置 | CTest | TLS/RPC 场景 | 拓扑与恢复场景 | CTest 耗时 | 构建及完整回归耗时 |
| --- | --- | --- | --- | --- | --- |
| Debug | 21/21 | 6/6 | 13/13 | 70.36 s | 469.31 s |
| Release | 21/21 | 6/6 | 13/13 | 69.52 s | 500.50 s |
| ASan/UBSan | 21/21 | 6/6 | 13/13 | 77.07 s | 544.80 s |
| TSan | 21/21 | 6/6 | 13/13 | 117.80 s | 568.58 s |

这些耗时来自本次虚拟机上的串行回归, 不能用于比较生产性能. 21 项 CTest 内部还包含多个检查点, 表格统计的是 CTest 项目数, 不是断言数量或代码覆盖率.

ASan 设置 `detect_leaks=1:halt_on_error=1`, UBSan 设置 `halt_on_error=1:print_stacktrace=1`, TSan 设置 `halt_on_error=1:exitcode=66`. 进程夹具也检查服务日志中的 Sanitizer 诊断, 预期的拒绝或强制重启不豁免该检查. 本轮未新增抑制项, 未通过重试消除失败结果.

每种配置还执行了协议生成的逐字节比较, 均通过. `common/src/generated` 的 16 个生成文件与整理前摘要一致, 未修改生成代码.

## 覆盖重点

- 新增 Star 初始化重复主体用例: 本地及远端重复均拒绝, 拒绝后状态不变, 后续合法初始化成功.
- 共用类型改名后的配置反射, C++26 契约, Protobuf 代理, 身份校验, 会话交接, 取消与连接清理.
- Store 写入与快照, 分配失败注入, TTL, 时间轮及纪元时钟. 保留完整时间追赶语义.
- Pulsar 账本, 账号准入, Pulse RPC, 物理参考故障恢复, 平滑调速及固定 Unix 时间轴重启.
- 真实 Pulsar 与两台 Star 的拓扑, 对时, 服务中断, 重连和 SIGTERM 清理; 沿用已批准的 500 ms 时源质量门槛.
- TLS 1.3/HTTP2 协商, 半握手期限与 FD 回收, 静默 Hello, 逻辑流复用, 重复会话隔离, 慢读与协议拒绝.
- 三台 Star 互联, Supervisor 离线及恢复, 正常与强制重启, 非法身份拒绝, 现有 Planet 的本组优先和跨组切换, 单上游约束及端口复用. 测试现有 Planet 不代表恢复其功能开发.

## 补充编译与静态检查

受改名影响的 `bench/store.cpp` 已通过 `star_sync_store_bench` 目标编译和链接. `common/tests/allocation_measure.cpp` 已通过独立 Release 配置中的 `star_allocation_measure` 对象目标编译. 未运行基准, 未构建或测量完整分配统计服务. 补充步骤共 7.65 s, 之后已将 Debug 配置恢复为关闭基准目标.

68 个整理范围内的手写 C++ 文件通过 clang-format 22.1.3 的 `--dry-run --Werror` 检查. 五个相关 Python 构建及进程夹具通过 Black 格式检查. `git diff --check` 通过.

## 清理与证据

各进程回归确认了子进程退出, 端口可重新绑定和所属临时目录清理. 最终独立检查未发现遗留 Astra/Supervisor 测试进程, 回归 PID 文件或相关测试临时目录. VM 的 `oom_kill` 为 0, 本轮未发生 OOM.

Windows 原始证据位于 `build/style-regression-20260918/`, Ubuntu 同路径保留各轮记录:

- `summary.json`, `source.json`, `static.json`, `cleanup.json`.
- 每种配置的 `<profile>.json`, `<profile>.log`, `<profile>-ctest.log`.
- 每种配置的 `<profile>-astra-rpc-*.json` 和 `<profile>-services-*.json`.
- `supplemental.json` 和 `supplemental.log`.

各配置记录实际执行的 Star, Planet, Pulsar 二进制 SHA-256, RPC 场景另记录探针和 Supervisor 的摘要. 证据存放于忽略目录, 不随源码提交.

## 边界

本次通过证明上述配置与已执行场景未检出回归, 不代表达到 100% 行或分支覆盖率. 本轮未采集覆盖率, 未验证长期容量或吞吐性能. 前次静态审阅记录的 Planet 初始化异常窗口以及 Star 本地身份复制失败的精确注入缺口, 不因普通回归通过而视为已经修复或补齐.
