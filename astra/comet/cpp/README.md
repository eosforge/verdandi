# Comet 原生 C++ SDK

本文描述当前 C++ SDK 源码接口. Publisher 同步原子更新、Beacon 同步注册/更新与 beat/tick、同逻辑 ID 恢复、Reader/Subscriber 回调及 Observer 本地选择均已接线. Star、内部复制和 C++/Go 生成协议随 Beacon 恢复一起更新.

精确构建、测试配置和源码身份统一见 [验证记录](../../docs/validation.md), 不沿用旧版回归作为当前工作区的发布证明. 原始字节接口以 [Client](include/comet/client.hpp)、[Beacon](include/comet/beacon.hpp)、[Observer](include/comet/observer.hpp)、[Subscriber](include/comet/subscriber.hpp) 和 [Reader](include/comet/reader.hpp) 为准.

| 接口 | 当前行为 |
| --- | --- |
| Client.beacon | 必须传 beat, 首次注册同步确认后返回句柄 |
| Beacon.update | 同步 Result, 失败不补发, 只缓存已确认 Data |
| Beacon 身份与续租 | 同逻辑 ID 恢复, 注册代次独立于 Data 版本, Update 原子延期 |
| 读取对象 | Reader/Subscriber watch/state/changed/stop; Observer.one/stop, 本地估计受代次和比较交换保护 |

当前 Catalog 入口如下, 不需要应用填写 version:

```cpp
auto publisher = client.publisher({"routing", "main"});
if (!publisher) {
    return std::unexpected(publisher.error());
}
auto result = publisher->update("service-a", {1, 2, 3}, std::chrono::seconds(30));
// result 为 Result<Publisher::Receipt>; 批次重载接受 vector<Publisher::Entry>.
```

同一 Publisher 的重叠 update 返回 busy, 不排队合并. 同步 RPC 使用调用线程, 共享 Core 控制轮负责认证与切换; Client/Publisher 关闭通过标准停止令牌直接取消同步 RPC, 不等待 watch/changed 回调返回. 停止回调先于 RPC context 析构注销, 旧控制轮快照只能取消其捕获的那次调用. SDK 通知回调内同步 update 返回 busy, 避免阻塞共享控制轮; 显式关闭优先报告 closed.

