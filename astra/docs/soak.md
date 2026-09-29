# 三 Star 长时验收

本规约验证常驻稳定性, 不用于峰值 QPS 排名. 执行需要本轮授权; 入口只使用现有产物, 不构建、不下载、不修改部署配置. 最新执行结果见 [validation.md](validation.md).

## SDK 实现与验收边界

长测探针已迁移到 [当前 SDK](../comet/cpp/README.md): 有界测试线程并行调用 Catalog/Beacon 同步 update, 用实际回执验收, Beacon 切换保持同一逻辑 UUID. 有界三节点验收的最新执行结果见 [验证记录](validation.md); 已停止的无限长测未重新启动, 旧结果只证明当时源码, 有限冒烟不证明数小时稳定性.

标准模型为三台 Star 各自本地写入和交叉订阅. Catalog 每次显式更新带 TTL, 静默阶段应观察到期删除; Beacon 静默时按 beat 续租, Update 原子延期. Observer 的原有不可变 select 入口保留供全量一致性核对, one/Item 本地估计另有定向用例, 不把本地估计当作 Star 权威数据.

首次注册失败不重试、未知 update 不补发、迟到采样隔离、Reader 版本下限及 stop/destroy 关闭已加入源码用例, 仍须获准后执行. 既有证据保留原语义与源码身份.

## 入口与资源

```bash
python3 -B tests/soak.py --binaries build/cmake/release \
  --output build/results/soak-current \
  --fault-seconds 7200 --steady-seconds 43200 --interval 60 --records 16
```

输出目录必须不存在, 防止覆盖旧证据. 有限时长模式的总请求时长最多七天. 故障阶段至少完成三轮, 每台 Star 各故障一次; 达到时长后完成正在进行的恢复, 所以实际时间还包括初始化、恢复、TTL 清理和收尾. `--records` 为每台 Star 的 Publisher 和 Beacon 数, 各 1..16 个; 默认两域各 48 条, 每条 Catalog 正文和 Ephemeris Data 都为 2 KiB.

只有显式要求手动停止的长测使用以下入口, `--until-stopped` 与 `--steady-seconds` 互斥, 不用一个很大的秒数假装无限. `--fault-seconds 0` 仍依次完成 A/B/C 三轮故障恢复, 随后同一集群和同一批 SDK 对象保持运行, 总运行时间不设上限:

```bash
python3 -B tests/soak.py --binaries build/cmake/release \
  --output build/results/soak-current \
  --fault-seconds 0 --interval 0 --records 16 --until-stopped
```

不设总时限不等于取消故障检查. 单次 RPC、收敛和恢复仍有期限; 常驻原生探针必须持续输出完成全目标校验后的递增轮次, 超过 180 秒没有新轮次则失败. 重复进度不能刷新看门狗. 意外退出即使返回零也不能把无限模式标记为完成. 资源和日志预算仍生效, 失败后保留本轮证据并排查, 修复后开启新证据目录, 不合并多次运行时长冒充连续通过.

使用一个 Pulsar、一个 Polaris、一个 Astrolabe、三台 Star. 公共链路启用 TLS/APIKEY 登录, 真实 SQLite 仅位于夹具临时目录. Comet 只访问公共业务接口. 三个固定入口的 Client 同时写入, 三个固定入口的 Subscriber/Observer 验证全部来源, 不让观察客户端退避到来源节点来伪装复制成功.

普通回归不会自动启动长测. 原 `build.sh soak/scale` 及旧服务包装选项继续拒绝执行, 不能与本入口混用. 不与性能基准或另一套大构建同时运行. 先核实 Linux 实际可用内存, 不以虚拟机最大配置当作当前预算.

## 现有夹具阶段与断言

1. **控制面闭环**: 登录和凭据脱敏, Almanac Set/Delete/空值, Polaris 退出时缓存仍可读, 同数据库重启恢复权威版本, Astrolabe 抓取真实指标.
2. **三来源写入**: 每轮以有界工作者并行调用 Catalog 同步 update, 与 Beacon 同步 update 并行检查完成和回执. 在三个目标逐 Key/UUID 核对来源实例、版本、完整正文与固定 Attr. 一个目标缺失或内容错误就失败, 不以总条数相同替代内容核对.
3. **静默到期与续租**: 初次收敛后等待超过原三秒 TTL, Catalog 必须到期删除, Beacon 必须继续存活且业务投影版本不变. 后续每轮都有内容变化, 不让高频自动续租掩盖停止推进的正文.
4. **轮换故障**: 轮流强杀 A/B/C. SDK 自动切换时必须获得新 Star 身份和新 UUID; 旧注册到期消失. 原部署端口重启后必须重新取得 Almanac 和动态来源, 最后再次更新、空值、关闭并验证全体删除.
5. **常驻阶段**: 使用同一批 SDK 对象、同一 Scope/Key、同一集群持续写入和全目标核对. 不通过不断重建所有进程清空潜在泄漏. 多轮夹具的版本区间递增, 因为 TTL 删除不能清除 Catalog 版本水位.
6. **清理**: 正常、异常、SIGINT 和 SIGTERM 均关闭自己创建的进程组与临时目录. 正常完成记录 `completed`; 用户中止只记录 `interrupted`, 不冒充通过. 强杀的节点不声称通过退出时 LeakSanitizer 检查.

## 证据与停止条件

`events.jsonl` 记录阶段、二进制 SHA-256、单调采样时间和实际 UTC. 每五秒采集活跃 PID 的 RSS/峰值 RSS、线程、文件描述符及主机可用内存/Swap. 按 PID 比较, 不将重启前后内存下降解释为没有泄漏. 保存各服务最后 8 KiB 日志; 运行中扫描全部新增字节中的 Sanitizer 诊断.

意外服务退出、业务确认失败、内容不收敛、清理失败、Sanitizer 报告都失败. 可用内存低于 128 MiB 或日志预算耗尽也停止并标记失败, 不为了跑满时间冒险 OOM. 日志上限为单文件 128 MiB、合计 512 MiB; 它们是夹具证据预算, 不修改产品行为.

用户可向长测 Python 进程发送 SIGINT/SIGTERM, 然后等待进程清理完成. 不使用全局 `pkill star` 或清空整个 build 目录. 正式长测结束后检查常驻阶段的 RSS/FD/线程趋势、恢复次数与时间、全目标核对结果; 单个峰值无法证明泄漏, 短时冒烟也无法证明数小时稳定.

## 覆盖边界

这套长测准备好的是现有真实业务链路, 不是所有故障的穷举. 轮换故障时由专用 Publisher/Beacon 检查切换, 多记录并发在故障前稳定窗口和末尾常驻阶段执行; 不声称所有来源在故障全过程都保持满负载. 网络分区/高延迟/丢包、宿主机休眠、系统时钟阶跃、磁盘满/掉电、多机拓扑和大规模慢消费者仍须独立场景. 部分已有组件/RPC 故障注入测试不能替代这些系统级长期证据. Pulsar 停机对时保持和恢复另由 `cpp_pulsar_process` 验证; 本长测不会修改系统 NTP 或宿主机网络规则.

ASan/UBSan、TSan 使用对应已构建目录和明确的运行环境, 与普通版分开执行; Sanitizer 耗时不能作为生产吞吐. 有限窗口和持续到手动停止的长测均需分别记录实际参数与结果, 不沿用早期两小时加十二小时的固定验收时长.
