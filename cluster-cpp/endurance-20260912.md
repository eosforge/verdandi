# 扩大拓扑的两阶段长时测试

日期: 2026-09-12. 用户要求先运行两小时故障与恢复, 再持续运行直到手动停止.
本次只修改测试编排和说明, 复用已通过四配置回归的 C++ Release 产物及 Go Supervisor.

## 已按用户要求停止

北京时间 **2026-09-12 16:30:17.775** 完成手动停止, 监控 `star-planet` 已暂停.
总运行 14282.776 秒, 其中故障阶段 7205.739 秒 / 833 轮, 持续阶段 7046.696 秒,
后一个计时包含最终退出收尾. 所有 25 个服务与控制器已退出, 端口可复用,
测试拥有的临时目录已移除, 清理报告通过. 没有新故障, 整体状态保留为手动 stopped.

最后一次存活采样 RSS 合计 438.5 MiB, 比持续阶段基线增加 3.297 MiB;
全部 PID 保持不变, 线程总数不变, FD 总数比基线减少 2, 最低可用内存 2962 MiB.
这段持续观察没有发现资源持续显著增长, 不把有限时长解释为永久无泄漏证明.

[最终原始报告](../testkit/results/peer-cpp-endurance-final-20260912-r2.json) 已保存.
本轮实际执行的是目录改名前的 `peer-cpp` 产物, 原始报告中的运行时路径与指纹保持不变;
完整日志和滚动采样保留在两端的 `build/peer-cpp/endurance-20260912-r2/`.

第一轮控制器 PID 为 `310386`, 北京时间 07:47:16 开始故障阶段,
07:53:14 因测试器错误停止. 实际故障阶段含收尾共 357.517 秒, 完成 41 轮,
进程与端口及临时目录清理通过. 第一轮没有通过两小时验收, 也没有进入持续阶段.
失败记录保留在 `build/peer-cpp/endurance-20260912/status.json`, 后续运行使用独立目录.

本次修复轮的原始输出为 `build/peer-cpp/endurance-20260912-r2/status.json`.
第二轮控制器 PID 为 `314941`, 北京时间 12:32:15 启动; 两小时起点以报告中的
`fault_started_utc` 为准, 不包含逐台组网准备时间.
完整两小时重新计时, 不把第一轮失败前的时间累计进来. 检查提醒 `star-planet`
恢复后每 5 分钟读取状态, 正常不重复汇报; 发现问题会调查、修复并验证后继续,
而不是仅因单次测试失败就结束用户已授权的工作.

## 第二轮故障阶段通过记录

第二轮从北京时间 12:32:45.339 到 14:32:51.079 完成 **7205.739 秒 / 833 轮**故障恢复,
随后保留原进程自动进入持续阶段. 每轮三项恢复检查均完成 833 次, 没有跳过失败后继续计数.
当时 8 Star / 16 Planet / 1 Supervisor 共 25 个进程进入持续运行, 最终手动停止结果见上文.

| 恢复观察 | 次数 | 最大观测耗时 |
| --- | ---: | ---: |
| Supervisor 离线时 Planet 上游切换 | 833 | 1.205 秒 |
| Star 重启与组网恢复 | 833 | 3.636 秒 |
| Planet 重启与重新准入 | 833 | 2.227 秒 |

以上为测试器轮询到状态恢复的耗时, 包含服务每秒状态发布, 不等同于 RPC 网络延迟.
首次持续阶段检查的服务 RSS 合计约 436.9 MiB, 整轮最低可用内存 3175 MiB.
进入持续阶段后的进程 PID 与基线一致; C++/Go 二进制摘要仍与维护验收产物一致.
这份阶段快照只确认故障阶段通过; 持续阶段与最终退出清理见单独保存的最终报告.

阶段快照保存为 [第二轮故障阶段证据](../testkit/results/peer-cpp-endurance-fault-20260912-r2.json).
该快照采于持续阶段开始后约 197.5 秒, 保留整体状态 running,
不会把当时仍在进行的持续测试标成最终完成. 最终报告单独保存, 不覆盖阶段快照.

