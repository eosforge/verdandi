# 贡献指南

Astra 使用 [MIT License](LICENSE), 当前为开发中的服务骨架. 先阅读 [项目入口](README.md)、[当前架构](docs/architecture.md)、[通用规范](coding.md); C++ 还须遵循独立 [C++ 规范](cpp-coding.md).

1. 明确当前改动的职责和范围, 区分已实现功能与业务设计草案. 不为未来需求预建空层或顺带推进冻结组件.
2. 使用已有格式化工具. 生成代码通过规范输入和生成器更新, 第三方文件与许可证保留原文.
3. 修复增加针对性用例并审查各语言、共享核心和消费者中的同类路径. 先完成整理, 再依 AGENTS.md 获得当轮测试授权.
4. 选择 [Testkit](testkit/README.md) 中适用的验证范围, 报告未执行及失败项. 缺少工具或依赖需具体下载授权, 不能隐式安装.
5. PR 说明具体问题、最终行为、验证与边界. 协议变更同时核对 schema、生成源码和所有活动消费者; 持久字段覆盖失败事务、损坏恢复和重开.
6. 当前事实更新到所属稳定文档, 最新执行结果写入 [validation.md](testkit/validation.md), 不新增逐轮日期化 MD 或持续工作日志. 不把历史成绩转记给新代码.

缓存、产物、原始日志和临时分析放在 `build/`; 不提交部署秘密、数据库或个人环境. 不修改用户/系统配置, 不在未获指令时提交或推送.
组件细则见 [C++ 维护指南](astra/CONTRIBUTING.md)、[Supervisor](supervisor/README.md)、[Admin](admin/CONTRIBUTING.md). 旧 SDK 的使用说明仅适用于 [冻结代码](legacy-sdk.md).
