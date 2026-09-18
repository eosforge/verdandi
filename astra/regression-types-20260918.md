# 类型内嵌迁移修复与回归

日期: 2026-09-18. 基线提交: a782ff6, 在用户已有的未提交类型迁移上修复.
测试环境: Ubuntu 192.168.0.119, Linux x64, 项目内 GCC 16.2.0.
本轮用户明确授权修复、复核和回归. 复用现有工具及依赖, 单任务构建, 不修改系统配置.

## 修复范围

| 位置 | 修复 |
| --- | --- |
| common/include/astra/clock.hpp | 将时钟相关类型完整放入 EpochClock, 恢复 Elapsed 的 chrono 接口、Estimate/Reading 字段及聚合初始化顺序; deadline_after 保持专用错误类型和 noexcept |
| common/include/astra/types.hpp | 保留 Status 命名, 修正 Member 内 Role 的声明, 明确包含 utility; 清理迁移后挂错位置的注释 |
| common/include/astra/policy.hpp | 将 Direction 和 NetworkStatus 真正定义到 Policy 内, 保留 outbound=0、inbound=1 的数组索引约定 |
| common/src/admission.hpp | 恢复 Joined 的端点、身份、名单、Hello 所有权; 删除误加的重复 RegistrationCall, 保留回调式 RPC 状态; 恢复 idle/connecting/registration 三阶段 |
| common/src 与 pulsar/src | 补齐遗漏的 Error 到 Status 迁移, 恢复被类型替换误改的角色拒绝提示 |
| pulsar/src/server.hpp | 配置归入现有服务类 PulsarServer::Config, 恢复全部选项和默认值, 同步入口及测试调用 |
| 当前时钟设计文档 | 更新为 EpochClock::Time 和 EpochClock::Reading, 历史回归报告保持原记录 |
| .gitignore | 忽略 astra/build 的本地 CMake 缓存, 不删除现有文件 |

保留 Member::Role、Policy::Direction、Policy::NetworkStatus、Admission::Joined、
EpochClock::Time/Elapsed/Estimate/Reading/DeadlineError 的命名方向.
PulsarServer::Config 取代没有对应宿主类型的 Server::Config.

## 修复后复核

逐项检查声明与定义、名称遮蔽、聚合字段顺序、枚举编码、错误返回、头文件依赖以及异步完成后的资源所有权.
排除名称、声明归属和格式差异后, 本轮不引入新的业务状态或同步分支.
Admission 的请求仍保持稳定地址; 回调持有独立的共享调用状态, 以 release/acquire 发布完成.
租约计算保留 expected 错误, 未就绪不能误转为永久期限.
协议文件及生成消息没有因 C++ 类型迁移而变化.

500 ms 总误差门槛、200 ms 往返/处理上限、5 s 样本新鲜度和调速参数保持不变.
同时验证基线中尚未回归的 SnapshotIndex 根收缩: 旧视图不变、预备路径保留、
非零高位不能错误提升、最高树层收缩、删除不分配以及收缩后再次扩展.

## 回归结果

| 配置 | 构建 | CTest | TLS/RPC 场景 | 拓扑/恢复场景 |
| --- | --- | --- | --- | --- |
| Debug | 通过 | 21/21, 69.49 s | 6/6 | 13/13 |
| Release | 通过 | 21/21, 69.42 s | 6/6 | 13/13 |
| ASan/UBSan | 通过 | 21/21, 76.41 s | 6/6 | 13/13 |
| TSan | 通过 | 21/21, 119.03 s | 6/6 | 13/13 |

CTest 包含 Store/TTL、时间轮、时钟、准入、会话、Pulsar 账本、真实 TLS/RPC、
Pulsar 与 Star 进程场景, 以及构建入口和 Python 夹具检查.
额外进程回归验证与既有 Go Supervisor 互通, 不扩展冻结 SDK 的测试范围.
用例组数不是行或分支覆盖率; 本轮未运行长期压力测试或性能基准.

四种配置均首次通过, 没有失败重跑, 生成协议逐字节比对通过.
ASan/UBSan 未报告内存或未定义行为问题; TSan 使用项目内已有的插桩版依赖,
未报告数据竞争. 本轮复核和上述测试未发现新增的阻断缺陷, 不代表穷尽所有调度与部署环境.
这是 Linux/GCC 的回归结果, 不声明 Windows/MSVC 编译通过.

测试结束后独立检查未发现残留的 Star、Planet、Pulsar、RPC 探针或测试 Supervisor 进程,
已知服务和 Pulsar 测试临时目录均已清理. 进程回归同时确认监听端口可复用.

## 证据

- 本机聚合日志: build/types-regression-20260918.log.
- 分配置日志与结果: build/testkit/results/types-20260918-1789709435333733772/.
- 源码同步清单: build/testkit/results/types-source-20260918.json.
- 源码最终核对: build/testkit/results/types-verified-source-20260918.json, 186 个源码和构建相关文件两端一致, 测试期间没有代码变动.
- 格式及空白检查: build/testkit/results/types-static-20260918.json, 71 个修改中的 C++ 文件通过 clang-format 检查.
- 四配置退出码、耗时、12 个服务二进制摘要及清理检查: 上述结果目录内 verified.json.

原始分配置日志、CTest LastTest.log 和进程结果 JSON 已归档至本机项目, Linux 构建缓存继续保留.
