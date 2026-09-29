# 冻结组件

Verdandi Redis SDK 保留源码, 不再作为 Astra 新实现开发. 其协议、生成产物和测试不迁移为 Comet, 历史成绩不计入当前验收.

维护入口为 [工作约定](../AGENTS.md)、[开发规范](../astra/docs/development.md) 和 [编码与文件组织规范](../astra/docs/coding.md); C++ 写法遵循唯一的 [C++ 规范](../astra/docs/coding.md#cpp). 本目录保留现有 C++23 核心及低版本门面的兼容边界, 语言版本、ABI、Lua 和业务契约以当前头文件及下列组件说明为准. Astra C++26、Comet 协议和新三域模型不自动适用于冻结 SDK, 不因共用规范升级语言标准、改写生成/第三方代码或恢复开发.

仍有用途的最后版本使用说明保留在组件目录:

- [C++](cpp/README.md), [构建](cpp/BUILD.md), [C ABI](cpp/C_ABI.md), [低版本 C++ 门面](cpp/LEGACY.md).
- [Go Client](go/client.md), [Go Redis 投影](go/redis.md), [Rust Client](rust/client.md), [C#](csharp/README.md).
- [Registration API](docs/registration/api.md), [Catalog API](docs/catalog/api.md), [Lua](lua/README.md).
- [旧 SDK 测试工具](testkit/README.md), [共享协议向量](testkit/conformance/README.md).

旧根目录需求、架构、协议草案和审计报告只保存在 Git 历史. 上述冻结说明中指向历史文件的链接固定到清理前提交, 不替换成语义不同的 Astra 当前协议.
这些页面描述冻结代码, 不授予安装依赖、运行旧测试或重新推进开发的权限.
