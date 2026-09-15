# Supervisor 与 Star/Planet v1 验证计划

日期: 2026-09-12. 当前实现为 C++ Star/Planet + Go Supervisor, gRPC/TLS,
单次 Register 和一个签名 bearer 凭证. 以[身份契约](cluster/identity-contract.md)为准.
本轮执行证据见[准入精简报告](astra/admission-simplification-20260912.md).
旧 Rust 服务及 v3/v4/v5 结果保留为历史, 不能计入 v1 验收.
Planet 业务推进暂停, 此处只保留已有连接骨架的回归要求.

## 测试矩阵

| 范围 | 当前用例 | 必须验证的行为 |
| --- | --- | --- |
| 生成 | proto/generator/tests | 源码无漂移, 历史编号确定性与冲突拒绝 |
| 账号 | supervisor/internal/admission/accounts_test.go | 随机盐, 无明文输出, 配置错误, 取消等待 |
| gRPC 登记 | supervisor/internal/admission/server_test.go | 同账号多节点, 密码错误, 角色越权, 一次 Register 与独立凭证 |
| 登记重试 | admission/issuance_test.go, membership/startup_test.go | 响应丢失与并发相同请求幂等, 新请求按提交顺序替换, 已提交旧请求重开库后仍拒绝 |
| 成员表 | membership/store_test.go, startup_test.go | 地址/id/槽位冲突, 持久恢复, 完整快照, 请求索引损坏, 满额不驱逐且当前重试仍可完成 |
| 候选 | admission/candidates_test.go | Planet 不进入 Star 名单, 最多 8 个候选, 本组优先, 轮换覆盖 |
| 身份与解码 | astra/common/tests/identity_test.cpp | 不透明 id, bearer 可复用, 凭证篡改及旧签名域拒绝, 成员字段校验 |
| C++ 准入 | astra/common/tests/admission_test.cpp | 连接/登记总期限, 取消, 相同请求键, 候选刷新不能改变已安装实例身份 |
| 会话 | astra/common/tests/session_test.cpp | 有界队列、绝对期限、旧 Pong 不延时、旧关闭不删除新会话 |
| gRPC 流 | astra/common/tests/rpc_probe.cpp | 同一通道多个逻辑会话, 双向 Ping/Pong, 错误不回声, 重复会话拒绝 |
| 资源 | 同上及 Go admission 测试 | 消息/连接限制, 慢 TLS 和缺失 Hello 不能拖住关闭, 端口可复用 |
| Star 网络 | astra/common/tests/core_test.cpp, testkit/services.py | 并发加入收敛, 完整名单原子安装, Supervisor 离线重连 |
| Planet 网络 | 同上及 astra/test_processes.py | 一个上游, 本组优先、跨组切换, 旧候选接受合法较高 epoch, 空候选后发现新 Star |
| 实际进程 | testkit/services.py | 真实 Go Supervisor 与 C++ 二进制, 信号与清理; 当前 C++ 验收平台为 Linux |
| 配置与错误 | astra/common/tests/core_test.cpp, process_test.cpp | 极端时间/容量在运行前拒绝, RPC 错误分类不误隔离候选, 退避饱和不溢出 |
| I/O 所有权 | astra/common/tests/connect_test.cpp, session_test.cpp | 取消并排空真实 I/O, 回调不借用已销毁 Runtime, 逻辑会话与底层连接分离 |
| 公共身份向量 | cluster/tests/fixtures/admission-v1.json | Go/C++ 的名称、规范地址、不透明 id 和 principal 摘要一致; v1 使用这些字段规则 |
| 输入与持久边界 | admission/boundaries_test.go, membership/boundaries_test.go | 无效 UTF-8, KDF 前后取消, 角色/地址不可变, epoch 耗尽, 快照无别名 |
| 编排器 | testkit/tests/test_services.py | 临时身份副本隔离, 进程命名不被覆盖, CLI 退出码与帮助契约, 无效场景不创建进程 |
| 覆盖引导 fuzz | 两个 Go boundaries_test.go 中的 Fuzz 入口 | 账号配置不会接受无效角色, 持久 Member 成功解码后必须满足不变量并可 round trip |

## 执行顺序

实现和用例完成后整理所有权及冗余路径, 再执行格式、生成一致性、Clippy/vet、锁文件验证,
原生单元与网络测试、Linux Go race、Release 与 ASan/UBSan/TSan 构建, 然后运行真实进程回归.
一键回归和可设时长的故障循环都必须清理自有进程、监听器和临时目录, 失败也不能留下后台服务.
只使用项目内已经存在或明确批准的工具依赖. Linux 小内存环境限制编译任务数.
一键服务测试先执行 Python 编排器单测. `check-services` 的 `-FuzzSeconds 10` / `--fuzz-seconds=10`
分别让两个 Go fuzz 入口执行有限时长探索; 默认 0 只跑种子, 不计作覆盖引导探索.
Go race 的认证夹具使用 15 秒 RPC 预算, 生产 Serve 仍为 5 秒; 真实进程回归必须调用生产入口.

## 观察与通过标准

- 查询实际状态索引, 不依赖有损事件数量. Star 的连接数与成员数必须同时吻合.
- 同账号不同端点不会互踢; 同端点新启动获得新 id 且 epoch 递增; 相同请求重试保持二者不变.
- 已提交后被替换的旧请求不能恢复; 从未提交的新请求按事务提交顺序处理, 不推断物理启动先后.
- 没有完整有效名单的节点不能初始化. Supervisor 离线时旧网络继续运行, 新进程等待.
- 不可信本地证书或错误账号不能初始化. 账号角色不能由请求字段提升.
- Planet 更换上游保持自身进程身份, 同时最多一个活动上游.
- 重启、取消、慢客户端和输入超限后, 自有资源均能回收且端口可再次绑定.

## 实现限制

签名凭证是 bearer, 不防完整凭证被复制后使用. 无独立 TTL、在线撤销和自动账号锁定;
密码更改不撤销旧凭证. TLS 验证服务端身份, 账号授权不等于证明公布地址的所有权,
当前没有每账号 IP 白名单或反向地址验证. 完整边界见[当前身份契约](cluster/identity-contract.md).

成员库不是业务存储. 本轮不测试或声称已实现 Catalog/Registry 复制、SDK 接入或业务持久恢复.
分钟级故障循环是短时验证, 不等于生产耐久性认证. 旧协议性能或耐久结果不替代新版测试.
最终报告按每个平台分别记录通过、失败和未执行, 不合并掩盖缺口.