| 查阅内容 | 唯一定义 |
| --- | --- |
| RPC、字段、版本、服务端 TTL、Session、Watch | [协议](../../proto/README.md#comet) |
| 三域原生存储、提交与快照 | [存储](../../common/README.md) |
| Star 初始化、监听与业务认证配置 | [Astra](../../docs/build.md#运行模式与接线) |
| 管理与指标 | [Astrolabe](../../astrolabe/README.md) |
| 用例和验收证据 | [验收计划](../../docs/comet.md) / [实际结果](../../docs/validation.md) |

## 边界与组织

公共头文件归属 include/comet, 私有实现 src, 测试 tests, C++ 命名空间 comet. gRPC/Protobuf 类型仅在适配层, 不进入应用签名. 首版交付静态库, 不同时实现共享库、C ABI 或其他语言绑定.

SDK 最低语言标准为 C++23, 遵循 [C++ 规范](../../docs/coding.md#cpp); 服务器继续使用 C++26. Windows/MSVC 19.51 x64 Release 已通过 SDK 编译链接、五项本地测试、源码接入和安装包接入; 本机 CMake 将 C++23 映射到 `/std:c++latest`. Linux/GCC 16.2 的 C++23 SDK 已随 Debug、Release、探针版、ASan/UBSan、TSan 完整矩阵及独立安装包验证通过. Sanitizer 构建向消费端传播编译及链接要求, 避免 Protobuf 插桩布局不一致. 复用既定 gRPC/Protobuf 与配套 BoringSSL, 不引入独立 OpenSSL 或隐式下载依赖. SDK 不承担 Star 复制、日志保留、发布权威裁决或对时.

## 公共职责

下表列出当前公共职责. load/watch/select 与 close/wait 的原有快照和退出入口继续保留, 新代码可以使用下列接口.

| 对象 | 已确认的公共职责 |
| --- | --- |
| Client | 所有角色共享连接、活动 Star、TLS/Session、调度、预算和取消 |
| Beacon | 同步首次注册与 Data 更新; 可选 tick、必启 beat、已确认数据缓存及同 id 注册恢复 |
| Observer | 仅 Ephemeris; 从同步到本地的池中 one(selector), stop() 停止 |
| Publisher | 绑定 Catalog Scope; 同步提交单键或多键原子更新, 内部管理版本, 每次指定 TTL |
| Subscriber | Catalog 全 Scope 或精确 Key 的 watch/state/changed/stop |
| Reader | Almanac 只读, 与 Subscriber 同形接口, 保留权威版本下限 |

子对象持有 Client 共享核心. 最后一个 Client 公开句柄释放不关闭仍存活的子对象; 显式 Client::close() 才关闭全部角色. 单个 Beacon::destroy() 或读取对象 stop() 只结束自身. C++ 业务句柄最后释放启动自身非阻塞清理, 不在析构中等待网络, 内部引用不能让已无人持有的业务永久续租.

### 私有实现收敛

Client 的共享核心集中持有活动端点、Channel/Stub、Session、退出状态、预算和定时资源; Publisher 持有版本元数据和当前同步调用, Beacon 持有身份、已确认 Data 及有限尝试, 三种读取对象复用私有 Watch 核心. 新状态先归入真实拥有者, 不为每个阶段另建 Manager/Service/Executor, 不把同一会话或退避配置复制进每个业务对象.

当前核心将 Session/Watch 与 unary 分别交给两类共享 Channel, 不按业务对象或 Watch 数量新建连接池. 这用于隔离长流占用的并发流额度, 不保证底层固定两条 TCP 连接. 续租合并和多目标订阅是否值得扩展按 [协议评估边界](../../proto/README.md#comet-batching) 与性能矩阵判断, 不能从对象数直接推导连接数或批量收益.

相同待办只保留一次唤醒, 在实际调度时重查当前操作及截止; 已取消但未完成的 RPC 仍按真实寿命单独归还资源. 通用能力限于已有网络、定时、所有权和完成机制, 不把 Publisher 的内容版本规则与 Beacon 的独立顺序强行合成一个业务状态机.

Core 的本地完成事件进入独立弱引用就绪队列, 同对象重复事件合并; 普通网络唤醒只解析 K 个实际就绪对象, 不扫描 N 个目录对象. 接纳时预留队列容量, noexcept 唤醒不分配, 队列不延长对象寿命. 只有活动期限到达或共享身份/关闭/满额归还时才扫描目录; 自动续租和操作超时仍按各自截止调度, 不降为每秒清算. 定向推进保存保守的最早期限, 旧值至多造成额外一次提前维护, 不延后任务. 处理期间的新事件可再次入队, 至多八轮后交还线程. 不增加执行器、逐对象线程或用户可操作的排队状态; 普通构建与回归已通过, 性能结果见 [验证记录](../../docs/validation.md); 尚未测量数千空闲 Watch 的定向收益, 不宣称各负载普遍提速.

当前不可变 View 的 each 遍历会固定本次数据根, 即使用户回调重新赋值原 View, 当前 Key/Value 和剩余条目仍有效. 回调持有的参数仍只在该次回调及所属数据根存活期间有效; 跨回调保存正文需复制 Value 所有权. 取消、基线缓存及遍历修复的实际执行范围与源码身份见统一验证记录.

### 同步写入与明确结果

当前 Client 及各角色的 wait(timeout) 只等待本地清理. 非正超时立即检查完成状态; 极大正值按 steady_clock 可表示的最远时刻饱和处理, 不发生毫秒转换或截止相加的有符号溢出. wait 不隐式 close, SDK 通知回调内仍禁止阻塞等待.

当前 `client.beacon(...)`、`beacon.update(data)`、`publisher.update(batch, ttl)` 均为同步 RPC 调用. 工厂或操作返回 `std::expected<T, Error>`; Beacon 工厂为 `std::expected<Beacon, Error>`, 不是不能表达错误类型的 optional. 公共写入不再返回 future, 不另保留“接纳成功后继续提交”的第二种成功语义.

成功表示这一次操作已获接入 Star 的提交确认, 不表示所有副本已更新或业务已经持久化. 明确未提交、明确拒绝与已发送但结果不确定分别表达. 超时/断链/取消可能发生在远端提交之后, 不把这些错误伪装成未写入; 迟到成功不能改写已经返回的结果.

每个调用只有一个 deadline, 包含准备、认证、版本查询、允许的有限冲突修复及 RPC. 子步骤使用剩余预算, 不能每步重新计时. 返回失败后不继续补发这次 update, 不保存失败 Data 为待发送期望, 不合并或覆盖其他同步调用的结果. 原 RPC 可能尚在远端完成, 本地缓冲和额度须保留到实际完成.

同一调用捕获目标实例及生命周期. Client 切换不能将原失败请求暗中改投新 Star; 后续新调用可使用新的就绪目标. Beacon 自动注册恢复是独立生命周期操作, 不是重新执行一个失败 update. Catalog 仅有下文明确限定的一次调用内版本冲突修复.

结果返回不依赖 watch/changed 等观察回调完成. 预期业务错误用值表达, 不泄漏 gRPC/Protobuf; 分配失败和程序错误不因使用 expected 就自动变成可恢复错误, 不承诺所有入口 noexcept.

### 原始载荷与可插拔编解码

Data、Attr 和 Catalog/Almanac 值由应用决定格式与完整性. 应用的类型适配在调用前编码成 Value, 在读取回调内显式解码; SDK 不固定 JSON、注册 codec 或借 C++26 反射另建序列化框架. 编解码错误由应用适配层保留, 不作为 nullopt 删除事件传入业务. tick 回调只返回 Data, 不要求套一层 Result/Optional; 合法空 Data 仍是一次完整值更新, 不是“跳过”.

编码失败不发请求, 解码失败明确报告, 不伪装成删除、空值或网络错误. Star 不解释普通业务 Buffer 的字段.

### 标准字节容器与所有权

不新增 Buffer 胶水类. C++ 原始入口按用途使用拥有数据的 `std::vector<std::uint8_t>`、借用输入 `std::span<const std::uint8_t>` 和共享不可变载荷 `std::shared_ptr<const std::vector<std::uint8_t>>`. vector 按值接收, span 在跨越调用寿命前转成拥有存储; 支持共享入口时 nullptr 拒绝, 指向空 vector 合法. 不为每个方法机械排列全部重载.

同步返回不证明 gRPC 已释放输入. 一旦请求可能超过调用返回时刻存活, SDK 必须拥有其缓冲, 不保留应用临时借用; 取消和超时均如此. 共享/移交之后应用不能通过可写别名修改在途内容. Key、范围和回调载荷遵循同一所有权约束.

Publisher 只持有当前调用及仍未结束 RPC 所需内容, 不保存跨调用恢复副本. Beacon 额外保存固定 Attr、最近成功确认的完整 Data 及其版本, 用于注册恢复. “仍在清理中的请求缓冲”不等于“允许后台补发的业务缓存”. 生成消息在真实完成后才复用, 不为排队、发送、恢复各复制一份 Protobuf.

读取回调的值可安全保留, 后台不得改写已交付快照. Observer 的短期可变选择视图采用独立规则, 见下文; const shared_ptr 本身不是对可写别名和业务逻辑并发的保护.

<a id="catalog"></a>

## Catalog Publisher

`pub = client.publisher(sector, spectrum)` 只绑定 Scope. `pub.update(batch, ttl)` 同步提交一组 Key/Data 的完整新值, 单键是批次的特例. 这里的 batch 仅表示集合语义, 不强制变长参数或某一种 C++ 容器. 不绑定固定 Key/TTL, 不要求应用提供 version, 不提供公开续租或 Delete.

### 版本与原子提交

- 同 Scope 允许多个 Publisher 写不同 Key, 使用者保证每个 Key 的写入者唯一. SDK/Star 不增加分布式所有权锁、选主或全局版本服务.
- 同批共享一个版本, Star 逐 Key 比较. 不同 Publisher 的不相交 Key 可以使用相同数值; 较大的 Scope 视图游标不能使另一 Key 的合法更新被跳过.
- SDK 内部通过初始化/查询 RPC 取得目标 Star 已知的相关 Key 版本. 新批次版本高于这些版本及该 Publisher 已发出的版本; 可跳号, 溢出明确失败. 未知结果消耗过的版本不能配上另一份内容重新使用.
- 同一目标和精确 Key 集合的连续成功更新复用已确认基线, 正常后续调用只发 Publish. Key 集合按排序后全文比较, 换序仍相同; 换目标、换集合、未知结果或其他已接纳调用失败后重新 Query. 只保留最近一批至多 128 个 Key、实例和水位, 计入共享预算, 不保留 Data 或累计历史 Key 表.
- 进程重启/新建 Publisher 通过内部查询建立基线, 不要求业务保存 version. 查询不是全网最新值或唯一发号证明; 多 Star 的暂时不一致仍允许. 业务自行持久化需要保留的 Data.
- 整批校验、准备与内存提交原子完成, 任一条失败不提交部分键. 全 Scope Watch、快照及回调在完整批次边界安装. 两条精确 Key 订阅不提供联合原子读取.
- 版本是数据排序依据, 不是全网事务 ID. 来源历史与传播需要辨认批次时不得只凭该数值合并不同 Publisher 的提交; 内部批次标识和传输边界在协议实施时明确. 不承诺跨 Star 的全局事务顺序或同时可见.

### 失败与有限修复

只在服务端明确返回版本冲突且证明整批未提交时, SDK 才可在原调用内查询同一目标 Star 的相关 Key 版本, 重新分配版本并重发原完整批次一次. 查询结果用于版本基线, 不替换用户本次提交的 Data. 第二次失败、查询失败、目标切换或原 deadline 耗尽即返回; 不转成后台循环.

超时/断链等不确定结果不进入该修复路径, 普通认证、格式、容量错误也不自动加版本重发. 返回失败后内容不保留为待发业务缓存. SDK 可保留版本元数据, 以及仍未完成 RPC 的必要缓冲, 但不能以后再提交它们.

### TTL 与切换 Star

每次 update 都显式附带新的 TTL. SDK 不自动 Renew, 不按定时器重复 Data; Star 在该次受理时计算期限, 服务端到期后通过 Watch 告知删除. 订阅者不按客户端墙钟自行删键, SDK 无需与 Star 对时.

切换 Star 后, Publisher 保留内部已经发出版本的依据, 必要时查询新目标, 下一次显式 update 带完整 Data, 因而目标缺少 Key 也可创建. 切换不恢复旧内容或整个 Scope, 不使旧失败调用变成功. 关闭 Publisher 停止新调用, 不发送 Delete; 之前可能已经提交的记录按最后实际期限到期.

<a id="registry"></a>
<a id="ephemeris"></a>

## Beacon

当前入口见 [beacon.hpp](include/comet/beacon.hpp). 工厂首次注册总期限由 Beacon::Options::timeout 控制, update 的可选 timeout 默认 3 秒, 均接受 (0, 1 min].

`beacon = client.beacon(sector, spectrum, attr, data, ttl, beat)` 合并初始化和首次提交. 工厂同步等首次注册确认, 成功才返回可用句柄; 失败由外部决定是否重新调用, 不在返回错误后继续初次注册. 初次注册超时可能留下无人续租的远端记录, 由 TTL 回收.

Attr、ttl、beat 在创建时固定; update 仅提交完整 Data. SDK 与 Star 分别校验自己负责的条件, 不以客户端验证代替服务端的 TTL/身份/版本/容量检查. beat 必须始终启用并满足 `0 < beat < ttl`; tick 可不配置. tick 间隔在调用 tick() 时验证, 必须大于零, 不用 interval=0 关闭保活.

### 更新、tick 与 beat

`beacon.update(data)` 同步返回, SDK 内部分配 Data 版本/order. 成功的新 Update 在 Star 的同一提交边界替换 Data 并延长期限, 固定 TTL 不变. 同一 order 的重复确认不能再次延期; 只改顺序但内容不变可不触发内容通知.

`beacon.tick(interval, callback)` 设置或替换自动采样任务, callback 只返回 Data. 同一 Beacon 的采样串行, 不重叠、不积攒错过的历史 tick. 合法的手动 update 重置 tick 倒计时; 比它更早启动而晚返回的采样结果作废, 不能覆盖新手动值. 采样异常通过状态/错误报告, 不以空 Data 冒充失败. 内部提交走同一同步结果规则, 失败后不补发该采样值.

beat 在一个 beat 间隔内没有实际发出的新 Update 时才发 Renew. 有新 Update 可省去独立 Renew; 本地参数失败、只写缓存或比较出相同内容不能无限推迟保活. 实际新 Update/Renew 的发送安排下一次检查, 但发送成功不等于远端租约已确认. 数据比对只能在不改变同步结果和 TTL 语义时省带宽, 不能仅凭本地相等返回“本次已提交”.

### 创建和身份恢复

成功创建过的 Beacon 随 Client 活动 Star 变化接收内部通知, 自动恢复注册; 应用句柄、tick 设置和回调不变. 保持同一逻辑 id, 恢复使用固定 Attr/TTL 与最近成功确认的 Data/版本. 失败或结果不确定的 update 不替换这个恢复缓存, 也不因恢复顺便补发.

同 Star 重连优先继续仍有效的注册. 换 Star、实例重建或租约结束需要恢复时, 注册代次与 Data 版本分别处理: 不能仅为了让缓存覆盖其他副本就把旧内容改成较高版本. 目标已知更高 Data 版本时不得用较旧缓存覆盖. 旧操作按目标、注册代次与尝试隔离, 迟到 Update/Renew/Remove 不影响新注册.

这里接受弱恢复: 旧 Star 停止续租, 残留按 TTL 消失; 不同 Star 暂时持有不同数据或切换后暂见较旧值均允许. 例如本地确认到 v10, v11 在 A 提交但回执丢失, B 尚未知 v11, 恢复时暂用 v10 是允许的, 不能将其冒充 v12. 单份订阅投影按逻辑 id 去重, 不承诺全网瞬时只有一个来源或全局永不回退.

新工厂调用/新进程不自动认领任意外部 id. Star 首次签发 32 字节随机 capability, 与 Scope、Attr 和 TTL 一起导出固定版本/变体位的 16 字节逻辑 UUID. SDK 私有保存能力, 恢复携带 generation 和已确认 order/Data; 目标返回实际采用的 Data/order, 包括它已知的较新内容. 能力不进入 Watch、复制或诊断. 只知道 UUID 或共用 APIKEY 无法恢复新版注册. 新版 SDK 需要同步更新 Star 和复制协议, 不支持与旧 Star 混用.

`state()` 与 `changed(callback)` 同时提供, 报告已就绪、恢复、失败、租约不确定和关闭等状态及具体原因. 原因或状态未变的每次 beat 不必触发通知. 永久参数/身份错误停止相应恢复, 不无限重试或生成新身份绕过错误.

### 本地租约预算

SDK 不比较服务端绝对截止与本机墙钟, 不连接 Pulsar. 每次新 Create、恢复、Update 或 Renew 保存首次发送的本地经过时间, 成功后以首次发送时刻 + 确认 TTL 形成保守预算. 同 order 重试保留原起点, 不能从迟到回执重新给满 TTL; beat 调度与租约已确认状态分别记录.

预算耗尽标记不确定, 不凭本地估计断言全网删除; 恢复探测仍有有限 RPC deadline 和退避. 本地读时失败暂停依赖该时间的发送并报告错误, 不伪造时钟. 系统挂起恢复后立即重查预算, 不补发错过的所有 tick/beat.

Linux 使用包含 suspend 的 CLOCK_BOOTTIME, Windows 使用 QueryInterruptTimePrecise; 普通 RPC 的单调 deadline 与租约经过时间分开. 不声称跨机器时钟速率、长时间挂起下的严格存活证明, 最终期限由 Star 判定.

### destroy 与生命周期

`beacon.destroy()` 立即完成本地逻辑关闭, 幂等且不等待网络, 与 Client 或其他角色的存活无关. 停止新 update、tick、beat、注册恢复, 取消旧尝试并在有界清理预算内尽力注销; 不保证远端立即消失.

可以从自身回调调用 destroy. 关闭前已经开始的回调允许结束, 返回的采样值丢弃; 关闭先生效则不得再启动新通知或采样. 已开始同步 RPC 仍结算一次, 远端可能已提交. 内部清理到真实完成才释放资源, 不在自身回调等待自己, 已关闭句柄永不因切换 Star 或换凭据而复活.

## 业务认证

部署凭据来源和服务端授权见 [Astrolabe](../../astrolabe/README.md) 与 [Session 协议](../../proto/README.md#session). Client 默认启用 auth, 匿名模式必须显式关闭且服务端允许; 不在失败后降级. Comet 只配置 Star 业务地址, 不连接 Pulsar 或内部管理/指标端口.

同一 Client/Star 合并并发认证, 持有一条当前 Session 流. 认证开启时, 首条确认前不得发送受保护业务, 后续调用复用该会话 metadata. 首次确认使用独立短等待, 确认后取消该计时, 不把短 deadline 设为长期 Session 的寿命. 迟到确认/结束不能覆盖新会话.

Client 仍有业务拥有者时可以维持空闲 Session; 流失效后按退避重新认证, 明确 SECRET 错误/APIKEY 撤销不无限重登. 正常关闭先停止业务、有界尽力注销, 最后取消 Session, 单对象关闭不影响其他对象的共享会话. 停止接纳新业务后, 只允许已有 UUID 的内部 Remove 进入保留的清理额度, 不被普通接纳关闭门误拦, 也不重新创建注册; 无可用会话/连接时可直接交由 TTL 清理. 凭据和会话 token 不进入日志、URL、错误正文或命令行.

### Client 凭据更新与业务恢复

应用可以在原 Client 更新同一 APIKEY 的 SECRET, 供下一次认证使用, 无需重建业务对象. 配置替换对并发认证呈现完整值, 不修改在途认证所持有的缓冲. 新值不会自动发送到 Star 或改写内部 Almanac, 也不能把已经失效的会话重新标记为有效. 本规则不隐含在原 Client 上切换 APIKEY; 当前 APIKEY 不承担注册所有权.

凭据更新后, 因旧凭据认证失败而暂停的 Client 可以使用新配置恢复认证; 同一目标的业务对象共用该认证过程. 仍有效的会话不因仅更改本地配置而被强制中断. 旧配置发起的迟到认证结果不能覆盖新配置下的会话或把新状态改回失败, 旧会话业务 RPC 的迟到失效错误也不能清除已经建立的新会话. 已显式关闭的 Client 不能被配置更新重新启动. 对已暂停的认证显式提交相同 SECRET 也可重新尝试一次, 用于服务端恢复原凭据; 不因值相同而忽略用户的恢复动作.

同名 APIKEY 删除后重建不使旧 Session 重新有效. 原注册对象可经 Client::secret 恢复认证, 再按来源实例与实际期限处理原 UUID; 新 Client/新对象不自动接管已有 UUID. 这是 SDK 的对象生命周期约束, 不是 APIKEY 所有者校验, 具体见 [凭据生命周期](../../astrolabe/README.md#凭据生命周期).

同一 Star 内重新认证不改变订阅范围或直接清空已接收视图, 订阅继续按最后完整游标恢复. Ephemeris 的有效注册可在认证恢复后继续管理; 认证中断期间 TTL 仍正常经过, 必须恢复注册时保持同一 Beacon 的逻辑 id. 更换 SECRET 不改变旧调用结果; Publisher 只为后续显式 update 恢复连接和版本准备, 不补发已经失败的内容. 切换 Star 或实例变化仍遵守独立的恢复与重注册规则.

服务端因 [凭据快照恢复](../../proto/README.md#credential-snapshot) 统一结束旧 Session 时, 复用上述共享重认证路径, 不为每个业务对象分别登录或仅因此切换 Star. 已确认 Session 的失效不同于首次登录被拒绝: 当前 SECRET 仍有效时自动重认证即可, 无需应用调用 Client::secret; 新登录明确拒绝才暂停. 旧流迟到结束和旧快照的延迟清理不能覆盖新会话, 视图、UUID、操作顺序及 TTL 继续按原恢复规则处理.

<a id="tls"></a>

## 可选 TLS 与证书来源

TLS 与 APIKEY/APISECRET 分别配置. Client 默认启用 TLS, 应用可显式关闭以使用明文 gRPC, 所选方式必须与目标业务监听匹配. 单机部署也使用相同 TLS 配置, 不存在 standalone 自动关闭分支. SDK 不在连接前猜测服务端模式, 不在证书错误或握手失败后自动降级. 自签 CA 和正规 CA 都保留服务端证书链及主机名验证, 不通过关闭验证支持自签证书.

Star 的默认值、材料缺失处理和内部链路边界统一见 [运行模式](../../docs/build.md#运行模式与接线), SDK 不复制服务端配置矩阵.

各平台统一由 CMake 生成私有字节列表, 可选内嵌 CA 公钥证书, 支持外部证书文件覆盖, 不使用 `#embed`. 此处“公钥”指 TLS 可用的 X.509 CA 证书/信任束, 不是节点 Ed25519 准入公钥, 也不是裸公钥固定. 首版不因此增加双向 TLS 或客户端私钥.

- CMake 通过 `COMET_CA_FILE` 指定公开证书文件, 使用内置十六进制读取生成私有字节列表, 无额外工具依赖, 证书内容变化触发重新配置. 嵌入长度显式传递给 TLS 接口, 不假设文件自带 NUL 结尾.
- 运行时显式外部文件优先于内嵌默认值. 显式文件缺失或无效时直接报错, 不静默换用另一份信任. 没有外部文件与内嵌证书时, 可使用目标平台可用的默认 CA 信任源, 不声称所有平台自动读取相同证书库.
- 每个项目可以嵌入自己的自签 CA 或选择外部信任束, 也可使用正规 CA. Star 部署匹配的服务端证书和私钥, 始终验证证书链与目标主机名.
- Star 服务端私钥、CA 签发私钥和 APISECRET 均不编入 SDK. 可选嵌入路径为空时不生成或引用证书字节文件, 支持无内嵌 CA 的外部加载模式.

TLS 和业务 Session 的协议关系见 [业务会话](../../proto/README.md#session); Comet 的证书配置不更改管理或节点信任.

## 回调与生命周期

watch/changed 在完整状态发布、解除内部状态锁之后调用用户代码. 同一个读取对象的内容通知有序, 可合并未交付的中间状态; 状态和不同对象的通知可能并发. 通知使用拥有存储的值, 不借用马上回收的网络消息, 不依赖回调完成才返回写入结果.

通知必须快速且非阻塞, 不在 SDK I/O 回调中执行同步 RPC、等待本地清理或持锁等待另一通知. 应用需要写入或耗时处理时投递到自身有界执行环境. tick 是单独的采样任务, 其业务回调和随后同步提交由 SDK 有界工作资源推进, 不把等待放在共享 gRPC reaction 上; 不承诺任意慢采样不影响该 Beacon 自身任务.

回调可立即 stop/destroy, SDK 捕获 C++ 回调边界异常并有界报告, 不重放回调、不回滚已提交数据. 用户提供的上下文须覆盖已开始回调的寿命. 逻辑关闭和资源完全排空是两个边界, 取消不等于 gRPC OnDone; 内部清理不得自等或保留业务引用环.

Beacon 工厂要等首次 RPC 确认, 其余角色工厂完成本地校验后可开始后台同步. 应用之后挂接 watch/changed 时必须能取得当前完整基线/当前状态, 不能因同步早于挂接而永远漏掉首次通知. stop/destroy 已生效后不再启动回调. 回调使用 move_only_function; 不引入公开 Task/Executor 或跨语言包装. tick 返回 Value, 空指针是输入错误, 指向空 vector 是合法更新; tick(interval, {}) 解除采样, beat 保持启用.

<a id="subscription"></a>

## 订阅与不可变视图

Reader/Subscriber 在完整安装后交付拥有式 Map 或精确 Key/optional<Value>, state/changed 返回含状态及数据的 View. Observer 的 Pool/Item 用于本地选择及受控估计; 旧式 load/watch/select 不可变 View 入口继续可用.

### Subscriber 与 Reader

Catalog 支持 `client.subscriber(sector, spectrum)` 和 `client.subscriber(sector, spectrum, key)`. Almanac 的 `client.reader(...)` 使用同形接口:

| 入口 | 回调含义 |
| --- | --- |
| 全 Scope: `watch(callback(Map<Key, Data>))` | 每次给出已完整安装的逻辑 Map |
| 精确 Key: `watch(callback(Key, std::optional<Data>))` | 有值表示存在, nullopt 表示完整基线确认不存在或后续删除 |
| `state()` / `changed(callback)` | 查询/接收就绪、陈旧、恢复、失败、关闭状态及原因 |
| `stop()` | 立即停止当前对象, 幂等, 不关闭共享 Client |

首份完整基线交付一次, 包括空 Map 或精确 Key 不存在; 之后在完整变化边界通知. optional 中的空 Data 是合法值, 未同步、断流和解码失败不是 nullopt. 应用可以自行缓存已交付值. 全量 Map 是逻辑接口, 网络仍使用快照和增量, 不要求每次传输整表或深拷贝全 Map.

多页或原子多键批次必须完整准备再安装和通知, 不暴露半页/半批. 共享快照可以降低复制, 但任何已经交付的数据不得被后续网络更新或对象关闭改写/释放. 不要求应用遵循服务端 Index::View 的读完成协议, 也不将生成类型或私有页结构暴露为公共 ABI.

### Observer 的短期可变选择视图

Observer 仅作用于 Ephemeris: `observer = client.observer(sector, spectrum)`, 初版只提供 `one(selector)` 与 `stop()`. 内部持续 Watch, one 从本地池选择, 不逐次发 RPC. 未就绪/停止、没有可用项、选中一个视图分别表达; 暂不增加 watch/change 或统一的权重接口.

选中视图包含完整注册和业务 Data, 允许业务为短期选择调整 Data 中的估计字段, 影响后续 one. 它不是长期自动更新的远端对象, 不产生写回 Star 的操作. 同一 id 的新权威 Data 安装后覆盖本地估计, 较旧视图的迟到修改不能覆盖新的数据代次. 引用保留可以保证内存安全, 不保证永远属于当前可选择池.

Selector 负责筛选、禁止使用、优先命中、打分/权重和本次选择后的本地调整, Data 可提供相应业务函数. 不固定只取最小权重, 不硬编码“加 M”或“填到 500”, 不让 SDK 凭空定义通用负载单位. 高请求量可选抽样/P2C、分桶或索引; 不强制每次全池遍历, 也不声称任意 selector 都是 O(1).

选择与本地调整需要受控的并发边界, 同时保证网络新版本优先. 不能把任意长用户逻辑放在整个池的写锁下, 也不能返回裸可写指针后失去代次校验. Observer::Pool 为固定快照, selector 返回 optional<Observer::Item>; one 返回 Result<optional<Item>>. Item::record() 提供完整 Attr/Data, Item::update(Value) 以权威值身份和上次本地值作比较交换. 不同 Item 副本可并发竞争, 失败分别返回 obsolete 或 conflict; 同一个 Item 实例由调用方串行修改. stop 后旧 Item 仍可读, 不能再写入本地池.

stop 只结束此 Observer, 后续 one 明确报已停止. 已取得视图的存储仍有效, 但不能继续修改已停止或已替换的池.

### 内部订阅复用

Reader、Subscriber 和 Observer 共用私有 Watch 核心: 有界暂存、complete 安装、旧流隔离、取消、认证与退避. Observer 另外维护短期本地选择状态, 不修改共享权威快照. 三域的版本及恢复策略独立, 不因代码复用而合并.

Ephemeris data 增量只替换已有完整记录的 Data 并保留 Attr; 缺少 Attr 不能创建半条注册, 按 [协议](../../proto/README.md#registry-delta) 有限重置. 纯续租不重复交付未变内容. 同逻辑 id 的来源去重由 Star 投影负责, SDK 不直接合并来源 stream.

### 断线与恢复

同步未完成时是未就绪, 断线后保留最后完整数据为陈旧, 新快照独立构建, 失败不混合新旧状态. Catalog/Observer 切换 Star 后接受新目标的完整投影, 允许暂时缺项或较低 Data 版本; 不从旧本地视图拼补缺项. 旧流迟到帧不得进入新目标.

Almanac Reader 保留同一对象、同一权威范围已见版本下限. 已见 v100 后转到只有 v90 的 Star, 继续保留陈旧 v100 并等待目标达到至少 v100, 不安装 v90; 新建 Reader 不自动继承别的对象下限. 这不保证随时得到 Polaris 全局最新版本, Almanac 没有 Catalog TTL 或自动到期.

回调通知可以合并中间状态, 不是必达事件日志. 永久格式/范围错误、无法容纳完整视图的本地容量错误停止自动下载并明确报告, 不无限重试同一坏快照. 应用持有旧值的内存不能由 SDK 强制回收, 也不会因此维持订阅或 Client.

### 错误后的恢复入口

下表描述当前 SDK 的恢复规则. 自动 Beacon 恢复与显式业务写入分别持有结果和截止.

| 观察结果 | SDK 动作 |
| --- | --- |
| 初次 Beacon 注册失败 | 工厂返回错误, 外部决定是否重新注册 |
| 显式 Update/Publish 失败或不确定 | 返回一次结果, 不后台补发; Beacon 保留此前已确认缓存 |
| Catalog 明确版本冲突且整批未提交 | 原调用内查询同目标并修复一次, 不超原 deadline |
| 成功 Beacon 的 Star 切换/注册失效 | 有界恢复同 id 注册, 报告状态, 不重放失败 update |
| 旧 Session 失效 | Client 合并重认证; 不改旧调用结果, 恢复 Watch/Beacon |
| 登录明确拒绝 | 暂停认证, 应用可用 Client::secret 更新同一 APIKEY 的 SECRET |
| Almanac 目标版本暂时落后 | 保留旧视图和版本下限, 有界重连等待 |
| 永久参数/范围/容量/协议错误 | 停止相应自动恢复, 明确原因; 不影响无关角色 |
| 版本/order 耗尽 | 明确失败, 不回绕或新身份掩盖 |

恢复成功和单次写入成功分别记录, retry_ms 不能覆盖总 deadline 或使永久错误自动重试. 业务方法不配置额外 gRPC retry/hedging, 防止与明确的一次修复或注册恢复叠加. 库的透明重试仅遵循“尚未交给服务端应用”的保证, 不提供 exactly-once.

## 多 Star 接入约束

Client 同时选择一个活动 Star. 多地址用于有界切换, 不逐 RPC round-robin 或同时向全部端点创建 Beacon. 地址须稳定路由到指定 Star, 连接重建不等于实例变更, 每次操作仍校验实际实例.

一次业务错误不单独触发整个 Client 切换风暴. 切换由共享核心处理, 原写入不改投, 新写入使用新目标; Watch 清除不适用的实例游标, Almanac 另保留权威版本下限. Beacon 按稳定 id 弱恢复, Catalog 等下一次显式完整更新. 服务端无业务持久化时不承诺全群重启无损.

<a id="rpc"></a>

## 协议适配

Schema、服务及字段号以 [协议](../../proto/README.md#comet) 和 comet.proto 为准. Catalog.Query、Beacon 的 capability/generation/order 恢复、Update 原子延长 TTL 及内部来源合并已接线. Star 与 SDK 需要一起部署; Go 仅同步生成协议, 本次没有实现 Go SDK.

## 本地资源

以下是可配置、未实测的初值, 服务端和编码上限独立见 [协议预算](../../proto/README.md#服务端与编码预算). 不通过 Limits/Inspect 动态协商, 本地预检查不代替服务端拒绝.

| 项目 | 初值 | 计量与处理 |
| --- | --- | --- |
| Client 同时接纳的显式 unary | 256 | 包含连接/认证等待、版本准备及在途, 不设 Publisher FIFO; 超限立即拒绝, 自动续租与清理保留独立额度 |
| Client 自动恢复 / 续租 / 清理在途 | 合计 64, 其中至少 16 留给续租/清理 | Beacon 注册恢复不能占满保留额度; 等待项合并在各对象状态中, 不无限创建 RPC |
| Client 订阅对象 / 实际 Watch RPC | 各 64 | 空范围也占用对象额度, RPC 含等待/取消中但尚未完成的旧流; 超限不暗中创建更多连接 |
| SDK 单读取视图 / 在建批次 | 各 64 MiB | 安装时另计旧视图仍被持有的存储, 超限不发布部分状态 |
| Client 受控存储总预算 | 256 MiB | 连接/认证等待中的请求、Catalog 单次调用载荷、Ephemeris Attr/Data、在途、当前视图及在建状态合计; 应用额外保留的历史视图不能强制回收 |
| 显式 unary 操作 deadline | 3 秒 | 从接纳起覆盖连接/认证/版本查询/有限冲突修复/RPC, 发送只用剩余预算; 自动续租优先在本地租约预算内发起, 预算耗尽后的探测仍使用有限 RPC deadline, 不将其当成租约延长 |
| Session 首次确认等待 | 3 秒 | Client 本地可取消等待预算, 成功确认后撤销, 不作为长期 Session RPC 的总 deadline |
| 恢复退避 | 初值 100 ms, 上限 5 秒并加抖动 | 成功建立连接不立即清零持续失败的退避; 续租调度受更近的租约期限约束 |

Client 并发与总字节同时约束连接/认证等待及在途请求, 不引入 Publisher FIFO. 业务超限不能耗尽续租、关闭和结果结算资源. 取消/本地超时不能提前退还仍由 gRPC 持有的在途额度和字节, 实际完成回调后才能回收; 必要的新尝试仍受同一总额度限制, 不用不断替换“当前尝试”隐藏旧请求积压. 共享载荷按实际拥有的存储计量, 不只算 shared_ptr 大小; 用户保留旧视图与 gRPC/TLS/分配器开销不被内部配额强制限制.

视图与在建预算包括容器、Key、解码载荷和准备峰值. 本地快照/投影本身超限时取消该流, 清理暂存, 保留旧完整视图为陈旧且不推进位置, 报告本地容量错误. 不自动反复下载同一份过大快照; 应用调整预算或缩小范围后显式重建订阅. HTTP/2 背压不能替代此累计内存上限.

### 传输与调度边界

同一活动 Star 初始使用有界的长期流与 unary 两类传输: Session/Watch 使用流式 Channel, Publish/Create/Update/Renew/Remove 使用 unary Channel. 两者共享 Client、活动实例及 Session 凭据, 不再认证两次, 也不要求唯一 TCP. 需要确保底层连接池实际隔离, 仅 new 两个指向同一目标的 Channel 不足以证明这一点; 配置采用当前 gRPC 支持的独立 subchannel pool, 不让长期流填满同一连接后阻塞续租. 首版不建立每订阅连接或无限扩大的 Channel 池, 也不将这个分离声称为严格优先级保证. 参见 [gRPC 并发流与 Channel 说明](https://grpc.io/docs/guides/performance/).

长期流数量与视图字节分别限额, 空分组或空精确订阅也消耗 HTTP/2 流、服务端观察项和本地对象. Session 不消耗 Watch 业务额度; 重认证先暂停新 Watch, 取消依赖旧会话的流, 在旧 Session 实际结束并有传输容量后发起新登录, 再恢复 Watch, 防止登录被自己的待恢复订阅堵住. 这些计数不能预约对端的 HTTP/2 流名额; 传输排队仍服从本地确认超时, 不把应用保留额度声称为物理优先级.

外部 Keepalive 的两端约定见 [协议](../../proto/README.md#外部链路保活). 长期流/unary 分离只处理流名额互相挤占, 普通 unary 的 CPU/网络负载仍可能拖慢 Renew; 保留额度及按期限调度不构成延迟保证, 必须在并发 Publish/Update 压力下验收, 不先加第三类 Channel 或每对象连接.

所有流的接纳、等待和在途仍计入本地预算. 每条流最多一个在途读和一个在途写, 复用消息对象须等对应完成后再清空或改写. Session 首次成功确认后继续读取结束, 不因不期待第二条业务消息就停止接收流终止; 非法第二次确认按协议错误处理. 回调可能并发, 取消不代表 OnDone 已到达, 资源释放使用真实完成边界; 参见 [gRPC Callback 规则](https://grpc.io/docs/languages/cpp/best_practices/).

自动任务共享有界定时/调度资源, 不为每个 Publisher、Beacon 或 Scope 启动线程. 同一对象的版本分配与实际提交有明确顺序, 不用“最新待发值”覆盖另一同步调用. Catalog 没有自动发布或续租任务; Beacon 的注册恢复、tick 与 beat 复用有界调度, 恢复与当前注册的操作隔离, 已取消的旧尝试仍按真实寿命计费. tick 的业务采样及同步 RPC 等待不得占用共享 gRPC reaction 线程; 每个启用 tick 的 Client 延迟创建 2 个采样线程, 共享 64 个候选槽; 满额仅推迟下一次采样, 不积累历史. 关闭后排空任务并回收线程, Client::wait 完成线程回收; 不引入公开 Task/Executor 协议. 最后拥有者释放仅发起关闭, 内部线程不得在自己的回调上 join 或访问已释放状态; 应用卸载库前须确保本地清理结束.


## C++ 公共接口

本节概述公共接口, 具体类型与重载见 include/comet. 所有 Scope 均由两个字符串组成.

以下用伪代码概述调用形状, C++ 实际传入 Scope{sector, spectrum}; batch 使用 vector<Publisher::Entry>, selector 接受 const Observer::Pool& 并返回 optional<Observer::Item>. 版本、注册代次和传输元数据由 SDK 管理, 普通业务调用不传 version.

```text
beacon = client.beacon(sector, spectrum, attr, data, ttl, beat) // 同步 expected<Beacon, Error>
beacon.update(data)                                         // 同步结果
beacon.tick(interval, callbackReturningData)                 // 可选, interval > 0
beacon.state()
beacon.changed(callback)
beacon.destroy()                                            // 本地立即关闭

observer = client.observer(sector, spectrum)
view = observer.one(selector)
observer.stop()

pub = client.publisher(sector, spectrum)
pub.update(batch, ttl)                                      // 同步, 单键/多键完整值

sub = client.subscriber(sector, spectrum)
sub.watch(callback(Map<Key, Data>))
exact = client.subscriber(sector, spectrum, key)
exact.watch(callback(Key, optional<Data>))
sub.state() / sub.changed(callback) / sub.stop()

reader = client.reader(sector, spectrum[, key])              // 与 subscriber 同形
reader.watch(callback)
reader.state() / reader.changed(callback) / reader.stop()
```

下面片段在持有 client 的应用作用域内使用实际 C++ 类型. 示例保留 Beacon/Observer/Subscriber 句柄直到业务结束; 所有 Result 都应由应用处理错误.

```cpp
using namespace std::chrono_literals;
auto beacon = client.beacon({"services", "main"}, {1}, {2}, 30s, 10s);
if (!beacon) {
    return std::unexpected(beacon.error());
}
auto updated = beacon->update(std::vector<std::uint8_t>{3});
if (!updated) {
    return std::unexpected(updated.error());
}
auto sampled = beacon->tick(1s, []() -> comet::Value {
    return std::make_shared<const std::vector<std::uint8_t>>(1, 4);
});
if (!sampled) {
    return std::unexpected(sampled.error());
}

auto observer = client.observer({"services", "main"});
if (!observer) {
    return std::unexpected(observer.error());
}
const auto id = beacon->state().identity->uuid;
auto selected = observer->one([&id](const comet::Observer::Pool& pool) {
    return pool.find(id);
}); // 尚未收到完整基线时返回 busy 或当前恢复错误, 业务可以在随后再次选择.
if (selected && *selected) {
    auto adjusted = (**selected).update(std::vector<std::uint8_t>{5});
    if (!adjusted) {
        return std::unexpected(adjusted.error());
    }
}
```

Observer 的本地调整不改变 Beacon/Star 的 Data. Reader/Subscriber 的 `watch` 回调可以保存传入的 Map/Value; 解除回调时用对应的空 move_only_function, 精确和全 Scope 重载不能互换. destroy/stop 为非阻塞关闭, 应用结束时用 Client::close 和 Client::wait 排空本地资源.

Client::open、Client::secret 与显式 Client::close 继续承担配置、凭据和共享关闭. Publisher 的句柄释放/共享 Client 关闭负责结束本地资源, 不附带远端删除; 不扩展业务 update 为任意任务框架. C++23 使用 expected/optional 和标准所有权类型, 业务错误不抛异常作为正常控制流.

### 本地结果与诊断

Error 必须包含具体原因和提交确定性, 区分原请求目标与响应声称的实例. 成功回执须匹配本次目标、版本/order、身份及 TTL 契约; 不合法回执按协议错误处理, 已发送请求仍可能提交. 内部诊断可以记录版本, 不要求应用理解或生成它们.

Beacon 状态区分就绪、恢复、租约不确定和关闭; Subscriber/Reader 状态区分未就绪、可用、陈旧和错误. 状态快照不是远端实时查询. Catalog Key 版本、Almanac 权威版本、Ephemeris Data order 和 Star Watch 游标不能互换, 不建立编号映射字典.

### C++ / Go / Rust 的实现边界

- C++23: expected 表达同步结果, optional 表达精确 Key 的存在性, 共享不可变存储供回调保留; Observer 需要带代次的受控可变视图, 不能裸引用逃逸到后台修改.
- Go: 同步返回值/error, 有界 RPC 使用 Context; map/[]byte 的可写别名需要复制或拥有权转移. 精确 Key 用明确存在性区分空值和删除, stop/destroy 立即逻辑停止, goroutine 清理在内部完成, 不依赖终结器.
- Rust: Result/Option、拥有载荷和 Arc 等承载相同语义; 需要 Send/Sync 的回调在所属边界声明. 同步 façade 不能阻塞内部异步 I/O 执行线程, Drop 仅作清理兜底.

三种语言共用业务契约, 不强行统一容器 ABI 或运行时. Data 由应用保持完整, 不另加通用权重服务. 这里只评估可实现性, 不恢复已废弃的 Rust 项目, Go SDK 也未因此实现.

## 静态库与 CMake 交付契约

已确认首版只交付原生 C++ 静态库, 提供源码树接入和安装后的 CMake 包, 不同时维护共享库、C ABI 或其他语言绑定. [CMakeLists.txt](CMakeLists.txt) 已加入下列目标和安装定义, 实际构建、安装及独立消费结果见 [验证记录](../../docs/validation.md).

| 接入项 | 首版约定 |
| --- | --- |
| 库与目标 | 静态库名 `comet`, 应用链接 `comet::comet`; 不要求应用直接链接 Astra 的服务端目标 |
| 源码树接入 | `add_subdirectory` 接入 `comet/cpp/`, 复用同一依赖解析规则; 单独构建 SDK 不连带生成 Star/Pulsar/Astrolabe 可执行程序 |
| 安装后接入 | `find_package(Comet CONFIG REQUIRED)` 取得同名目标; 包含公共头文件、静态归档、CMake 导出和许可证, 不引用构建机器的源码/缓存绝对路径 |
| 公开依赖 | 公共头文件只暴露 Comet 类型及所需标准库类型, 不包含 gRPC/Protobuf 生成头; 消费者最低要求 C++23, CMake 导出 `cxx_std_23`; 当前本机 MSVC/CMake 将其映射为 `/std:c++latest` |
| 链接依赖 | 导出目标传递静态链接实际需要的 gRPC/Protobuf 等依赖; “不暴露生成类型”不等于最终链接不需要第三方库, 不把排列归档顺序留给应用 |
| 协议生成 | 沿用[统一生成规则](../../proto/README.md#generation): 生成源码纳入源码版本管理, 仅显式生成/检查时使用匹配工具, 普通库构建不运行 protoc; 安装包使用者无需生成协议或连接 Pulsar |
| 依赖获取 | 复用已批准版本和项目内缓存; 缺少依赖明确失败并说明, 配置/构建/安装不隐式下载, 不写全局包路径或用户配置 |
| TLS 材料 | 可选编译嵌入公开 CA 证书, 外部文件覆盖遵循既定规则; 安装包和生成头不含私钥或 APISECRET, 不因静态链接承诺无运行时依赖 |

静态库接入仍要求编译器、标准库、编译选项及依赖 ABI 相容, 安装包检查构建时的平台与编译器版本, 不允许将 Linux 归档直接用于 Windows. Windows/MSVC 与 Linux 的实际源码及安装包验收范围以 [验证记录](../../docs/validation.md) 为准; 不据此承诺跨编译器二进制兼容. Windows 暂不启用 Linux 专用探针或 Sanitizer. 已安装包只分发自身拥有的文件; 第三方依赖由导出的 CMake 依赖查找取得, 若以后要发布包含依赖的完整二进制包, 另行定义其平台和许可证清单.

构建脚本按使用者实际选择启用测试与示例, 普通库构建和包接入不暗中运行测试. 独立消费用例分别检查源码树接入与安装包接入, 确认只链接 `comet::comet` 即可完成最小程序, 并检查缺依赖时不会触发网络下载. 嵌入公开 CA、显式外部 CA 和关闭公共 TLS 的配置也纳入该用例; 真实 TLS 握手由 RPC 用例验证, 不由配置成功推断.

## Windows 独立构建

Windows 适配入口只构建 Comet SDK, 不通过 `tools/build.py` 构建服务端. 当前选择 Visual Studio 18/MSVC 19.51、x64 Release 和 `/MD` 运行库; 依赖也必须使用匹配的架构、运行库和构建配置. SDK 和生成协议均要求 C++23, 不继承父级服务器工程的 C++26 设置. 当前本机 MSVC/CMake 将 `cxx_std_23` 映射到 `/std:c++latest`, 固定版本 gRPC 另需下述兼容入口. 工具链版本门槛暂时保留, 更早编译器的支持需独立验证. 实际通过边界以[最新验证记录](../../docs/validation.md)为准.

Windows 依赖需要经过 ABI 配套验证. 当前通过组合使用锁定的 gRPC 1.84.0、Protobuf 36.1.0 及配套依赖, C++ 依赖统一配置 `CMAKE_CXX_STANDARD=23`, 架构与运行库为 x64 Release `/MD`. SDK 的生成协议目标显式使用与手写代码相同的模式, 避免 Protobuf 全局类型随标准模式变化导致链接失败. 切换依赖标准时使用新的构建目录或重新生成能力检测缓存, 避免 Abseil 的旧检测结果留在安装头文件中. `build/deps/comet-msvc/install` 已用于本机接入验证, 不代表 Debug、其他工具链或目标机器均已验证.

构建兼容入口 [GrpcMSVC.cmake](cmake/GrpcMSVC.cmake) 已通过原失败文件编译、gRPC 重建及 SDK 接入验证. 配置 Windows gRPC 依赖时追加 `-DCMAKE_PROJECT_grpc_INCLUDE=<Astra 根目录>/comet/cpp/cmake/GrpcMSVC.cmake`. 入口仅为 `fused_filters.cc` 定义上游开关 `GRPC_NO_FILTER_FUSION=1`, 不改依赖源码、标准模式或其他库; SDK 构建自身添加此开关无法修复已经构建的依赖. 此开关跳过可选融合过滤器注册, 普通认证、压缩和消息大小过滤器链保留; `fuse_filters` 在固定版本中默认关闭, SDK 未主动开启. 当前构建不能再选择融合路径, 以后启用须移除入口并重新验证. 本轮未测量性能, 不据默认配置推断性能差值.

租约时钟在 Windows 使用 `QueryInterruptTimePrecise`, 保留包含系统挂起时间的计时语义; 运行系统要求 Windows 10/Windows Server 2016 或以上, 参见 [Microsoft API 文档](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryinterrupttimeprecise)和[时钟语义](https://learn.microsoft.com/en-us/windows/win32/sysinfo/interrupt-time). 不通过墙钟或调整系统计时器精度实现租约预算.

以下命令说明独立构建入口的用法, 从仓库根目录执行; 前提是已获准并准备好 ABI 匹配的 Windows 依赖. 命令本身不会获取依赖, 安装 SDK 也不写入系统目录. 实际验收另使用 `tests/consumer` 检查源码树接入和安装包接入.

```powershell
cmake -S comet/cpp -B build/comet-msvc/sdk -G "Visual Studio 18 2026" -A x64 "-DCMAKE_PREFIX_PATH=$PWD/build/deps/comet-msvc/install" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
cmake --build build/comet-msvc/sdk --config Release --parallel 4
cmake --install build/comet-msvc/sdk --config Release --prefix "$PWD/build/comet-msvc/install" --component Comet
```

应用使用 `find_package(Comet CONFIG REQUIRED)` 和 `target_link_libraries(app PRIVATE comet::comet)`, 并把 SDK 安装目录及上述依赖安装目录加入 `CMAKE_PREFIX_PATH`. 不把 SDK 私有头文件或 gRPC 生成头复制进应用. 可选的 `COMET_CA_FILE` 在所有平台使用 CMake 内置字节转换, 不要求编译器支持 `#embed`. 普通构建不运行测试; `COMET_BUILD_TESTS=ON` 以及 CTest 执行仍需当轮明确测试授权.

MSVC SDK 自身采用 `/W4 /WX`, 针对 gRPC 1.84 在 MSVC STL 中实例化旧 TLS 类型产生的 C4996 设有私有例外; SDK 使用的是新的 `InMemoryCertificateProvider`, 此诊断例外不传播给应用. Windows 的探针和 Sanitizer 暂不在支持范围内.

## 验收准则

- 验证同步结果、未知提交、一次版本修复和“返回失败后不补发”, 不拿旧 future/期望值恢复用例直接证明新接口正确.
- 覆盖 Beacon 首次失败、成功后的同 id 恢复、tick/beat 竞争、Update 延期、缓存只保留已确认值及立即 destroy.
- 验证 Observer 选择和本地调整被新权威 Data 覆盖, Subscriber/Reader 的 Map/optional、空基线、完整批次和状态通知.
- 标准多节点模型至少三台 Star, 每台均有本地写入, 覆盖交叉订阅和双向复制. 定向单/双节点用例不替代系统验收.
- 认证/TLS、预算、旧回调隔离、C++23 静态包与跨语言所有权边界按 [验收规约](../../docs/comet.md) 检查.

新契约的实际验证需当轮明确授权, 结果只维护在 [validation.md](../../docs/validation.md).
