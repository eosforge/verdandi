# Star / Planet 身份与准入

2026-09-12: C++26 Star + Go Supervisor 使用控制协议 v6. Planet 的业务推进暂停;
已有连接骨架共享此次准入修改并继续接受回归. 旧 Rust 服务不参与当前实现或验收.
本契约取代 v5 的 Challenge、启动票据及客户端首次 CAS 基线.

## 一次启动

1. Star 加载账号密码、TLS 材料和 Supervisor 准入公钥.
2. 本进程生成一次 32 字节随机 `request_id`, 仅用于登记幂等, 不作为实例身份或授权凭证.
3. 通过 TLS 发送一次 `Admission.Register`, 携带账号密码、Galaxy、具体端点、角色、group 和 request_id.
4. Supervisor 验证账号及角色, 在一次 bbolt 事务中登记实例、保存请求结果依据并取得完整成员快照.
5. 提交成功后签发唯一实例的准入凭证, 与 Star 名单一起返回.
6. Star 对凭证原始字节验签, 核对自身部署信息并固定实例身份, 随后连接名单中的 Star.

不再有 Challenge RPC、RegistrationChallenge、LoginRequest、启动 Ticket、第二个签名域,
或由客户端提交的 id/expected_epoch. 普通重连不再次登录, Planet 原有候选刷新复用同一请求键.

## 身份和会话

- `id` 仍是 Supervisor 签发的不透明 UTF-8 字符串, 1..128 字节. 客户端不生成或解析它.
- `principal` 是账号、Galaxy、规范端点的 SHA-256 指纹, 用于识别可被新实例替换的部署槽位.
- `epoch` 由 Supervisor 在事务中分配, 只排序同部署的实例替换, 不承担业务版本含义.
- `SessionGeneration` 只属于本地 RPC 生命周期, 用于忽略旧会话完成事件, 不发送给 Supervisor.
- request_id 只发给 Supervisor, 不复制到成员名单、Hello、日志或业务版本中.

两个全新启动请求按 Supervisor 的提交顺序登记. 后提交者替换前者, 不比较物理启动时间;
这取代 v5 的同 CAS 基线只能成功一个的语义. 从未提交的迟到请求无法被识别为较早进程,
不得将这一规则描述为墙钟最新进程保证. 正常客户端同一进程永远不更换 request_id.

## 幂等与持久记录

同一次启动的每次重试使用相同 request_id 和部署信息:

- 第一次事务成功后, 回复丢失也不会改变已签发 id 或再次增加 epoch.
- 该请求仍对应当前实例时, 返回相同准入正文及最新名单; 候选轮转不改变身份.
- 该请求已被其他启动替换时, 返回 Aborted. Supervisor 重启后也不能让旧请求重新占位.
- 相同 request_id 改变账号、端点、角色或 group 会被拒绝.
- 事务失败不留下部分成员或请求记录. 并发重复请求只产生一次提交.

每个 Galaxy 的 bbolt 成员 bucket 下有一个私有 `@starts` 索引:
32 字节请求键 -> 32 字节部署摘要 + 8 字节 epoch. 每条原始键值共 72 字节,
不含 bbolt 页、索引、事务与文件空间开销. 不保存密码、签名、历史名单或完整旧 Member.
登记按键查询, 不逐次扫描历史; 打开库时检查索引和成员引用的完整性.

`--max-startups` 默认 1,048,576, 范围 1..16,777,216, 是每 Galaxy 累计已提交启动记录预算.
到达预算后新启动返回 ResourceExhausted, 当前实例重试和原有网络通信继续工作.
运维可提高预算并重启 Supervisor; 不能通过自动驱逐旧记录解除限制, 否则旧请求可能再次登记.
当前没有记录压缩或在线回收协议, 不承诺无限次重启. 这是将幂等责任集中到服务端的存储代价.

## Star 接入验证

`StarTransport.OpenSession` 的首条消息仍为 Hello. Star 本地校验:

1. 主版本、消息边界和 Supervisor Ed25519 签名.
2. Galaxy、角色、端点及已知部署替换关系.
3. 当前逻辑会话的重复或冲突.

只有一个签名域: `verdandi-admission-v6` + NUL + 原始 Member 字节.
后续消息沿用会话身份, 不逐条重新验签; Star 不处理其他节点的密码、登记请求键或持久幂等索引.
认证通过不代表业务写入已经授权或数据同步完成; 业务协议仍待实现.

TLS 1.3 继续负责加密与服务端证书验证, bearer 凭证继续承担客户端身份.
已运行节点可以在 Supervisor 离线时使用现有凭证通信; 新进程等待 Supervisor 登录.
本轮未增加凭证 TTL、在线吊销、共享密码直连或 mTLS 证书签发机制.
成员替换只在收到较新凭证后被本地观察, 不承诺所有隔离副本即时撤销旧实例.

## 升级和范围

- v6 与 v5 不互通, Supervisor、Star 和保留的 Planet 应一起升级. 旧字段号保留, 不复用.
- v5 的当前 `id` 成员记录可以保留; 首次 v6 登记创建私有启动索引并递增已有部署 epoch.
- 更早的 `peer_id` / public_key 成员格式仍拒绝, 不自动删除或清空数据库.
- 添加启动索引后的数据库不能交给旧版 Supervisor, 无自动降级.
- Planet 的业务缓存、换绑再登记、Star 业务复制和 SDK 仍未实现.
- 历史 v4/v5 性能与长测报告不自动成为 v6 验收结果.

验证结果见 [准入精简记录](../cluster-cpp/admission-simplification-20260912.md).
