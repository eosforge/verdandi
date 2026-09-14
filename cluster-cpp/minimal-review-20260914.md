# C++ 连接骨架精简与 review

日期: 2026-09-14. 状态: 整理与验证完成, 等待维护者 review.

本轮以开始整理时的工作副本为基线, 不混入此前目录重命名或 Admin 的改动.
保持协议 v6、Supervisor 签发身份、现有 Star/Planet 连接规则和日志字段.
Planet 的业务推进继续暂停, 本轮没有新增 Registry、Catalog、复制或 SDK 代码.

## 实际精简

| 原实现 | 当前实现 | 原因 |
| --- | --- | --- |
| `HexId<N>` + `Principal` 别名 | 具体 `Principal`, 编解码在 types.cpp | 只有 32 字节部署摘要一个使用者, 不需要通用 ID 模板 |
| `DialTarget { Member member; }` | 直接传递 `Member` | 包装没有独立语义或校验, 还增加访问层次 |
| `due(now, budget)` 返回 vector | `due(now)` 返回 optional<Member> | Runtime 每次只拨一个目标, 无需批量接口及结果容器分配 |
| 通用 `Call<Response>` | `RegistrationCall` | 准入只有 Register 一种 RPC |
| 准入失败保留调用到下一轮 | 成功或失败都消费已完成调用 | 明确单次调用资源释放点, 回调保留独立共享所有权 |
| `closing_` + `error_` | 首次关闭原因表示关闭阶段 | 两者原本同步变化, 删除可出现不一致的重复状态 |
| `upstream` bool + `active_member` | 仅保留 `active_member` | 状态输出本来就使用成员快照, 删除未使用的布尔副本 |
| 已禁用 benchmark 的参数和分派代码 | 删除不可执行的命令与分支 | 帮助只列可执行命令; 历史实验单独保留 |

不通过删除注释、错误处理或测试来缩短代码. C++26 反射只用于现有 CLI 元数据,
不为未来业务添加注册表、插件层、协程包装或持久状态框架.
`Principal` 的编码与 Go 保持一致, 此次不是新身份协议.
内部 C++ 类型及策略函数签名有变化, 静态库、服务和测试需一起重编译; 不需要升级线协议.

本轮手写生产 C++ 从 2,738 行变为 2,731 行, 不含测试、bench 或生成文件;
其中非空且非整行注释的行数从 2,161 变为 2,152. 类型实现移入 `.cpp` 主要减少头文件负担,
不意味着整段逻辑消失. 构建入口从 333 行变为 261 行, 包含不可达代码删除及标准格式化.
12 个生产生成文件共 9,951 行, 继续由 protoc 管理, 本轮未修改.

## 建议阅读顺序

1. [types.hpp](common/include/verdandi/cluster/types.hpp): 进程 ID、部署摘要、成员及会话代次的区别.
2. [policy.hpp](common/include/verdandi/cluster/policy.hpp): 角色只收发独立值, 不持有 gRPC 对象.
3. [topology.cpp](star/src/topology.cpp): Star 名单安装、成员替换、单目标拨号与失败退避.
4. [admission.cpp](common/src/admission.cpp): 一次 Register、幂等重试、固定已安装身份.
5. [runtime.cpp](common/src/runtime.cpp): 单控制循环, 入站所有权转移和有截止的退出.
6. [grpc_session.cpp](common/src/grpc_session.cpp): Hello、心跳、四槽队列及读写回调交接.
7. [core_test.cpp](common/tests/core_test.cpp) 与 [session_test.cpp](common/tests/session_test.cpp): 状态约束及可控回调交错.

## 保留的必要边界

- `Principal` 识别同一部署, `MemberEpoch` 排序该部署的实例, `SessionGeneration` 防止旧本地完成删除新会话.
  它们不是 Catalog/Registry 的业务版本, 不能为减少类型数量而混用.
- 角色锁保护一致快照; 会话锁保护 gRPC 与控制循环的缓冲交接. 不在锁内处理角色验签或执行跨会话取消.
- `read_inflight` 与 `read_ready` 对应不同的缓冲所有者, 不能合并成一个“正在读”标志.
- `cancel` 与最终 `OnDone` 分开, 停止使用不代表回调已经结束. 保留四槽队列、原始截止和关闭预算.
- Star/Planet 的策略接口有两个实际使用者. Planet 已有行为继续回归, 未拆出第二套连接实现.

## 审核范围与验证

检查两种角色及其共享会话/准入路径, 对照 Go Supervisor 的 Principal 生成、成员绑定和幂等规则.
协议定义、生成文件、Go Supervisor 及既有 SDK 没有因本轮内部重构改变.
新增针对单目标拨号、退避、旧拨号失败不能清除新实例 pending、摘要格式和首次取消原因的检查.

| 验证项 | 结果 |
| --- | --- |
| Windows | 手写 C++ clang-format、修改的 Python Black 检查通过, 构建入口 6 项测试通过 |
| Linux Debug | 8 个 CTest 套件、6 组真实 RPC、13 组进程场景及生成一致性通过 |
| Linux Release | 同上, 全部通过 |
| Linux ASan/UBSan | 同上, 全部通过, 未报告内存访问或未定义行为错误 |
| Linux TSan | 同上, 全部通过, 未报告数据竞争 |
| 短故障恢复 | Release, 4 Star + 2 Planet + Supervisor, 实际 61.507 秒, 完成 9 轮重启恢复 |
| 输入对应 | 264 个相关源码、脚本、配置及测试输入的本地/虚拟机 SHA-256 一致 |
| 资源清理 | 进程回收、端口复用、自有临时目录删除均通过, 末次进程检查无本轮服务残留 |

四种配置运行同一套场景, 不将其相加描述为不同功能的覆盖数.
验证使用 `build/cluster-minimal-20260914/source` 的隔离源码与已有依赖, 单任务构建.
短故障循环最低可用内存为 1,668 MiB; 它用于回收与恢复验证, 不作为长时稳定性或性能结论.

详细场景、二进制和输入哈希、CTest 结果及资源采样见
[机器可读报告](../testkit/results/cluster-minimal-20260914.json).
本机 `build/cluster-minimal-20260914/review.patch` 保存仅针对本轮开始前工作副本的差异,
完整日志与原文件快照也保留在该项目内目录, 不与 Git HEAD 之前的未提交迁移混为一轮改动.

首次同步遇到 Visual Studio 锁定的 `.vs` 缓存, 已限定同步为源码并排除 IDE/构建缓存;
没有关闭编辑器、改动其缓存或将该准备失败当作服务测试结果.

## 交付边界

这是供维护者审查的连接基础, 不等同于业务服务已经达到生产发布条件.
没有本轮性能提升幅度或长时稳定性结论, 之前停止的耐久测试保持停止.
所有工具和依赖复用项目中已有版本, 不下载、不修改全局配置, 不 commit/push.
