# Verdandi 服务端协议

v4 使用 TLS 1.3/h2 和 gRPC, 不再使用自定义 MessageID/长度帧头.
包名保留 `verdandi.peer.v1`, Hello 主版本为 4; 包路径不表示旧协议兼容性.

| Schema | 用途 |
| --- | --- |
| `peer.proto` | Hello、Ping/Pong、成员、登记和有限错误类型 |
| `admission.proto` | Supervisor 的 Challenge / Register 一元 RPC |
| `peer_transport.proto` | Star/Planet 的 OpenSession 双向流, SessionPacket oneof |

账号密码只进入 Supervisor 请求. Member 是独立签名正文, 由 Ed25519 签署
`verdandi-admission-v4` 加一个 NUL 字节和原始 Protobuf 字节. 验证方验证收到的原始字节,
不重编码后验签. 凭证为 bearer, 无 exporter 或额外进程签名.
详细角色和限额见 [gRPC 实现](../peer/grpc-implementation.md).

## 显式生成

生成器使用 protoc 36.1、Prost/Tonic 0.14 系列、protoc-gen-go 1.36.12 和
protoc-gen-go-grpc 1.6.2. 工具默认路径分别为:

```text
build/tools/protoc/36.1/bin/protoc[.exe]
build/tools/protoc-gen-go/1.36.12/protoc-gen-go[.exe]
build/tools/protoc-gen-go-grpc/1.6.2/protoc-gen-go-grpc[.exe]
```

生成到 `peer/common/src/generated` 和 `supervisor/internal/generated`, 随 schema 一同提交.
C++ 生成入口为 `python3 peer-cpp/build.py generate`, 使用额外的 `grpc_cpp_plugin 1.84.0`,
生成到 `peer-cpp/common/src/generated`. `check-generated` 逐字节比较; C++ 的普通构建同样不运行生成器.
生成器先在项目内独占临时目录完成全部生成和格式化, 成功后更新源码, 结束后清理临时文件.
普通服务构建只编译已有源码, 不调用 protoc、不开启下载. 修改 schema 后显式执行:

```powershell
./scripts/generate-proto.ps1
./scripts/generate-proto.ps1 -Check
```

```bash
bash scripts/generate-proto.sh
bash scripts/generate-proto.sh --check
```

检查模式不写源码, 任一生成差异均失败. 一键服务检查包含此步骤.
Rust 生成的 RPC 使用标准 Prost codec 的有界适配, 在反序列化成员列表前扫描对象数量,
避免空 repeated 条目导致字节预算内的大量对象分配.

## 历史编号与兼容性

`message-ids.lock` 继续记录旧自动编号历史, v4 gRPC 调度不使用它.
已有编号和删除消息占位永不复用. 显式生成新增顶层消息时继续追加编号,
四项生成器测试检查确定性、重排、删除、冲突及只读行为.
已删除的 public_key 和 session_proof 字段在 schema 中 reserved, 不复用字段号.
PlanetRegistrationResponse 仅为 v3 类型历史保留, v4 两个角色统一返回 RegistrationResponse.

新协议需 Supervisor、Star、Planet 同时升级, 不提供旧 TCP 后端回退.
业务存储、Catalog/Registry 复制和 SDK Bind 消息尚未实现.
手写注释采用中文和 ASCII 标点, clang-format 按当前配置使用 4 空格.
bytes 字段生成 Rust Bytes, 不宣称端到端零拷贝.
