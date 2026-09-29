# 当前代码审核

本页维护仍有效的问题、取舍和后续审查边界. 源码已经实现、存在测试用例和实际运行通过是三个不同结论. 实际日期、输入摘要、失败及清理结果统一见 [验证记录](validation.md); 当前未提交工作区不继承迁移前冻结快照的通过状态.

## C++ SDK 当前整改

Catalog Publisher 已改为绑定 Scope 的同步提交, 内部查询 Key 版本, 不自动续租、不保存失败正文供后台重发. 只有服务端明确整批未提交的版本冲突, 才在同一次调用和 deadline 内查询同一目标并修复一次. 超时未知结果不能盲目重放; 换 Star 后由下一次显式完整 update 提交.

当前已写入实现的生命周期修正:

| 边界 | 当前处理 |
| --- | --- |
| 旧控制轮误取消新请求 | 捕获实际操作的停止源, 旧取消不落到下一次 update |
| 慢 watch/changed 回调拖住同步 RPC 取消 | 共享 stop_source 直接取消; stop_callback 先于 RPC context 析构注销 |
| 切换端点与读取角色退避并发 | 退避读取固定配置, 端点在短锁内冻结 |
| each 回调替换正在遍历的 View | 遍历期间固定旧根, 回调不会释放被遍历容器 |
| 重复启动和极端等待 | Alarm 不重复 Set, 等待使用饱和截止计算 |
| 服务端缺少新 RPC | UNIMPLEMENTED 表示协议不兼容, 不驱动反复切换 |
| 回调在执行中解除自己 | 最后一份业务捕获在对象锁外释放, 析构期间仍禁止 wait 等待当前回调或采样自身 |

发布路径只缓存有界的目标/Key 版本基线, 同一次冲突修复复用已编码请求. 这不等于保留待发 Data, 也没有当前吞吐收益的测量结论.

