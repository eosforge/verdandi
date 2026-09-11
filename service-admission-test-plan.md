# Supervisor 与 Star/Planet v4 验证计划

日期: 2026-09-11. 当前传输为 gRPC/TLS, 账号授权与签名 bearer 凭证.
历史 v3 原始证据保留在 [2026-09-10 验证](service-admission-validation-20260910.md),
不能将旧证书角色、TLS exporter 或自定义帧用例计入新版覆盖.
最新结构整理与用例补强的执行证据见 [服务骨架报告](peer/service-hardening-20260911.md).

## 测试矩阵

| 范围 | 当前用例 | 必须验证的行为 |
| --- | --- | --- |
| 生成 | proto/generator/tests | 源码无漂移, 历史编号确定性与冲突拒绝 |
| 账号 | supervisor/internal/admission/accounts_test.go | 随机盐, 无明文输出, 配置错误, 取消等待 |
| gRPC 登记 | supervisor/internal/admission/server_test.go | 同账号多节点, 密码错误, 角色越权, 独立签名, Challenge 基线 |
| 登记重试 | 同上及 membership/store_test.go | 响应丢失幂等, 同端点 CAS 只有一个胜者, 旧实例不能覆盖新实例 |
| 成员表 | membership/store_test.go | 地址/UUID/槽位冲突, 持久恢复, 完整快照, 损坏与容量约束 |
| 候选 | admission/candidates_test.go | Planet 不进入 Star 名单, 最多 8 个候选, 本组优先, 轮换覆盖 |
| 解码 | peer/common/tests/unit/codec.rs | 空 repeated 条目在 Prost 构建对象前被计数限制, 畸形字段拒绝 |
| 身份 | peer/common/tests/unit/identity.rs | UUID 独立, bearer 可复用, 凭证篡改拒绝, 成员字段校验 |
| 会话 | peer/common/tests/unit/connection.rs | 有界队列、绝对期限、超限不入队、角色方向规则 |
| gRPC 流 | peer/star/tests/unit/wire_network.rs | 双向推送/心跳交错, 旧 Pong 不延时, 错误不回声, 重复会话拒绝 |
| 资源 | 同上及 Go admission 测试 | 消息/连接限制, 慢 TLS 和缺失 Hello 不能拖住关闭, 端口可复用 |
| Star 网络 | peer/star/tests/unit/network.rs | 并发加入收敛, 完整名单原子安装, Supervisor 离线重连 |
| Planet 网络 | peer/planet/tests/unit/network.rs | 一个上游, 本组优先、跨组切换, 旧候选接受合法较高 epoch |
| 实际进程 | testkit/services.py | 真实 Go Supervisor 与 Rust 二进制, 两端原生与混合主机, 信号与清理 |
| 隔离比较器 | testkit/transport/tests | 两种传输和消息调度工具仍可编译测试, 不是第二个生产后端 |
| 配置与错误 | common/tests/unit/config.rs, protocol.rs, retry.rs | 极端时间/容量在创建任务前拒绝, RPC 错误分类不误隔离候选, 退避饱和不溢出 |
| I/O 所有权 | common/tests/unit/rpc.rs | 客户端或服务端所有者 Drop 即取消真实 TLS/HTTP2 I/O, 同一 socket 第二次 RPC 拒绝 |
| 公共身份向量 | peer/tests/fixtures/admission-v4.json | Go/Rust 的名称、规范地址、UUID 和 principal 摘要一致 |
| 生命周期交错 | planet/tests/unit/lifecycle.rs, admission/listener_test.go | 取消 wait 后仍可关闭, 根任务错误可观察, 旧租约不清除新会话, 并发/交错 Close 幂等 |
| 输入与持久边界 | admission/boundaries_test.go, membership/boundaries_test.go | 无效 UTF-8, KDF 前后取消, 角色/地址不可变, epoch 耗尽, 快照无别名 |
| 编排器 | testkit/tests/test_services.py | 临时身份副本隔离, 进程命名不被覆盖, CLI 退出码与帮助契约, 无效场景不创建进程 |
| 覆盖引导 fuzz | 两个 Go boundaries_test.go 中的 Fuzz 入口 | 账号配置不会接受无效角色, 持久 Member 成功解码后必须满足不变量并可 round trip |

## 执行顺序

实现和用例完成后整理所有权及冗余路径, 再执行格式、生成一致性、Clippy/vet、锁文件验证,
原生单元与网络测试、Linux Go race、Release 构建, 然后运行本机与跨机真实进程回归.
一键回归和可设时长的故障循环都必须清理自有进程、监听器和临时目录, 失败也不能留下后台服务.
只使用项目内已经存在或明确批准的工具依赖. Linux 小内存环境限制编译任务数.
一键服务测试先执行 Python 编排器单测. `check-services` 的 `-FuzzSeconds 10` / `--fuzz-seconds=10`
分别让两个 Go fuzz 入口执行有限时长探索; 默认 0 只跑种子. Rust 畸形输入 corpus 为确定性测试, 不计作覆盖引导 fuzz.

## 观察与通过标准

- 查询实际状态索引, 不依赖有损事件数量. Star 的连接数与成员数必须同时吻合.
- 同账号不同端点不会互踢; 同端点重启 UUID 改变且 epoch 递增.
- 没有完整有效名单的节点不能初始化. Supervisor 离线时旧网络继续运行, 新进程等待.
- 不可信本地证书或错误账号不能初始化. 账号角色不能由请求字段提升.
- Planet 更换上游保持自身进程身份, 同时最多一个活动上游.
- 重启、取消、慢客户端和输入超限后, 自有资源均能回收且端口可再次绑定.

## 实现限制

签名凭证是 bearer, 不防完整凭证被复制后使用. 无独立 TTL、在线撤销和自动账号锁定;
密码更改不撤销旧凭证. TLS 验证服务端身份, 账号授权不等于证明公布地址的所有权,
当前没有每账号 IP 白名单或反向地址验证. 完整安全边界见 [gRPC 契约](peer/grpc-implementation.md).

成员库不是业务存储. 本轮不测试或声称已实现 Catalog/Registry 复制、SDK 接入或业务持久恢复.
分钟级故障循环是短时验证, 不等于生产耐久性认证. 旧 v3 性能原始结果不替代新版测试.
最终报告按每个平台分别记录通过、失败和未执行, 不合并掩盖缺口.
