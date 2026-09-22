# Astrolabe

Astrolabe 是 [admin/](../../admin/README.md) 的 Go 管理与实时观测后端. 首版提供必要的登录、Almanac 管理和节点状态入口; 完整 Orrery 界面暂缓. [Go 入口](main.go) 连接 HTTP 管理、Polaris 代理与指标抓取, 旧 C++ 占位入口已退出构建. 实施状态见 [进度](../../docs/progress.md), 实际运行和浏览器验收范围见 [验证记录](../../testkit/validation.md).

## 开发语言与数据职责

| 对象 | 归属与边界 |
| --- | --- |
| 管理账号和部署参数 | 部署配置提供一个管理账号及后端接入材料, 不建立 Astrolabe 数据库 |
| Almanac 当前数据和有界历史 | 由 [Polaris](../polaris/README.md) 的 SQLite 唯一持久保存, Astrolabe 通过管理 RPC 读取和提交 |
| APIKEY/APISECRET | 属于内部 Almanac, 经同一 Polaris 提交与分发路径管理, 不另存一份凭据表 |
| 管理登录会话 | 仅存在 Astrolabe 进程内存, 固定期限见 [管理会话](#管理会话) |
| 节点与指标观察 | Pulsar 名单与各服务实际观测分别取样, 内存保存有界的最新状态, 不保存长期时序 |

Astrolabe 不引入 SQLite/PostgreSQL、GORM、文件内容源或 KV 适配层, 不再承担“先保存独立来源, 再直接发布到 Star”的双写流程. Almanac 唯一底稿属于 Polaris; 新 Catalog 的业务持久化仍由业务发布者负责. Astrolabe 不将观测视图回写为权威内容.

首版 Comet C++ 不依赖 Comet Go 完成. 后续 Go SDK 可用于管理界面所需的普通业务视图, 但必要管理写入、内部凭据和 Star 启动恢复直接走内部协议, 不以 Go SDK 或前端完成为前置条件.

## 管理准入

基础设施沿用 Pulsar 准入与内部 TLS, 为 Polaris 和 Astrolabe 增加各自角色. Polaris 接受 Astrolabe 的管理写入; Star 的 Almanac 安装只接受 Polaris. Astrolabe 的观测身份不能绕过 Polaris 直接修改 Star 的 Almanac, 也不自动成为 Comet 会话.

不保留架构级 standalone 或同机免节点认证分支. All-in-One 只是部署 Profile, 使用同一条初始化与准入链路. Comet 的认证/TLS 开关不改变内部管理身份、传输或 `__` Sector 边界.

Astrolabe 登记实际可达的后端端点, 不使用浏览器地址、Vite 端口或静态站点地址冒充服务身份. Polaris/Astrolabe 不进入 Star 全互联、Planet 候选或业务副本数, 浏览器登录也不登记节点. Orbit 定义 Polaris/Astrolabe 明确角色及各消费者的转换, 未知角色不能退化为 Star.

## 管理登录与 Admin 接入

首版管理用户仅验证登录, 不引入账号分级、读写权限、Sector/Spectrum ACL 或逐按钮权限表. 已登录用户具有相同的已开放管理能力; 登录不绕过数据版本、输入校验或资源限制. 浏览器登录与后端基础设施身份分开, 不将 Pulsar 的账号、节点凭证或私钥交给前端.

### 管理账号

已确认使用部署配置提供的一个管理账号, 不建立账号表、在线账号 CRUD、开放注册或 Polaris 管理账号 Almanac. 配置在启动时完整加载和校验; 缺失或非法时不能退化为免登录. 首版修改该账号通过更新部署配置并重启 Astrolabe 生效, 原内存会话随进程结束失效, 不增加热重载撤销协议.

账号包含 username、salt、hash. 密码摘要参数复用 [Pulsar 既定规则](../pulsar/README.md#启动与已有材料), 配置保存摘要而非明文密码, 不自动导入节点 login.json、Pulsar accounts.json 或 Comet APISECRET. Go 使用相同摘要与比较语义, 不通过 C ABI 链接 C++ 身份实现.

登录输入和并发数有界, 密码派生不持有会话表锁, 正常请求只检查内存会话. 浏览器不取得密码摘要或服务接入材料. 登录本身不依赖 Pulsar/Polaris 在线; 某个管理操作是否可用仍取决于该后端的准入和目标状态, 登录成功不能伪装依赖已经就绪.

### 管理会话

已确认仅在当前 Astrolabe 进程内存保存不透明会话, 自登录成功签发起固定有效 8 小时. 活动、状态查询和页面刷新不滑动续期, 不增加 refresh token、JWT 或会话数据库. 到期需重新登录, 显式注销及进程重启使相应会话失效; 浏览器仍持有 Cookie 不能恢复已消失的服务端会话.

凭据由可靠随机源生成并检查活动表冲突, 不采用用户名、时间戳或递增编号. 使用 HttpOnly Cookie 携带, 不向前端返回可存入 localStorage 的会话 token. HTTPS 使用 Secure, Cookie 绑定后端主机且不配置共享 Domain; Path、SameSite 和删除属性使用一致配置. 浏览器 Cookie 寿命不超过服务端剩余会话期限, 服务端仍在每次请求接纳时检查真实截止.

登录、会话总数及清理工作有界. 注销先使内存会话失效, 再清 Cookie, 重复注销保持幂等; 已受理的 Polaris 写入不会因注销、过期或页面关闭而自动回滚. 页面关闭不主动注销其他标签页. 首版不跨 Astrolabe 实例共享会话, 相同账号配置不能让另一实例接受该 token; 部署需固定后端或明确重新登录.

### 同源与跨源部署

首版同时支持浏览器同源和显式配置的跨源部署. Admin 的后端地址与允许的前端 Origin 分开配置; Origin 按规范化后的 scheme/host/port 精确匹配, 不用字符串后缀、任意反射或 * 放行带凭据请求, null Origin 不受信任. 同源代理和跨源直连共用一套登录与业务 API.

跨源请求显式使用 credentials: include. 服务端仅对匹配的 Origin 返回对应 Access-Control-Allow-Origin、Access-Control-Allow-Credentials: true 和 Vary: Origin, 方法与请求头也使用明确名单; 允许来源的错误响应同样携带 CORS 信息. OPTIONS 预检不要求登录 Cookie, 只校验来源、方法和头部, 不执行管理副作用或视为登录成功. 协议约束见 [Fetch 标准](https://fetch.spec.whatwg.org/#cors-protocol-and-credentials).

Cookie 的跨源与跨站不是同一个概念. 同站部署可以使用 SameSite=Lax; 需要跨站 Cookie 时必须显式使用 SameSite=None; Secure, 经 HTTPS 接入. 浏览器仍可能阻止第三方 Cookie, 不能承诺通过 CORS 配置绕过; 应明确报告会话未建立, 部署可改用同站域名或同源代理, 不静默改为 URL token、localStorage 或免登录. 参见 [Cookie 属性](https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Set-Cookie) 与 [浏览器 Cookie 策略](https://developer.mozilla.org/en-US/docs/Web/HTTP/Guides/CORS#third-party_cookies).

修改操作、登录和注销由服务端在产生副作用前校验 Origin, 并要求非简单请求头; 不只依赖浏览器是否允许读取响应. 缺失或不允许的 Origin 拒绝这些浏览器写入请求, GET/HEAD 不修改数据. 来源校验不能替代 Cookie 登录; 代理只能信任显式配置的转发来源, 不从任意 Host/Forwarded 推导可信站点. 具体头名和 HTTP 字段随接口定义固定.

## Almanac 编辑与同步发布

### 内容来源

Polaris 保存唯一权威底稿, Astrolabe 读取其明确版本的内容供管理编辑. Star 的业务视图用于观察实际安装情况, 不能作为 Polaris 的新权威底稿. 不因 Star 缺失、落后或不可达推导管理 Delete, 不用界面搜索、分页或失败的读取结果清空整个 Scope.

必要管理入口保留单 Key Set/Delete, Set 提交完整 Buffer, 空 Buffer 与 Delete 分开. 批量编辑若由界面组织, 仍是多个单 Key 提交, 不承诺多 Key 原子事务. 首版不增加文件、数据库和 KV 导入适配器, 也不通过此入口替业务持久化动态 Catalog.

### 编辑保存与发布

管理请求从 Astrolabe 同步调用 Polaris, 只有确认持久提交才能返回“已提交”. Polaris 入队、Star 内存安装或 Watch 观察都不替代这一确认. 管理接口分别呈现持久提交位置与各 Star 的安装进度, 不等待全部 Star 同步完毕才确认一次合法写入.

并发编辑仍受 Almanac 分组的版本条件约束, 不能因为只有一个 Polaris 进程就把所有旧编辑当成合法覆盖. 冲突时明确返回并由调用方重新读取和决定, 不自动换成最新版本强行重发. 具体管理请求字段与回执由内部协议统一定义.

超时、断链或 Astrolabe 重启可能发生在 Polaris 提交之后, 因此结果不确定不等于未提交. 不回滚已确认内容, 不由 Astrolabe 保存第二份持久任务队列, 不通过 Watch 回调反复发布. 原结果只能依据原操作的有效证据确认, 当前内容或版本相同不自动证明历史请求曾经提交.

多次单 Key 提交遇到冲突、失败或结果不确定时停止该轮后续操作, 保留已确认部分. 原不确定结果不会因后续新编辑成功而改写. Polaris 的有界历史不保证永久保留任意旧操作的重试证据, 首版没有任意版本回滚接口.

### Star 启动恢复

Star 从 Polaris 接收保留权威版本的完整 Almanac 基线, 包括合法空集合和内部凭据. 恢复不等待浏览器登录、Astrolabe 页面打开或 Comet Go 订阅; 初始化顺序和降级边界只在 [架构](../../docs/architecture.md#启动与运行) 定义.

Polaris 为空时也必须明确完成空基线, 不能把读取失败当成空库. 认证开启但没有 Comet 凭据时拒绝业务登录; Astrolabe 仍可凭独立内部身份向 Polaris 写入首批凭据. Star 无需开放公共 `__` 访问来解除引导依赖.

## Comet 凭据管理

APIKEY/APISECRET 记录属于 `Almanac["__auth"]["comet"]`, 每个 APIKEY 对应一个完整凭据记录, 经 Polaris 持久提交再分发. 不包含 Grant、逐范围权限或注册所有者; 普通 Comet 登录后可访问全部普通业务数据, `__` Sector 仍由 Star 的外部入口拒绝.

管理面使用内部结构校验凭据, 普通 Buffer 可为空不代表凭据必需字段可为空. Star 安装凭据时, 记录、已安装版本、登录查找索引与必要会话失效共同生效, 不提前确认安装. 只需要 APIKEY 查找, 不构建各域/Sector/Spectrum 权限位图; 凭据字段与容量归 [内部协议](../../proto/README.md#credentials).

### 凭据生命周期

部署的 APIKEY/APISECRET 长期有效, 由管理面显式轮换或撤销, 不设自动过期或周期重登录. 每个 APIKEY 只有一个有效 SECRET, 不设新旧重叠窗口. 连续安装新 SECRET 或删除 APIKEY 后使对应旧 Session 失效并结束关联订阅; 连续同值更新和相同安装版本的重放不误关连接.

Polaris 持久提交与各 Star 安装不是同一瞬间, 未收到更新的 Star 不承诺同时撤销, 已发送数据不可收回. 暂时失联时 Star 使用已安装凭据, 不改为匿名, 不为每个 Comet 请求回查 Polaris. 凭据轮换和会话检查的并发边界由协议闭合, 不以取消逐范围权限为由接受失效会话.

删除后重建同名 APIKEY 只允许新的登录, 不复活旧 Session, 不重置业务版本/操作顺序或重新获得 TTL. APIKEY 不是 Ephemeris 所有权或 Catalog 发布权威. SDK 自己的活跃对象仍按其原身份、期限和目标恢复, 不因同名凭据出现就自动接管任意 UUID.

若 Star 跳过中间历史安装较新完整凭据快照, 已确认让该 Star 全部旧 Comet Session 重新认证, 包括最终 SECRET 未变的账号. 接受这一恢复时的影响以保持 Credential 只有 secret, 不新增逐账号身份标记; 安装原子性、重复快照及 SDK 恢复只在 [凭据快照](../../proto/README.md#credential-snapshot) 定义.

APISECRET 如何交给业务属于部署流程. Comet 不取得凭据表, 不从 Astrolabe/Pulsar 查询 SECRET; SDK 的本地 SECRET 更新不回写 Polaris. 服务密码、摘要、私钥、APISECRET 和会话 token 不进入日志、URL、命令行、错误、指标或前端普通拓扑响应.

## 实时观测

Astrolabe 从 Pulsar 的 [只读成员查询](../../proto/README.md#directory) 取得名单, 复用进程内有界缓存, 再观察各服务的实际连接、就绪和同步状态. 浏览器刷新不直接触发一次新的 Pulsar 查询. 登记存在不等于在线, 抓取失败不等于已经确认节点退出; 观察携带采样时间与成功/未知/陈旧状态. 不依据一次失败自动删除成员、改写 Almanac 或切换发布权威.

Star 已编写独立只读 HTTP GET /metrics, 使用 Prometheus 文本格式, Astrolabe 直接抓取用于实时展示. 首版不部署或依赖 Prometheus/Grafana, 不保存长期指标时序, 不另建私有指标 gRPC. 标准格式为未来外部接入保留可能, 不扩大当前实施范围.

指标入口默认关闭, 显式配置监听后启用; 本机绑定回环地址, 跨机使用明确受保护的网络或代理. 它与业务/内部 gRPC 的服务注册和资源预算分开, 不因 Comet 关闭认证/TLS 而扩大暴露范围, 也不承载管理写入或敏感转储.

首版实际暴露八个固定状态 gauge, 不声称已有请求计数或固定桶直方图; 标签只使用有限方法、状态和角色集合, 不使用 APIKEY、UUID、Key 或任意 Sector/Spectrum. 指标随事件维护, 不为每次抓取遍历全部分组或保留逐请求样本. 抓取并发、缓冲、等待、缓存条数和采样频率均有界, 慢节点不阻塞整个观测轮次; 不将业务估算字节冒充 RSS.

Admin 经适配层消费这些结果, Three.js 渲染器不持有基础设施凭据或参与恢复. 真实后端与演示模式显式分开, 后端不可达或数据陈旧时不静默回退为演示成功. 必要管理 API 与完整星图界面分别交付.

## 实施与验收边界

单管理账号、8 小时内存会话、同源/跨源规则、经 Polaris 持久提交和直接实时观测已经确认. 管理与目录接口见下节, 首版不扩展为账号系统、通用内容导入平台或外部监控集成.

[验收规约](../../testkit/comet.md) 按三域和本页边界维护, 旧 Astrolabe 数据库、直接写 Star、Grant 和 standalone 用例不作为新功能通过标准. 管理 HTTP、真实进程与浏览器已有验证基线, 执行证据和后续未验证改动只维护 [最新验证](../../testkit/validation.md).

## HTTP 接口与当前参数

源码中的基础参数为 `--listen=IP:PORT`, 可选 `--advertise`, `--super=IP:PORT`, `--galaxy`, `--group`, `--identity=目录`, `--account=摘要文件`, `--public=https://管理API来源`. 默认管理端 TLS 1.3, 使用明确部署的证书目录; `--http` 只允许回环监听, 供单独配置的反向代理使用. `--public` 决定 Cookie 的 Secure 属性, 不读取任意 Forwarded 头. `--origins` 可列出额外的逗号分隔浏览器来源; 显式跨站配置为 `--crosssite`, 只允许 HTTPS 公共来源. 默认最多 4096 个内存会话, 可用 `--sessions` 降低.

所有修改请求必须使用允许的 `Origin` 与 `X-Astra-Request: 1`; JSON 正文采用 `application/json`, 不接受重复/未知字段. 已登录的管理读取也使用同一 `astra-session` Cookie. 管理版本使用十进制字符串, 不经 JavaScript Number 转换. API 不自动重试写入, `effect` 分为 `committed`, `unapplied`, `unknown`.

| 方法与地址 | 请求与结果 |
| --- | --- |
| `POST /api/session` | `username`, `password`; 成功只设置 HttpOnly Cookie 并返回 username/expires, 不返回 token 正文 |
| `GET /api/session` | 返回当前用户名与固定期限, 不续期 |
| `DELETE /api/session` | 幂等注销并清除相同作用域 Cookie |
| `GET /api/metrics` | 返回最近抓取的 samples, 含实例 id、采样/尝试时间、stale 与固定数值字符串; 不由请求触发网络抓取 |
| `GET /api/nodes` | 缓存目录的脱敏成员与 observed/stale, 不包含在线断言、principal、密码或签名材料 |
| `GET /api/almanac` | Polaris 的完整分组清单, 每项 sector/spectrum/version |
| `GET /api/almanac?sector=...&spectrum=...` | 单分组 NDJSON 快照; 普通行 key/value, value 为 Base64; 最终行 complete/position |
| `POST /api/almanac` | sector/spectrum/key/version, 以及二选一的 Base64 value 或 erase=true; 成功 position 只来自 Polaris 的持久确认 |

合法空 value 为 `""`, 不等于删除. 每次管理提交仍受 Polaris 的严格版本规则约束. 快照读取不在 Astrolabe 保存第二份完整底稿: 逐页接收、逐行输出, 最终 `complete: true` 只在源 gRPC 正常结束后发送. HTTP 中途关闭、错误行、缺少最终行都表示本次快照不完整, 接入方必须丢弃暂存而不是清空旧视图. 对内部凭据的 Buffer 编码使用 [Credential](../../proto/README.md#credentials), 不把普通配置转为默认凭据.

实现初值为最多 4 次并发 KDF、16 个普通管理请求和 2 个完整快照流. 密码计算不占会话表锁; 单个修改 RPC 5 秒、快照总接收 30 秒, 慢 HTTP 写入受独立服务器期限约束. 这些限制是工程初值, 尚未测量吞吐、尾延迟或 RSS.

凭据管理使用 POST `/api/credentials`, JSON 为 `key`, 正十进制字符串 `version`, 以及 Base64 `secret` 或 `erase: true` 二选一. 编码内部 Credential 由 Astrolabe 负责, 不要求浏览器维护 Protobuf. GET `/api/credentials` 返回分行的 APIKEY 与 `redacted: true`, 最后仍需完整标记和范围版本; 通用 Almanac 对 `__auth/comet` 的读取同样脱敏, 不通过另一 URL 返回已存储的 SECRET. 普通配置范围仍返回 Base64 value.

指标目标通过可选 `--metrics=metrics.json` 明确部署, 文件为 `{ "Star内部IP:端口": "http://指标IP:端口/metrics" }`, 最多 64 项. 允许 HTTPS 受保护代理, 使用部署 CA; 不跟随重定向、不读取环境代理、不向指标端发送节点 bearer. 四个工作者、单请求两秒、正文 64 KiB, 每轮完成后按五秒周期继续; 慢目标可能延长实际轮次. 未配置时不采样. 响应实例头须与可信目录相符, 失败保留旧值并标陈旧, 超过 15 秒未成功抓取也标陈旧.

Star 每秒至多生成一次固定快照, 抓取不访问业务锁; 控制循环超过十秒未更新时拒绝提供旧成功响应. 当前指标为 astra_ready、astra_almanac_ready、astra_clock_ready、astra_clock_synchronized、astra_clock_uncertainty_nanoseconds、astra_members、astra_sessions、astra_recovery_bytes. 最后一项是复制恢复的逻辑计费, 不是 RSS; 全局 Almanac ready 也不是每 Scope 已安装版本.
