# C++26 骨架维护性整理

日期: 2026-09-12. 本轮在连接骨架验收后整理代码归属、资源所有权和测试入口.
保持 `common / star / planet` 的结构与 gRPC v4 行为. 当前代码阅读入口见 [维护指南](CONTRIBUTING.md).

## 整理结果

| 项目 | 完成的改动 | 工程上的作用 |
| --- | --- | --- |
| 进程设施 | 信号、唤醒、JSON 日志移入私有 `common/src/process.*` | Runtime 集中协调生命周期, 进程设施不依赖 gRPC 类型 |
| 控制循环 | 分离准入推进与拨号, 明确 step 次序、incoming 与 sessions 的所有者 | 关闭顺序和失败路径更容易独立审查 |
| 所有权 | Signals / Logger 禁止复制, Admission 禁止搬移 | 防止重复恢复进程状态或使 gRPC 借用的请求地址失效 |
| 传输分类 | `rpc_status.hpp` 共用稳定错误码转换 | Session 不再包含 Supervisor 准入接口; 回收时避免临时 Error 字符串和重复分类 |
| 角色状态 | 移除未使用的 `require_exact_id` 标志 | Star 精确匹配与 Planet 合法代次替换仍由角色策略负责 |
| Planet 查重 | 在八槽 `inplace_vector` 中直接比较已准备成员 | 移除三组临时集合及额外地址编码副本, 最多比较 28 对候选 |
| 测试诊断 | 六份 CHECK 合为 `common/tests/check.hpp`, 使用 `source_location` | Debug/Release 都检查, 失败包含文件、行号和表达式 |
| 维护文档 | 增加依赖图、模块归属、回调规则和验证选择表 | 后续改动有具体入口, 不需要从整个 Runtime 推断职责 |

`runtime.cpp` 从 412 行调整为 321 行. 新进程设施的头文件与实现共 164 行, 含接口和中文注释;
这个数字表示职责划分, 不能解释成总代码量减少. 实际去重来自未使用的标志、三组临时集合、
重复错误处理和测试宏. Star 的较大成员表继续使用有序容器, 没有照搬八候选场景的线性查重.

## 修复的入口问题与跨语言审查

**零测试返回成功.** 旧入口可能继承 `BUILD_TESTING=OFF`, 而默认 CTest 对空集合返回 0.
已经用真实 CTest 复现: 默认行为退出 0, `--no-tests=error` 退出 8.
Peer 入口现在主动恢复 `BUILD_TESTING=ON`, 并拒绝零测试成功. C++ SDK 的独立 `test` 路径也补上同一检查.
仓库要求的 CMake 3.28 已覆盖此选项, 无需升级工具.

**诊断选项被静默省略.** core-only 提前返回曾使分配测量或推流目标未构建, 请求却仍可能成功.
Python 入口和直接 CMake 配置现在都拒绝不支持的组合. sanitizer 字符串采用有限白名单,
`--benchmark-smoke` 必须用于 benchmark 操作.

**空运行库搜索项.** 拼接空的继承值会得到末尾分隔符, 在 Linux 上额外搜索当前目录.
修复 `cluster-cpp/build.py`, `prepare_dependencies.py` 和共享 `testkit/suites.py`;
显式提供的继承路径继续保留, 设置仅作用于子进程.

| 审查路径 | 结论 |
| --- | --- |
| C++ Star / Planet | 共用入口和进程设施, 修复同时生效 |
| C++23 SDK | 独立 test 存在同类空 CTest 问题, 最小修复一个参数, 构建策略测试通过 |
| Rust Peer / SDK, Go Supervisor / SDK | 不消费 CMake 测试开关, 也没有发现同样的 LD_LIBRARY_PATH 拼接路径 |
| C# / Python 原生运行库测试 | 通过共享 native_environment 受到空搜索项影响, 已修复并补回归 |
| gRPC 协议与生成器 | 本轮没有修改字段或生成消息, 生成一致性和跨实现互通验证通过 |

Admission 禁止搬移属于所有权约束加固, 此前实际 Runtime 一直通过唯一指针固定持有它;
没有把这一潜在误用描述成已经发生的网络故障.

