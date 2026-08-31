# Go/Rust JSON 配置与 Catalog 写锁复审

> 历史说明：本文冻结的是 2026-08-29 的复审结论。2026-08-30 的
> SDK-018 已取消 Catalog 外部锁、Acquire/Release 脚本和全部锁配置；TLS 与
> JSON 结论仍有效。当前契约以 `catalog/api.md`、`configuration.md` 和
> `decisions.md` 为准。

日期：2026-08-29

## 1. 结论

当前配置设计已经从“人工保证两套原生结构数值一致”提升为“一份稳定 JSON
边界，加载后转换成各语言精确类型”：

- [`configuration.schema.json`](configuration.schema.json) 固定 v1 JSON 形状；
- [`configuration.example.json`](configuration.example.json) 完整列出所有字段，
  并由 Go/Rust 两边读取；
- Go `configuration` 包与 Rust `configuration` 模块都在 Redis、检查点或
  TLS 文件 I/O 之前完成严格解析、默认值展开、拓扑检查、数值转换和原生
  结构 `Check/check`；
- 原生配置仍由所属包负责最终关系校验，没有恢复配置代码生成器，也没有
  导出默认值/范围常量污染公共 API。

本轮已经把 `redis.tls` 从布尔开关扩成对象。两端都支持系统根、私有 PEM CA、
Standalone 固定 SNI 和成对的 PEM 客户端证书/私钥；每个文件最多读取 1 MiB，
TLS 最低 1.2 且不能关闭证书校验。JSON 解析只校验结构和路径关系：Go 在生成
`*tls.Config` 时读文件，Rust 在构造 Fred rustls 连接器时读文件。

Catalog 的“抢锁超时”不是锁回收，也不是一个常驻 Catalog 后台计时器。它
只是每个正在等待锁的前台写入所拥有的一次性总截止。锁自身的 TTL 才负责
进程消失或连接中断后的回收。保留外部锁时，两个机制缺一不可：TTL 防死锁，
获取截止防止不公平竞争让某个调用永远排不到。

“每次请求先取得原子锁，写完回收”本身可行。外部锁现已确定保留：Catalog
Publisher 通常是单点写入者，或只有少量网络距离很近的写入者，正常路径近乎
无竞争。当前实现进一步把回收和写入
放进同一 mutation Lua：确认成功时 Redis 原子写入并删除精确 token，不再由
SDK 发送第三次空 release；只有投影失败或没有确认成功的冷路径才尽力执行
token-fenced release，最终仍由 TTL 兜底。

## 2. 统一 JSON 边界

顶层固定为：

```text
version
redis
registration (optional)
catalog      (optional)
```

`redis`、`registration`、其内嵌 `selector`、以及 `catalog` 在两种语言中使用
相同 JSON 字段名和嵌套关系。外部层只包含字符串、布尔值、十进制整数、数组
和对象；加载后才得到语言相关类型：

| JSON | Go | Rust |
| --- | --- | --- |
| 毫秒整数 | `time.Duration` | `Duration` |
| Redis topology | `Standalone`/`Sentinel` | 经安全编码的 Fred URL |
| TLS 对象 | `*tls.Config`、`x509.CertPool` | 原生 `TlsConfig`、Fred rustls connector |
| 数量/字节 | 越界检查后的 `int`/`int64` | 越界检查后的 `usize`/`u64`/`u8`/`u32` |
| 本地路径 | `string`，Catalog 再规范化 | `PathBuf` |

两个加载器都限制输入不超过 1 MiB，并拒绝：

- 未知字段；
- 同一对象内的重复字段；
- `null`；
- 第一份对象后的第二个 JSON 值；
- 浮点/非整数数值；
- 非 `v1` 版本；
- Cluster 或不完整的 Standalone/Sentinel 拓扑；
- 超出范围的值以及 min/max 等关系错误。

