# Peer 序列化选型复核

日期: 2026-09-09. 状态: 已获准接入当前控制消息, 尚无项目级性能对比结果.

## 判断

目前不能证明 Protobuf 是 Verdandi 的性能最优解. 按当前目标, 仍推荐把它作为控制消息和状态元信息的默认候选, 业务正文采用不可变字节块. 这项建议来自实现与维护取舍, 不是吞吐或延迟基准结论.

当前 Rust 服务已通过 protoc / Prost 接入 Hello, Ping/Pong 和发现消息. 根据后续用户确认,
传输格式是自动 MessageID 加长度和具体 payload, 没有统一 Protobuf envelope.
实现与正确性回归不证明其运行时开销最小; 性能比较仍需独立负载基准.

## 当前项目的实际前提

- Peer 使用 Rust, 通过 TCP 连接, 后续需要与多语言 SDK 交换协议消息.
- Peer Core 主要比较 key, owner, version 和 hash, 保存并分发正文; 业务字段由后续领域层理解.
- 用户讨论过几百字节到约 1 KiB 的小 data, 但尚未把它确认为全部负载的硬上限.
- 目标是同一状态内容只编码一次, 广播时共享不可变缓冲区. 这一能力来自缓冲区所有权和发送路径, 不由某一种序列化格式独占.
- 网络生命周期、同步规则和队列必须保持简单. 更换序列化格式不能自动修复这些层的问题.

## 候选比较

| 候选 | 已核实的技术特点 | 对本项目的判断 |
| --- | --- | --- |
| Protobuf + Prost | 生成普通 Rust 类型, 默认 string 为 String, bytes 为 Vec, map 为 HashMap; bytes 字段可配置为 Bytes | 适合控制结构和元信息; 大量细碎字段反序列化为持有型对象时, 仍需关注分配 |
| FlatBuffers | 通过缓冲区偏移访问字段; Rust 安全入口先验证数据; 构建端有自己的缓冲区和辅助容器 | 若需要频繁访问结构化正文的少数字段, 是主要性能对比候选 |
| Cap'n Proto | 使用带对齐和分段规则的内存布局; 访问时校验指针, packed 格式还需要解包 | 直接访问正文的另一候选; 需要验证布局、读取限制和缓冲区管理的集成成本 |
| 自定义二进制 | 字段与布局完全受项目控制 | 能针对固定场景精简, 但需自行承担跨语言编码器、版本兼容和边界验证, 不宜仅为 Hello 推广成整个项目的正式协议 |

Prost 类型和生成配置来自 [Prost 文档](https://docs.rs/prost/0.14.4/prost/) 与 [Bytes 字段配置](https://docs.rs/prost-build/0.14.4/prost_build/struct.Config.html#method.bytes). FlatBuffers 的访问和验证行为见 [官方 Rust 指南](https://flatbuffers.dev/languages/rust/). Cap'n Proto 的布局、packing 和读取限制见 [官方编码规范](https://capnproto.org/encoding.html). 表中对本项目的适用性属于工程推断.

这些格式都不能替代 TCP 连接管理、身份认证、同步状态机或应用层资源限制. 使用 Protobuf 不要求采用 gRPC; 使用 Cap'n Proto 的序列化也不要求采用其 RPC 系统.

## 分配与复制需要分别判断

Prost 的 bytes 解码实现对输入和输出均为 Bytes 的路径支持共享内容; 输入若换成其他 Buf 实现, 可能发生复制. 这不意味着整条消息不分配, 也不意味着再次编码到连续发送缓冲区时不复制正文. 依据为 [Prost bytes 编解码源码](https://docs.rs/prost/0.14.4/src/prost/encoding.rs.html).

本项目更应首先落实:

1. 控制消息与状态元信息使用有类型的字段.
2. 正文保持 opaque bytes, Core 不为每条数据构造通用嵌套 map 或字段对象树.
3. 相同状态和分块条件下复用已编码内容帧, 避免按接收者重复编码.
4. 接收和发送路径都记录真实分配、复制与缓冲区保留量. Bytes 切片可能延长整块接收内存的存活期, 不能只统计字段可见长度.

以上是建议的实现约束, 当前只有网络骨架, 尚未验证实际广播性能. FlatBuffers 和 Cap'n Proto 也能共享已完成的不可变缓冲区, 因此不能把“共享广播”作为 Protobuf 独有优势.

数据 hash 还应独立定义. Protobuf 的确定性序列化不等于跨版本规范编码, 不能直接把任意 Protobuf 对象重新编码的结果当作稳定内容 hash. 见 [官方非规范化说明](https://protobuf.dev/programming-guides/serialization-not-canonical/).

## 格式与 Rust 实现分开选择

采用 `.proto` 不等于已经锁定 Prost. 当前查询的 Prost 0.14.4 文档说明其生成普通 Rust 类型, 同时维护章节表明主要接受修复和小改进, 暂不推进新特性评审; 这不代表项目停止维护. 见 [Prost 维护说明](https://docs.rs/prost/0.14.4/prost/#contributing).

实际接入前仍需审查所选实现版本、依赖、生成流程和兼容性. 本轮没有安装任何候选库或编译器, 也没有取得额外下载授权.

## 能支持最终判断的最小对比

比较应先围绕 Protobuf 与 FlatBuffers, 使用相同数据、相同验证要求与相同共享广播方式:

- 少量控制字段的 Hello, 避免只优化低频握手而忽略数据路径.
- 状态元信息和 256 B, 1 KiB, 16 KiB 的正文; 数据规模是测试样本, 不代表已有业务上限.
- 包含许多小字段的结构化正文, 区分“只读元信息”和“读取正文所有字段”.
- 单接收者与 8 个接收者, 两方都采用一次编码后共享广播.
- 包含非法输入验证, 不把 FlatBuffers unchecked 路径与 Protobuf 已校验路径直接比较.

应记录编码、校验和解码 CPU, 分配次数和字节, 帧大小, 持续运行内存, 实际 TCP 吞吐及 P95/P99 延迟. 如果端到端瓶颈仍在索引、hash 或网络, 仅有编解码微基准优势不足以证明换格式值得. 当前缺少这些数据, 因而保留 Protobuf 为候选, 不宣称它已被证明最优.
