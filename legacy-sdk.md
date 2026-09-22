# 冻结组件

Verdandi Redis SDK 与旧 Rust 服务保留源码, 不再作为 Astra 新实现开发. 它们的协议、生成产物和测试不迁移为 Comet, 历史成绩不计入当前验收.

仍有用途的最后版本使用说明保留在组件目录:

- [C++](sdk/cpp/README.md), [构建](sdk/cpp/BUILD.md), [C ABI](sdk/cpp/C_ABI.md), [低版本 C++ 门面](sdk/cpp/LEGACY.md).
- [Go Client](sdk/go/client.md), [Go Redis 投影](sdk/go/redis.md), [Rust Client](sdk/rust/client.md), [C#](sdk/csharp/README.md).
- [Registration API](registration/api.md), [Catalog API](catalog/api.md), [Lua](lua/README.md).
- [旧 SDK 测试工具](testkit/legacy-sdk.md), [共享协议向量](testkit/conformance/README.md).

旧根目录需求、架构、协议草案和审计报告只保存在 Git 历史. 上述冻结说明中指向历史文件的链接固定到清理前提交, 不替换成语义不同的 Astra 当前协议.
这些页面描述冻结代码, 不授予安装依赖、运行旧测试或重新推进开发的权限.