省略字段会直接展开为文档默认值；显式零只在语义允许时保留。例如抖动 0、
Selector 立即发布、时钟人工误差 0、retained 0、Catalog 总视图限制 0 都不会
被误当成“省略”。超时显式写 0 会被拒绝。

TLS 对象另外保证：`system_roots=false` 时必须提供 `ca_file`；`cert_file` 与
`key_file` 必须成对；禁用 TLS 时不能留下会被静默忽略的非默认子字段；固定
`server_name` 只允许 Standalone。最后一条不是任意削减功能：Fred 无法把一个
固定覆盖传播给 Sentinel 后来发现的新主节点，若只让 Go 接受会制造跨语言
伪一致。Sentinel 应让证书名称匹配其实际公布的主机名。

## 3. 抢锁超时与 Catalog 计时器

### 3.1 三种时间不能混为一谈

1. 根 `timeout`：限制每一条 Redis 命令，默认 2 秒。
2. `catalog.lock` TTL：写入 Redis 锁键的生存期，默认按根超时乘 3 后钳制，
   全默认配置为 6 秒；所有者崩溃后自动回收。
3. `catalog.lock.acquire_timeout_ms`：一次调用为反复 `SET NX` 竞争总共愿意等待
   多久，默认 30 秒。

第三项产生的是一个调用期 one-shot deadline。它不启动 goroutine/线程，不在
空闲时存在，也不会定期唤醒 Catalog。Go 使用 `context.WithTimeout`，Rust 使用
`tokio::time::timeout`；它们只在并发写入正在等待锁时存在。

### 3.2 为什么锁有 TTL 仍需要获取超时

TTL 只能保证当前锁最终消失，不能保证某个等待者下一次一定获胜。Redis
`SET NX` 没有公平队列：A 的锁刚删除后，B、C 或一个新请求可能连续抢先。
如果只给每次 Redis 命令设置 2 秒超时，但允许 SDK 永久重试，那么每条命令
都能按时返回，整个前台 API 仍可能永远不返回。

因此，保留当前轮询锁模型时有三种合理策略：

- 当前策略：SDK 在有限总预算内重试，超时返回 `deadline`；
- 只尝试一次：锁忙立即返回 `busy/deadline`，由业务方决定是否重试；
- 引入公平分布式队列：保证排队，但 Redis 数据结构、清理和故障语义明显更重。

以 Catalog 目标约 10 次写入/秒而言，当前有限重试是最简单的折中。

### 3.3 写完回收是否有竞态

现在没有“写成功后再单独 DEL”的窗口。Replace/Patch/Delete Lua 在验证 token
后完成写入，并在返回成功前 `DEL` 同一锁键。错误辅助路径也只在 token 仍匹配
时删除。因此：

- 旧所有者不能删除新所有者的锁；
- 已确认成功不多一次 Redis 往返；
- 回复丢失时额外 release 最多是一次安全 no-op；
- 进程在任何位置消失时，TTL 最终清理锁；
- 锁若在 Patch 投影期间过早到期，最终 mutation 会因 token 不匹配而拒绝，
  不会在失去所有权后错误提交。

## 4. 本轮发现并修正的协议漂移

Go/Rust Catalog 配置一直允许 `max_record_bytes` 达到 4 MiB，但 Catalog Lua
生成源仍把 `maximum_encoded_bytes` 固定为 524,288。结果是应用配置为 4 MiB
后，本地校验会通过，Redis 却在 512 KiB 处拒绝。

生成源和全部 Go/Rust 嵌入脚本现已统一为 4,194,304。Redis 8.8 短验证实际
写入了一个编码长度超过旧 512 KiB 上限的 Value，并确认 4 MiB 以上仍被拒绝、
拒绝路径锁被删除、旧值保持不变。

## 5. 性能分析

### JSON

配置加载是冷路径，不进入 Registration Update、Selector 选择、Catalog 事件
解析或 Lua 热路径。为了严格拒绝重复字段和 null，Go 先做 token 扫描再解码，
Rust 先用派生结构拒绝重复/未知字段，再做一次 Value 扫描拒绝 null。最坏输入
被限制为 1 MiB，因此这份严格性成本有硬上界；当前没有理由为它增加复杂的
自定义单遍反序列化器。