## 运行范围

- Ubuntu `192.168.0.119`, 项目 `/home/ubuntu/verdandi`.
- 8 个 Star, 16 个 Planet, 1 个 Supervisor, 在单 VM 上通过 loopback 建立真实 TLS/gRPC 连接.
- Star 与 Planet 分配 east/west 两组, 全量组网时有 56 条 Star 方向逻辑流和 16 条 Planet 上游流.
- 两小时从初始完整组网后开始计时, 最后一个已开始的故障循环完成后切换阶段.
- 每轮强制退出 Supervisor 和一个真实 Planet 上游 Star, 验证现存授权下的故障恢复;
  恢复 Supervisor 和 Star 并检查新进程身份, 再轮换重启一个 Planet 并检查旧入站回收.
- 第二阶段保留当前进程, 不再主动注入故障, 持续检查心跳状态和拓扑.
- 本次是连接骨架耐久测试, 不含尚未实现的 Catalog/Registry 业务更新负载.

## 检查和资源边界

复用 `testkit.services.Host` 的进程、日志和清理设施, 以及现有规模测试的 `/proc` 采样.
每次观察都检查进程存活; 状态连续 10 秒不更新会失败, 避免拿缓存的健康状态充当当前状态.
交叉核对每个 Planet 的上游与每台 Star 的 Planet 入站数量, 不仅检查总数.

每 10 秒保存 RSS、线程、FD、CPU 累计值、PID、实际 TCP 数量和最新拓扑.
报告保留各项峰值、恢复耗时计数/总量/最大值和阶段信息. 第二阶段另存 PID 与资源基线,
便于区分同一进程的增长和进程重启. 详细采样滚动保存, 最多 8 个 8 MiB 文件;
服务日志继续采用每进程 4 MiB 上限与最近 32 行内存尾部.

可用内存低于 384 MiB 或可用磁盘低于 256 MiB 时, 测试失败并清理资源.
进程异常退出或拓扑恢复超时也会停止, 不会通过自动重启整个测试抹去失败结果.
运行状态不代表通过; 两小时故障阶段与后续持续运行的结果分别保留.

单次测试失败后, 控制器先保存现场并释放资源. 随后的维护流程应定位原因,
在既有授权范围内修复测试器或骨架, 审查跨语言同类路径, 补充回归后使用新目录继续测试.
只有用户明确停止或遇到确实需要外部输入的阻碍才结束推进. 不盲目重跑同一失败,
不删除失败证据, 不绕过具体软件/依赖的下载许可. 本轮增加了失败调用栈记录.

## 启动与停止

已构建产物存在时, 在 Ubuntu 项目根目录运行:

```bash
build/tools/python-build/bin/python -B cluster-cpp/test_endurance.py \
    --output build/cluster-cpp/endurance-20260912-r2 \
    --stars 8 --planets 16 --fault-seconds 7200 --steady-seconds 0
```

`--steady-seconds 0` 表示无限持续, 正数用于有限预演. 每次运行必须使用新输出目录,
项目级文件锁拒绝同时启动另一个长测. 本次实际控制器在 VM 中脱离 SSH 会话运行,
本机终端关闭不会终止它. 不构建、不下载, 不改全局环境.

手动停止使用同一项目入口:

```bash
build/tools/python-build/bin/python -B cluster-cpp/test_endurance.py \
    --output build/cluster-cpp/endurance-20260912-r2 --stop
```

也可以向控制器发送 SIGTERM 或在输出目录创建 `STOP` 文件. 发出停止请求后,
等待 `status.json` 的 `status=stopped` 和 `cleanup.status=pass`, 才表示清理完毕.
控制器逐个正常停止所持有进程, 检查端口可复用, 并删除带所有权标记的临时目录.
报告和采样保留. SIGKILL 或 VM 断电不属于可执行优雅清理的停止方式.

Windows 当前项目已有辅助入口:

```powershell
& .\build\tools\python-build\Scripts\python.exe -B build/peer-cpp-endurance-control.py status
& .\build\tools\python-build\Scripts\python.exe -B build/peer-cpp-endurance-control.py stop
```