Beacon 同步工厂/update、beat/tick、同逻辑 ID 恢复及 Observer/Reader/Subscriber 新接口已补入源码. 新增能力凭据不进入公开投影, 多来源按 Data/generation/截止选择; 采样使用共享有界工作线程并隔离迟到结果, 本地选择估计以权威身份和旧值作 CAS. Observer 保留单页单条的免去重分配路径, 在完整网络提交时淘汰受影响的本地估计; 不先复制固定页根. 执行结果及未覆盖边界统一见 [验证记录](validation.md), 接口见 [SDK](../comet/cpp/README.md) 与 [协议](../proto/README.md#已确认目标与实现差异).

## 动态域复用边界

Catalog 的内容版本、过期后水位和多来源合并, 与 Ephemeris 的 UUID 归属、固定 Attr、独立 Data/Renew 顺序不同. 差异影响提交、回滚、删除和恢复, 不能仅用两个布尔 Traits 将 State 化为类型别名.

| 共用机制 | 当前实现及保留边界 |
| --- | --- |
| 原生页、候选提交、来源日志 | Pages/Origin/Scene 已共用, 各域选择记录及提交语义 |
| 来源恢复与传输 | Restore、Borrowing、Dispatch、Landing、Exchange::Pipe 共用恢复及预算流程 |
| 公开分页 | [Pagination](../star/src/pagination.hpp) 共用冻结引用、排序/压缩和页预算; 各域保留 Encoding/merge |
| 公开读取 | [Reading](../star/src/reading.hpp) 共用共享快路径、独占推进、释放域锁等待来源及锁外回收 |
| 时钟、范围目录及记账 | 私有 [Context](../star/src/context.hpp) 共用 reading/locate/obtain/allowance/单条 publish; 原 State 仍持有锁、字段、Pending 和业务方法 |
| SDK Watch | Watching 共用网络恢复、取消、回调和计费; 内容安装保留各域候选与版本规则 |
| SDK 写入 | Publishing/Beaming 继续独立; Catalog 同步完整提交与 Beacon 注册/续租/恢复不同 |

Context 不新增拥有对象、继承层或运行时域选择, 不移动状态锁. 取时顺序仍为 gate_ -> timing_, 无效读数不更新 observed_. Catalog 批次和 Ephemeris owners_ 不被通用帮助函数接管. 模板复用不是构建时间、产物大小或性能改善的证据.

## SDK 与 Scope 并发复核

Comet 普通完成事件进入按对象去重的弱引用就绪队列, 只推进就绪对象. 到期、共享绑定变化、关闭及容量归还仍可能遍历目录. drain 是 O(K), 不是绝对 O(1); 遍历持有的强引用在锁外释放, 处理期间新事件可再次入队.

Star 自有 Origin 用独立 export_ 锁导出, 不触发 GC; 本地发布保持 gate_ -> export_ 顺序. 公开读取在下一整拍前走共享路径, 越过维护边界才独占推进到期. Scene 不保存第二份 TTL, 不能只在 find 中过滤期限而让快照/后缀对同一版本呈现不同内容.

远端原生候选由来源锁保护并在域锁外准备, 最终水位、投影、期限与预算仍在域锁内提交. 忙来源等待释放域锁, 重新取时后再推进. 完整来源恢复允许各 Scope 先后安装, 每 Scope 候选完整提交, 整个来源仅在完成后 ACK; 中途恢复不能清掉其他 Scope 的 coverage.

下行 StartWrite 已移出服务索引锁, 单流 io 与最终认证 Permit 仍覆盖发送. pop 返回强引用, 索引增删由同一锁同步; 普通 bool 不提供无锁保证. Watch 在途链减少扫描集合, sweep 只将真正到期的 busy 流入队; 周期扫描仍为 O(在途流数).

同 Scope 的批次原子性只对相应提交和完整视图成立. 多个精确 Watch 各有安装时序, 不是联合事务读; Catalog 跨 Star 仍为异步复制. 不把域锁直接改为每 Scope 锁而忽略跨 Scope 来源序列、连续 ACK 和全域预算.

## 弱页缓存与期限索引

[Broadcast](../star/src/broadcast.hpp) 按页保留 weak_ptr. 消费者时间不重叠时会重建页面, 这是寿命取舍, 不是无保护的并发缓存竞态. 缓存共享 Protobuf 对象, 不保证 gRPC 最终线编码也共用.

Downstream 写完成后归还 encoded 计费, Edition held 只描述冻结来源. 直接改 shared_ptr 会在计费已归还后继续持有正文, 被拒绝候选也可能滞留. 强缓存须先设计独立预算、淘汰、拒绝与取消语义; Limits 不能直接解释为 RSS 上限.

当前 rebuilds() 与 star.broadcast.rebuild/rebuilt_bytes 区分首次构造和释放后成功重建. 使用弱引用控制块区分空槽与过期槽, 构造失败不记成功. rebuilt_bytes 是消息线长, 不是堆大小或 CPU 收益. 新增用例的执行边界见验证页.

SDK 期限索引保留为测量候选. [Core](../comet/cpp/src/core.cpp) 使用 steady_clock, [Agenda](../star/src/agenda.hpp) 使用 Unix 时间及 10 ms 拍. 直接移植还涉及无限期限、向上取整、长暂停补拍、跨线程析构摘链和节点寿命. Agenda::next 是下一拍, advance 仍处理逐拍级联和到期节点, 不能承诺整轮 O(1). 先用 directory_items/ready_items/polled_items 证明目录扫描是热点.

## 算法与资源边界

| 建议或疑点 | 当前结论 |
| --- | --- |
| Store retention 为零 | [构造契约](../common/src/store.hpp) 明确零表示不保留增量历史, 与 trim 清空一致. 不改成仅关闭年龄淘汰; capacity 为零同样不保留历史. 后续工作区修正的运行边界仍需单独验收 |
| Scene::Batch 默认移动 | 不等价. owner_ 是回滚责任, 必须 exchange 为 nullptr; 默认移动裸指针可能令旧对象析构撤销有效编辑 |
| Pagination 直接排序 | 保留索引排序和置换环. 先前直接移动 Event 的 GCC Release 构建遇到 maybe-uninitialized 且警告视为错误; 不虚构移动次数减少的实测收益 |
| Pagination 尾部容量 | resize 和移动到 shared_ptr 都不释放实际向量容量, 继续计入持有预算. 真正压缩须考虑额外分配和峰值, 不能只删账 |
| Dispatch 就地编码 | 保留临时 Entry 校验后 Swap. RemoveLast 会保留子消息供复用, 被软预算拒绝项的容量不应隐含留在已准备包里 |
| Scene 残批恢复 | upper_bound 不保证返回批次首项; first->version != first->first 分支拒绝被历史裁剪截断的批次, 包括 since == first->first - 1 |
| Projection 去重桶 | discard 用空容器 swap 释放桶, 成功完成也清理; records * 2 是暂存上界, 不是每次 reset 的必然峰值 |
| Exchange 消息分派 | 使用 body_case 显式 switch, 保留域校验、拒绝和预算语义, 不用反射 |
| Exchange 工作区 | 先 acquire 新预算, clear 只释放旧 bytes_ + workspace_, 再登记新工作区. Guard 在失败/异常退出清理, 成功分页继续持有, 不重复归还 |
| coverage 查找 | 两域将精确定位、范围最大覆盖及容量统计合为一遍, 仍为 O(N). 不额外引入须回滚维护的索引 |
| 名单排序 | map 的 principal 顺序与业务 id 顺序不同, 不能因容器有序就删除排序 |
| 时间轮取整 | 保留商加非零余数, 避免 remaining + width - 1 的无符号溢出 |
| 位图与大端编码 | countr_zero 定位与 bits &= bits - 1 消费互补. 固定八字节编码保留可移植实现, byteswap 不是无条件替换 |

Runtime shutdown 的 server_ 判空属于防御性加固, 不把未来可能路径称为已复现崩溃. 资源守卫的价值是即时归还及异常边界明确, 不能把原本在断流析构时归还的预算描述为已证实永久泄漏.

## 身份与时钟限制

Pulsar principal 绑定账号、Galaxy 和端点. 地址变动使用新成员槽位, 旧条目不会自动回收; 达限明确拒绝. 一个账号可部署多个节点, 不能按账号合并或删除旧成员. 动态地址迁移需要稳定部署标识及退役/旧代次拒绝契约, 不通过清库绕过.

正式长测曾在新 Star 首次可信时钟处失败, 该结果及原始诊断保留在 [验证记录](validation.md). 已校准实例继续计时与新实例取得首次锚点是不同条件. 内核 maxerror 是保守误差上界, 不能当作实测墙钟漂移, 也不能用 esterror 或重复读取旧观测代替新校准.

clock 功能探针提供质量拒绝、四时间戳与模型校正数据, 尚不代表故障原因已经消除. 不自动修改 Chrony、系统时间或质量门槛; 只有所属进程全部退出后才解析探针, 方法见 [时钟诊断](profile.md#时钟偏移定位).

<a id="performance"></a>

## 优化分析的证据边界

剩余候选按实际负载归因, 不一次替换多个核心机制:

| 范围 | 需要先测量的问题 | 保留的不变量 |
| --- | --- | --- |
| SDK | 目录/就绪扫描、回调排队、版本查询、消息编码与工作空间分配 | 取消及时、旧完成隔离、失败不重放、预算与对象寿命 |
| Star 下行 | 精确/全 Scope 高扇出、pending 收集、进度通知、分页重建、索引锁等待 | 连续游标、原子安装、每流背压、慢消费者有界 |
| 动态域 | 来源候选准备、最终域锁持有、TTL 大步推进、大来源恢复峰值 | 来源连续 ACK、水位与归属正确、旧视图稳定、回滚完整 |
| RPC/复制 | prepare 与 StartWrite 同步耗时、包大小、在途等待、批量覆盖查找 | 确认只覆盖完整前缀, 不把多个提交的运输装包当作事务 |
| Polaris | WAL/检查点、读池、快照缓存、交错 Scope 发送、历史前缀裁剪 | 持久成功才确认, 每 Scope +1 次序, 不降低 FULL 耐久性 |
| Astrolabe | 大目录和慢节点下的采样新鲜度、工作者复用与退避 | 有界并发、拒绝旧实例结果、陈旧/未知显式表示 |
| 构建与容量 | 暂缓 Planet 的默认目标、静态库依赖、分配与缓存工作集 | 不将链接声明等同于实际产物内容, 不隐式升级依赖 |

Astrolabe 已取消独立 64 节点上限, 但四工作者和两秒请求截止意味着全慢一轮可达 ceil(N/4) × 2s; 五秒 ticker 不保证全目录五秒新鲜度. Star 全互联仍有 n(n-1)/2 条逻辑流和来源扇出, 共享页不消除网络复制.

Bidi 写入、Arena/PMR、共享强缓存、RCU/Actor、容器替换、LTO/PGO 均保留为需要独立证据的候选. Unary 已有明确失败与取消契约; 流化须增加关联、背压和恢复语义. atomic shared_ptr 不保证无锁, Arena 不保证所有字段零分配, ByteBuffer 不保证端到端零复制.

比较应固定源码、三 Star 同时本地写入、总资源与应用负载, 同时报告吞吐、尾延迟、CPU、RSS、FD/线程、复制和控制面成本. 同 VM 短窗口不外推跨物理机容量, 旧接口样本不用于宣称新接口收益. 数据及原始证据只在验证页索引.

## 后续验收

已有用例覆盖原生状态、故障注入、RPC、持久库、三 Star 传播及 SDK 生命周期; 用例列表见 [验收规约](comet.md). 当前仍缺系统化的跨机器分区/交叉重连、大来源恢复、真实休眠/时钟突变、断电/磁盘满组合, 以及浏览器/GPU 和长期资源证据.

优先补当前变化的回归, 再按问题选择随机参考模型、畸形分页/序列属性测试和分配剖析. 固定交错不能证明所有并发安全, 没有覆盖率采集就不报告覆盖百分比. 编码以 [C++ 规范](../cpp-coding.md) 为准, 运行范围遵循 [授权约定](../AGENTS.md); 文档整理不启动测试或前置构建.
