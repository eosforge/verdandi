# TLS 尾部刷新问题审计

日期: 2026-09-10. 状态: 已修复. 范围: Star/Planet 共用控制帧写入器, 以及传输实验的 TCP 适配器.

## 问题与触发条件

`FrameWriter::write` 原来在 `write_all` 成功后直接返回. 对 tokio-rustls,
这不保证所有密文已经交给底层连接. 底层暂时不可写时, TLS 层可能接受明文并保留待发送数据.
如果此后进入空闲或等待对端答复, 最后一条消息可能滞留, 表现为握手/控制答复延迟或同步水位超时.
依据: [tokio-rustls 0.26.4 官方刷新说明](https://docs.rs/tokio-rustls/0.26.4/tokio_rustls/).

不刷新的推流预实验复现了 `final progress timeout`, 包括普通推流和 16 个接收会话的场景.
它与 `push receiver lagged; resynchronization required` 不同: 后者表示接收端超过有界广播环的保留范围.
新诊断保留服务端具体原因, 不再只根据客户端看到的 TLS EOF 判断根因.

之前测试经常让底层连接立即可写, 或者不断发送后续消息. 原有阻塞测试直接使用小容量 duplex,
覆盖了 `write_all` 自身阻塞, 没有覆盖“写入已被上层缓冲接受, 实际阻塞发生在 flush”的情况.

## 修复与回归

- [正式控制帧写入器](common/src/protocol.rs) 在写完一帧后刷新. 刷新仍包含在调用方原有的绝对写入期限内,
  超时/取消后关闭会话, 不复用可能只有部分帧的连接. 控制消息无需为了批量等待其他消息.
- [控制帧回归](common/tests/unit/connection.rs) 使用 BufWriter + duplex 稳定模拟缓冲传输:
  没有后续写入时, 最后一帧仍能被接收; 底层持续阻塞时, flush 不能绕过绝对期限.
- [TCP 推流适配器](../testkit/transport/src/push_io.rs) 合并至多 16 个已就绪帧,
  一次写入后刷新. 队列暂空立即刷新, 不人为等待凑满批次.
- [批次回归](../testkit/transport/tests/wire.rs) 验证独立消息边界、尾帧刷新和 16 帧上限,
  超限不得写出半个批次.
- Windows 与 Ubuntu 的正式 Star/Planet 工作区各通过 39 项测试, 传输原型各通过 14 项测试;
  两端 Clippy 零警告通过. SDK 按本轮范围没有运行测试.

## 扩散检查

| 路径 | 检查结果 |
| --- | --- |
| Rust Star/Planet 控制会话 | 共用 FrameWriter, 同时受影响并修复. |
| Rust 准入注册请求 | 共用 FrameWriter, 同时获得修复. |
| Rust TCP 实验 | 缓冲写入同样需要刷新; 单条控制与有界批量推流分别处理. |
| Rust gRPC 实验 | HTTP/2 驱动管理刷新, 未发现同样的手写缓冲写入遗漏. |
| Go Supervisor | 当前直接向 Go TLS 连接写帧, 没有 tokio-rustls 这一缓冲契约, 也未增加 bufio 写缓冲. |
| Go/Rust/C++ SDK | Redis 传输交由 go-redis/Fred/Boost.Redis 等现有驱动管理, 未发现同样的手写 tokio-rustls 帧写入点. |
| C# SDK | 复用 C++ 核心, 没有新增独立 TLS 写入器. |

此检查定位具体缓冲契约的扩散, 不等于重新认证所有第三方驱动或运行全部 SDK 场景.

## 性能对照规则

逐帧刷新和完全不刷新的旧推流数据只保留为预实验. 正式 v3 同时比较“有界批量 TCP”与 gRPC 长期双向流,
均保证流转为空闲时最后一条消息被刷新. 不用缺少必要刷新的实现冒充更快的正确实现,
也不据逐帧写入的开销推断 TCP 协议的理论极限.

结果见 [推流详细报告](grpc-benchmark-results.md).