辅助脚本仅管理这次已授权的 VM 和专用目录, 验证现有 known_hosts, 不更改 SSH 系统配置.
停止后应等待状态刷新, 再停止附属于当前任务的长测检查提醒.

## 预演证据与适用性

- Windows 共享 Python 测试 37 项通过; Linux 36 项通过, 1 项 Windows 平台检查跳过.
- 8 Star / 16 Planet 完成 43.408 秒 / 5 轮故障循环, 自动进入有限持续阶段,
  含收尾观测共 12.731 秒, 最后资源清理通过.
- 分别用 STOP 标记和 SIGTERM 停止无限持续预演, 两者均正常退出并完成资源清理.
- 额外强杀一个子进程, 控制器如实返回失败, 其余进程及临时目录仍全部释放.
- 预演二进制摘要与维护验收报告中的 Release / Go Supervisor 摘要一致.

本轮新增判定位于测试器, 未改动 C++/Rust/Go 服务行为. 对状态停更的保护覆盖本次所有
Star/Planet; Supervisor 使用进程存活与就绪检查. 历史短测和 Rust 对照入口保持其原有语义,
没有把本轮扩大耐久检查描述为其他语言或 SDK 的新增验收.

第一轮报告位于 VM 的 `build/cluster-cpp/endurance-20260912/status.json`.
Windows 的 status 辅助命令当前拉取 r2 的同路径副本. 第一轮预演结果位于
`build/cluster-cpp/endurance-smoke-20260912/status.json` 和 `endurance-control-checks.json`.
正式运行完成前不将其记为通过, 不覆盖历史验收报告.

## 空上游修复与跨语言审查

第一次失败来自 `test_endurance.py` 的 `value.get("upstream", {}).get("peer_id")`.
字典键存在且值为 JSON null 时, 默认 `{}` 不会生效. Planet 在已准入且等待换绑时
输出 null 是正常行为, 测试器应把它判为尚未收敛, 继续受超时保护地等待.
修复采用现有共享入口的 `(value.get("upstream") or {})` 写法.

新增回归逐项检查 null、空对象、字段缺失和已失效上游, 确认返回未就绪;
恢复合法上游后才返回就绪. 修复前的 null 子用例准确复现同一 AttributeError,
修复后 Windows 的 38 项共享 Python 检查通过, Linux 37 项通过及 1 项 Windows 平台跳过.
STOP、SIGTERM、子进程异常退出的实际控制预演再次通过; 异常退出报告现在包含调用栈.

| 路径 | 审查结论 |
| --- | --- |
| C++ Star/Planet | Logger 在没有 active_member 时明确输出 null, 服务行为正确, 本轮未改动生产代码 |
| Rust Planet | Option 上游通过 map 序列化为空值, 与 C++ 语义一致, 本轮未改动生产代码 |
| Go Supervisor | 提供准入和成员表, 不使用这个 Planet 状态读取表达式 |
| Python 共享服务/互通测试 | 已采用 `or {}` 正确处理 null |
| Python 规模测试 | 同样写法前有 mesh 非空检查, 当前快照受短路保护; 统一写法以消除隐含前置条件, 不宣称已发生同一故障 |
| C++/Rust/Go/C# SDK | 不消费此骨架的 upstream 状态字段, 不受本次测试器错误影响 |

第二轮仍运行原有已验收 Release 服务, 只修复 Python 编排. 新预演保留在
`build/cluster-cpp/endurance-smoke-20260912-r2/status.json` 和 `endurance-control-checks-r2.json`.
8 Star / 16 Planet 新预演通过 43.200 秒 / 5 轮故障恢复和 12.655 秒含收尾的持续观察,
清理通过. 修改后的规模入口 2/0、4/1、8/8、16/32 四个场景通过,
报告为 `build/testkit/results/peer-cpp-scale-1789187415213048227.json`.
新预演的 C++/Go 二进制摘要继续与维护验收记录逐个相同.
