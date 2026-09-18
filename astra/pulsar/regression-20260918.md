# Pulsar 与连续纪元时钟回归记录

日期: 2026-09-18. 平台: Ubuntu 虚拟机, Linux / 项目内 GCC 16.2.0.
本次测试经过用户明确授权; 全部复用已有工具与依赖, 构建并行度为 1.
本文件保留 100/200 ms 阶段的历史结果; 后续获准放宽至 500 ms 的结果另见 [500 ms 回归](regression-20260918-500ms.md).

## 本轮补齐

- Pulsar 测试正式随 CTest 进入 test / regression / soak 入口.
- RPC 用例覆盖越界 T0、八包后主动结束、慢流取消与资源归还.
- 实际 TLS 对时产生 Store 期限, Pulsar 停机期间继续到期, 重启后不复活.
- 物理参考用例覆盖正负十秒跳变、源异常、误差超限、恢复和空 Provider.
- 进程用例逐条核验公开时间不倒退, 失联仍前进, 恢复必须使用本阶段的新日志.
- 新增八个 Python 夹具测试, 防止历史 ready 误报恢复、时间锚点丢失和 Sanitizer 诊断被丢弃.
- 失败时输出有界日志尾部; 自有进程先清理, 再删除临时目录.

## 已确认的参数变更

首次 Debug 检查在 100 ms 门槛下通过 19/20, 随后补齐夹具版本通过 20/21.
两次失败均为真实系统时源未达到门槛, 不是绕过或跳过用例.
只读检查发现 Ubuntu 内核 maxerror 约 189 ms, chrony 已同步但远端时源误差估计约 118–171 ms.

用户随后明确批准放宽至 200 ms. 已集中为 clock_uncertainty_limit_ns,
Pulsar、Star 滤波和对外就绪判断共用这一门槛; 测试覆盖 150 ms、200 ms 和超限.
RTT/处理时长上限仍是 200 ms, Pulsar/Star 调速分别仍为 500/1000 ppm.
没有修改系统 NTP 配置、系统时间或时源.

## 结果矩阵

下表为生产代码固定在 200 ms 门槛后的完整回归. 末次仅收紧 Python 失联日志边界,
其针对性补测结果单列在下方; 不把此前通过改写为最终所有场景通过.

| 范围 | 结果 |
| --- | --- |
| Windows Python Pulsar 夹具 | 8/8 通过 |
| Debug CTest, 200 ms | 21/21 通过, 68.42 s |
| Debug TLS/RPC 进程场景 | 6/6 通过 |
| Debug 组网/故障恢复场景 | 13/13 通过, 21.281 s |
| C++ 协议生成一致性 | 通过 |
| Release CTest | 首次 20/21, 76.92 s; 时源恢复后剩余 1 项复测通过, 11.38 s |
| Release TLS/RPC 与组网 | 6/6 和 13/13 通过 |
| ASan/UBSan CTest | 21/21 通过, 79.14 s; 未报告 ASan/UBSan 错误 |
| ASan/UBSan TLS/RPC 与组网 | 6/6 和 13/13 通过 |
| TSan CTest | 21/21 通过, 114.20 s; 未报告数据竞争 |
| TSan TLS/RPC 与组网 | 6/6 和 13/13 通过, 组网 33.897 s |
| 源码格式与 diff 空白检查 | 通过 |

Debug 的真实 Pulsar + 两台 Star 进程用例在 10.51 s 内完成.
其他进程回归同时验证原 Go Supervisor 互通与既有 Star/Planet 行为, 不扩展冻结 SDK 的测试范围.
Go Supervisor 已使用当前生成代码离线重建.

Release 验证时宿主内核 maxerror 已升至约 241 ms. 200 ms 是固定的准入门槛,
不是“只要测试失败就继续加大”的参数; 本轮保持该值, 保留真实时源场景的失败记录.
其余带注入参考的真实 TLS/RPC 测试仍运行, 不将环境失败等同于全套未测试.
随后 ASan 真实进程场景确认时源已恢复, 对 Release 的失败场景单独复测通过.
这里记录完整首次失败与复测, 不把它写成 Release 单次全绿或稳定物理精度的证明.

### 最终夹具收紧后的补测

失联判断新增停机前日志边界, 和恢复判断一样只接受本阶段新事件,
避免把此前偶发的物理时源质量下降当作本次失联的证据. 生产 C++ 和产物没有再次修改.

| 配置 | Python 夹具 | 真实 Pulsar + 两台 Star |
| --- | --- | --- |
| Debug | 8/8 通过 | 失败, Pulsar 冷启动未能达到物理时源质量要求 |
| Release | 8/8 通过 | 同上 |
| ASan/UBSan | 8/8 通过 | 同上 |
| TSan | 8/8 通过 | 同上 |

每次约 20 s 超时, 日志均显示 Pulsar ready=false, 随后正常退出; 没有继续启动 Star.
补测后只读检查记录内核 maxerror=276749 us、esterror=10281 us, chrony Leap status 为 Normal.
maxerror 是保守误差上界估计, 不等同于实际墙钟偏差; 本机 chrony 更新间隔约 1026 s,
现有质量检查在该估计超过 200 ms 时仍会拒绝就绪. 这项部署条件未被放宽或绕过.
因此本轮具有各配置的成功执行记录, 但最终真实时源进程补测没有全绿.
没有修改 NTP 配置、强制校时、伪造生产参考或循环重试到成功.
结束时确认没有遗留本轮服务进程或 Pulsar 临时目录, 清理记录保存于 build/testkit/results/pulsar-cleanup-20260918.json.

## 覆盖与边界

本轮覆盖时钟单调性、四时间戳、误差门槛、holdover、参考重启、期限不改写、
Store 批次原子性、分配失败回滚、快照寿命、轮调度、准入持久提交、TLS/RPC 和进程退出.
这些是用例和场景数量, 不是代码行覆盖率; 本轮未测量行/分支覆盖率.

ASan/UBSan 按现有 CMake 策略对手写目标启用, 不声称第三方库与生成源码全部经过同样插桩.
TSan 使用项目内 linux-gcc16-tsan 依赖前缀, 生成协议源码也启用线程检查.
两个 Sanitizer 配置均遇到诊断即失败, 没有为本轮添加屏蔽规则.

真实物理时源仍是部署条件, 200 ms 不是绝对准确度保证.
未进行修改宿主墙钟、虚拟机休眠/断电、业务数据持久化或长时负载测试.
结果仅对应本轮源码与产物, 不继承旧版本的长测或性能结论.

## 证据

本机原始日志位于 build/pulsar-regression-debug-200ms.log、build/pulsar-regression-matrix-200ms.log、
build/pulsar-regression-sanitizers-200ms.log 和 build/pulsar-release-clock-recheck.log.
最终补测见 build/pulsar-final-process.log, 时源检查见 build/pulsar-final-time-source.log.
Ubuntu 结构化进程报告及 JUnit 已复制到本机 build/testkit/results, 保留二进制摘要与逐项测试输出.
112 个本机/Ubuntu 源码、协议和测试文件已逐字节一致. 清单保存于
build/testkit/results/pulsar-source-20260918.json, SHA-256 为
463d74f8b1b52c8a5b7104af5e4e3951332754c2bf9e8db883fea15aacd475b1.