### Catalog 写入

保留外部锁后的无竞争网络往返为：

| 操作 | 当前确认成功路径 |
| --- | --- |
| Replace | acquire Lua + mutation Lua |
| Delete | acquire Lua + mutation Lua |
| Patch | acquire Lua + HMGET 投影 + mutation Lua |

此前成功路径还会执行一次 release Lua，本轮已经删除该冗余往返。抢锁截止只
增加一个本地 timer/future 的生命周期，相比两至三次网络往返很小，但本轮未
用微基准虚构具体纳秒或分配收益。

## 6. 优点、缺点和修改方案

### 已有优点

- 同一 JSON 可直接供 Go/Rust 使用，字段和默认行为有真实双端测试；
- JSON 与语言原生类型分层，既不把毫秒整数带进运行热路径，也不强迫两种
  语言公开同构的底层驱动结构；
- 严格拒绝策略适合生产配置，错误尽早且字段定位明确；
- 原生 Config 自身仍可独立 `Check/check`，不依赖 JSON 才能正确使用；
- Cluster 明确拒绝，Standalone/Sentinel 的无意义字段不会被静默忽略；
- Catalog 写锁具备 token fencing、TTL、有限前台等待和原子成功回收；
- Lua 4 MiB ceiling 已与 SDK 配置重新一致。

### 剩余缺点与方案

| 缺点 | 影响 | 修改方案 |
| --- | --- | --- |
| Schema、Go DTO、Rust DTO 仍是三份人工源码 | 后续加字段可能漂移 | 保留完整示例双端测试；发布前可增加只生成“字段清单一致性测试”的小工具，但不要恢复生成运行时代码 |
| TLS 尚未在真实加密 Redis/Sentinel 拓扑上做握手和故障转移资格测试 | 当前证据证明配置、PEM 和连接器构造，不证明部署证书链与切主行为 | 发布前增加独立 TLS Standalone 与 Sentinel 集成矩阵；当前不虚报为已完成 |
| Rust 原生根 Config 内部仍以 Fred URL 保存拓扑 | 原生结构可读性低于 Go，凭据也必须编码进 URL 字符串 | JSON 外层已经屏蔽差异；后续可把 Rust 原生 Config 改成内部枚举，到 Client 构建时再生成 Fred 配置 |
| 两边为严格 null/duplicate 检查都解析两遍 | 冷启动有一次额外的 <=1 MiB CPU/分配 | 只在实际启动 profile 显示问题时改成自定义单遍 visitor；当前不值得增加维护成本 |
| 外部锁让 Replace/Delete 多一次 Redis 往返，Patch 多两步 | 在高写入率或高 RTT 环境吞吐下降 | v1 接受此成本，因为 Publisher 是单点或少量近端写入；若未来工作负载改变，应整体重审协议，不做局部删锁 |
| `SET NX` 重试无公平性 | 极端竞争时可能有调用直到总截止仍未获锁 | 当前返回 deadline；需要严格公平时只能引入排队协议或上层串行化 |
| 极端自定义 lock max TTL 可小于 Patch 两条命令的最坏总预算 | 不破坏数据，但可能在慢 Redis 下频繁丢失锁并失败 | 若保留锁，增加“有效 TTL 至少覆盖两倍根命令超时”的跨配置检查，或禁止把 TTL 上限调得过低 |

## 7. 外部锁最终决策

Redis Lua 本身就是单线程原子执行，因此 Replace 和 Delete 不需要外部锁。
Patch 当前需要锁，是因为 SDK 在 Lua 之前用 HMGET 投影完整编码字节数。Patch
Lua 已经检查 base revision、token、投影值和当前 Hash；若把配置的单记录上限
作为参数传入，让 Lua 根据当前字段长度计算最终大小，Patch 也可以成为一次
原子脚本调用。

