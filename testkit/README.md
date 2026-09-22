# Astra 测试入口

测试与其前置构建先遵循 [AGENTS.md](../AGENTS.md) 获得本轮明确授权. 本文只定义执行方法, 最新实际结果维护在 [validation.md](validation.md). [Comet 首版验收](comet.md) 独立维护业务场景与用例映射; 规约包含尚未执行的性能和规模矩阵, 不能将常规回归通过扩展为全部场景通过.

## 服务回归

当前核心链路使用下列 Linux 入口, 离线构建 C++ Star/Pulsar/Comet 和 Go Polaris/Astrolabe, 不调用退休 Supervisor/Planet 的综合场景:

```bash
bash astra/build.sh regression --profile debug
bash astra/build.sh regression --profile release
bash astra/build.sh regression --profile asan
bash astra/build.sh regression --profile tsan
```

每个 regression 先配置与构建, 顺序执行 Go 包测试、CTest 和 C++/Go 协议生成逐字节比较. CTest 包含原生三域、RPC/SDK、独立 SDK 消费、Pulsar 进程及多 Star 恢复夹具. 项目数量以实际 CTest 清单为准, 不沿用旧连接骨架的 21 项统计.
ASan 同时启用 UBSan 与泄漏检测; TSan 使用已有独立插桩依赖. 不下载、安装或升级缺失工具. 测试数量不是代码覆盖率, 存储单元通过不替代真实进程验收.

当前 C++26 只支持 Linux/GCC 16.2, 不声称 MSVC 已通过. 本轮活动 Go 模块为 astra/go.mod; 旧 Supervisor 不属于新链路验证. Windows Admin 的验证入口见其 README.

## 长时、规模与实验

长时与规模必须在授权范围内显式选择. 当前 build.py 明确拒绝旧 soak/scale, 待迁移到新四件套后再开放; 不用旧场景冒充新业务压力验证. 普通 regression 不因内部 duration 默认值而自动变成长测, 不恢复已经停止的无限测试任务.
当前三域与真实 Comet 的有限性能入口见 [bench/README](../astra/bench/README.md), 实际结果见 [性能记录](validation.md#performance). `--benchmarks` 仅启用探针编译, `--measure-allocations` 使用独立 Release 目录; 分配统计不用于正常性能排名. core-only 只证明其子集, 不能替代真实 RPC/进程验收.
旧 Redis 与当前 Comet 的同口径应用比较见 [统一基线](baseline/README.md), 覆盖多注册/Selector、多 Publisher/Subscriber、独立范围和共享 Client. 其轮询可见性口径与原有回调探针分开报告.

## 并行与清理

VM 已由用户调整为 16 核、最高 8 GiB. 下次执行前检查实际可用资源; 构建与测试共享预算, 独立用例尽量并行, 尊重 RUN_SERIAL/RESOURCE_LOCK 和各夹具的端口/目录隔离.
build.py 读取实际 CPU、MemAvailable 与可读的 cgroup 上限, 分别计算构建/测试并发; --jobs 与 --test-jobs 只降低上限. Go 和 CTest 顺序调度, Sanitizer 单任务预算更高. 动态内存尚未伸展时可能仅允许一个编译任务, 不能按配置最大值过度并发.

测试只拥有本轮创建的进程、目录和端口, 退出或失败后清理并验证回收. 不复用部署数据库, 不广泛杀进程或删除不属于本轮的资源.
缺失工具、必测跳过、Sanitizer 报告、清理失败都必须显示为失败或未完成; 不静默重试直到通过.

## 证据与冻结范围

原始日志、源码/二进制摘要及结果写入忽略的 `build/`, 默认服务报告位于 `build/testkit/results/`. 最新人工汇总写入固定 validation.md, 旧版本保留在 Git.
Linux 项目为 `/home/ubuntu/verdandi`, 同步修改过的输入并复用 `build/astra` 和项目依赖缓存; 不为每次测试重建第三方依赖.
旧 SDK 的测试说明见 [legacy-sdk.md](legacy-sdk.md). Redis 场景和退休 transport 实验不属于 Astra 当前业务验收; 共享辅助与公开夹具继续按实际消费者保留.
