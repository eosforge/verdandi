# Supervisor 准入精简

日期: 2026-09-12. 状态: 准入精简及本轮回归完成.

## 变化

| 项目 | v5 | v6 |
| --- | --- | --- |
| 首次准入 RPC | Challenge + Register | Register |
| 启动票据 | Ticket + TicketSignature | 移除 |
| 客户端 CAS | 固定 expected_epoch | 移除 |
| 实例身份 | Supervisor 签发不透明 id | 保留 |
| 重试关联 | 启动票据及基线 | 每进程固定随机 request_id |
| 幂等持久依据 | 当前成员和客户端基线 | Supervisor 私有请求索引和当前成员 |
| 凭证签名域 | 启动票据域 + Hello 准入域 | 仅 Hello 准入域 |
| C++ 准入阶段 | idle/connecting/challenge/registration | idle/connecting/registration |

Star 保留 TLS、原始凭证验签、角色/Galaxy/部署绑定、成员替换检查和本地旧会话过滤.
认证不再依赖新端的账号密码或每次查询 Supervisor; 账号密码仅发送给 Supervisor.
第一版业务 SDK、Catalog/Registry 同步以及 Planet 业务扩展不属于本轮.

## 明确的成本和语义

- 新启动请求按持久事务提交顺序替换同部署实例, 不再提供两个已取得相同 CAS 基线请求只有一个成功的旧语义.
- 已经提交且被替换的旧请求始终拒绝, 包括 Supervisor 重启后; 从未提交的迟到请求无法凭随机键推断实际启动时间.
- 请求索引每项原始键值 72 字节, 另有 bbolt 开销; 默认每 Galaxy 1,048,576 次已提交启动预算.
  --max-startups 可配置至 16,777,216. 满后拒绝新启动, 不驱逐记录, 当前身份重试仍可完成.
- v5 当前成员格式可保留, 首次 v6 登记原子创建私有请求索引; 索引创建后不能降级给旧 Supervisor.
- 凭证仍是 bearer, 没有新增 TTL 或在线吊销; 旧成员在其他 Star 观察到较新代次后才被替换.
- 业务状态恢复、性能提升和无限重启均未作承诺. 详见[身份契约](../cluster/identity-contract.md).

## 静态审核与扩散检查

- Go Supervisor: 账号验证 -> 事务内去重 -> 分配成员代次 -> 同事务提交成员/请求 -> 凭证签发.
  检查请求失败不部分提交、重开库后的旧请求隔离、当前重试先于容量判断、角色与地址不可绕过.
- C++ Star/Planet: 共同 Admission 只保存一次随机请求键、一个 RPC 的上下文与完成状态;
  校验首次部署绑定并固定已接纳身份, 刷新不能更换身份, 取消后仍排空回调.
- Protobuf: 删除的字段号和名字保留; 删除的消息 ID 保留历史, 不重用; Go/C++ 由已有工具重新生成.
- Rust 服务已废弃, 不维护第二套新准入实现. Rust 协议生成工具继续通过编号与生成检查.
- 现有 Redis Go/Rust/C++ SDK、C ABI/C# 绑定不使用服务准入协议, 本轮没有修改其业务或身份语义.
- Python 服务夹具继续验证 C++ 两角色与 Go Supervisor, 不增加账号验证或版本状态机.

## 验证

| 平台 / 配置 | 本轮结果 |
| --- | --- |
| Windows Go | 格式、离线模块一致性、vet、全包测试、构建通过 |
| 协议生成 | Go/C++ 生成一致性通过; Rust 生成工具 fmt、Clippy、4 项编号测试通过 |
| Windows Python | 服务夹具 7 项单元测试通过 |
| Linux Go race | 全包通过, 最终第三轮; admission 包 114.078 秒 |
| Linux C++ Debug | 8 个 CTest 套件 + 6 组真实 RPC + 13 组进程场景通过 |
| Linux C++ Release | 同上, 全部通过 |
| Linux C++ ASan/UBSan | 同上, 全部通过, 未报告地址或未定义行为错误 |
| Linux C++ TSan | 同上, 全部通过, 未报告数据竞争 |
| 源码对应 | 85 个相关 Go/C++/Proto 及生成源码文件的本地/虚拟机 SHA-256 一致 |
| 清理 | 四配置的进程回归均确认进程回收、端口可复用和自有临时目录删除; 最后检查无本轮服务残留 |

8/6/13 是每个配置的套件或场景数, 四种构建运行相同场景, 不是四倍的独立功能覆盖.
源码哈希只覆盖报告列出的相关源文件, 不代表整个工作区. Debug 最后再次使用最终 Go 二进制回归;
四配置的 RPC 报告记录同一个最终 Supervisor 二进制哈希.

准入专用测试覆盖回复丢失、并发相同请求、连续替换后重开库拒绝旧请求、请求部署信息变化、
启动预算耗尽仍允许当前重试、索引损坏、服务返回另一实例身份时 C++ 拒绝刷新等边界.
真实进程场景保留三 Star 全互联、Supervisor 离线与恢复、强制/正常重启、错误账号与角色拒绝,
以及既有 Planet 候选刷新和故障切换; 没有推进 Planet 业务.

### 失败记录及修正

1. 首次 Debug RPC 准备缺少隔离目录中的 Supervisor 可执行文件. 补齐构建后重跑通过, 原失败报告保留.
   SSH 输出助手原 300 秒无输出超时也已在本轮专用助手延长, 没有将助手超时判为服务崩溃或通过.
2. Go race 前两轮分别在错误密码、正常 Planet 登录返回 DeadlineExceeded.
   插桩下固定 PBKDF2 计算超过生产 5 秒限制. 精确认证错误码改为直接 handler 检查,
   另保留真实 TLS 拒绝测试并验证没有登记成员.
   测试入口改用 15 秒 RPC 预算, 共用生产处理链; 生产 Serve 仍固定 5 秒,
   PBKDF2 仍为 600,000 次. 第三轮全包 race 通过. 四配置真实进程回归继续使用生产入口.

详细场景、二进制及源码哈希、CTest 结果和前两次 race 失败输出保存在
[机器可读报告](../testkit/results/cluster-admission-20260912.json).
完整临时构建日志保留在项目 build/admission-review 下.

所有验证使用 /home/ubuntu/verdandi/build/admission-review/source 的隔离源码与自有服务资源.
复用已有项目工具和依赖, 不下载新包、不修改全局配置、不启动之前停止的耐久测试.