理论上仍有两个自洽方案：

1. **保留当前锁。** SDK 继续拥有主要投影/校验逻辑；实现容易审计，目标
   10 次 Catalog 写入/秒下成本通常可接受。获取超时和 TTL 都应保留。
2. **整体取消外部锁。** 删除 acquire/release 脚本、锁键、TTL、获取截止和
   重试配置；Replace/Patch/Delete 各一次 Lua 往返。性能和配置表更简单，但
   Patch Lua 会承担更多容量计算。

最终确认方案 1：保留当前锁，并且已经优化掉成功后的重复 release。实际部署
假设是一个 Publisher，或少量距离 Redis 很近的 Publisher；在这个前提下，
无竞争获取占绝大多数，额外往返可接受，而 Patch 投影、编码预算和复杂校验仍
留在 SDK，Lua 继续是原子粘合层。

这不是说锁在所有工作负载都最优。若 Catalog 将来变成高频、多写入者或跨公网
写入，方案 2 才值得作为完整协议变更重新评估。当前不再把删锁列为发布前待决策
项，也不承诺 `SET NX` 公平性。

## 8. 验证结果

- Go：`go test ./...`、`go vet ./...` 通过；configuration 包测试覆盖共享
  示例、默认值、显式零、Standalone/Sentinel、未知/重复/null/尾随字段、
  地址、1 MiB JSON 上限，以及 TLS 私有根、mTLS、SNI、关系约束和 1 MiB
  单 PEM 文件上限。
- Rust：68 个库测试、4 个无需 Redis 的外部测试通过；严格 Clippy 和
  warning-denied rustdoc 通过。12 个显式要求隔离 Redis/Sentinel 的既有测试
  在本轮短回归中保持 ignored。
- Lua：生成一致性检查通过；唯一 Zone 的 Redis 8.8.0 Catalog 套件通过，
  覆盖锁 fencing、4 MiB ceiling、Replace/Patch/Delete、Pub/Sub、tombstone
  floor 和 `2^53-1`。测试后其命名空间键数为 0。
- JSON：schema 与完整示例均通过 JSON 语法解析；完整示例通过 Go/Rust 两个
  严格加载器。当前环境没有额外安装 JSON Schema validator，因此不把“外部
  validator 通过”列为证据。

## 9. 评分

### Go 配置：9.8 / 10

结构化拓扑、严格 JSON、默认值精确展开、原生 `Check`、字段级错误和全部短
测试都完整。TLS 对象已覆盖私有根、mTLS 和 Standalone SNI。主要扣分是 JSON
DTO 与 schema 仍需人工同步，以及真实 TLS Redis 拓扑尚未资格测试。

### Rust 配置：9.7 / 10

Serde 严格结构、checked conversion、惯用 Duration/PathBuf、原生 rustls
连接器和 Sentinel 凭据安全 URL 编码均清楚。扣分来自原生 Fred URL 形态、
严格 null 检查的第二次冷路径解析、与 Go/schema 的人工同步，以及 Fred 对
Sentinel 固定 SNI 的客观限制。

### 跨语言 JSON 契约：9.8 / 10

同一完整示例已由两边真实加载，缺省/显式零、TLS 关系和错误策略一致；私有
CA、双向 TLS 与 SNI 连接器构造都有双端测试。未达 10 的原因是没有自动证明
schema 与两份 DTO 的所有范围永远一致，也没有真实 TLS 服务握手证据。

### Catalog 外部锁：9.5 / 10

正确性强：token fencing、TTL、有限等待、原子写入/回收、错误兜底和 Redis
实测都成立；成功路径也已减掉一次往返。按单点或少量近端 Publisher 的真实
模型，额外获取往返是明确接受的权衡。扣分来自无公平性、工作负载改变后成本
会放大，以及极端 TTL 配置尚未与根 timeout 做跨结构关系约束。
