# Astra 回归记录 (2026-09-17)

## 范围

本轮由维护者明确授权补齐 astra/ 测试并运行回归. 复用 Ubuntu 项目内 GCC 16.2.0、gRPC/Protobuf 和两套普通/TSan 依赖前缀; 不下载工具或依赖, 不更改全局配置.

覆盖当前 C++ 连接骨架、内部 Store/Wheel、Proto Proxy 和与 Go Supervisor 的协议协作. 不启动长期耐久任务, 不实现 Catalog/Registry 网络业务、Planet 新功能或 SDK.

## 修正

- 跟随当前 Wheel 的五层默认值和 limit 名称更新测试, 覆盖压缩配置的五层级联及默认槽宽的高位边界.
- Store 初始化正的 tick 间隔, 按已确认的单调时间语义补拍. 续租以已推进的拍边界为基准, 有限截止向上取整, 长延迟保留原始截止并分段调度.
- 到期收集或历史分配失败后重排节点, 保留批次原子性和后续重试能力; 空拍不预分配记录数组. 恢复版本耗尽检查.
- 修复 Config/Options 的 galaxy 字段改名遗漏, 将 Supervisor 地址解析放回可复用端口解析的实现文件.
- 按当前 schema 重新生成 C++/Go 文件. Proto Proxy 使用真实的子消息与 repeated API, 修正 DeltaRecord 类型并独立包含所需生成头.
- 修复 Supervisor 测试夹具的误改路径及历史 JSON 向量字段映射, 保留夹具原始签名和哈希.

## 补充用例

- Store TTL: 非法间隔、边界与亚拍精度、重复/倒退时间、续租与缩短 TTL、取消、补拍批次、2048 条目扩容、删除重建、时钟极值、并发读写与补拍.
- 分配故障: 原有逐点注入增加租约可重试检查; 96 个长 Key 的批次逐点失败后继续续租、取消与清理; 空拍零分配检查.
- Proto Proxy: 标量、二进制字符串、oneof 切换、repeated 整体替换、复制/移动、源 Arena 销毁及序列化往返.
- CLI/准入: 新消息容量选项上下界、解析结果实际传递、请求发送上限及自定义响应接收上限.

## 验证结果

| Linux / GCC 16.2.0 配置 | CTest | TLS/RPC 场景 | 进程场景 | 清理 |
| --- | ---: | ---: | ---: | --- |
| Debug | 14/14 | 6/6 | 13/13 | 通过 |
| Release | 14/14 | 6/6 | 13/13 | 通过 |
| ASan/UBSan | 14/14 | 6/6 | 13/13 | 通过 |
| TSan | 14/14 | 6/6 | 13/13 | 通过 |

四配置合计 56 次 CTest 执行、24 次 RPC 场景和 52 次进程场景; 这些是相同场景在不同配置下的执行次数, 不表示互不重复的用例数量. Sanitizer 未报告内存错误、未定义行为或数据竞争.

ASan/UBSan 检查项目代码并使用已有普通依赖前缀. TSan 使用已有独立插桩依赖前缀, 生成协议也启用线程检查.

每种配置的组件结果一致:

- 168 个写入分配失败点和 7 个读取失败点通过, 包含失败后的到期重试.
- Wheel 完成 180000 步参考模型操作, 另有边界、生命周期、回调异常与五层级联用例.
- 正常时间轮路径 0 次分配、2048 次回调; Linux x64 上 Node 为 24 字节, 默认五层 Wheel 为 10264 字节. 不把这些数值解释为吞吐提升比例.
- 进程测试退出并验证端口复用, 汇总检查未发现仍运行的项目测试服务.

辅助检查结果:

- Windows Supervisor 单测与 Go vet 通过; Linux Supervisor 单测、Go race、Go vet、模块校验与离线构建通过.
- 两端协议生成一致性通过. Linux 生成器格式、Clippy 和 4 个生成器用例通过.
- Windows Python 测试工具 38/38 通过, 构建入口另 6/6 通过.
- Linux Python 测试工具 37 项通过, 1 项仅针对 Windows 文件替换语义的测试按平台跳过; 6 项构建入口测试已包含在各配置 cpp_build 中.
- 本轮修改的 C++ 格式、Go 格式、Python Black 和 Git 空白检查通过.

## 复现与证据

后续执行仍需遵守根目录 AGENTS.md 的当轮测试授权规则. 已准备工具的 Linux 项目可通过 `bash astra/build.sh regression --profile debug --benchmarks` 复现单配置回归, 将 profile 换为 release、asan、tsan 可运行其他配置.

两端原始日志保存在各自项目的 `build/astra-regression-20260917/`. `debug.log`、`release.log`、`asan.log`、`tsan.log` 是通过的完整回归, `*-ctest-final.log` 保存组件输出. 更早的 `service-build.log`、`debug-build.log`、`debug-ctest.log` 等用于排障, 不覆盖本节最终通过结论.

版本化摘要见 [regression-20260917.json](regression-20260917.json), 包含各次执行的场景、实际运行二进制摘要和组件结果. 源码核对清单位于 `build/astra-regression-20260917/source-hashes.json`; 两端测试源码保持一致, Linux 复用原有 build/astra 和依赖缓存.

Linux 遗留的五个 sync_store 旧名源文件已移入该证据目录的 `retired/`, 避免与当前 store 文件混淆; 它们未进入本轮 CMake 目标, 构建缓存保留.

## 边界

未计算行/分支覆盖率, 用例数量不等于覆盖率. 高层结构用压缩槽宽穷举验证, 不逐拍遍历默认五层的全部 40 位时间范围.

Delta 的单调截止仅用于本地检查. 当前 Snapshot 不携带完整租约恢复语义, 本轮不据此宣称跨主机 TTL 同步或重启恢复完成.
