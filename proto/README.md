# Orbit / Astra / Comet v1

实现阶段统一为 v1, 无旧版本兼容、别名 RPC 或备用传输.

| 文件 | package / C++ 命名空间 | 职责 |
| --- | --- | --- |
| [orbit.proto](orbit.proto) | `proto.orbit.v1` / `proto::orbit::v1` | Supervisor Register、Member、Role 和准入凭证正文 |
| [astra.proto](astra.proto) | `proto.astra.v1` / `proto::astra::v1` | StarTransport、Hello、Ping/Pong、错误及未接入的 SyncTransport 草案 |
| [comet.proto](comet.proto) | `proto.comet.v1` / `proto::comet::v1` | 未来 SDK 接入边界, 当前无消息或 RPC |

手写 C++ 类型位于 `astra`. 传输适配使用 `wire = ::proto::astra::v1` 和 `orbit = ::proto::orbit::v1`.
Member 和 Role 仅由 Orbit 定义; Astra Hello 原样携带 Member 编码与签名, 不复制定义.
签名输入为 `proto.orbit.v1.admission` + NUL + 原始 Member 字节.
控制消息发送与接收共用主版本常量 1, 不把业务版本或成员 epoch 当作协议版本.

## 生成

```powershell
./scripts/generate-proto.ps1
./scripts/generate-proto.ps1 -Check
```

```bash
bash scripts/generate-proto.sh
bash astra/build.sh generate
bash astra/build.sh check-generated
```

复用项目 protoc 36.1、protoc-gen-go 1.36.12、protoc-gen-go-grpc 1.6.2、grpc_cpp_plugin 1.84.0.
Go 源码在 `supervisor/internal/generated`, C++ 源码在 `astra/common/src/generated`.
同一 Go 包 `wire` 按不同完整协议名注册描述符; Go module/import/go_package 仍对应暂未迁移的仓库地址.
生成器不下载工具, 普通构建不执行 protoc. 删除 schema 时显式移出对应旧生成文件.

`message-ids.lock` 只保留原自定义帧的编号历史, 不参与 gRPC 路由或提供兼容后端.
已有有效消息编号保持不变, 新顶层 Orbit Member 追加编号. 退役消息只保留编号, 不生成其消息类型.
旧帧辅助测试是历史测试, 不编入服务. 当前接口与行为验证以 gRPC 回归为准.

独立推流实验 schema 在 `astra/bench/proto/probe.proto`, 包为 `proto.astra.bench.v1`, 不注册到生产服务.
旧 SDK 和 Rust 对照材料冻结不变. 详细范围与验证见 [v1 修正记录](../protocol-v1.md).