## 验证结果

最终行为代码完成后运行以下检查. 机器可读证据及代码/产物指纹见
[维护验收结果](../testkit/results/peer-cpp-maintenance-20260912.json).

| 配置或范围 | 结果 |
| --- | --- |
| Debug | 8 组 CTest + 8 组真实 RPC/互通 + 13 组进程场景通过 |
| Release 默认一键入口 | 8 组 CTest + 8 组 RPC/互通 + 13 组进程场景通过, Go vet/单元/race 与协议检查通过 |
| ASan / UBSan | 8 + 8 + 13 组通过, 发现诊断立即失败, 未增加抑制规则 |
| 完整 TSan | 8 + 8 + 13 组通过, 使用独立插桩依赖前缀 |
| Release 短故障循环 | 请求 60 秒, 实际 61.531 秒, 9 轮故障恢复, 4 Star / 2 Planet |
| Python 构建入口 | Windows / Linux 各 6 项通过, 包括实际 CMake 缓存恢复与空 CTest 失败 |
| 共享 Python 编排器 | Windows 32 项通过, Linux 31 项通过及 1 项平台跳过 |
| C++ SDK 构建策略 | Windows 12 项通过, 未扩大到 SDK 数据库回归 |
| 格式与语法 | 手写 C++ clang-format、修改 Python 的 Black/语法检查及 git diff 检查通过 |
| 公开头文件 | 4 个头文件分别独立编译通过, 只提供公共 include 路径 |
| 原生 CMake 配置 | 4 种无效组合按预期诊断失败, 没有把无关编译失败算作通过 |
| 安装树 | 两个程序、根许可证及授权目录 13 个文件到位, --help/--version 通过, 无开发机 RPATH |

ASan/UBSan 覆盖项目手写目标; 完整 TSan 还包括生成代码与第三方 C/C++ 库.
两者均不声称覆盖系统运行库或第三方汇编. 安装检查显式为子进程提供匹配的 GCC 运行库,
不是依赖操作系统碰巧已安装同版本.

收尾统一了三行注释的英文标点. Debug、Release、ASan、TSan 均重新构建,
两套程序的 SHA-256 与本轮已经通过测试的对应产物逐一相同. 完整前后指纹保存在机器可读证据中.

新增验证直接检查以下行为: 缓存关闭测试后仍实际执行测试, 空测试必须失败,
无效构建选项被拒绝, 继承环境没有被改写或追加空搜索项, Planet 跨组别名被拒绝且旧名单保持完整,
信号处理器恢复、日志管道背压和并发唤醒. Windows 的 CMake 夹具只验证构建入口策略,
不作为 Windows C++26 服务已经支持的证据.

本轮所有进程场景都确认子进程退出、端口可复用和持有的临时目录清理.
短循环观测到 Star 单进程 RSS 峰值最高 19,064 KiB, Planet 最高 17,356 KiB;
最低可用内存 1,437 MiB. 这些是短时采样值, 不构成长期内存上界或容量承诺.

主日志位于 `build/cluster-cpp/cleanup-debug-final-20260912.log`, `cleanup-asan-20260912.log`,
`cleanup-tsan-20260912.log`, `cleanup-release-20260912.log` 和 `cleanup-inspection-20260912.log`.
首次 Debug 检查日志另保留, 最终报告采用加入 Planet 查重回归后的版本.

## 完成边界

这次完成的是连接骨架的维护性整理和相应验收. 业务存储、复制与 SDK Bind 仍按后续设计推进.
此前的一小时长测与 225 组推流对照属于 [原验收报告](qualification-20260912.md) 中的代码和产物,
本次没有重新执行整套性能矩阵或把旧成绩改挂在新产物上.

当前支持范围仍为 Linux x64 / GCC 16.2.0. clang-tidy 对 GCC 反射扩展的解析限制仍在;
严格编译和 sanitizer 成功不等同于该分析器已经通过. 安装产物需要部署环境提供匹配的 GCC 运行库.
本轮复用已批准的工具和项目缓存, 没有下载依赖、修改全局配置或执行 commit / push.
