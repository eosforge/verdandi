# 当前实现的性能测量

本目录新增的 `native.cpp`、`almanac.cpp`、`comet.cpp` 测量当前三域及原生 Comet C++. `store.cpp`、`push.cpp` 是旧 Store/独立传输探针, 不能代替当前系统结果. 实际数据及结论只维护在 [最新验证](../../testkit/validation.md#performance); 本文定义复现方法, 不复制结果.

## 构建与执行

需要本轮明确测试授权. 仅消费项目已有工具和依赖, 缺失时失败, 不下载或安装. 使用 Release, 不将 Sanitizer 结果当作生产性能. 性能场景顺序运行, 不与编译、其他测试或另一份压测并行.

```bash
bash astra/build.sh configure --profile release --benchmarks
cmake --build build/astra/release --target star_native_bench star_almanac_bench comet_bench --parallel 1
python3 -B astra/bench/run.py --binaries build/astra/release --native --cases astra/bench/native.json --output build/performance/native
python3 -B astra/bench/run.py --binaries build/astra/release --almanac --cases astra/bench/native.json --output build/performance/almanac
python3 -B astra/bench/run.py --binaries build/astra/release --cases astra/bench/network.json --output build/performance/network
python3 -B astra/bench/run.py --binaries build/astra/release --cases astra/bench/auth.json --output build/performance/auth
python3 -B astra/bench/run.py --binaries build/astra/release --cases astra/bench/concurrency.json --output build/performance/concurrency
```

`star_restore_bench <每远端记录数> <Scope数量> <0或1>` 单独诊断六个本机写者与两个远端全量恢复的竞争, `0` 为无恢复对照, `1` 为并发恢复. 例如 `4096 1 1` 与 `4096 64 1` 保持总记录量相同, 仅改变恢复的 Scope 分布. 使用真实 Catalog/Origin/Scene, 业务时间固定, 不含网络、Pulsar 或 SDK, 不将此成绩冒充三 Star 系统吞吐. 普通 CTest 不自动运行性能探针.

构建并发应按当次实际资源调整, 示例的一任务不覆盖项目的自适应并行规则. 网络场景还需要先通过统一构建入口生成当前的 `star`、`pulsar`、`polaris`、`astrolabe`; 运行器不会替用户构建. 输出目录必须不存在, 不覆盖已有证据. `--repeat=1..5` 控制有限轮次, 默认三轮, 不启动长期后台任务.

Polaris 使用真实 SQLite 文件、生产 WAL/FULL 配置和已填满的历史, 建库/预填不计入提交耗时. 在本项目的离线 Go 环境中运行:

```bash
python3 -B - <<'PY'
import subprocess, sys
sys.path.insert(0, 'astra')
import build
env = build.environment(jobs=build.resources()[0])
subprocess.run([str(build.ROOT / 'build/tools/go-1.27.1/bin/go'), '-C', 'astra', 'test', './polaris/internal/storage', '-run', '^$', '-bench', '^BenchmarkCommit$', '-benchtime=2s', '-count=3', '-benchmem'], env=env, check=True)
PY
```

## 统计口径

- 原生微基准: 每个普通操作 20,000 次, 逐次计时, 输出吞吐及最近秩 p50/p95/p99/p99.9. 记录准备在窗口外. Catalog/Ephemeris 使用预先分配的不可变正文; Almanac 拥有式接口的正文/Key 复制计入窗口. 不能据此直接横比三域的纯算法效率. 全量恢复只有五次/轮, Create 次数等于记录数, 单次长暂停只有一次/轮; 这几项主要看持续时间, 不宣称尾部分布已稳定.
- 时钟/采样本身有成本, 不作估算扣减. 亚微秒数值用于本机量级判断, 不承诺同等精度. 原生域使用固定可用业务时间排除 TTL 到期; Agenda 单独测 1 小时、1 天和 1 周完整空拍追赶, 不是实际将操作系统挂起.
- `comet.commit`: 从调用开始到实际写入确认. `comet.visible`: 同一次调用到所有本次 Subscriber/Observer 的推流回调都确认正确版本/正文. 回调只从传入的 View 点查本次热点, 不反向轮询共享 Subscriber/Observer. 每个写者由独立条件变量唤醒, 后者包含 Star 复制、推流、SDK 投影安装及应用通知/线程唤醒, 不只是 gRPC 传输延迟.
- `rate=0`: 固定 1/8/16/32 个写者的闭环负载, 每写者最多一个在途请求, 等所有观察者可见才写下一条. 完成率是该负载下的端到端吞吐, **不是 Star 独立服务的最大 QPS**. Watchers 是逻辑流数量, 不等于 TCP 连接数. 每个目标 Star 共用一个订阅 Client, 生产者也共享一个 Client.
- `rate>0`: 从公共起点按固定速率产生计划时间, 延迟从原计划计算, 包含未能及时发起的客户端排队. 仍保持每写者单在途, 不将客户端积压称为服务器拒绝或已收到的请求; 不是无界并发的开环网络发包器. 超过总时间预算失败, 不删掉积压样本来美化尾延迟.
- 网络样本先创建全部记录, 等真实订阅完整安装, 再等待 200 ms 排空握手工作, 正式窗口默认五秒. 初始化及正常退出不计入网络吞吐, CPU/RSS 资源轨迹覆盖整个探针进程, 包含初始化. 所有写入和可见性检查必须成功, 失败使样本无效而非跳过. 已有基线存储/认证/副本逻辑没有测试替身.
- TLS 只切换公共业务接口; 内部始终使用真实准入和 TLS. 认证场景由 Astrolabe -> Polaris 正式安装随机测试凭据, 再由 Comet 登录; 不向 Star 内存直接塞入 Session. 临时密码/SECRET 文件仅存本次私有临时目录, 结束后删除.

## 资源与证据

运行器保存产物 SHA-256、CPU/亲和性、内核、实际内存、换页计数、原始分位数及各进程 RSS/线程/CPU 轨迹. 每份网络样本独立启动 Pulsar、Polaris、必要的 Astrolabe 及 1/2/4 个 Star, 结束或异常时仅停止本次进程组并删除临时数据库. 日志和结果留在指定 `build/` 目录. 不设置系统参数、关闭动态内存或修改全局环境.

`stable_memory=false` 表示本次范围内内存总量变化或系统换页计数增加, 原始结果保留, 不混入稳定样本中位数. 该标志不是“系统完全无干扰”的证明: 同宿主调度、其他工作负载、频率和冷缓存仍会造成波动. RSS 采样约 20 ms/200 ms, 不是逐次分配审计; 多进程 RSS 求和会重复计入共享页, 不等于物理常驻内存. 现有 swap 占用与本次新换页分开报告.

在同一 VM 中运行客户端及多台 Star, 可以验证实际协议路径和相对扩展成本, 不能推导跨物理机网络、真实生产容量、长期内存稳定性或 p99.99 SLA. 未覆盖的验收矩阵分支继续见 [B01–B14](../../testkit/comet.md#性能与规模矩阵), 不因本套有限基准通过而一并关闭.
