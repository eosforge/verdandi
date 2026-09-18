# 贡献指南

Astra 使用根目录 [MIT License](LICENSE). 当前服务骨架处于开发阶段, 欢迎通过可复现缺陷、测试和小范围改进参与.

修改前阅读 [项目入口](README.md)、[编码规范](coding.md) 和 [服务基础规范](service-foundation.md).
C++ 工作还须先阅读独立的 [C++ 编码规范](cpp-coding.md), 具体编码习惯以该文为准.
Star / Supervisor 的已实现行为分别记录在各自 README; 设计文档中的未来能力不能当作当前 API 使用.

1. 为改动选择真实职责边界, 避免无业务需求的分层、接口和新依赖.
2. 使用各语言格式化器. 当前评审阶段源码注释为中文和 ASCII 标点, 发布前再统一转换英文.
3. 为修复增加能够重现错误的行为回归, 同时检查其他语言是否存在同类问题.
4. Linux 执行 `bash scripts/check-services.sh`; Windows 检查 Go 使用 `scripts/check-services.ps1 -Service supervisor`, C++ 服务检查在 Linux 执行.
   C++26 服务当前要求 Linux, 说明实际测试的平台和未验证部分.
5. PR 解释原问题、改变后的行为和验证结果, 协议改动同时审查 schema、编号清单与兼容性.

默认构建只使用已有缓存, 缺少依赖时按 owning README 准备. 不提交 `build/`、个人配置、凭据或运行日志.
依赖变更需要明确名称、版本、来源、许可证、用途和缓存位置, 不顺带升级无关包.
旧 Verdandi Redis SDK 已冻结, 不迁移为 Astra SDK. 新功能只使用活动服务和协议入口; 历史 SDK 测试不计入 Astra 验收.

服务测试按职责归位: Common 的纯逻辑由 Common 测试, Star/Planet 各自维护拓扑与生命周期场景,
Go/C++ 的身份规则共用 `cluster/tests/fixtures/admission-v1.json`. 不通过复制同一用例到多个 crate 增加测试数量.
新增 RPC 错误分支须说明重试还是终止, 覆盖取消及资源释放; 新持久字段须覆盖损坏输入、重开与失败事务不改变状态.
账号文件与成员解码另有 Go fuzz 入口, 可用 `scripts/check-services.ps1 -Service supervisor -FuzzSeconds 10` 或对应 Bash 参数运行.
测试配置必须使用公开夹具或本次测试拥有的临时副本, 不提交真实部署的登录信息、签名私钥或用户数据.
