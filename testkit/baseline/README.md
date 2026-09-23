# Redis / Astra 统一应用基线

使用冻结的 Redis C++ SDK 和当前 Comet C++ 的公开接口. 共用 `workload.hpp` 生成、调度、核验和统计同一负载; 两个适配器独立进程、独立链接, 不混合 OpenSSL/BoringSSL. 当前实测结果只维护在 [validation.md](../validation.md#baseline).

## 整理边界

- 共用严格参数解析、正文生成、完整视图验收、线程异常传播、有限等待和统计. SDK 适配器只处理公开接口差异, 不接入私有 Store 或裸 Redis 写命令作为性能捷径.
- 不为跑分改变生产协议、历史保留、TTL、确认语义或恢复逻辑. 旧 SDK 保持冻结.
- 所有写者结束后重新读取每个订阅的当前视图, 验证记录齐全、无重复、版本/正文正确和固定 Attr 完整, 不能用历史观察水线代替当前状态. 后台诊断非空使样本失败, 不将超时或失败记入成功吞吐.
- 明确的探针负载失败会记录并继续其他预定场景, 全部结束后返回非零. 环境、内存预算或清理失败立即停止. 不无限重试, 不用成功重跑覆盖失败证据.
- 独立的 `cpp_baseline_workload` 验证多组/多订阅/多写者、两种完成模式、数字边界、后台错误传递及“曾观察到、最终却丢失”必须失败. 真实适配器再由服务场景验证.

## 负载与统计契约

`records` 是全场景记录总数, 均匀分布到 `groups` 个范围. 每个范围有 `fanout` 个真正独立的 Selector/Subscriber/Observer, 总订阅数为 `groups * fanout`. 生产侧和消费侧各有 `clients` 个共享 Client, 记录/订阅轮流分配. `writers` 个线程拥有互不重叠的记录序列, 轮转更新全部记录, 每线程最多一个在途请求.

标准矩阵从 **3 台 Star 同时写入** 开始. `stars` 缺省为 3, 每个生产 Client 只持有一个固定 Star 地址, 按 Client 序号轮转; 消费 Client 再错开一个节点. `clients`、`writers` 必须覆盖并整除 Star 数, 每 Scope 的 `fanout` 至少等于 Star 数, 保证全部来源都有写入且每个 Scope 在全部节点上验收. 不把多个地址传给同一个 Client 后依赖故障转移来制造负载分布. 单 Scope 测三来源合并, 多 Scope 测独立范围的同时更新. Ephemeris 始终向注册它的 Star 更新/续租, Catalog 不同来源使用互不冲突的 Key.

`cases.json` 为三 Star 标准矩阵, 共 36 个配置, 包含两个动态域各 24 写者/6 Client 和 24 写者/3 Client 的提交场景. Catalog 另包含 2048 字节正文的普通/高扇出, 两种完成模式各一项; 与相同拓扑的 128 字节场景比较, 2K 明确为 2 KiB, 不是总 RPC 线长. 当前探针最多支持 32 个写者, 不将参数拒绝记为性能失败或零吞吐. 单/双 Star 必须显式配置 `stars` 和 `diagnostic: true`, 只用于定位串行成本或协议边界. 测试参数变化后不得与旧单 Star 数字直接计算代码提速比例; 前后代码必须重新使用相同拓扑、参数和总资源测量.

逻辑 Data 前十六字节为记录编号和递增版本, 剩余为固定内容; 两个适配器使用相同正文和 Attr. 旧 SDK 为满足 Fields 接口多包一个 `body` 字段, 编码、复制和解码成本保留. 不对真实接口成本做估算扣减.

| 模式 | 下一次写入条件 | 延迟 | 吞吐含义 |
| --- | --- | --- | --- |
| receipt | 本次写入获得服务端确认 | commit p50/p95/p99/p99.9 | 多订阅同时在线时的应用提交完成率 |
| visible | 确认且该范围全部订阅观察到本次正文 | 同时报告 commit 与 visible | 包括采样和最慢订阅的完整可见完成率 |

两种模式最后均验收全部最新记录. receipt 模式可能合并中间更新, 不报告未经观察的逐次可见延迟. 旧版没有对应的新式推送回调, 因此两边都由一个采样线程轮询公开视图, 默认间隔 1 ms. visible 含采样量化、扫描耗时和线程唤醒, **不能和原有 `astra/bench/comet.cpp` 回调型延迟直接相除**. `sampler.busy_seconds` 是整个探针生命周期的扫描墙钟累计值, 不是 CPU 时间; 进程 CPU 另由 `/proc` 采样.

`rate=0` 是固定并发闭环. `rate>0` 从计划发送时间开始计时, 包含应用排队; 超过有界排空预算报错. 初始化在测量窗口之外, 吞吐包含最后一次回执/可见等待, 最终一致验收单独进行. 每轮五秒是有限基线, p99.9 仅作观测, 不作为生产 SLA.

## 明确的差异

- 两边均为回环网络、关闭业务认证/TLS、纯内存业务提交. 标准 Astra 模型真实启动 Pulsar/Polaris/三台 Star, 内部链路保留 TLS/准入和全网复制; 冻结 Redis SDK 仍使用一台 Redis, AOF 与 RDB 关闭. 比较是固定总 VM 资源下的应用方案成本, 不是相同复制保证的数据库引擎排名; 报告须明确节点数、每节点及合计资源, 不将三台吞吐相加后称作单台提速.
- 旧 Selector 的 `view_publish_interval` 显式设为 0, 去掉默认 10 ms 合并等待; Redis 根连接池上限为每 Client 32, Comet 每 Client 允许 128 个 reader. 逻辑 Client 数相同不代表实际 TCP、HTTP/2 流和线程数相同, 资源轨迹如实记录.
- `coalesced.json` 显式保留单 Star 诊断, 恢复旧 Selector 默认 10 ms 合并窗口, 对照 256 条注册/8 个 Selector 的提交与可见完成率. 不与三 Star 零合并主矩阵混合, 不把 10 ms 配置当作实际可见延迟上限. Comet 对照侧不改变生产配置.
- 旧 Registration 更新刷新 TTL, Comet 的 Data 更新和续租独立. 旧 Catalog 没有动态 Catalog 的租约. 短 TTL 场景刻意包含实际自动续租差异, 不宣称协议工作量完全相同.
- 旧 Catalog 的 Entry 读取执行 Fields 解码, 旧 Selector 的候选事务执行其公开投影步骤; Comet 遍历不可变 View. 这是应用接口的实际成本, 不是纯网络带宽测量.

## 离线构建与执行

必须先取得本轮构建/测试授权. 缓存缺失直接失败, 不自动下载. 使用同一 GCC 16.2, Release; 旧 SDK 保持 C++23, 当前 SDK 使用 C++26. 从已配置项目环境执行:

```bash
cmake -S astra -B build/astra/release -DASTRA_BUILD_BENCHMARKS=ON
cmake --build build/astra/release --target baseline_comet baseline_workload_test
cmake -S testkit/baseline -B build/baseline-current/redis-release \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_C_COMPILER="$PWD/build/tools/gcc-16.2.0/bin/gcc" \
  -DCMAKE_CXX_COMPILER="$PWD/build/tools/gcc-16.2.0/bin/g++" \
  -DVERDANDI_DOWNLOAD_CACHE="$PWD/build/deps/common/downloads/fetchcontent" \
  -DVERDANDI_OPENSSL_ROOT="$PWD/build/deps/openssl/linux/x64"
cmake --build build/baseline-current/redis-release --target baseline_redis
python3 -B testkit/baseline/run.py \
  --binaries build/astra/release \
  --legacy build/baseline-current/redis-release/baseline_redis \
  --cases testkit/baseline/cases.json --repeat 3 \
  --output build/baseline-current/results
```

构建并发依据实际可用内存设置; 性能场景不得与构建/其他测试并行. 原有 Release 服务产物应先通过项目入口完成构建. Python 使用标准库和项目已有测试辅助, 没有新 Python 依赖. C++ 文件使用 `astra/.clang-format`.

运行器只使用已缓存的 `redis:8.8.0`, `--pull=never`, 回环监听、关闭持久化、512 MiB 容器内存限制. Docker 需要已有访问权限; 可用 `--sudo` 消费已经授予的 `sudo -n` 权限, 不配置全局 Docker 用户组. 正常退出、异常和终止信号都会停止自有服务并移除自有容器/临时数据库. 外层已管理 Redis 时可提供 `--redis` 与 `--redis-pid`; 此模式由外层负责容器清理, **仅可指向本轮独占实例, 每场会 FLUSHALL**.

各场景交替先后顺序, 默认重复三轮. 原始结果包含逐轮参数、产物摘要、内核/CPU、实际内存、换页、RSS/线程/CPU 轨迹和失败原因. 内存扩容或换页的样本保留但不混入稳定样本汇总. `python3 -B testkit/baseline/report.py build/baseline-current/results/results.json` 只比较同场景同轮次双方均稳定的配对样本, 输出各轮分位数中位数、吞吐范围及有效轮数. 不足三对时明确保留数量, 不伪装成三轮稳定结论.

Redis 在矩阵内复用本轮独占容器并逐场清空业务数据, allocator/脚本缓存仍可保留; Star 每场重启. CPU 应使用该样本期间的增量, 不能比较 Redis 累计 CPU 时间或把其跨场景 HWM 当作本场峰值. RSS 采用本场实际采样, 初始化也在资源轨迹内. 输出目录必须不存在. 同机结果能建立相对回归基线, 不能外推跨物理机容量或所有生产工作负载.
