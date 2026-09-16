# Orbit / Astra / Comet v1 修正

本文件是当前协议与 C++ 命名的依据. 实现阶段全部使用 v1, 不维护旧协议兼容路径.
`astra-migration.md` 和日期化测试报告保留其原时点记录, 不将旧成绩转记给本轮.

## 文件与职责

| 文件 | package | 职责 |
| --- | --- | --- |
| [proto/orbit.proto](proto/orbit.proto) | `proto.orbit.v1` | Supervisor 账号准入、幂等登记、Member 与 Role |
| [proto/astra.proto](proto/astra.proto) | `proto.astra.v1` | Star/Planet 会话、Hello、Ping/Pong、拒绝码及同步草案 |
| [proto/comet.proto](proto/comet.proto) | `proto.comet.v1` | 未来 SDK 的协议边界, 当前没有消息或 RPC |

C++ 生成命名空间分别为 `proto::orbit::v1`、`proto::astra::v1`、`proto::comet::v1`.
手写代码直接使用 `astra`, 公共头文件为 `<astra/config.hpp>`、`<astra/types.hpp>` 等.
传输适配直接使用完整名称, 例如 `proto::orbit::v1::Member` 和 `proto::astra::v1::Hello`, 不使用协议命名空间别名.
隔离探针同样直接使用 `proto::astra::bench::v1`. 生成类型不扩散到公共业务头文件.

移除别名后已重新通过 Linux Debug 的 10 项 CTest、6 项 RPC 和 13 项进程场景, 包含隔离探针编译和生成一致性检查.
本次仅修改手写类型引用与格式, 未重跑 Release 或 sanitizer; 下文较早的完整复验记录保留其原时点范围.

原 `cluster.proto`、`admission.proto`、`star_transport.proto`、`sync_transport.proto` 已移除.
节点会话与同步草案归入 `astra.proto`, 不保留别名文件或旧 RPC 注册.
Member 从登记应答的嵌套类型提升为 Orbit 的顶层消息, Role 同样只在 Orbit 定义一次.
成员字段编码未作无关重排; 保留的字段号空洞不代表实现旧协议兼容.

## 版本与签名

- C++ 当前控制主版本常量为 `1`, 发送、接收和独立推流探针共用该常量.
- Supervisor RPC 路径为 `/proto.orbit.v1.Admission/Register`.
- 节点会话 RPC 路径为 `/proto.astra.v1.StarTransport/OpenSession`.
- 签名输入为 `"proto.orbit.v1.admission" + NUL + 原始 Orbit Member 字节`.
- Hello 原样携带该正文和签名, Star 验签并解码 Orbit Member 后转换为内部成员值.
- 更换用途或协议所有者的签名均拒绝; 不根据多个旧前缀依次尝试验签.
- 账号密码、TLS 和 Ed25519 密钥不因本次协议命名更改而重置; 节点需按当前协议重新登记.

本次发现隔离推流探针曾发送主版本 5、接收端要求 6, 已统一使用当前常量.
生产会话、C++ 测试探针和 Go 准入契约一并审查. Go 不另行维护节点 Hello 发送版本.
隔离探针包为 `proto.astra.bench.v1`, 不属于生产接口, 不恢复旧 Rust 接收器.

## 生成与测试边界

两个生成入口只使用已安装的 protoc 和插件. Go 输出仍集中在 `supervisor/internal/generated`;
C++ 输出在 `astra/common/src/generated`, 基准协议输出在 `astra/bench/generated`.
文件集合检查会拒绝旧生成源码残留, 普通构建不执行生成或下载.

gRPC 使用完整服务/方法名路由. `message-ids.lock` 是原自定义帧的编号历史, 不提供兼容后端.
13 个仍有效的既有消息保持编号, 新顶层 Orbit Member 追加为 29; 已退役消息不再生成类型.
历史编号、旧 SDK、Lua、Redis 契约和退休 Rust 实现均未重写为新协议.

协议归属测试独立检查描述符包名、RPC 完整路径、Member 唯一归属、签名域和 Comet 当前无接口的边界.
真实 C++ RPC 探针独立要求收到主版本 1, 并测试非 v1 消息被拒绝.
已有身份测试检查跨用途、跨所有者签名拒绝; 不以两端同时改错的自洽结果作为唯一证据.

## Linux

继续在 `/home/ubuntu/verdandi` 根目录使用 `build/astra` 构建树及项目工具/依赖缓存.
旧 schema、生成文件与嵌套头文件归档于 `build/protocol-v1/retired`, 不清空任何安装前缀.
本轮仅重新编译受协议和 C++ 命名变更影响的项目目标. 不下载、不重编第三方依赖.
没有无限时测试、后台耐久任务或自动 commit/push.

当前仍是连接骨架和内部 Store. SyncTransport 未注册, Comet SDK 未实现, Planet 业务推进继续暂停.

## 首次迁移验证结果

- Windows Go Supervisor 的格式、模块校验、vet、测试及构建通过; 协议生成器 fmt、Clippy、生成一致性和 4 项测试通过.
- Linux Go Supervisor 检查与 race 通过; 新的协议归属测试验证 Orbit、Astra、Comet 描述符和准入签名域.
- Linux Debug 的 10 项 CTest、6 项真实 RPC 场景、13 项进程回归通过. 最后补充的回复主版本断言再次通过 RPC 验证.
- Debug 独立存储/推流探针编译通过, 未执行吞吐排名或恢复旧 Rust 对照实验.
- Windows 构建入口的 6 项测试通过. 本轮未重跑 Release、ASan 或 TSan, 不引用上一轮成绩代替.
- 两端 154 个服务、协议、生成器及脚本文件哈希一致; 704 个冻结文件未变.
- 第三方安装目录内 3420 个文件的大小和修改时间保持不变; 所有测试进程已退出.

结构化证据: [protocol-v1-20260915.json](testkit/results/protocol-v1-20260915.json).

## 后续完整复验 (2026-09-15)

用户要求再次运行当前修改的测试后, Debug、Release、ASan/UBSan、TSan 均重新构建并通过各 10 项 CTest、6 项 RPC 和 13 项进程场景.
两端 Go 与协议生成检查、Linux Go race、Windows 44 项 Python 和 53 项 Admin 测试及生产构建也通过.
未发现 sanitizer 诊断, 未改动生产代码. 两端 338 个相关文件一致, 第三方安装产物未变, 测试进程已全部退出.
详细范围与限制见 [复验报告](testkit/results/protocol-v1-recheck-20260915.md); 上述首次迁移证据保留原样.
