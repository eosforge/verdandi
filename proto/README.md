# 当前 gRPC 协议与准入

实现阶段统一使用 v1, 不维护旧 v4/v6、MessageID 隧道、别名 RPC 或兼容回退. 下表列出当前接口及归属; 构建与执行状态以具备源码身份的验证记录为准.

| Schema | package / C++ 命名空间 | 状态与职责 |
| --- | --- | --- |
| [orbit.proto](orbit.proto) | `proto.orbit.v1` / `proto::orbit::v1` | Register、List、Member、四种 Role、Credential; 当前准入由 C++ Pulsar 提供 |
| [astra.proto](astra.proto) | `proto.astra.v1` / `proto::astra::v1` | StarTransport 单条类型化双向流承载控制与两动态来源; 旧 SyncTransport 已移除 |
| [pulsar.proto](pulsar.proto) | `proto.pulsar.v1` / `proto::pulsar::v1` | C++ Pulse 四时间戳对时 |
| [comet.proto](comet.proto) | `proto.comet.v1` / `proto::comet::v1` | 三域业务、Session、Watch 和错误消息; Star 与原生 Comet C++ 已接入 |
| [polaris.proto](polaris.proto) | `proto.polaris.v1` / `proto::polaris::v1` | Authority 管理及 Almanac 双向流; Go 权威端和 C++ Star 接收端已接入 |

