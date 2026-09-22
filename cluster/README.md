# 冻结 Rust 服务与共享夹具

`common/`, `star/`, `planet/` 是已经退休的 Rust 服务实现, 不作为当前 Astra 构建或回归入口.
当前服务位于 [astra/](../astra/README.md), 角色与实现边界见 [架构](../docs/architecture.md), 准入见 [协议](../proto/README.md).

[tests/fixtures](tests/fixtures/README.md) 仍供当前 C++/Go 测试使用, 不能因为 Rust 服务冻结而删除或重新生成其公开身份与协议向量.
本目录旧设计和逐轮验证由 Git 历史保存, 不再保留一套与当前服务冲突的规范.
