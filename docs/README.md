# 当前文档导航

本目录及各组件 README 只描述当前有效内容. 历史方案、逐轮审计和旧验证报告由 Git 历史保留, 不另建工作区归档.

新业务已按 [当前架构](architecture.md) 实现 Almanac、Ephemeris、Catalog 三类数据及对应控制面、Comet C++ 与多 Star 恢复. 不再按旧 Catalog/Registry 两分法、Grant、架构级 standalone 或 Astrolabe 持久库开始新实现. 实施状态、实际验证和静态审核分别维护, 后续修改不自动继承以前的测试结果.

| 文档 | 负责内容 |
| --- | --- |
| [AGENTS.md](../AGENTS.md) | 授权、项目内工具和测试并行约定 |
| [coding.md](../coding.md) | 通用质量、语言工具与跨语言审查 |
| [cpp-coding.md](../cpp-coding.md) | C++ 唯一编码习惯定义 |
| [架构](architecture.md) | 星图拓扑、组件边界、阶段与实施边界 |
| [推进进度](progress.md) | 当前实施检查点、已完成边界、依赖与待验证项 |
| [代码审核](review.md) | 当前确认的问题、修复、性能机会、规范及测试缺口 |
| [协议](../proto/README.md) | 当前 Schema、身份与签名、三域业务及来源恢复契约 |
| [Astra](../astra/README.md) / [维护指南](../astra/CONTRIBUTING.md) | C++ 构建、启动、文件职责与生命周期 |
| [三域存储](../astra/common/README.md) | 原生记录、来源组、读取投影、提交与底层工具 |
| [Pulsar](../astra/pulsar/README.md) | 登记、只读目录、SQLite 成员库、物理参考与连续时间 |
| [Polaris](../astra/polaris/README.md) | Go/GORM 官方 SQLite 驱动、WAL 持久提交、有界历史与 Star 恢复 |
| [Admin](../admin/README.md) | 真实管理入口、独立演示星图及前端开发; 完整 Orrery 暂缓 |
| [Astrolabe](../astra/astrolabe/README.md) | Go 管理与实时观测入口、部署管理账号、Polaris 提交与凭据管理 |
| [Comet C++](../astra/comet/cpp/README.md) | 客户端 API、所有权、恢复、计时与静态库交付 |
| [Comet Go](../astra/comet/go/README.md) | 后续原生 Go SDK 方向, 不作为当前 C++ 业务闭环的前置条件 |
| [Comet 验收](../testkit/comet.md) | 首版业务场景、预期结果与用例映射; 执行结果统一见验证记录 |
| [Testkit](../testkit/README.md) / [最新验证](../testkit/validation.md) | 测试方法与实际执行结果, 两者分开维护 |
| [性能测量](../astra/bench/README.md) | 当前三域、真实 Comet 推流和 Polaris 的有界离线测量方法; 结果仍在验证记录 |
| [分层探针](../testkit/profile.md) | C++ 编译开关、采样开销、源码站点与离线分析边界 |
| [三 Star 长测](../testkit/soak.md) | 真实四件套故障轮换、常驻数据核对、资源记录及清理 |
| [冻结组件](../legacy-sdk.md) | 旧 SDK 的边界与必要使用说明 |

## 维护约定

- 一个事实只在其所属文档定义, 其他文档链接引用. `codex.md` 和 `GEMINI.md` 仅作会话入口, 不复制规则和工作日志.
- 已实现、已确认但未实现、待讨论三者必须区分. Schema 草案或通过连接测试不等于业务同步已经实现.
- 修改设计和实现时更新对应稳定文档. 最新验证写入固定的 `testkit/validation.md`, 必须包含源码身份、平台、范围、失败与未验证边界; 不再新增日期化 Markdown 报告.
- 未重新测试时, 保留最新执行结果的原日期和源码身份, 不把它改写为当前工作区已通过. 被修复的问题在新结果中保留必要说明; 更早完整过程交由 Git.
- 临时分析、原始日志和一次性脚本放入忽略的 `build/`, 不用持续增长的 `worklog.md` 承载规范.
- 清理历史 Markdown 不代表删除协议向量、测试夹具、许可证或结构化原始证据. 它们按各自用途维护.
- 目录和链接属于文档契约. 移除入口时同步引用, 不保留自称权威的旧版跳转副本.