业务按 [当前架构](../docs/architecture.md) 分为 Almanac、Ephemeris、Catalog 三域. 下方维护 [Comet v1](#comet)、[来源恢复](#replication) 与 [Polaris 同步流](#polaris-stream) 的已确认边界; 语言 API 见 [Comet C++](../astra/comet/cpp/README.md), 存储见 [存储](../astra/common/README.md), 验收见 [Comet 验收](../testkit/comet.md). Comet Go 暂缓. 当前源码接线、待实施与待验证状态见 [进度](../docs/progress.md), Schema 已生成不表示完整业务已交付.

三域使用专用结构. Catalog/Ephemeris 的来源组版本范围均为 (来源 Member.id, 业务域), 覆盖组内全部 Scope; Ephemeris 直接保存完整原生记录. 下游动态 Watch 仍使用接入 Star 的 Scope 视图游标, 不向 Comet 暴露来源组清单或对账. 数据布局和投影边界见 [存储设计](../astra/common/README.md#data), 已实现的回归基线与后续未验证修改分别列在 [验证记录](../testkit/validation.md).

APIKEY/APISECRET 仅用于登录准入, 存放于内部 Almanac, 字段见 [Credential](#credentials), 不采用 Grant、范围权限或 APIKEY 所有者规则. 凭据类型归内部管理协议, 不复制到 Comet 公共协议.

手写 C++ 在 `astra` 命名空间, 直接使用完整生成类型, 不使用 wire/orbit/probe 协议别名. Go 生成代码中的包名 wire 是生成组织, 不改变协议归属.
Member 与 Role 只在 Orbit 定义, Hello 原样携带已签名 Member 字节. HTTP/2 连接由 gRPC 管理, 业务身份绑定逻辑 RPC 会话.

<a id="admission"></a>

## 准入身份

当前准入由 C++ Pulsar 提供, 独占 SQLite 成员事务在 COMMIT 成功后确认, 已纳入成员库、故障注入和真实进程回归. 首版只支持显式初始化新群组及恢复该 SQLite 库, 不实现旧 journal 导入. 身份、幂等与持久确认契约如下, 容量与初始化边界见 [Pulsar](../astra/pulsar/README.md#sqlite).

### 一次启动

1. Star 加载账号密码、TLS 材料和登记服务的准入公钥.
2. 本进程生成一次 32 字节随机 `request_id`, 仅用于登记幂等, 不作为实例身份或授权凭证.
3. 通过 TLS 发送一次 `Admission.Register`, 携带账号密码、Galaxy、具体端点、角色、group 和 request_id.
4. 登记服务验证账号及角色, 通过所属持久存储登记实例、保存请求结果依据并取得完整成员快照.
5. 提交成功后签发唯一实例的准入凭证, 与 Star 名单一起返回.
6. Star 对凭证原始字节验签, 核对自身部署信息并固定实例身份, 随后连接名单中的 Star.

不再有 Challenge RPC、RegistrationChallenge、LoginRequest、启动 Ticket、第二个签名域,
或由客户端提交的 id/expected_epoch. 普通重连不再次登录, Planet 原有候选刷新复用同一请求键.

### 身份和会话

- `id` 在线上使用 bytes, 内容保持合法 UTF-8 (当前为 `p_` 前缀文本), 1..128 字节. 它是不透明标识, 客户端不生成或解析它; C++/Go 的准入入口继续校验文本有效性.
- `principal` 是账号、Galaxy、规范端点的 SHA-256 原始 32 字节指纹, 用于识别可被新实例替换的部署槽位.
- `epoch` 由登记服务在持久提交中分配, 只排序同部署的实例替换, 不承担业务版本含义.
- `Generation` 只属于本地 RPC 生命周期, 用于忽略旧会话完成事件, 不发送给登记服务.
- request_id 只发给登记服务, 不复制到成员名单、Hello、日志或业务版本中.

两个全新启动请求按登记服务的提交顺序登记. 后提交者替换前者, 不比较物理启动时间;
从未提交的迟到请求无法被识别为较早进程,
不得将这一规则描述为墙钟最新进程保证. 正常客户端同一进程永远不更换 request_id.

### 幂等与持久记录

同一次启动的每次重试使用相同 request_id 和部署信息:

- 第一次事务成功后, 回复丢失也不会改变已签发 id 或再次增加 epoch.
- 该请求仍对应当前实例时, 返回相同准入正文及最新名单; 候选轮转不改变身份.
- 该请求已被其他启动替换时, 返回 Aborted. 登记服务重启后也不能让旧请求重新占位.
- 相同 request_id 改变账号、端点、角色或 group 会被拒绝.
- 事务失败不留下部分成员或请求记录. 并发重复请求只产生一次提交.

启动记录受持久预算限制, 不能通过自动驱逐旧请求解除容量限制, 否则旧请求可能再次登记. 当前没有压缩或在线回收协议, 不承诺无限次重启. 存储格式、工程上限及正常恢复只在 [Pulsar 成员库](../astra/pulsar/README.md#sqlite) 定义.

### Star 接入验证

`StarTransport.OpenSession` 的首条消息仍为 Hello. Star 本地校验:

1. 主版本、消息边界和登记服务的 Ed25519 签名.
2. Galaxy、角色、端点及已知部署替换关系.
3. 当前逻辑会话的重复或冲突.

准入只有一个签名域: `proto.orbit.v1.admission` + NUL + 原始 Member 字节.
后续消息沿用会话身份, 不逐条重新验签; Star 不处理其他节点的密码、登记请求键或持久幂等索引.
基础设施认证通过不代表初始同步完成, 角色仍限制系统职责; 普通 Comet 准入成功后不再区分业务权限, 但始终拒绝内部范围.

TLS 1.3 继续负责加密与服务端证书验证, bearer 凭证继续承担客户端身份.
已运行节点可以在登记服务离线时使用现有凭证通信; 新进程等待登记服务登录.
当前不提供凭证 TTL、在线吊销、共享密码直连或 mTLS 证书签发机制.
成员替换只在收到较新凭证后被本地观察, 不承诺所有隔离副本即时撤销旧实例.

<a id="directory"></a>

### 成员名单查询

登记后的成员发现采用按需及低频、带抖动的只读 unary RPC, 不增加成员 Watch、目录增量日志或新目录 revision. 该接口属于 Pulsar 的 Orbit 控制面, 使用内部 TLS 与已有准入身份; Star、Polaris 和 Astrolabe 复用同一查询能力, Comet 不访问. 注册回复仍提供初始名单, 后续查询不重复 Register、密码派生、签发或持久写入.

每次查询在初始 metadata 携带唯一的 astra-admission-bin 与 astra-signature-bin, 复用现有 Pulse 的字段及签名域. Pulsar 校验大小、签名、Galaxy、允许的基础设施角色和当前成员身份, 不能只验签而放行已被替换的进程. 凭据被明确拒绝时不生成新 request_id 抢占身份; 暂时不可达则保留原身份和旧名单并报告陈旧. 本次查询不建立新的登录会话.

服务端从一个已提交的不可变成员快照取得本次结果, 同一快照也用于判定请求身份是否当前有效. 响应只含该 Galaxy 的当前 Member 列表, 不含启动索引、账号材料、业务内容或逐节点健康结论. 新角色接入后, 注册初始名单与此查询使用一致的角色投影; Star 从中筛出 Star 对等目标和 Polaris, Astrolabe 取得观测对象, 不把所有角色都加入全互联. 既有 Planet 候选路径不因此成为首版新增功能.

一次请求返回完整、有界的成员快照, 不按实时表逐页拼接不同时间的名单. 登记容量与收发编码预算必须配套, 合法部署容量内的完整名单应可传输; 超出预算明确失败, 不截断后返回成功. 编码与发送在登记锁外执行, 共享已发布的只读状态, 不逐次遍历磁盘启动历史或令慢查询阻塞 Pulse.

每个进程共用一个刷新调度, 同时至多一个实际未完成的查询. 周期工程初值为 30 秒并加入抖动, 可配置而非协议要求. 缺少必要角色、目标持续不可达或可信替换提示可请求提前刷新, 仍合并为一次调度并受最小间隔、退避、deadline 和总预算约束; 不为每个对端、分组或业务 RPC 独立拉名单. 失败后的周期触发不能绕过退避, 发出取消也不提前返还尚未完成的查询额度.

收到完整合法响应后更新发现视图, 失败/畸形响应保留上一份完整名单. 名单是捕获时的观察, 不能因旧查询晚到而撤销已从合法握手观察到的较新同 principal 实例. 复用现有替换关系和本地生命周期隔离, 不再引入一套目录代次; 相同 epoch 对应冲突身份时明确报错. 名单缺项或抓取失败不等于对端已退出, 不据此删除业务数据、撤销健康流或清除已知替换依据.

新增候选只唤醒已有连接管理并受拨号并发/退避预算约束, 不在每次刷新后重建所有流或向其他 Star 广播整份名单. 临时目录失败不阻断已获准节点的现有通信、TTL 推进和已安装 Almanac; 明确撤销本进程凭证时停止服务, 冷启动缺少 Polaris 继续等待. 接口为 Admission.List(DirectoryRequest) -> DirectoryResponse, 字段以 [orbit.proto](orbit.proto) 为准. 它有发现延迟, 不承诺变化即时到达所有节点.

### Polaris 与 Astrolabe 管理接入

Astrolabe 是 Go 管理入口, 首版管理用户仅做登录验证且使用相同的已开放管理能力, 不增加管理账号的角色/Scope ACL. 它不拥有持久管理库, 也不直接成为 Almanac 发布者. Astrolabe 接受管理请求并调用唯一 Polaris, Polaris 持久提交后向各 Star 同步. 管理成功以 Polaris 的持久提交为界, 不以某一 Star 的内存提交或订阅回环代替.

基础设施统一经 Pulsar 准入和内部受保护连接, 不保留架构级 standalone 免准入分支. Polaris 接受 astrolabe 身份的管理写入, Star 接受 polaris 身份的 Almanac 安装; Astrolabe 的观测身份不能绕过 Polaris 直接修改 Star 的权威 Almanac. 这些服务身份不等于浏览器登录, 节点账号、签发材料及 Comet SECRET 不交给前端. 关闭 Comet 认证或业务 TLS 不取消内部身份校验与 __ Sector 隔离.

Orbit Schema 已增加 polaris/astrolabe, 不把它们放入 Star 对等拨号名单、Planet 候选或数据副本数. 登记成员与实际可达性分开, 不将浏览器会话登记为节点. [Astrolabe](../astra/astrolabe/README.md) 已确认部署单管理账号、内存会话与经 Polaris 提交; 必要 HTTP/RPC 字段、脱敏读取与错误映射已经实现接线, 仍待本轮运行验证. 角色变化须同时检查 C++ 与 Go 的生产者、消费者和夹具, 未支持的新角色不能退化为 Star.

<a id="polaris-stream"></a>

### Polaris 发现与同步流, 实施中待验证

Star 从 Pulsar 的可信登记名单发现同 Galaxy 的唯一 Polaris, 不额外配置 Polaris 固定地址或将 Astrolabe 当作服务发现代理. 名单只提供候选身份及登记端点, 不证明进程在线、数据库已恢复或 Almanac 已可用. 实际建流仍校验内部 TLS、准入签名、角色、Galaxy 和可信部署替换关系; Star 主动连接不表示它可以向 Polaris 上传业务底稿.

按已有规则识别同 principal 的当前实例后, 没有 Polaris 时等待并有界退避; 存在多个不同部署的 Polaris 时报告单点部署冲突, 停止建立或推进有歧义的 Almanac 同步. 不按在线先后、不同 principal 的 epoch 大小或谁的版本更高自动选主, 不做多地址轮询写入. 已安装数据继续按原状态提供并标记同步异常, 冷启动不能跳过初始基线. 名单通过 [只读查询](#directory) 刷新, 不把现有 Register 当作已经提供持续成员通知.

Star 主动打开 Polaris 的内部双向 gRPC 同步流. 每个 Star 实例只保留一个当前 Polaris 同步流, 复用它发送恢复位置、实际安装确认及版本核对响应; Polaris 在流中下发范围清单、快照、增量与核对请求. 一条流承载全部 Almanac 分组, 不按分组、快照或确认额外建连接. 这条流不承载 Astrolabe 管理提交或 Star 对等动态数据, 也不是 Comet Session.

双方在任何业务页前交换并校验已有格式的签名准入材料, 发送方与接收方角色固定为 Polaris 与 Star. 后续消息沿用该逻辑流身份, 不逐页验签. 消息名称/字段号以 [polaris.proto](polaris.proto) 为准, 不复用旧 SyncTransport. Snapshot 每页携带相同 Scope 和 version, 便于直接构建私有候选; 只有 complete 后才能安装和确认, 提前知道版本不表示提前提交. 恢复位置区分无基线与已安装版本 0, 新 Scope 先发送完整快照而不是用首个 Patch 偷建零基线.

Star 只报告已经完整安装的每 Scope 权威版本, 不报告接收/排队/暂存页的进度. 内部新 Patch 携带该 Scope 的下一权威版本并严格承接 +1, 跳号先恢复缺失历史或快照; 已安装范围内的重放不重做写入. 可以传输连续补丁批次, 不能按 Key 合并后跳过中间版本来冒充 +1. Polaris 按持久底稿和仍连续的历史选择增量或完整快照; 版本核对复用此流, 不增加每 Scope 定时 RPC. 恢复清单和后续新增范围不能遗漏, 分页失败不推进确认. 底稿、快照与持久提交的约束只在 [Polaris](../astra/polaris/README.md#向-star-同步) 定义, 不将 Star 对等来源序号引入 Almanac.

Polaris 按 [固定部署规则](../astra/polaris/README.md#deployment) 从同一数据库重启后, 节点 Member.id 可以变化, Almanac 权威版本不随它归零. 重新验证身份后可使用仍保有的完整安装版本请求恢复, 当前库必须能支持对应范围与版本; Star 报告高于底稿的版本时停止该范围覆盖并报告不一致, 不静默降级. 空范围版本同样保留, 不以数据库路径相同或新进程身份为由允许旧备份回退.

流中断后 Star 保留完整 Almanac 及安装版本并标记陈旧, 由 Star 按有界退避重连, Polaris 不反向拨号补流. Pulsar 暂不可达时, 已准入 Star 可继续使用已知且未被可信替换的 Polaris 身份和端点, 不为重连重新提交账号密码. 已观察到可信实例替换后拒绝旧流的新安装, 未完成页不得接入新流; 旧回调通过本地生命周期标识隔离, 不再增加分布式业务代次.

每方向只允许一次写操作在途, 发送、解析、快照引用、待安装状态及退出中的流共同计费. 确认/核对控制消息有保留额度, 不能抢占已在途大页; 单个慢 Star 不阻塞其他流或 Polaris 持久提交. 流身份、连接等待和网络背压不持有 SQLite 写事务或 Star 业务锁. 当前服务与 Star 接收端已接线, 实施及验证边界见 [进度](../docs/progress.md).

## 内部会话与来源同步

/proto.astra.v1.StarTransport/OpenSession 首条为 Hello, 后续包含 Ping/Pong、有限拒绝码和分域的来源数据消息. 每方向最多一个未完成保活, 入队不代表送达, 旧会话不能更新新会话. SessionPacket 是类型化 gRPC 流的消息选择, 不是自定义 TCP 分帧.

旧 SyncTransport 的 OpenDeltaStream/OpenSnapshotStream 已移除. 当前使用同一 SessionPacket 内的两域快照、连续增量、累计确认和精确回补, 绝对期限保留原生含义. 多 Star 恢复的实施边界见 [架构](../docs/architecture.md), 不把现有 Schema 或旧按 Key/Version 清单对账当作业务已实现能力.

<a id="star-stream"></a>

### Star 双向流

每对已鉴权的 Star 最终保留一条双向 gRPC 逻辑流, 同时承载双方的保活、来源恢复与动态数据更新. 每个端点只发送自身负责的 Catalog/Ephemeris 来源数据, 不把收到的副本再作为本地写入广播. Almanac 继续由 Polaris 直达各 Star, 不进入 Star 对等复制. 拓扑、类型化消息、两域来源恢复和 Runtime 已接线, 组件与两 Star 进程故障用例的实际结果见 [验证记录](../testkit/validation.md); 不据此推定大规模分区恢复已验证.

新 Star 主动拨号已获知的其他 Star, 已有有效流时不为补齐另一个方向再建流. 断线后两端都可按有界退避与抖动重拨, 每个端点对同一对端至多保留一次主动连接尝试. 不将逻辑流数量解释为固定数量的 TCP 连接, 连接复用和重建仍由 gRPC 管理.

重复流只在双方已验证对端身份后裁决. 单条健康流无须因发起方不同而主动翻转; 两条相反方向的候选同时竞争时, 统一保留发起方 Member.id 按无符号 UTF-8 字节字典序较小的一条. 两端看到的发起方相同, 不按各自接收先后分别保留不同方向. id 仍是不透明身份, 这里只排序字节, 不解析 UUID 或由它判断启动先后, 不比较不同节点的 epoch 来选胜者.

同一发起方的重复尝试不抢占已经接纳的流, 待原流明确结束后再按退避重试. 握手期间候选可短暂并存, 不承诺全网在同一瞬间完成切换. 未通过鉴权的候选不能取消当前流; 失败候选不能把仍健康的对端标为断线. 败选流停止新的业务处理并取消, 已实际受理的数据不回滚, 接收重放仍走相同版本与期限规则.

当前槽位替换、退避状态和旧流完成回调通过已有的本地生命周期标识隔离. 旧 OnDone 只能回收它自身的在途引用和资源, 不能清除较新的当前槽位或推进其同步位置. 发出取消不等于已完成回收, 候选与退出中的流也计入连接和内存额度; 不为解决重复建连新增一套分布式业务代次.

每个发送端按 gRPC 约束串行提交写操作, 双向接收和业务处理不持有连接状态锁等待网络. 发送缓存、在途消息和恢复工作共享有界预算; 保活与恢复控制消息有保留额度, 可在下一次写入时优先调度, 不能抢占已经在途的大消息. 快照分片大小是部署资源参数, 不将 64 KiB 固定为协议要求, 也不承诺单流能消除所有排队延迟. 具体业务消息、来源确认位置与分片边界随三域复制协议定型.

<a id="stream-batching"></a>

### 批量发送与累计确认

Star 对等流与 Polaris 同步流优先复用已有的一写在途调度. 空闲且已有可发送数据时立即发送; 上一次 Write 在途期间累积的已提交记录, 在下一次发送机会内按条数和编码字节上限装包. 首版不增加凑批定时器, 不固定等待 500 ms, 不扫描全部活跃租约来重新生成续租包. 满页立即结束本次装包, 少量尾部也必须发送, 装包预算不等于业务 TTL 或新的持久事务.

传输装包和业务提交是不同边界. 一个数据包可以携带同一来源/域的多个连续完整提交, 或同一 Almanac Scope 的多个连续 +1 补丁; 保留每次提交的编号、操作和原绝对截止, 不按 Key 丢弃中间续租/删除或重新编号. 接收端先验证整包结构与预算, 再按原提交逐个受理; 后一提交失败不回滚已完整提交的前缀, ACK 也只能覆盖该前缀. 下文“一次源端批次”指一个来源版本内的原子事实, 不是整包多个版本的事务. 一个原子提交必须能落入配置的单消息上限; 大量到期可以形成多个有界提交, 仍完整推进本轮时间和必要投影, 不恢复 max_ticks 或将未完成过期伪装成追平. Watch 继续使用自己的 [最终状态合并](#订阅流与安装边界), 不把两种合并语义互换.

发送历史由来源共享, 每个目标只持有恢复位置、有限在途包/快照引用及必要控制状态, 不复制一整条业务事件 FIFO. 发送准备在同一边界检查历史连续性并取得稳定引用; 对端过慢落出窗口后转入既定恢复, 不能为了装包无限固定日志. 本流已交给 gRPC 的发送位置与对端已实际安装的 ACK 分开, Write 完成不证明对端提交.

ACK 采用已完整安装的累计位置, 尚未发送的同组确认只保留最新值, 不为每个版本分配一个确认任务. 同一流内确认位置不能回退, 新 ACK 不得超出已确认恢复起点或本流已交给 gRPC 的完整提交边界; 精确回补 R 仍不能冒充连续 ACK. 有待确认尾部且发送端空闲时必须发出, 不等待下一次业务变更; ACK 本身不推进版本, 不再触发 ACK 的 ACK. 发送器不在每个业务提交后停等一次往返, 未确认和实际在途仍受预算约束.

控制/回补具有保留额度, 数据轮次在两个动态域或多个 Almanac Scope 间有限调度, 不让一个大快照无限独占发送机会. 网络写入、编码和用户代码不在业务锁内执行; gRPC 一次在途写入和 reaction 应快速返回的限制见 [官方 Callback 规则](https://grpc.io/docs/languages/cpp/best_practices/). 首版仍使用生成的类型化消息, 不为共享载荷直接改成 GenericStub、手写字节协议或另一套网络框架; 运输批量收益和尾延迟须实测.

<a id="replication"></a>

### 来源恢复

Catalog/Ephemeris 各按 (来源 Member.id, 业务域) 保留一条 uint64 组版本, 初始为 0. Scope 在消息内定位记录, 不构成独立复制序列. 只有该来源在本域的成功提交产生下一位置, 不用提前分配后异步提交留下编号空洞. 来源身份仍使用既有准入及可信部署替换关系, 重连不新增业务纪元; 新 Member.id 才使用新组空间.

普通数据帧的来源由已验证的双向流确定, 不允许自报第三方来源. 源端版本、原生记录和必要有界历史共同提交, 一次源端批次可以包含同域不同 Scope 的到期结束, 仍只有一个组版本并完整处理/确认. 不跨 Catalog/Ephemeris 合并编号, 不把数据版本、order 或下游视图游标当作该版本. 同域提交须定序, 但完整准备/编码及网络等待不持有整个 Star 的写锁.

来源组当前为空、组内 Scope 被清空或历史被裁剪均不归零. 组/Scope 元数据计费, 不能回收组编号去接纳新写入; 耗尽时拒绝需要新位置的提交. 来源组的增量覆盖全部 Scope, 接收端只确认连续已完整处理的前缀, 不为不同 Scope 另报进度或用最大已见位置跳过缺号. 合法忽略的旧内容版本也需完成相应水位和位置处理, 收到/排队/部分准备不算安装.

同组位置 <= 已连续确认值的普通重放不重新修改数据或延长租约. 下一位置必须完整承接前缀; 缺号先恢复连续历史, 对端报告超过来源头部的位置属于异常, 不静默归零. 同一批次解析、预算或准备失败不确认其中一部分. 已安装的精确回补按下文局部覆盖规则跳过被覆盖目标, 不能跳过同组其他 Scope.

只记录本节点直接受理的 Catalog/Ephemeris 新事实和本节点 Ephemeris 的权威结束. Catalog 本地到期、其他来源接收及副本 TTL 清理不生成新的自有来源事件. 远端组不保留用于再次向对等广播的日志; SDK 写入成功仍仅确认本 Star 原生状态和必要恢复依据已提交, 不等待对等 ACK.

发送器以已提交组头部和本流待发送位置为依据, 提交通知只是唤醒提示. 合并通知或发送器转入空闲时须在同一调度边界重查两者, 防止最后一次提交没有后续写入便永远停在本地. 提交后无法安排发送时保留可恢复状态并使受影响流明确失败/重连, 不丢弃尾部事实后仍报告已追平. 不为此增加全 mesh 定时版本扫描、每 Key 任务或每次写入克隆全量发送队列.

同来源同域重连先报告该组连续位置, 历史足够则重放其后完整批次. 来源历史按条数/字节有界, 发送引用计费, 慢对端不能无限固定历史或自己的发送队列. 裁剪后保留当前来源组, 历史不足则导出整个来源组的完整基线, 不能仅补一个落后 Scope 后声称组已追平.

完整来源组快照在同一捕获边界固定来源、域、基线 B、全部 Scope/记录与必要水位. 可分多页按 Scope 传输, 但无独立 Scope 基线或提前确认. 未创建/当前为空的组也明确完成; 完整组清单的缺项只限定本来源状态, 不是其他来源删除证据. 不导出合并客户端视图或借来的第三方记录.

B 之后新增 Scope、写入、续租和权威结束全部进入同一组连续后续历史/有界暂存. 首次恢复只需分别完成 Catalog/Ephemeris 的组基线, 不等业务发布者停止写入, 不再维护不断增长的每 Scope 初始恢复清单. 不能只冻结各 Scope 不同时间的内容却共享一个 B, 也不把后来的当前记录冒充 B 时刻的快照.

接收端先完整接收组快照, 再按 Scope 逐步原子安装原生记录、必要合并投影/水位和期限; 只有全部 Scope 成功才确认组位置. 接收中断保留原状态, 安装中断则保留此前已经完成的 Scope. 已安装范围的内部覆盖证据阻止基线 B 以内旧事件覆盖该范围, 不增加线上的 Scope 版本或 ACK; 具体锁与回收边界见 [存储恢复](../astra/common/README.md#快照与历史). Ephemeris 替换本来源组, 其他来源不受影响; Catalog 仍按每 Key 内容版本及原绝对截止合并, 更高水位不因缺项丢失. 最终安装须重查其间的身份、期限及其他来源新状态, 不用旧快照覆盖新投影.

组恢复比单 Scope 恢复的准备范围更大. 活动记录、无载荷水位、分片、投影准备和旧/新根共同计费, 合法组容量须与 [恢复预算](../astra/common/README.md#capacity) 相容. 暂时预算不足或无法接续 B 后的变化时终止本次恢复并退避; 已知完整状态超过配置硬上限时明确报告容量不相容, 停止重复下载同一不可接纳基线, 等待配置或状态条件改变. 两者都不截断部分 Scope 宣称成功. Scope 级 SDK 视图仍分别交付, 组安装不承诺多个独立订阅回调同时观察.

同一来源实例/域的完整快照 B 不得低于接收端已连续确认的位置, 也须覆盖仍在生效的精确回补位置. 接收开始及最终安装均检查, 不能只比较请求发起时保存的值. B 相等可以修复缺失载荷, 仍保留原绝对截止并执行当前期限校验, 不恢复已经结束的来源身份或用旧快照撤销较新的结束事件. 新实例使用独立来源组, 不把两个实例的数字直接比较.

同一组同一时刻只有一条恢复路径和一个在建批次, 不在组快照页中插入该组增量或第二张快照. Catalog 与 Ephemeris 可共享双向流并按各自预算调度, 控制消息/回补不因长期等待数据形成互相等待; 一写在途及背压规则保持不变. 历史不足恢复整组, 缺少单条载荷则使用精确回补, 不再请求整 Scope 快照代替一条回补.

<a id="catalog-watermark"></a>

#### Catalog 来源版本水位

完整 Catalog 来源组快照包含本来源在全部 Scope 直接受理过的每个 Key 的最高内容版本, 即使对应内容已经过期或释放. 每个 Scope/Key 只出现一次: 仍有有效本来源内容时发送版本、完整载荷和原绝对截止; 否则发送明确的 Key/正版本水位. 水位没有载荷或租约, 不使用空 bytes、版本 0 或 Delete 冒充它; 合法零字节值仍必须是完整记录分支.

本来源水位来自本节点实际接受的 SDK 发布/保活, 不把只从其他来源学到的更高版本再次声明成自己的来源事实. 合并视图的防回退水位和自有来源记录据此区分, 内容仍共享不可变引用. 全部 Scope 的元数据与组基线在一致边界捕获; 本地内容到期转为仅水位不制造来源删除或新的来源序号, 不重置原版本. 后续更高版本的直接受理仍通过正常来源增量衔接.

接收端按以下规则合并, 必要本地删除与水位更新在同一安装边界提交:

- 收到水位 10, 本地最高版本或可见内容为 8: 保留最高版本 10, 移除版本 8 的可见内容并通知下游, 不展示一个空值或转播为本节点权威删除.
- 本地已有仍有效的同版本 10 内容: 保留该内容及已接纳截止, 包括此前由这一次水位的相同来源取得的内容. 来源端已过期不等于接收端也已过期; 仅水位或完整快照缺项不能变相广播 Catalog 到期删除, 也不刷新 TTL. 若本地已经到期, 仅水位不能恢复内容.
- 本地最高版本为 12: 忽略较低水位 10, 不恢复或降低版本. 同版本内容冲突仍按完整记录规则拒绝, 水位不能证明已经释放的字节一致.

这些水位只在 Star 内部恢复使用, 不加入 Comet Watch 的全量视图或要求 SDK 保存版本墓碑. 副本接纳的最高版本保持到本 Star 进程结束; 不因来源断线、来源替换、快照缺项或历史裁剪而回收. 首版不提供跨全群重启的永久防回退证明.

水位的 Key、来源索引、快照引用与准备峰值全部计入资源预算, 分页发送不等于常量总存储. 超额不能截断清单却宣称快照完整, 也不能驱逐旧水位来接纳更低版本; 保留原完整状态及确认位置后报告容量不足. 来源活跃值、仅水位、空 Scope 和后续新增之间都须保持前述完整边界.

<a id="repair"></a>

#### 单条回补

回补复用现有 Star 双向流, 不新增每 Key RPC/连接, 也不让 Comet 上传清单. 请求关联来源实例、域/Scope、Key 或 UUID、触发缺失的来源位置; Catalog 同时关联触发事件的内容版本. 同一来源组/Scope/目标至多一个当前回补, 后续缺失提示合并, 请求中的触发位置用于匹配回复, 不另造全局请求 ID 或分布式代次.

来源在一个受控读边界取得该 Scope/目标的当前原生记录和所在来源组位置 R, 回显请求关联信息, 返回完整记录或明确的当前缺失. R 不得早于触发位置. Catalog 完整记录包含业务版本、内容和原绝对截止; Ephemeris 同时包含固定 Attr、当前 Data、原绝对截止及必要操作顺序, 不只补 Attr 后混用另一时刻的 Data. 返回的是本来源状态, 不是从合并视图借来别人的记录; 回补本身不生成新来源序号或延长 TTL.

回补可以返回已经变化的当前记录, 不要求来源无限保留触发时的旧载荷. 安装仍检查来源、内容版本、Attr 不可变性与本地当前期限; 高版本已在本地生效时忽略过时修复, 到期或缺失回复不能创建空记录. 仅缺少字段/消息、超时、容量拒绝和来源失联不是明确缺失.

Ephemeris 的明确缺失表示该来源 UUID 在 R 已不活动, 只结束其本地来源投影并通知下游, 不广播成接收端的权威删除. Catalog 回补也返回本来源仍保留的最高版本: 无活动载荷但有水位时使用 [仅水位规则](#catalog-watermark), 仅本来源从未受理该 Key 时才能明确返回未知. 不把未知/无载荷解释为删除其他来源有效的同版本内容, 不隐藏已知更高版本或将水位归零.

回补位置 R 只覆盖这个 Scope/目标, 不能直接将整个来源组确认到 R. 例如正处理位置 10 的缺失 Key, 回补来自位置 15, 仍必须处理 11..15 中其他 Key 和其他 Scope 的事件. 已安装回补需保留这个目标在 R 的局部覆盖标记, 将该来源/目标在 R 以内的旧事件视为已覆盖, 防止旧 Data、旧期限或旧删除反向改写; 对其他来源、目标或 R 以后的事件没有跳过权. 连续确认只能按实际处理的完整前缀推进, 这不是新的业务版本.

完整记录/明确缺失与所需覆盖标记共同准备并生效, 准备失败不能只装入较新记录后丢掉防旧事件依据. 标记只针对在追赶中的回补目标, 持续计入有界状态, 不为所有 Key 常驻建立第二套版本表. 本地 TTL 删除或同实例断线不能提前清除仍有迟到事件的标记; 该组连续位置达到 R、安装覆盖 R 的完整来源快照, 或该来源已被可信身份替换后才可释放. 旧恢复任务与新流仍通过既有本地生命周期隔离.

同来源组回补的在途数、记录字节、覆盖标记和排队增量共享恢复预算. 慢来源或高频缺失不得无限新建请求或积压; 超限停止推进受影响组并有界退避, 已有完整业务值按原 TTL 处理, 不伪造追平. 需要完整组快照接管时, 其基线不能低于已安装回补的覆盖位置, 不能丢掉未受其覆盖的标记. 回补发送/编码/大载荷准备在业务锁外进行.

等待回补只阻止受影响组的连续提交/ACK, 不在 gRPC 读取回调内等待回复, 也不能停止读取承载该回复的同一双向流. 后续数据在总预算内暂存, 控制/回补消息继续分派. 若待处理数据已满而所需回复尚未读到, 显式终止本次流并从最后完整位置恢复, 不以永久停读等待回补造成互相等待. 取消中引用继续计费, 重新建流不提前清掉已经安装的覆盖标记.

可信较新 epoch 只证明同一 principal 的旧进程已被替换, 不比较不同部署的 epoch. 观察到替换后, 旧 Ephemeris 副本保留原截止并继续本地倒计时, 不立即全删、不补一个完整 TTL、不迁移到新进程 UUID. 拒绝旧身份后续事件与尚未安装的旧恢复结果, 旧流回调仅清理自身资源. 新进程从新来源位置恢复, Beacon 重新创建 UUID; 普通断链、探测超时和未验证的名单不构成替换证明.

Catalog 的旧来源活动内容同样按原截止本地清理, 不因可信替换突然删除有效内容或取消合并水位. 已替换来源不再接受新事实; 待其活动记录结束、流和恢复任务实际退出、快照引用释放后, 可回收旧来源组和调度器. Catalog 合并水位及同 principal 的较新身份依据继续保留, 不为每个旧 Member.id 永久保留空组/时间轮, 也不把旧身份晚到当成新空组重新接纳. 普通失联来源不满足这一回收条件.

本规则只约束已观察到可信替换的节点, 不承诺隔离节点即时获知. 既有已安装数据的本地过期只向下游传播; Catalog 的每 Key 版本记录也不会因来源连接退出而删除. 此处不新增全对等周期扫描、SDK 对账或跨 Star 同步确认后才响应业务的要求.

<a id="comet"></a>

## Comet v1 业务协议

本文定义 Almanac、Catalog 和 Ephemeris 的服务端受理、消息和同步边界. Comet Schema 已生成, 公共服务和 SDK 实施中, Planet 暂缓; 最新实际验证见 [记录](../testkit/validation.md).

### 服务与实例

外部业务服务归属 proto.comet.v1, 尚未发布兼容承诺. 方法名称和字段号以 [comet.proto](comet.proto) 为生成来源, 下表说明其业务含义, 不维护另一份替代 Schema.

| 服务 / 方法 | 方向 | 请求与结果 |
| --- | --- | --- |
| Gateway.Session | server streaming | APIKEY/APISECRET 登录, 首条确认实际 Star 实例及会话凭据; 流结束则会话失效 |
| Almanac.Watch | server streaming | 分组或精确 Key 的权威数据视图; Comet 无 Almanac 写接口 |
| Catalog.Publish | unary | 单 Key 完整内容、业务版本和 TTL; 当前 Star 内存提交成功后确认 |
| Catalog.Renew | unary | 当前 Key/内容版本及请求 TTL; 验证后保活, 不重发内容或生成新内容版本 |
| Catalog.Watch | server streaming | 分组或精确 Key 的动态视图, 每条内容携带该 Key 的业务版本 |
| Ephemeris.Create | unary | 固定 Attr、初始 Data 和 TTL; 每次受理分配新 UUID |
| Ephemeris.Update | unary | UUID、Data order 和完整 Data; 不修改 Attr 或 TTL |
| Ephemeris.Renew | unary | UUID 与独立 Renew order; 使用注册时保存的 TTL |
| Ephemeris.Remove | unary | 结束指定来源实例上的 UUID, 返回 Empty; 与该 UUID 写入定序 |
| Ephemeris.Watch | server streaming | 分组或精确 UUID 的完整注册视图, 已有基线可只推 Data |

Almanac 的管理写入在内部控制面由 Astrolabe 调用 Polaris, 不暴露给 Comet 或绕过持久确认. 权威分组版本、单 Key Set/Delete、幂等证据和恢复规则只在 [Polaris](../astra/polaris/README.md) 定义; 不能将原 Catalog 的整分组 CAS 套用于新的动态 Catalog.

不提供 Inspect 或动态 Limits 握手. Client 的 TLS、认证及本地预算静态配置, 显式匿名必须与服务端一致, 失败不降级. 需要认证时先建立 Session; 成功后可访问全部普通范围, 没有逐域读写权限或 APIKEY 所有者. 外部入口始终拒绝 __ Sector. 非法 TTL 返回允许范围, SDK 不缓存动态 Limits 或自动钳制请求.

就绪监测采用标准 grpc.health.v1.Health, 不作为每个 Comet 的额外必经握手, 不返回身份/配额或代替具体业务检查. 首次业务开放按 [启动顺序](../docs/architecture.md#启动与运行), SERVING 不证明已取得全网最新数据. 参见 [gRPC 健康检查](https://grpc.io/docs/guides/health-checking/).

实例身份从 Session 确认或实际业务响应取得. Session、无恢复位置的 Watch 以及尚未知实例的首次 Publish/Create 可用空 instance; 这仅定位该次实际到达的 Star. 已发请求的目标、内容和版本固定, 迟到响应不能改写当前对象. Catalog 后台恢复是携带相同 Key/最新业务期望的新尝试, 不能将旧 future 改成成功; Ephemeris 未确认创建按新 Create/TTL 规则恢复, 不原地接管未知 UUID.

Catalog.Renew 必须带当前绑定 Star 的非空 instance, Ephemeris Update/Renew/Remove 必须带注册来源实例. 不匹配时拒绝, 不把请求实例改为空绕过检查. Watch 恢复绑定原实例、域、范围及完整安装位置, 实例变化返回 reset 而非借用旧视图游标. Almanac Reader 跨 Star 的权威版本下限另按 SDK 策略保留.

认证凭据仅在初始 metadata 携带, 不在业务消息重复 APISECRET. 单次 RPC 有有限 deadline, 长期 Session/Watch 使用取消与连接检测; 不将长期流套用短写入 deadline. 没有 Catalog.Version 查询或发布结果轮询接口, 当前内容版本可随 Watch 读取, 不能由某个版本数字倒推原未知请求是否提交.

<a id="comet-batching"></a>

### 业务 RPC 批量化边界

当前 Publish/Renew 保持单目标, Watch 保持全 Scope 或精确单目标; 不把运输装包等同于多 Key 事务. Star/Polaris 的既定装包见 [批量发送](#stream-batching), WatchReply 已可携带多条 changes. 下述是评估扩展的边界, 不是已实现的新协议或新增首版要求; 测量入口见 [性能矩阵 B10/B11](../testkit/comet.md#rpc-batching).

批量续租可以摊薄 RPC 创建、请求头和派发成本, 但每条记录仍需检查身份、顺序、期限并提交租约及来源事实. 50 项合为一次调用不表示 CPU、字节数或网络中断减少 50 倍. 原生 SDK 只能合并同一共享 Client、会话和目标 Star 下的请求, 同一物理机上的独立进程不会自动形成一批; 不为凑批恢复 Moon/代理功能.

若测量支持加入批量续租, 使用包含 Scope/UUID/order 的逐项记录, 或明确限定同 Scope 后共享 Scope, 不采用依赖下标配对的两个 repeated 数组. 每项独立确认或失败, 不建立全批原子事务; 整包格式/预算/认证失败和提交后丢回执须分别定义. 限制条数及编码字节, 重复目标须有明确拒绝规则, 重试保留每项原 order 和首次发送起点. 调度只合并本次已到期可发送的任务, 不等待其他租约凑齐、不扫描全体活跃记录; 最短 TTL、不同期限、部分失败及取消后的未知结果都须可独立恢复. Catalog.Renew 使用 Key/内容版本, 不能套用 Ephemeris 的 UUID/order.

多目标 Watch 的潜在收益是减少逻辑流、服务端订阅状态和重复恢复开销. 多条 gRPC 流可复用 HTTP/2 连接, 不按每条 Watch 计算一个 socket/文件描述符; Channel 也不保证始终只有一条物理连接, 参见 [gRPC 性能指南](https://grpc.io/docs/guides/performance/). 若扩展, 首先评估同域、同 Scope、建流时固定的有界目标集合, 不引入动态增删控制协议. 空集合、空目标、重复项和集合预算必须明确; 目标集合是恢复范围的一部分, 变更集合必须重建完整基线, 不能沿用旧游标遗漏新增目标. 过滤成本、无关写入唤醒、快照准备和慢消费者占用同样计入收益评估.

动态 Catalog 的内容版本属于每 Key, 不存在需要通过批量 RPC 保持的全 Scope 业务版本. repeated changes 本身不提供原子性; 真正跨 Key 原子提交还涉及准备/回滚、来源复制和下游完整安装, 不能仅修改消息数组. 当前需要一起替换的有限大小内容可以编码为一个 Key 的完整 Buffer, 使用已有单记录提交; 持久配置仍由 Almanac/Polaris 承担, 也不因此新增多 Key 管理事务.

### 消息数据与标识

Scope 表达 Sector/Spectrum, 域由服务确定. Watch 的 target 为空表示全 Scope, 非空精确匹配 Almanac/Catalog Key 或 Ephemeris UUID, 不提供跨 Scope 通配. Scope 内容仍为 UTF-8 文本 (1..128 字节、不含 NUL), 类型为 bytes 以跳过 Protobuf 层的重复校验; 按原始字节精确比较, 不自动修剪、大小写转换或 Unicode 规范化. * 是字面字符, 非法 UUID 不降级成全量目标. __ 限制检查原始 Sector, 不以路径拼接或客户端声明权限代替.

实例 ID 复用现有 Star 进程身份, 不新增业务纪元, 不把不透明节点 ID 当作业务 UUID. Ephemeris UUID 由 Star 使用可靠随机源生成 UUIDv4, 线上字段统一使用原始 16 字节二进制 (版本/变体位固定), 不再传输 36 字符连字符文本; 各域入口校验 16 字节长度与版本/变体位, 拒绝文本别名而非静默规范化. 内存 Key 与 SDK 身份同样按字节比较, 不在每次 RPC 中往返格式化; 随机源失败不降级为时间戳或普通伪随机数. 参见 [RFC 9562](https://www.rfc-editor.org/rfc/rfc9562.html#section-4).

UUID 二进制每条比连字符文本少 20 字节, 覆盖 Create/Update/Renew/Remove/Watch/Snapshot/Delta/Repair 全链路. 版本和顺序仍为 uint64. 内容版本必须大于 0; 空视图游标及新注册的操作顺序可以为 0, 新操作不能以 0 绕过校验, 溢出拒绝. Almanac Watch 使用权威分组版本, Catalog/Ephemeris Watch 使用接入 Star 的本地视图游标; Catalog 每 Key 内容版本与 Ephemeris 每 UUID 的 order 均不是该游标.

普通 Catalog Publish 只提交一个 Key 的完整 Buffer, 允许零字节, 没有 Delete、操作列表、字段 Patch 或分块上传. Almanac Set/Delete 属于独立管理契约. Ephemeris 为 UUID/Attr/Data, 续租与顺序元数据不塞进用户 Buffer; Observer 不按客户端墙钟独立删除, 由接入 Star 推送并明确陈旧状态.

<a id="almanac"></a>

### Almanac 读取

Star 仅安装 Polaris 提供的完整范围及权威版本, 通过 Almanac.Watch 提供只读快照/增量, 包含合法空分组. 空间和恢复预算与其他 Watch 共用实现, 但权威版本不能重新按接收条数编号或受本地 TTL 推进. 内部 __auth 不经外部 Watch 暴露. 写入、历史和安装确认见 [Polaris](../astra/polaris/README.md), Reader 的跨入口不回退策略见 [Comet](../astra/comet/cpp/README.md#subscription).

<a id="catalog"></a>

### Catalog 发布与期限

同一个 Scope 中的不同 Key 有独立业务版本和发布者. 唯一发布者由业务保证, 不增设 APIKEY 归属、发布者租约或 Star 选主. 业务自行持久化完整内容与版本; 本协议仅确认接入 Star 内存受理.

- Publish 提交完整 Key/value/version/TTL. 高于已知该 Key 的版本即可接受, 可从 7 跳到 10; 低于最高版本拒绝. 同版本且仍持有内容时逐字节比较, 不同内容拒绝, 不以哈希或消息编码顺序替代内容判定.
- 合法完整发布建立/刷新有限期限, 包括同版本同内容重试. 没有 Catalog order 或请求去重缓存; 响应丢失仍可能已生效, 后续重试是新的保活, 不是对原截止的幂等确认.
- 到期后释放内容但保留该地址/Key 的最高版本到当前 Star 进程结束. 此元数据计入容量, 不随历史裁剪或空分组回收丢弃, 不驱逐它来接受回退. 内存重启无跨进程防回退证据.
- 内容已释放时, 同版本恢复须提供完整内容, 不能伪称已比较旧字节. 业务必须保证同 Key/同版本始终对应唯一内容; 没有载荷时收到纯期限不得创建空记录.
- 纯 Renew 携带 Key、当前内容版本和 ttl_ms, 仅作用于本机来源组仍有效的完整记录, 并检查本机合并索引未掌握更高内容版本. 只有远端副本、仅水位、已过期或本机来源版本过时均不能直接续租, 须通过完整 Publish 恢复或由业务处理版本冲突. TTL 由请求给出, Star 每次按 [Ephemeris TTL](#registry) 的单位/范围校验, 不额外保存 Catalog 的固定 TTL 配置; 0 不表示沿用旧值. 到期/缺失须完整 Publish 恢复, 版本不符拒绝, 不仅凭 TTL 新建记录.
- 受理时用本地业务时间加本次 TTL 得到候选截止, 加法/换算失败不提交. 本机来源组同内容版本的截止取该组原截止与候选值之大, 较高内容版本使用新版本自己的截止; 不借用远端组较晚期限来延长本机来源记录. 下游可见期限另按同版本的各来源有效记录取大, 因而本机组过期和合并视图过期不必同时发生. Publisher 的 TTL 在对象内固定, Star 不借此增加 Publisher 身份或逐 Key 所有者. 复制/回补只传绝对截止, 不把接收端变成重新计算 TTL 的保活来源.
- 内容、最高版本、期限及必要来源恢复依据共同准备并提交. 校验/时钟/容量失败不先写内容或延长截止, 没有原子准备时不能先回复成功再补复制记录.
- 首版无显式 Delete. Publisher 停止后由各 Star 本地 TTL 删除; 本地删除仅影响下游, 不是对等权威删除, close 也不是撤回已在途请求.
- 对等重放携带原绝对截止, 不形成新保活. 同版本一致内容取较大截止, 较新版本使用自身截止; 已收到的更高版本即使期限已过, 仍须保留其版本依据并排除旧值, 不能因不安装过期载荷就继续展示旧版本. 缺失或相冲突的内容不能确认成完整可见状态.
- 来源快照不具有清空整个合并视图的权利, 缺项不能删除其他来源恢复的数据. 具体回补报文及来源进度见 [来源恢复](#replication), 不要求 SDK 上报逐 Key 版本清单.

Catalog Watch 同时区分每 Key 内容版本和 Scope 视图游标. 内容版本/可见性变化才推进该视图, 纯期限刷新不推进; 多 Key 和本地到期仍使二者无法 1:1, 不维护映射字典. Subscriber 切换 Star 后按新完整视图安装, 可暂时回退个别内容版本, 不抹去各 Star 自身已保留的版本下限.

### 提交前后与耗尽边界

业务数据、版本及必要元数据的原子提交是写入结果的分界. 提交前已观察到取消/deadline 失效时停止该次受理; 取消仍可能与最终提交竞争, 不能保证客户端取消必然撤回操作. 提交后即使 Watch 合并/编码/排队失败、响应丢失或连接断开, 也不能回滚写入或返回 Effect::unapplied. 可发送时返回已提交的成功, 否则客户端按不确定处理; 推送失败只结束受影响订阅, 不使写入线程等待它.

unapplied 只表示本次业务操作未提交, 不表示整个 Scope 从接收至返回完全未变化: 其他写入或先行到期维护仍可合法推进版本. 所有可能失败的提交准备须在分界之前, 提交之后不再追加会影响原子性的业务元数据; 异常映射按失败阶段处理, 不以统一 catch 将已提交请求改报未提交.

uint64 最大值不得回绕. Catalog 内容版本达到最大值后不能发布更高内容, 同版本合法保活仍按本地视图/来源进度是否可提交判断. Almanac 的旧提交确认与新提交耗尽分开, Ephemeris 两个 order 也分别检查. 本地视图或来源位置耗尽不能静默清零; 无法提交到期时受影响范围停止追平承诺并结束 Watch, 不在同游标下偷偷过滤记录. 此类终止性限制不以改 UUID、清库或自动重试掩盖.


<a id="registry"></a>
<a id="ephemeris"></a>

### Ephemeris 注册与期限

Star 在 Create 提交时分配新 UUIDv4, 在本机来源组内建立一条完整原生记录, 包含固定 Attr、可更新 Data、固定 TTL 及一个截止. 不拆双物理 Key, 结构及原子提交由 [原生存储](../astra/common/README.md#data) 定义. SDK 自动恢复和本地时间估计见 [Beacon](../astra/comet/cpp/README.md#registry).

- 一条注册对外只有一个 UUID. Attr 在该生命周期内固定, Data 整体替换且不延长期限. 创建、续租、注销和到期不得对读取者暴露半条注册.
- 更新/续租固定在所属 Star 实例, 副本不自动获得写入或续租权. 来源 Star 已到期或结束的 UUID 不可复活, 切换 Star 须新建 UUID. 副本因本地 TTL 删除后仍可由有效来源补齐并恢复, 不把副本删除当成来源结束; 已确认旧进程替换的处理见 [来源恢复](#replication).
- APIKEY 只负责准入, 不记录注册所有者或按范围赋权. 后续 Ephemeris 接入保留来源实例、UUID、操作顺序与期限检查, 不因重新登录或同名凭据重建而复活旧会话、清零顺序或延长 TTL; 凭据边界见 [生命周期](../astra/astrolabe/README.md#凭据生命周期).
- Create 不接受指定 UUID、request_id 或幂等查询, 每次受理都是新注册. 内容相同也不合并; 响应丢失可能留下孤立记录, 从服务端实际提交起正常计时并计入容量, 不是从客户端超时起保证很快消失.
- Attr/Data 分别允许零字节, 空内容仍表示记录存在. 内部凭据的必需字段约束不因此放宽.
- TTL 使用整数毫秒, 默认 30000 ms, 首版合法范围默认 [1000, 600000] ms. 下限固定 1000 ms, 上限可配置且不低于下限, 1500 ms 合法. 零值、越界、非整毫秒及有符号转换溢出明确拒绝, 不钳制或舍入.
- TTL 在注册成功时固定并由 Star 保存; Renew 不带新 TTL, 改时长需要新注册. 服务端以已初始化且本地计时正常的业务时钟检查期限与新截止的可表示范围, 非法续租保留旧状态, 不能靠饱和得到永久注册. 该业务限制不改变 Clock/Wheel 的纳秒时间模型.
- 到期判断不等待物理清理, 过期后的 Update/Renew 拒绝. 注销、到期与并发写入在同一 UUID 下定序; 先结束则不复活, 先写入则结束操作删除完整当前记录. 实际受理的取时点及原生记录、来源组和投影的锁边界见 [原子提交](../astra/common/README.md#commit).
- 已完成首次可信校准的 Star 在参考失联、样本过时或误差预算超限时继续本地走时, 并允许新的 Create 及大于最新 order 的 Renew 生成有限截止; 同步质量降级不等于业务时钟不可用. 同 Renew order 仅确认旧结果, 不延期, 仍检查当前身份和实际期限; Data/Remove 继续检查记录是否已到期. 未初始化或本地读时本身失败、计数反序/耗尽时不伪造当前时间, 也不能把时钟错误报为 ended 诱发反复新建. 具体时间保证见 [Pulsar](../astra/pulsar/README.md#clock).


<a id="registry-请求顺序"></a>

### Ephemeris 请求顺序

保留 Data order, 它保护的是独立 RPC 的最终提交顺序. 共享 Client/Channel/Session 不承诺唯一物理 TCP; 即使只有一条 TCP, 字节保序也不保证不同 HTTP/2 流的业务 handler 按同一顺序提交. gRPC 的消息保序保证作用于单个 RPC 内的流, 不是跨 unary 调用的业务写入, 见 [gRPC 调用顺序](https://grpc.io/docs/what-is-grpc/core-concepts/) 和 [Channel 连接说明](https://grpc.io/docs/guides/performance/).

例如 Update(A, 1) 的处理停顿, 客户端超时后用 Update(B, 2) 恢复最新值; B 先提交后, A 仍可能进入提交路径. 客户端取消不保证远端 handler 已停止, 见 [gRPC 取消规则](https://grpc.io/docs/guides/cancellation/). 在同一提交保护下比较 order 可拒绝 A, 单纯加锁或“后到生效”则仍可能把 B 覆盖回 A. 每个 UUID 只保留一个最新 Data 顺序, 不保存完整操作日志; 已有有界恢复逻辑统一处理 obsolete, 不新增一套独立状态机.

同一活动 UUID 保存最新已提交 Data 顺序号和最新续租顺序号, 两者独立推进. 它们是有限的注册操作状态, 不形成逐字段版本或跨 Scope 的全局 revision. SDK 为实际发送的新操作分配顺序, 重试保持原序号和内容; 后续 Data 可以跳过已合并的中间值, Star 不要求 Data 序号严格 +1.

| 条件 | Data 更新 | 续租 |
| --- | --- | --- |
| 注册不存在、已结束或到期 | 拒绝, 不执行 upsert | 拒绝, 不从旧成功回执推断租约仍有效 |
| 序号大于该操作最新值 | 校验后原子替换 Data 并保存新序号 | 校验后仅延长一次截止, 保存序号及该次受理结果 |
| 序号等于最新值 | 内容与该次提交一致时确认同一结果; 不同内容拒绝 | 返回该次已受理结果, 不按重试时刻重新延长 |
| 序号小于最新值 | 返回已过时及必要的最新位置, 不声称该旧请求一定曾提交 | 不重复延长, 不退回旧截止 |

顺序检查、当前 Session、来源实例与期限检查、数据或租约修改形成同一提交边界. 元数据准备失败不先推进序号. 同内容的新 Data 请求可以只更新确认位置, 不必产生重复载荷通知; 纯续租不向 Observer 重发未变 Attr/Data. 后台清理、Remove 与更新/续租在同一 UUID 下确定先后: 先结束则后续修改拒绝, 先完成修改则结束操作删除其当前完整状态.

服务端每类操作只保留最新确认信息, 不为整个注册生命周期保留每次调用的完整日志. 序号小于最新值时无法证明旧调用是否执行, 仍遵守本地不确定结果不改写规则. 凭据会话轮换不清零这些序号, 新 UUID 才使用新的起点. 没有额外 Ephemeris.Read 或接管 RPC, 读取通过 Watch, 未知或暂时失败不冒充已确认到期.

Create 不使用上述操作顺序或创建幂等缓存. SDK 未确认 UUID 时按创建契约退避后请求新注册, 原成功可能留下一个等待 TTL 的记录. 已有 UUID 的更新/续租防迟到规则仍保留, 不能因创建允许重复就允许旧 Data 覆盖或旧租约复活.

<a id="session"></a>

### 业务会话与准入

Comet APIKEY/APISECRET 与 Orbit 节点准入分开, 不向 Pulsar 登录、登记或对时. 凭据来自内部 Almanac, 经 [Astrolabe](../astra/astrolabe/README.md) 与 Polaris 管理; 初始化和业务开关见 [Astra](../astra/README.md#运行模式与接线). 下列规则只定义 Star 接受业务请求的边界, SDK 建连/重认证策略见 [Client](../astra/comet/cpp/README.md#业务认证).

- 认证开启时通过 Gateway.Session 的首个请求验证 APIKEY/APISECRET, 首条成功响应返回实例及随机会话凭据, 此后保持该服务端流. 后续各独立业务 RPC 在初始 metadata 携带 comet-session-bin, 不再次提交 SECRET.
- 会话仅在当前 Star 内存有效, 不落盘、不复制、不写 Catalog, 不是节点票据或 JWT. 缺失、未知、失效或跨实例会话拒绝; 同账号配置不使不同 Star 的会话通用.
- Session 绑定逻辑流, 不绑定 TCP/socket. 不提供 Release、五分钟业务空闲扫描或应用层认证 Ping; 空闲流可存活, 异常断链依赖传输检测, 不承诺瞬时回收.
- 检测到取消/结束、撤销或 SECRET 轮换时, 先使凭据不可授权, 再结束关联流/订阅并按在途引用回收. 失效不回滚已提交写入、不注销 Ephemeris 或改变业务版本/TTL; 网络清理不构成授权宽限期.
- 首次认证到会话生效, 以及写入最终 Session 检查到提交, 都必须与相关凭据变更定序. 变更先生效则旧身份不能提交, 先提交则保留结果; 不允许轮换扫描后又装入有效旧会话, 不增加对外认证代次.
- 解析/分配可在临界区外准备, 拒绝时丢弃准备状态, 不提交本次操作的数据、版本和幂等依据; 独立维护仍可推进分组. 连续安装普通凭据变更时, 无关 APIKEY 的请求不受影响; 跳过历史的完整安装按 [凭据快照](#credential-snapshot) 统一使旧会话失效.
- 不执行逐域/Scope 读写权限或注册所有者检查. __ Sector 在外部入口隔离, 关闭业务认证也不能访问内部数据或管理 RPC; 各业务的实例、版本、操作顺序和期限条件仍检查.
- 认证关闭时不要求凭据或建立 Session, 内部隔离仍执行; 此时显式调用 Gateway.Session 返回 FAILED_PRECONDITION, reason=input, 表达两端认证配置不符, 不签发伪匿名会话或让 Client 自动降级. TLS 与认证独立, 明文传送不具备机密性, 不以失败自动降级或加一套请求签名.
- Session 含未完成认证和空闲流均计入容量, 首次确认等待与整条长期 RPC deadline 不混用. 具体本地等待、关闭顺序及重认证由 SDK 定义.

外部业务的 Scope 统一在已解码请求进入业务入口时校验, 在查表、创建分组和准备提交之前拒绝以 __ 开头的 Sector; Publish、Create、Update、Renew、Remove 和三种 Watch 均走同一检查, 认证关闭也不绕过. 不把业务载荷中的 Sector 说成能在反序列化前由普通 Interceptor 读取: gRPC C++ 的接收消息拦截点面对已接收的消息对象, 首版不新增手写 Protobuf 扫描或第二份 metadata Scope. 解码前的资源保护由消息/传输预算承担, 见 [gRPC 接收实现](https://github.com/grpc/grpc/blob/master/include/grpcpp/impl/call_op_set.h).

撤销先令会话凭据不能继续授权, 再请求取消关联流; TryCancel/关闭 Session 本身不撤回已经执行或独立在途的 unary. 每个 RPC 仍检查轻量会话有效性, 写入在最终提交边界与撤销定序, 不重复校验 SECRET. 可用 APIKEY -> Session 的有界索引定位受影响流, 无需遍历所有业务 RPC 或新增对外认证代次; 取消只是清理动作, 见 [gRPC 取消语义](https://grpc.io/docs/guides/cancellation/).

Watch 的授权检查、挂入 APIKEY 关联索引与取得业务观察边界也须遵循同一锁顺序, 防止撤销扫描完成后旧请求才加入订阅. 凭据撤销/轮换先使相关订阅不能再取得发送许可, 丢弃尚未获准发送的准备页, 再在锁外取消; 已获准并进入在途的帧或已到达应用的数据不可收回, 不能承诺撤销后网络里立即没有旧字节. 不执行范围权限检查或额外只读 Version 握手.

APIKEY/APISECRET 的长期有效、轮换和分发责任见 [凭据生命周期](../astra/astrolabe/README.md#凭据生命周期), 这与 Session 存活和 Ephemeris TTL 是三种不同对象.


<a id="credentials"></a>

### Credential

凭据仅负责 Comet 登录, 不包含 Grant、Domain、Scope、读写位或 APIKEY 所有者. 认证成功后可访问全部普通业务范围, `__` Sector 由外部入口统一拒绝; 管理写入与系统复制仍按基础设施角色区分职责.

Credential 存放于 `Almanac["__auth"]["comet"][APIKEY]`, 由 Astrolabe 提交到 Polaris 持久保存, 再随 Almanac 安装到 Star. APIKEY 取自 Key, 不在载荷重复保存. 它不进入普通 Comet Watch, 不存在 standalone 本地凭据文件作为第二份权威.

Credential 已在控制面 `proto.orbit.v1` 定义, 仅为 `1 bytes secret`, SECRET 非空. 不保留 grants 或范围权限消息, 不让公共 SDK 导入内部凭据表.

SECRET 的工程初值上限为 4096 字节, 完整编码和凭据表同时受管理消息、条目及字节预算约束, 具体初值不是永久协议上限. 记录与查找索引及准备峰值都计入资源, 不因内部连接可信就取消容量和必需字段检查.

Polaris 在提交前校验记录, Star 在安装前准备新记录和登录查找索引; Star 的已安装版本只在记录、索引和必要 Session 失效共同生效后推进. 解析或准备失败不能留下半次 SECRET 替换. 没有范围权限索引或逐请求 Grant 遍历; 登录后各 RPC 仍检查当前 Session 有效性和业务自身不变量.

连续安装的同 SECRET Set 不误关对应会话, 相同安装版本的重复确认也不重复撤销. 轮换/撤销的部署责任见 [Astrolabe](../astra/astrolabe/README.md#凭据生命周期). Polaris 持久确认不代表全部 Star 已同时撤销旧凭据; Star 根据本机已安装版本执行, 不为每个业务请求回查管理面.

<a id="credential-snapshot"></a>

### 凭据快照与会话失效

已确认选择完整快照时统一重新认证, Credential 仍仅保存 secret, 不增加逐凭据创建版本、删除墓碑或认证代次. 原因是未连续安装的历史可能包含删除后重建或轮换后改回原 SECRET, 最终字节相同不能证明旧 Session 连续有效.

| 安装路径 | 本 Star 的会话处理 |
| --- | --- |
| 连续安装权威 +1 提交 | 只使 SECRET 实际改变或被删除的 APIKEY 的旧 Session 失效, 同值 Set 与无关账号不受影响 |
| 用较新完整凭据快照替代尚未连续安装的提交 | 在完整安装边界使本 Star 此前的全部 Comet Session 失效, 包括最终 SECRET 未变的账号 |
| 相同已安装版本的重放/确认重试 | 不重新安装或再次撤销, 丢失 ACK 不能反复踢掉已经重新认证的会话 |
| 旧版本、半份快照、解析/资源准备失败 | 不替换当前完整凭据及安装版本, 不触发本次会话撤销; 旧版本仍按恢复协议拒绝覆盖 |

上述比较使用最终安装边界的当前版本, 不使用开始接收快照时捕获的旧版本. 新表、登录索引、安装版本与旧会话集合失效共同生效, 复用 [Session](#session) 的定序和锁顺序; 大量连接取消与引用回收在锁外完成. 准备中的登录须按当前表完成最终检查, 已失效集合不能接收迟到登录; 安装后通过新表认证的会话不被旧集合的延迟清理关闭.

旧 Session 的写入和 Watch 仍在最终提交/发送许可边界检查有效性, 已提交结果与已获准在途帧按现有规则处理. 失效不修改 Catalog 内容版本、Ephemeris UUID/order 或既有 TTL. SDK 将已确认 Session 的终止视为共享重认证事件, 不直接当成 SECRET 错误; 后续登录确实被拒绝才按认证失败规则暂停. 恢复策略只在 [Comet](../astra/comet/cpp/README.md#client-凭据更新与业务恢复) 定义.

该规则仅针对 Almanac["__auth"]["comet"] 的完整安装. 首次引导没有旧业务会话, auth=false 不建立 Session; 普通 Almanac Scope 安装、Polaris 进程身份变化和同步断链本身均不触发统一撤销. 不为判断历史连续性新增对外字段, 仍使用该 Scope 的权威版本及本机安装位置.

### 消息字段契约

以下编号在各消息内部独立计数, 字段均属 `proto.comet.v1`. 使用本地空消息 Empty, 不为状态占位额外引入协议依赖. 表中的 oneof 必须恰有一个分支, 未设置不能被默认成合法操作; 未列出的 presence 要求由服务端明确校验. Scope 中 Sector/Spectrum 对应前述文本规则. 所有带分组的业务请求直接携带 `1 bytes instance` 和 `2 Scope scope`, 不再定义 Address 中间消息; 凭据仍仅放 metadata. 缺失 Scope 或空名称拒绝, 空 instance 的用途仍由各 RPC 的身份规则限定.

| 消息 | 字段号与类型 |
| --- | --- |
| Empty | 无字段 |
| Scope | `1 bytes sector`, `2 bytes spectrum` |
| AlmanacChange | `1 string key`; oneof action: `2 bytes value`, `3 Empty erase`; 普通值与删除分开 |
| CatalogChange | `1 string key`; oneof action: `2 bytes value`, `3 Empty erase`; `4 uint64 version`; value 时版本为该 Key 的正内容版本, erase 时为 0, 本地删除不表达权威业务删除 |
| EphemerisChange | `1 bytes uuid`; oneof action: `2 Record record`, `3 Empty erase`, `4 bytes data`; Record 为完整注册, data 仅替换已有注册的完整 Data |
| EphemerisChange::Record | `1 bytes attr`, `2 bytes data`; 不重复 UUID, 二者都为空时仍通过 record 分支表示完整注册存在 |

普通 APIKEY 是非空 UTF-8 标识, 初值上限 128 字节且不含 NUL; SECRET 为非空不透明字节, 初值上限 4096 字节, 不作文本规范化. Session 凭据仍为 Star 产生的 32 字节不可预测随机值, 用初始 metadata comet-session-bin 携带, 不复制进业务消息; 开启认证时该项必须恰有一个且为原始 32 字节, 缺失、重复或长度错误拒绝, 不把其文本/Base64 别名当作有效凭据. Session 随机值安装前检查活动表碰撞, 不覆盖已有会话; 随机源或有限重试失败拒绝本次登录. Ephemeris UUID 的原始 16 字节格式见 [消息数据与标识](#消息数据与标识), Create 不接受指定 UUID 或另一个创建请求标识. 业务 UUID 与 Session 凭据、Pulsar 节点身份相互独立.

| 消息 | 字段号与类型 / 约束 |
| --- | --- |
| SessionRequest | `1 string key`, `2 bytes secret`; 建立会话无需预先查询实例, 实际实例从 SessionReply 取得 |
| SessionReply | `1 bytes instance`, `2 bytes session`; 唯一的成功确认消息, 凭据仅在对应 Session 有效期间可用, 后续靠流结束表达失效 |

拍平只去掉一层子消息, 保留 Scope 对分组的表达. 字段号按本表统一调整; 当前 Schema 已生成且两端已接线, 不增加旧 Address 的兼容解析. 生成对象的具体分配取决于首次构造、对象复用与 Arena, 不将每次 RPC 都视为固定两次堆分配; 见 [Protobuf C++ Arena](https://protobuf.dev/reference/cpp/arenas/). 不据此承诺已测量的吞吐或内存收益.

不定义 InspectRequest/Reply 或 Limits 消息, 也不保留 patch_items/patch_bytes 等多 Key 批次限制. 单 Key 仍分别受 Key、Buffer、完整编码消息及准备资源预算约束, 不能只限制载荷而取消消息和总内存上限. Go 生成组织应为 Comet 独立包, 不把这些通用消息名继续生成到现有 Orbit 的同一个 Go 包内; 新 SDK 位于 astra/comet/go, 实际模块与生成路径在实现时固定, 当前没有生成新的业务 Schema. C++ 继续使用 `proto::comet::v1`, 不新增 wire 别名.

| Catalog 消息 | 字段号与类型 / 约束 |
| --- | --- |
| PublishRequest | `1 bytes instance`, `2 Scope scope`, `3 uint64 version`, `4 string key`, `5 bytes value`, `6 uint32 ttl_ms`; version/TTL 必须有效, value 可为空 |
| PublishReply | `1 bytes instance`, `2 uint64 version`; 确认本次内容及期限已在该实例受理, 不返回视图游标或原重试截止 |
| CatalogRenewRequest | `1 bytes instance`, `2 Scope scope`, `3 string key`, `4 uint64 version`, `5 uint32 ttl_ms`; instance/Key 非空, version 大于 0, TTL 明确给出并通过范围校验 |
| Catalog.Renew 的结果 | 复用 Empty; 本次上下文已含实例/Key/版本, 不增加 order、remaining_ms 或重复身份字段 |

Publish 不携带 Set/Delete oneof, 每次就是完整值发布. 编码大小、Key、TTL 和资源预算仍校验, 不自动拆包或修改版本. 没有 CatalogVersionRequest/Reply, 无需先查询整分组版本再发布.

| Ephemeris 消息 | 字段号与类型 / 约束 |
| --- | --- |
| CreateRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes attr`, `4 bytes data`, `5 uint32 ttl_ms`; 不含 uuid 或创建请求标识, ttl_ms 不接受 0 作为默认值 |
| CreateReply | `1 bytes instance`, `2 bytes uuid`, `3 uint32 ttl_ms`; 仅创建时确认固定 TTL, Data/续租顺序初始为 0, 只绑定当前创建尝试的成功结果 |
| UpdateRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes uuid`, `4 uint64 order`, `5 bytes data`; order 大于 0, data 零字节合法 |
| UpdateReply | `1 uint64 order`; 确认本次更新顺序, 不表示租约已续 |
| RenewRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes uuid`, `4 uint64 order`; order 大于 0, 不包含 TTL 修改参数 |
| RenewReply | `1 uint64 order`; 仅确认本次请求顺序, 不返回 Lease、ttl_ms 或 remaining_ms, 同序号重试不重新计算期限 |
| RemoveRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes uuid`; 对应来源实例、范围、UUID 和 Session 仍校验 |
| Ephemeris.Remove 的结果 | 复用 Empty, 无专有 RemoveReply; OK 确认该次请求目标在提交边界已不活动, 不证明先前未知 Update 从未提交 |

Ephemeris 不提供创建结果查询或四状态恢复消息. 对已确认 UUID 的 Renew/Update 校验来源实例、会话和活动期限. 不存在或来源已到期返回结束原因, 会话错误不伪装成到期诱发新注册; 不保存 APIKEY 所有者.

Remove 的目标 instance、Scope 和 UUID 始终来自请求, 成功响应无需重复. 实例、当前会话和业务范围仍须校验, 不把内部范围拒绝、实例不符或传输失败转成成功空消息. 合法目标已不存在时返回明确的 NOT_FOUND, 不新增永久 UUID 墓碑只为重复注销返回 OK; SDK 清理可将明确的已结束当作无需再删除. 迟到的 OK/NOT_FOUND 只处理其原 UUID 的清理结果, 不关闭新注册. Empty 的 Protobuf 消息体为空, RPC 仍有状态、HTTP/2 和可选 TLS 开销, 超时仍可能对应已提交的注销.

续租不定义 Lease 消息或 remaining_ms, CreateReply 仅确认固定 TTL. 客户端计时的唯一规则见 [SDK 本地租约预算](../astra/comet/cpp/README.md#本地租约预算), 不要求访问 Pulsar 或比较两端墙钟.

Update/Renew 请求均带非空预期 instance、Scope、UUID 和正 order. Star 在提交边界校验这些身份, 只有本次操作合法成功或幂等确认才返回相同 order. SDK 使用本次 RPC 上下文及当前生命周期匹配结果, 不从裸 order 推断 UUID/实例; 旧身份或旧尝试的回执不能更新当前状态. order=0 或与请求不符的成功回执是协议错误. 两种回复都不重复传 instance/uuid, 原 future 已结算时不被迟到响应改写.

简化回执不省略 Star 取时、到期校验或截止计算. 单字段 uint64 order=1..UINT64_MAX 的 Protobuf 载荷为 2..11 字节: 1 字节字段标签加 1..10 字节 varint, 不包含 gRPC/HTTP2/TLS 开销; 见 [编码规则](https://protobuf.dev/programming-guides/encoding/). 不把载荷缩短直接写成实测吞吐提升.

| Watch 消息 | 字段号与类型 / 约束 |
| --- | --- |
| WatchRequest | `1 bytes instance`, `2 Scope scope`, `3 bytes target`, `4 optional uint64 version`; target 为空订阅整个 Scope, 非空为精确 Key/UUID; version 缺失请求新快照, 显式 0 表示恢复版本 0, 携带 version 时 instance 必须非空 |
| AlmanacWatchReply | `1 Mode mode`, `2 repeated AlmanacChange changes`, `3 bool complete`, `4 optional uint64 version`, `5 bytes instance`; version 为已安装权威分组版本 |
| CatalogWatchReply | `1 Mode mode`, `2 repeated CatalogChange changes`, `3 bool complete`, `4 optional uint64 version`, `5 bytes instance`; complete 时 version/instance 必须存在 |
| EphemerisWatchReply | `1 Mode mode`, `2 repeated EphemerisChange changes`, `3 bool complete`, `4 optional uint64 version`, `5 bytes instance`; 与 Catalog 使用相同安装边界, 不共用跨业务域 Change |

Mode 的数值为 unspecified=0、reset=1、apply=2, 未知或 unspecified 拒绝. 业务域由调用的 Almanac/Catalog/Ephemeris 服务和响应类型决定, Scope/target 由本次请求确定, 不另包 Target、Position/Domain 或 bytes cursor, 不再有游标序列化与 4 KiB 游标限制. target 未设置与显式空字符串含义相同, 这是本协议选择的全 Scope 默认值; 参见 [Protobuf 标量默认值](https://protobuf.dev/programming-guides/proto3/#default). reset 仅允许 Almanac/Catalog 的 value 或 Ephemeris 的 record 分支, apply 也允许 erase, Ephemeris apply 还可使用 data, 具体前置条件见 [Ephemeris 增量](#registry-delta); 缺少 action、空 Key/UUID 或非法 UUID 均拒绝. Ephemeris record 始终含完整 Attr/Data, 不重复外层 UUID. 实例和版本不是访问凭据, Session 与 __ 边界仍独立校验.

三种响应让生成接口直接限定业务域, 消除 Catalog 流处理 Ephemeris 分支的情况. Catalog 仍区分 value/erase, Ephemeris 区分 record/data/erase, oneof 的判别和解码分支没有消失; oneof 成员共享存储, 不能把四个分支当成四份同时分配的载荷, 参见 [Protobuf oneof](https://protobuf.dev/programming-guides/proto3/#oneof). 此调整优先改善类型边界和去掉 Catalog 的一层记录包装, 不保证生成对象更小或吞吐必然提高. SDK 可共用完成边界、暂存和取消逻辑, 不复制两套同步状态机.

complete=false 的消息只暂存 changes, version 不出现且 instance 为空; complete=true 的最后一条同时携带最后一页、非空 instance 和目标 version, 包括合法的版本 0. Almanac 完成 version 为权威分组版本; Catalog/Ephemeris 完成 version 是接入 Star 的视图游标, 不等于 CatalogChange 的每 Key 内容版本. 空快照或仅推进版本可用一条 changes 为空的 complete=true 消息完成. SDK 累计实际接收及分配量执行预算, 不要求总条目/总字节声明或预遍历快照; 超限或断流不推进已安装位置.

Catalog 纯期限刷新和 Ephemeris 纯续租推进必要来源组状态, 不推进没有内容变化的 Scope 视图游标, 不生成重复下游历史或通知. Catalog 内容版本提高即为可见变化, Ephemeris 仅 order 改变而 Data 相同不属于可见变化. 恢复无差异时仍可发送空 complete 确认已有位置, 不为心跳制造内容版本或映射.

### 错误细节字段

非 OK 的 RPC 可通过有界 trailing metadata `comet-error-bin` 携带序列化 Failure, 不把本地错误对象或异常跨线传递. 使用独立的 metadata 名称, 不把自定义结构冒充标准 `google.rpc.Status`. 缺失、重复或不能合法解析的细节不能作为业务效果证明, 写入保持不确定; 已知的 gRPC 分类仍可用于退避或暂停, 不把每种拒绝都改成网络故障. 未识别的 Effect 不能当作 unapplied.

Failure 字段为 `1 Reason reason`, `2 Effect effect`, `3 optional uint64 version`, `4 optional uint64 order`, `5 optional uint32 ttl_min_ms`, `6 optional uint32 ttl_max_ms`, `7 optional uint32 retry_ms`, `8 string limit`, `9 optional uint64 maximum`, `10 string message`, `11 bytes instance`. instance 为服务端当前实际实例, 用于明确实例不匹配和只读恢复, 不能据此自动改投旧写入. 细节总量初值上限 4 KiB, message 上限 512 字节且不含凭据/载荷; 传输错误可能没有详情. 可选数值必须有 presence, 不把默认 0 误当成当前版本或合法 TTL 下限.

Reason 与 Effect 的数值以 [comet.proto](comet.proto) 为准. 未识别原因保留为未知服务端错误, 不自动改写输入或跨实例重放. Effect 只描述这一次尝试能确定的效果; SDK 对整个逻辑操作保留此前的不确定性, 不因某次重试 unapplied 就否定先前可能提交的事实.

## 订阅流与安装边界

Watch 的上行共用 WatchRequest, 下行按 RPC 分别固定为 AlmanacWatchReply、CatalogWatchReply 或 EphemerisWatchReply, 同一条流内不切换消息类型. 各类型每页均可携带多条本域变更, 最后一页用 complete=true 表示完成. 不单独发送开始/结束消息或要求逐块确认. 一个流同一时刻只安装一个批次, 批内 mode 保持一致, 不交错快照与增量; 一个消息足够时直接完整应用.

首批 reset 在独立暂存状态构建完整投影, apply 在已安装状态上准备修改. 首个 apply 必须承接请求中的同实例恢复版本, 后续 apply 承接上一完整批次, 不另传基线字段. reset 只允许作为新流首批, 首批无论使用哪种 mode, 完成后都只能继续 apply; 历史断档或需要重置时结束该流, 重连后重新确定基线. 旧流回调不能拼入新流的在建状态.

空快照、单页快照、单条增量和多页增量均使用同一完成规则. SDK 只有收到 complete=true、完成记录应用及预算检查后, 才一起发布视图、实际实例和 version; 中途断流保留旧完整视图并标记陈旧. 未完成页不能更新恢复位置, 重复 Key、越界记录、批内 mode 变化或非法完成字段使暂存作废. 完成标志不取消快照接增量规则, 已有 apply 的完成实例不得变化或版本回退. apply 完成版本等于已安装基线时必须没有变化记录, 不能在同一版本下改写视图; 空完成可用于确认恢复已追平, 不要求每次空闲都发送它.

流以 OK 结束也不替代 complete: 丢弃未完成批次, 保留最后完整视图为陈旧, 未关闭的对象按恢复规则重连. apply 的 erase 指向当前基线中不存在的 Key/UUID 是合法无操作, 仍安装完成版本; 基线后创建又删除可能被合并成一条 erase. 首个 apply 必须已有对应完整基线, 不能仅凭请求里有版本就从空容器接增量. 越界 target、未知 mode/action、非法完成位置及其他畸形报文终止该对象的自动恢复, 不反复请求同一非法快照; Ephemeris 缺 Attr 的有限回退另见下文.

恢复位置直接为实例 ID 和 uint64 版本, 与 SDK 对象的业务域、Scope、target 一起保存; 更换任何范围, 包括空 target 与精确 target 之间切换, 都必须清除恢复版本并请求新快照, 不能只携带原数字. 服务端不可能从一个裸整数证明客户端持有哪些范围的数据, 正确范围由调用契约保证, 准入检查不依赖恢复位置. Catalog/Ephemeris 的版本超前、历史不足或实例变化返回 reset, 不能用数字比较跨实例的本地游标. 精确订阅可跳过无关 Key 的提交, apply 完成边界保持同实例且不回退.

Almanac 的请求 version 还表达这个 Reader 在同一权威范围内已安装的最低版本, 实例变化也保留这一下限, 不另加握手或第二个版本字段. Star 的权威版本低于它时, 在发送任何快照页前返回暂时落后 (UNAVAILABLE, reason=version), 携带当前版本和可选退避提示. SDK 保留陈旧旧视图和下限后有界重试, 不因这类落后终止 Reader、清空状态或触发全部 Client 切换. 达到下限后再按实例/历史选择增量或 reset; 跨实例仍不得承接旧 Star 的增量位置. 未提供 version 的新 Reader 不具有这一进程内下限.

建立 Watch 时, 服务端在一致边界取得数据根及该分组的版本 B, 同时确保能观察 B 之后的修改. 随后在锁外按 [冻结投影](../astra/common/README.md#快照与历史) 分页发送快照, 不为每条 Watch 复制整表或预遍历计数, 最后一页携带 complete=true 和 B, SDK 完成后发布这一份完整状态, 后续增量再推进到更新边界. 这保证状态来自同一时刻, 不承诺发送结束时 B 仍是服务端最新位置. 增量同样先固定目标 T 和这一覆盖区间的最终投影, 各页不混入 T 之后的新值; 后续变化进入下一批.

Catalog/Ephemeris 捕获前先完成所需的受控 TTL 推进, 将本轮到期按其来源/副本规则提交到本地投影, 再取得该 Scope 的视图游标和根. Ephemeris 以完整原生记录可见, 不按两个物理键配对; 共享运行循环处理各来源组, 不为每次 Watch 全表扫描所有租约. 轮的向上取整可能使拍内到期延至下一拍删除, 不额外逐条扫描期限或把 Watch 内容当作实时存活证明. 捕获后到期通过后续版本的 erase 到达, 不在发送冻结根时过滤期限, 否则同一个版本会出现不同内容. 到期推进因读时/准备失败而不能完成时, 不建立新的“已追平”基线; 已有受影响 Watch 结束并报告暂时错误, SDK 保留旧视图为陈旧. 时钟质量暂时不合格但可继续推进已有期限的情况不因此停流.

获授权但尚未创建的 Scope 也能建立全 Scope 或精确目标 Watch, 完成就绪的空基线, 版本为 0; 订阅本身不创建业务提交或推进版本. 已存在但内容为空的分组保留原版本. 空基线建立与首次写入使用同一观察边界, 后续创建可自动到达, 不要求轮询重建订阅. 非法标识、内部范围或会话失效不能返回成功空视图; 等待未创建范围也计入订阅/内存预算, 关闭后释放.

Watch 表达目标版本的最新状态, 不保证交付每次中间事件. 同一批内相同 Key/UUID 的变化合并为最终 Set/record/data 或 erase, 不重复输出同一目标; 新建、修改和最终删除按覆盖历史定序. 合并不能串用不同 Scope 或伪造跨请求事务. 没有目标内容变化时可以仅发送完成位置, Ephemeris 的具体投影见 [增量规则](#registry-delta).

快照期间的修改在基线 B 建立时即接入有界待发送状态, 按变更目标合并; 可用历史补取, 但不能仅赌发送结束前全 Scope 历史尚未淘汰, 否则持续写入可能让正常快照反复重来. 不为每个 Watch 保存完整 UUID 清单, 不无限固定历史或积压以等待慢接收者. 如果资源或历史已不足以保证 B 之后连续覆盖, 结束该流并明确要求重建基线, 不发送声称可衔接的 complete 或偷偷跳过删除. 已经装入 B 的 SDK 可保留它但标记陈旧; 尚未完整装入的继续保留旧视图. 重连仍由服务端决定增量或 reset.

全量和增量批次在本地都先准备再发布, 断流、分配失败、超限或取消均不改写已交付视图和恢复位置. 先发布完整状态再调用观察者, 不要求服务端等待用户回调确认. 直接执行的观察者必须快速返回, 不承诺阻塞它时 SDK 仍能持续处理网络. 用户长期持有旧视图不影响服务器的历史回收.

WatchRequest 不上报 view_bytes, 服务端不估算客户端容器内存或根据声明预遍历. SDK 的视图/在建容量及超限停止恢复规则见 [本地资源](../astra/comet/cpp/README.md#本地资源), Star 独立执行发送/准备预算.

HTTP/2 流控不跟踪已经从 gRPC 读入业务 Map 的累计内存, 不能替代两端应用预算. 服务端待发送与实际在途持有均有界; 合并后仍超限则结束该流, 不持有业务提交锁等待网络, 不阻塞其他订阅或正常写入. gRPC 完成前不释放在途缓冲. 参见 [gRPC 流控](https://grpc.io/docs/guides/flow-control/).

<a id="registry-delta"></a>
<a id="ephemeris-delta"></a>

### Ephemeris 增量投影

EphemerisChange 的 data 分支替换完整 Data, 不解释 Buffer 内部字段, 不改变 Attr、TTL、UUID 或注册顺序. 接收者最终向应用发布的仍是完整注册视图, 并非把半条记录作为可见对象.

| 当前投影与变化 | 下行 action |
| --- | --- |
| reset 全量快照, 包括精确 UUID 首次命中 | record, 同时包含 Attr/Data |
| 已安装基线包含该 UUID, apply 只更新 Data | data, 不重传 Attr |
| 该 UUID 在本批基线之后新建, 包括创建后多次 Data 合并 | record, 携带该目标版本的 Attr 与最新 Data |
| 注册注销/到期, 或本批最后状态为删除 | erase |
| 仅续租且内容未变 | 不生成内容变更或新视图游标; 仅恢复确认可回传已有完成位置 |

服务端依据已覆盖的历史及创建/更新/结束语义选择 action, 不要求客户端上传 UUID 清单或在服务端维护每个 Observer 的完整成员集合. 合并只在确定基线至目标版本的范围内进行: 本批新建的 record 不能被后续 data 合并成 data-only; 最终 erase 优先, 后续新注册使用新 UUID. 无法证明接收者具有 Attr 时发送完整 record; 无法证明历史连续性时遵循 Watch 重建基线规则. 记录来自目标版本的一致投影, 不读取更晚当前值却标为较早完成版本.

SDK 仅对当前批次承接的完整基线应用 data. reset 中 data/erase 均非法; apply 遇到基线不存在或已删除的 UUID 的 data 时使该批次作废, 不创建空 Attr、不忽略并推进版本, 也不无限等待未来 record. 结束流并保留旧完整视图为陈旧; 仅允许一次省略版本的新快照恢复; 成功的新基线也未能恢复合法增量时停止该订阅并报告协议错误, 不在 reset 与失败间无限循环. 普通断线恢复仍按既定退避执行.

data 分支中的零字节是有效新 Data, 与缺少 action 或 erase 分开. 同一批次最多一条每 UUID 的最终 action; 原有 UUID 的完整 record 回退也不能改写其不可变 Attr. 任何协议错误、取消、预算失败或分配失败都不发布部分结果. 纯 data 更新可共享已有 Attr 的不可变存储, 不因拼装公开完整视图再复制 Attr 字节.

这种优化节约每次 Data 更新向每个订阅者重复传送的 Attr 载荷, 不消除 UUID、Protobuf、gRPC/TLS 和调度开销. 消息与两端编码已落地, 吞吐/延迟收益仍须获准后实际测量.


## 错误与资源边界

错误包含稳定原因、提交确定性和必要的受限细节. gRPC 状态提供传输分类, Comet 的结构化细节提供业务原因; 不仅凭错误字符串驱动恢复. 传输层直接拒绝、超时或断线可能没有业务细节, 已发写入在这种情况下按不确定处理, 不默认判成未提交. 提交之后的异常及取消统一遵循 [提交分界](#提交前后与耗尽边界).

| 原因 | 拟采用的 gRPC 分类 | SDK 处理 |
| --- | --- | --- |
| 非法标识、缺少操作、非法 TTL | INVALID_ARGUMENT | 明确失败, 不自动修改输入后重试 |
| 会话缺失或失效 | UNAUTHENTICATED | 普通业务触发合并重新认证; Session 登录自身被拒绝则暂停等待凭据更新, 不互相递归重登; 请求是否重试仍按各业务契约 |
| 外部访问内部 Sector 或服务角色不符 | PERMISSION_DENIED | 明确拒绝, 不匿名降级; 不引入逐业务范围 ACL |
| Catalog 内容版本/同版本内容冲突 | ABORTED | 报告该 Key 当前依据, 停止冲突期望的自动重发, 等待业务提供合法新内容; 不自动加号覆盖 |
| Almanac 低于 Reader 请求的已见权威版本 | UNAVAILABLE, reason=version | 保留旧完整视图和下限, 有界退避等待追平, 不反复下载较旧快照或当作 Publisher 冲突 |
| Catalog 本机来源记录缺失/到期/仅水位, 且没有更高内容版本冲突 | NOT_FOUND, reason=ended | Publisher 用自身最新完整期望重新 Publish 相同 Key; 不生成业务版本、不从远端副本直接续租 |
| Ephemeris 已结束/不存在 | NOT_FOUND | Update/Renew 按新 UUID 规则恢复, 不能复活旧项; Remove 按已结束清理, 不触发重注册; 创建不确定结果另行处理 |
| 旧顺序、游标失效或历史不足 | FAILED_PRECONDITION | 通过具体原因区分: 旧写入不覆盖, Watch 重新取得基线 |
| 单请求过大、并发/会话/订阅预算不足 | RESOURCE_EXHAUSTED | 单次输入/不可恢复上限用 reason=limit, 暂时接纳不足用 reason=busy; 本地视图超限不反复下载, 暂时压力有界退避; 不靠错误字符串猜测 |
| 暂时不可用或实例已变 | UNAVAILABLE / FAILED_PRECONDITION | 退避并重新确认目标, 不将旧实例写入自动改投新实例 |
| 时钟尚未首次校准或本地计时不可用 | UNAVAILABLE, reason=clock | 本次未提交新期限, 有界退避, 保留既有身份的不确定状态; 单纯 Pulsar 失联或同步质量下降不产生此错误, 不假装 ended 或全 Client 立即切换 |
| Almanac 旧提交的确认历史已淘汰, 内部管理接口 | FAILED_PRECONDITION, reason=uncertain | 原操作无法确认, 不凭当前状态自动提高版本覆盖 |

错误中的提交确定性默认是不确定, 只有服务端或本地接纳阶段能够证明未产生该操作效果时才标成未提交. 当前版本、允许的 TTL 范围及资源上限只向有权访问该范围的请求提供, 错误不回显 SECRET 或业务载荷. 用户观察者异常不改变已经就绪的 future 或业务提交.

“这次重试未修改状态”不等于“原操作从未提交”. 原调用已发送且结果未知时, 后来的权限拒绝、注册到期或幂等证据不足只能说明当前尝试的结果, 不能清除原操作的不确定性. Error/Receipt 映射须区分逻辑操作与一次传输尝试, 已确定的原操作结果仍只完成一次, 不用后来的拒绝覆盖它.

### 服务端与编码预算

以下仅为可配置、未实测的实现初值, 不是永久协议上限. Client 独立预算见 [SDK](../astra/comet/cpp/README.md#本地资源); 没有 Limits 协商或配额缓存.

| 项目 | 初值 | 计量与处理 |
| --- | --- | --- |
| Sector / Spectrum | 各 128 字节 | UTF-8 编码后的长度, 不按字符数截断 |
| Almanac / Catalog Key | 1024 字节 | 非空, 超限拒绝 |
| 单个业务 Buffer | 1 MiB | Catalog 值、Attr、Data 分别计算, 零字节合法 |
| 单个 Catalog Publish | 一个 Key, 完整 value | 同时受 Buffer、消息及准备预算约束; 没有 Delete 或多 Key 模式 |
| Comet 业务 gRPC 单消息收发预算 | 8 MiB | 包含编码开销, 与实际业务载荷限制分开 |
| Star 对等 / Polaris 数据流单消息收发预算 | 8 MiB | 单独配置两端, 不沿用现有控制会话的 4 KiB; 不放宽 Admission/Pulse 自身上限 |
| 内部数据页 / 装包目标 | 256 KiB | 按完整记录/提交装包, 较大单条可独占一包, 同时受消息硬上限约束 |
| Watch 页面目标 | 256 KiB | 尽量按完整记录分页, 单条较大记录可独占一页但仍受消息和视图预算约束, 不是 64 KiB 硬协议 |
| 单订阅发送积压 | 8 MiB | 包括待发送和在途持有, 合并后仍超限按慢订阅规则断流 |
| Star 业务层受控存储总预算 | 512 MiB | 当前业务、历史/幂等证据及在建/发送状态共同计量, 不声称该值等于整个进程 RSS |
| Star 业务会话 / 活动订阅 | 各 4096 | Session 流含未完成认证的占位受会话预算约束, 不做空闲回收; 匿名订阅也计入订阅预算 |
| Star 同时受理的业务 unary | 256 | Session/Watch 长期流另计, 普通写入不能耗尽清理和续租所需的全部受理额度 |

配置校验覆盖完整链路: 单条最大 Catalog/Almanac 值, 以及最大 Attr + Data 的 Ephemeris 完整记录, 连同 Scope、Key/UUID 和编码开销都必须能通过对应内部同步与外部 Watch 的单消息上限. 页面目标允许被单条合法记录超过, 消息硬上限不允许; 不接受“能写入但无法复制/订阅”的本机配置. 跨节点静态容量不相容明确报错, 不增加动态 Limits/Inspect 或静默截断载荷. 服务端/客户端传输配置与编码测试必须一起落实.

总预算包含当前业务、历史、幂等证据和准备峰值, 完整恢复及维护余量按 [存储预算](../astra/common/README.md#capacity) 保留. 共享载荷按实际分配计量, 不只算句柄. gRPC/TLS/分配器开销不自动被业务预算覆盖, 不能把这些数值当成进程 RSS 硬上限. 不驱逐仍有效记录来接纳新写入, 续租、到期与清理保留可推进资源.

业务 handler 并发计数不限制 handler 前的接收/Protobuf 解码分配, 须同时配置消息与传输资源并在获准后测量. 当前值、Catalog 最高版本表、来源历史、下游历史和在途引用共同计量; 共享值不重复分配, 不能只计算指针大小. Catalog 重试不维护 LRU, Ephemeris 无创建去重缓存, 孤立注册计入容量并按实际 TTL 清理.

Polaris 的管理幂等证据来自其有界持久历史, 不放入 Star Comet 会话. 动态来源历史按实例/域划分, 本地可见历史按域/Scope 划分, 两者不能互代; 不承诺某个配置保留时间就是任意慢对端可用的最小恢复窗口.

Session/Watch 使用 gRPC Callback/异步路径, 不用同步阻塞 Read 占住业务线程. 冷启动大快照及增量投影也不能在一个 reaction 内无界遍历: 按有界页推进, 必要的重计算使用已有共享且有界的工作资源, 不为每条流新建线程.

### 外部链路保活

空闲 Session/Watch 的存活检测显式配置 HTTP/2 Keepalive, 不默认依赖通常关闭的客户端保活. 初始部署值为客户端间隔 60 秒、确认等待 20 秒, Star 允许有活动 RPC 的连接按该间隔发送 PING; 双方及中间代理需要一致配置, 不使用远低于一分钟的探测. 无活动调用不主动 PING, 不增加应用层 Session 心跳或空闲认证扫描. 保活只检查所连传输端, 不能证明代理后面的业务处理健康或保证 1 秒 TTL; Renew 的有限 deadline 与本地预算独立执行. 这些是可配置的故障检测初值, 不是协议消息字段, 见 [gRPC Keepalive](https://grpc.io/docs/guides/keepalive/).

Session/Watch 等长期 RPC 也占用 HTTP/2 并发流额度. 三个业务域共享 Client 不等于所有流必须挤入一条底层连接; 流占满后的传输排队仍消耗请求 deadline. SDK 的传输隔离和真实在途计量见 [本地资源](../astra/comet/cpp/README.md#本地资源), 不能只靠业务 handler 数量或 TCP 保序保证续租延迟.


<a id="generation"></a>

## 生成与来源

```powershell
./scripts/generate-proto.ps1
./scripts/generate-proto.ps1 -Check
```

```bash
bash scripts/generate-proto.sh
bash astra/build.sh generate
bash astra/build.sh check-generated
```

复用项目 protoc 36.1、protoc-gen-go 1.36.12、protoc-gen-go-grpc 1.6.2、grpc_cpp_plugin 1.84.0. 普通构建不运行生成器, 生成器不下载工具; 运行规则仍遵循 AGENTS.md.
Go 源码在 `astra/internal/generated` (按 proto 独立分包, 由 `astra/generate.py` 生成), C++ 在 `astra/common/src/generated`. Pulsar schema 仅生成 C++ Pulse 类型, Orbit 的 pulse_endpoint 同步生成 Go/C++.
Go module/import/go_package 暂保留仓库原路径. 当前不手改生成源码, 不留下旧 schema 的替代入口.

`message-ids.lock` 仅是旧帧编号历史, 不参与 gRPC 路由. 独立 Rust 生成工具仍维护既有检查, 不代表恢复 Rust 服务.
隔离推流协议位于 `astra/bench/proto/probe.proto`, 包名为 `proto.astra.bench.v1`, 不注册到生产服务. 真实验证见 [最新结果](../testkit/validation.md).
