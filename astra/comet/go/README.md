# Comet Go

新原生 Go SDK 预留目录为 comet/go, 与 [Comet C++](../cpp/README.md) 共用 [proto.comet.v1 业务契约](../../proto/README.md#comet). 当前只有设计入口, 没有可调用 SDK、go.mod 或测试通过记录; 不复用冻结的 Redis SDK 作为新协议实现.

## 实施范围

Go SDK 后续接入 Almanac、Ephemeris 与 Catalog, 业务接口已与 [C++ SDK 契约](../cpp/README.md#c-公共接口) 一同确认, Go 命名、具体类型和包组织仍待实施. [Astrolabe](../../astrolabe/README.md) 的必要管理写入直接调用 Polaris, 不再要求先实现 Go SDK、独立内容源或直接发布到 Star. 未来若通过 Go SDK 展示普通业务数据, 仍遵循完整视图、恢复、认证和所有权规则, 不放开内部 __ 范围.

协议字段、版本、错误和服务端受理只在 proto/README.md 定义. Go 使用原生 Context、错误、所有权与并发机制, 不暴露 gRPC 生成类型 或通过 C ABI 包装 C++ SDK, 也不单独规定一套同步版本.

## 公共接口映射

| 角色 | 已确认的业务语义 |
| --- | --- |
| Client | 多角色共享活动 Star、会话与资源, 单角色停止不关闭 Client |
| Beacon | 同步工厂返回句柄/error, update 同步; 固定 Attr/TTL/beat, 可选 tick 回调只返回 Data, state/changed, destroy 立即关闭 |
| Observer | Ephemeris 本地池的 one(selector)/stop, 不提供 watch/change |
| Publisher | Scope 级 update(batch, ttl), 同步原子多键提交, 内部版本, 无自动续租/内容补发 |
| Subscriber | Scope watch 完整 Map; 精确 Key watch 明确存在性; state/changed/stop |
| Reader | 同 Subscriber, 但保留 Almanac 已见权威版本下限 |

Context 约束单次 RPC 总截止, 包括版本查询及允许的一次冲突修复; 同步失败后不保存请求供自动重发. 只有 Beacon 保存最近成功确认的 Data, 成功对象切换 Star 后恢复同一逻辑 id; 初次工厂失败由外部决定重试. Go 的返回值/error 可直接承载这些语义, 不需要 C++ 绑定或另一个异步公共提交模型.

精确 Key 的回调须明确存在性, 不能仅凭 nil/空 byte slice 将删除和合法空 Data 混淆. 全 Scope Map 回调是完整状态, 网络仍增量同步; 应用要缓存时必须获得可安全保留的数据. Observer 的可变 Data 用于短期业务估计, 不上传, 新权威 Data 优先; 用拥有值/受控修改和数据代次保护实现, 不让裸共享 map 写入绕过覆盖规则.

## 同步与资源边界

- 首次同步取得快照, 后续通过 Watch 推送增量; 初次未完成与就绪空集合必须区分, 不为“同步拉取”新增全量轮询 RPC.
- 同一完整视图携带 instance/Scope/target/version 和就绪状态. 分页未 complete 不替换可见基线; 断线保留旧视图并标记陈旧, 实例切换遵循协议 reset, 不把旧版本当作新 Star 的恢复位置.
- Go 的 map 和 []byte 不因返回给调用方就成为只读. 公共读取接口须保证调用方不能修改已安装状态或 SDK 在途载荷, 使用受控查询/迭代、拥有副本或明确的借用契约, 不暴露共享可变 map 或用约定伪装不可变.
- Client 共享连接与认证, 各订阅独立取消和恢复. 已确认 Session 失效时合并重认证, 包括服务端 [凭据快照恢复](../../proto/README.md#credential-snapshot), 不误当成首次登录被拒绝或重置业务身份/TTL. 在途流、视图、缓冲和重连工作有界; stop/destroy 立即逻辑关闭并取消 Context, 内部继续持有必要状态直到 goroutine 及真实在途引用退出, 不在自身回调同步等待, 不把发出取消当作已回收.
- Almanac Reader 保留已观察到的分组权威版本下限, Catalog Subscriber 切换后接受新 Star 的完整视图及暂时较低的 Key 版本; 两类策略不混用, 也不让 SDK 参与对等来源对账. 业务持久化和 Polaris 管理提交不下沉为 Go SDK 的内容适配器或数据库事务.
- 普通 Comet 业务连接继续遵循目标的业务 TLS/认证配置, Astrolabe 的管理凭据不自动成为 Comet 会话. __ Sector 始终拒绝, 内部 Almanac 的管理读取不通过放宽 Go SDK 获得.

## 生成与验证

Comet 的 Go 生成源码须处于独立协议包, 不与 Orbit 通用消息名混在同一包. 公共 Go API、模块路径、生成目标和依赖版本在实现前固定; 不为文档先运行生成器、下载依赖或恢复旧 SDK 开发.

跨语言行为与 Go 专属取消/视图所有权的验收见 [Comet 验收](../../docs/comet.md). 文档场景不是已经实现的用例, 构建及测试仍需本轮授权.
