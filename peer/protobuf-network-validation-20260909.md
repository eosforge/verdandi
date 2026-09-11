# Peer Protobuf 与拓扑验证记录

日期: 2026-09-09. 范围: Rust Peer 的基础网络和生成工具.

## 最终协议

- 用户确认采用 `[MessageID:uint16][PayloadLength:uint32][Payload]`, 两个头字段均为大端.
- 移除了 CoreEnvelope. 顶层消息自动分配 ID 并写入 `proto/message-ids.lock`, 既有 ID 不随声明顺序改变.
- 实际消息为 Hello, Ping, Pong, ProtocolError, DiscoverRequest 和 DiscoverResponse.
- `.proto` 使用用户修改后的 4 空格 clang-format 配置, 手写注释使用中文与英文标点.

## 工具和构建

| 检查 | Windows | Ubuntu 26.04 虚拟机 |
| --- | --- | --- |
| Cargo / Rust 工具链 | 1.98.1 | 项目内 1.98.1 |
| protoc | 36.1, 官方 win64 ZIP | 36.1, 官方 linux-x86_64 ZIP |
| Cargo 依赖 | Prost / prost-build 0.14.4, 锁文件固定 | 同一锁文件 |
| rustfmt / 严格 Clippy | 通过 | 未另行安装格式器或 Clippy |
| 单元与集成测试 | 31 / 31 通过 | 31 / 31 通过 |
| Release 构建和 `--help` | 通过 | 通过 |

Ubuntu 测试实际运行于 `192.168.0.119`, Linux 7.0.0-31-generic x86_64, 项目 `/home/ubuntu/verdandi`.
它不是 Git Bash 或 WSL 结果. 此次验证前可用内存约 6.7 GiB, 构建并发限制为 2, 测试线程限制为 2.

protoc ZIP SHA-256 与官方 GitHub 发布资产 digest 一致:

```text
Windows: 390e515cb456e6a978553bdb57baf087b054885077fd6da7f7ff0160279c07d6
Linux:   c4bc672d9d49214dc8cafdceadf4df92182d6ca8e3ec65a56b2d7de5602669b4
```

安装位置为两端各自的 `build/tools/protoc/36.1`, Cargo 缓存为 `build/deps/cargo`.
源代码通过项目内压缩包同步, Ubuntu 同步前备份保存在 `build/transfer/peer-proto-before-20260909.tar.gz`.
没有修改全局 PATH, 系统配置或已实现 SDK 的依赖声明.

## 测试覆盖

7 项私有单元测试验证阻塞写的期限, 退避封顶抖动, generation 耗尽,
候选表容量及淘汰取消, 旧 boot/lease 的影响, seed 地址别名和冲突地址保护.

3 项编号生成测试验证声明重排, 删除后的编号保留, 新消息稳定追加,
重复 ID/名称与 uint16 耗尽拒绝.

3 项原有网络测试验证 seed 连接, 错误集群拒绝和稍后上线的 seed 重试.
错误集群测试现在直接拒绝意外 Connected, 不再跳过该事件.

18 项协议与网络测试验证:

- 固定六字节头与独立 Protobuf payload, 默认 Ping/Pong 请求关联.
- 头部和正文分片跨过保活定时器, 读取进度仍保留.
- 旧 Pong, 对端 Ping 和发现流量不能掩盖本端探测或发现超时.
- 静默连接超时重连, 新 generation, 握手取消和被丢弃的 shutdown future.
- 非法字段, 未知 ID, 零关联编号, 超长帧, 非法 UTF-8, 截断和重复 Hello.
- ProtocolError 不产生错误回复循环.
- 三节点链式 seed 扩展为 6 条物理 TCP 会话.
- 首轮之后加入的节点通过周期查询被发现.
- 落后列表缺项和旧地址不会破坏已连接节点, 重复列表不制造重复会话.
- 拓扑有界分页及完整游标遍历, 过密请求被拒绝.

第一轮分页夹具把 payload 限为 128 字节, 无法容纳单个长名称条目及分页游标,
服务端正确返回 ResourceLimit. 夹具调整为 192 字节后, 验证了多页传输和每页大小上限.
测试阶段还纠正了一个把超限连接的尽力错误通知误当作保证送达的断言.
失败运行不计为通过结果, 上表为修正后的完整回归.

## 同类问题的跨语言检查

Peer 的握手, 自动 MessageID 和拓扑会话目前只有这一份 Rust 实现, 其他语言没有同一帧处理器.
同时核对了 Go / Rust SDK 的 Selector 退避与 PONG 关联逻辑, C++ 的共享退避实现,
以及当前 C# 对 C ABI 的调用路径. SDK 的退避在封顶值以下取抖动区间, 未发现本次旧 Peer 的封顶抖动消失模式;
C# 当前通过 native 调用复用 C++ 连接路径. 新 Peer 协议没有改动这些 Redis SDK 的协议契约.

此次没有重跑数据库 SDK 全量测试, 没有进行跨主机混合组网或 64 节点容量基准.
31 项测试证明所列正确性场景, 不证明长时稳定性或端到端零拷贝性能.
StateStore, Catalog/Registry 同步, 生产认证, 自动成员删除和 CLI 服务托管仍属于后续工作.
