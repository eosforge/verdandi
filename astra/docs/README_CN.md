# 当前文档导航

[English](README.md) | [简体中文](README_CN.md)

本目录及各组件 README 只描述当前有效内容. 历史方案、逐轮审计和旧验证报告由 Git 历史保留, 不另建工作区归档.

源码已包含 Almanac、Ephemeris、Catalog 三域、控制面、Comet C++ 与多 Star 恢复, 当前 C++ SDK 接口已接线, 见 [当前架构](architecture.md). 不再按旧 Catalog/Registry 两分法、Grant、架构级 standalone 或 Astrolabe 持久库开始新实现. 实施状态、实际验证和静态审核分别维护, 后续修改不自动继承以前的测试结果.

| 文档 | 负责内容 |
| --- | --- |
| [AGENTS.md](../AGENTS.md) | 授权、项目内工具和测试并行约定 |
| [AI 开发与长期维护](development.md) | 面向重要设计的跨语言高强度规范; 含覆盖门槛、可信证据、规则 ID、项目配置与公开依据 |
| [多语言编码与文件组织](coding.md) | 编码与文件组织的唯一正文; 包含 C++、C、Go、Rust、Python、JS/TS、前端、Java/Kotlin、C#、Lua、SQL、Shell 与协议配置章节 |
| [开发门禁与首个试点](features/verification.md) | 正式 Schema、命令与授权格式、门禁判定及 Polaris 存储试点; 执行状态见验证记录 |
| [Visual Studio 工程与 Windows 移植](features/windows.md) | 统一解决方案、各工程输出、标准库与平台边界、服务端移植契约; IDE/原生适配已实现, 完整服务端仍有阻断 |
| [架构](architecture.md) | 星图拓扑、组件边界、阶段与实施边界 |
| [协议](../proto/README_CN.md) | 当前 Schema、身份与签名、三域业务及来源恢复契约 |
| [构建与运行](build.md) / [维护指南](project.md) | 项目语言与工具约定、构建、启动、文件职责与生命周期 |
| [三域存储](../common/README_CN.md) | 原生记录、来源组、读取投影、提交与底层工具 |
| [Pulsar](../pulsar/README_CN.md) | 登记、只读目录、SQLite 成员库、物理参考与连续时间 |
| [Polaris](../polaris/README_CN.md) | Go/GORM 官方 SQLite 驱动、WAL 持久提交、有界历史与 Star 恢复 |
| [Admin](../admin/README_CN.md) | 真实管理入口、独立演示星图及前端开发; 完整 Orrery 暂缓 |
| [Astrolabe](../astrolabe/README_CN.md) | Go 管理与实时观测入口、部署管理账号、Polaris 提交与凭据管理 |
| [Comet C++](../comet/cpp/README_CN.md) | 客户端 API、所有权、恢复、计时与静态库交付 |
| [Comet Go](../comet/go/README_CN.md) | 后续原生 Go SDK 方向, 不作为当前 C++ 业务闭环的前置条件 |
| [Comet 验收](comet.md) | 首版业务场景、预期结果与用例映射; 执行结果统一见验证记录 |
| [测试方法](../tests/README_CN.md) / [最新验证](validation.md) | 测试方法与实际执行结果, 两者分开维护 |
| [性能测量](../bench/README_CN.md) | 当前三域、真实 Comet 推流和 Polaris 的有界离线测量方法; 结果仍在验证记录 |
| [分层探针](profile.md) | C++ 编译开关、采样开销、源码站点与离线分析边界 |
| [三 Star 长测](soak.md) | 真实四件套故障轮换、常驻数据核对、资源记录及清理 |

## 新版 SDK 设计入口

已确认的 Beacon、Observer、Publisher、Subscriber 和 Reader 接口集中在 [Comet C++ 公共接口](../comet/cpp/README_CN.md#c-公共接口), 生命周期与恢复规则在同一文档展开; [Go 映射](../comet/go/README_CN.md) 只描述后续实现边界.

[协议边界](../proto/README_CN.md#已确认目标与实现差异) 说明当前已接线的 Catalog 版本查询、Beacon 稳定身份/Update 延期及读取接口. [进度](architecture.md#sdk-implementation-and-verification-boundaries) 区分源码完成和执行验证. 实际执行结果只在验证记录维护.

## 维护约定

- 一个事实只在其所属文档定义, 其他文档链接引用. `GEMINI.md` 仅链接统一约定与文档入口, 不复制规则和工作日志.
- 已实现、已确认但未实现、待讨论三者必须区分. Schema 草案或通过连接测试不等于业务同步已经实现.
- 修改设计和实现时更新对应稳定文档. 最新验证写入固定的 `docs/validation.md`, 必须包含源码身份、平台、范围、失败与未验证边界; 不再新增日期化 Markdown 报告.
- 未重新测试时, 保留最新执行结果的原日期和源码身份, 不把它改写为当前工作区已通过. 被修复的问题在新结果中保留必要说明; 更早完整过程交由 Git.
- 临时分析、原始日志和一次性脚本放入忽略的 `build/`, 不用持续增长的 `worklog.md` 承载规范.
- 清理历史 Markdown 不代表删除协议向量、测试夹具、许可证或结构化原始证据. 它们按各自用途维护.
- 目录和链接属于文档契约. 移除入口时同步引用, 不保留自称权威的旧版跳转副本.

- 文档语言: development.md 和 coding.md 在同一文件中先英文后中文, 两部分保持一致; 各 README.md 使用英文并链接同目录 README_CN.md, 中文页提供返回链接. 其他自有 Markdown 使用英文. 上游许可证原文不翻译或改写. 修改内容、路径或标题时同时维护对应语言和链接.
