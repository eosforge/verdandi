# Verdandi 服务控制协议

当前主版本为 6, 使用 TLS 1.3/h2 和 gRPC, 包名为 `verdandi.cluster.v1`.
只维护 Go Supervisor 与 C++ Star/Planet. 旧 Rust 服务已废弃.

| Schema | 用途 |
| --- | --- |
| `cluster.proto` | Hello、Ping/Pong、成员和单次登记 |
| `admission.proto` | Supervisor Register 一元 RPC |
| `star_transport.proto` | StarTransport.OpenSession 双向流 |
| `sync_transport.proto` | 业务同步草案, 仅生成类型, 当前服务未注册 RPC |

Supervisor 签发不透明 `string id`. 接收方验证签名和绑定, 不解析 UUID 格式.
仅已提交的 Hello 凭证使用 Ed25519 签名, 启动请求按随机键幂等, 详见[身份与准入契约](../cluster/identity-contract.md).

## 显式生成

Go 生成器自身使用已有 Rust 工具链, 不再生成废弃 Rust 服务代码.
使用 protoc 36.1、protoc-gen-go 1.36.12、protoc-gen-go-grpc 1.6.2;
C++ 另使用 grpc_cpp_plugin 1.84.0. 工具来自已准备的 `build/tools/` 或 C++ 依赖前缀.

```powershell
./scripts/generate-proto.ps1
./scripts/generate-proto.ps1 -Check
```

```bash
bash scripts/generate-proto.sh
bash scripts/generate-proto.sh --check
python3 cluster-cpp/build.py generate
python3 cluster-cpp/build.py check-generated
```

Go 输出在 `supervisor/internal/generated`, C++ 输出在 `cluster-cpp/common/src/generated`.
生成源码随 schema 维护, 不手工修改. 普通构建不调用 protoc、不下载工具或依赖.
检查模式只比较, 任何输出或文件集合差异均失败. 删除或重命名 schema 时需显式移除旧生成文件.

## 兼容性

v6 不兼容旧 v5/v4 的准入流程和签名域, 所有服务需一起升级, 没有旧 Rust 服务回退.
持久成员库的 `peer_id` 字段已改为 `id`, 旧库不会被静默覆盖, 参见身份契约.
`message-ids.lock` 保留编号, gRPC 不使用它分派消息. 字段号和历史编号不复用.
Catalog/Registry 业务流同步与 SDK 接入尚未实现. SyncTransport 中的实例/范围校验和快照衔接仍待实现,
生成源码不表示该服务已经开放. 见 [本轮整理说明](../cluster-cpp/sync-foundation-review-20260914.md).
