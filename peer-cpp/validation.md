# C++26 Star / Planet 首版验证记录

日期: 2026-09-11. 状态: 接收交接竞争已修复, 最终 Debug/Release、ASan/UBSan、完整 TSan 与一小时故障循环通过.
按维护者要求, 保存本轮 TSan 结果后停止执行; 其余设计验收及实现暂停, 等待维护者通知.

机器可读的场景、产物摘要、资源采样和历史失败索引见
[汇总结果](../testkit/results/peer-cpp-foundation-20260911.json). 当前状态为 regression_passed_work_paused, 不代表 M0..M4 全部完成.

## 已有证据

| 范围 | 结果 |
| --- | --- |
| Linux GCC 16.2, 完整 Debug 构建/CTest/进程回归 | 最新接收交接修复后 5 组 CTest 通过, 进程场景另在 Release/ASan 验证 |
| Release 构建/CTest/进程回归 | 通过; 60 秒故障预检实际运行 62.449 秒, 9 轮重启, 自动清理通过 |
| 完整 ASan + UBSan CTest/真实 RPC/混合互联/进程回归 | 最新修复后 5 组 CTest + 8 组 RPC/混合场景 + 13 组进程场景通过; 历史 SEGV 及复现证据仍保留 |
| 核心 TSan CTest | 通过; 含角色快照并发读写, 后续测试扩展后重新通过 |
| gRPC 适配与严格编译 | 身份、准入、会话、运行时和最终测试源码均已完成真实编译/链接 |
| Windows Python 测试工具 | 30 项通过, 包含 sanitizer 失败判定与异常退出后的目录清理 |
| Linux Python 测试工具 | 29 项通过, 1 项 Windows 文件替换语义测试按平台跳过 |
| Go/Rust 协议生成一致性 | 最终 Hello 顺序注释更新后通过 |
| Rust 准入顺序修复 | Windows / Linux 各 54 项单元/网络测试通过, Linux Release 对照产物已重建, 无下载 |
| TSan 完整服务 | 项目、生成代码及第三方 C/C++ 库插桩后, 5 组 CTest + 8 组 RPC/混合场景 + 13 组进程场景通过; 自动清理通过 |
| 一小时故障循环 | 修复后实际运行 3603.207 秒, 510 轮故障恢复, 13 组进程场景及自动清理通过 |

最终接收/失败状态复查后的日志为 `build/peer-cpp/debug-accepted.log`, `asan-accepted.log` 和 `release-hour-fixed.log`.
历史的 `debug-final.log`, `asan-final.log`, `release-hour.log` 保留, 名称中的 final 不代表最终版本通过.
长时入口使用 Release; 计时只覆盖故障循环, 不把编译和前置检查算进一小时.
每轮停止 Planet 当前连接的 Star, 验证切换/重新组网, 并重启 Supervisor. 测试按时间结束, 不要求固定轮数.
本次最终长时原始报告为 `services-1789125200056444977.json`; 包含前置进程场景与清理的总时间为 3627.467 秒.
旧版本的 2808.645 秒、396 轮因修复主动中止, 单独保留为未完成记录, 不与新版本累加.

最终 TSan 日志为 `build/peer-cpp/tsan-final.log`, CTest 用时 16.76 秒;
RPC 报告为 `peer-cpp-rpc-1789132888205834959.json`, 进程报告为 `services-1789132925763255264.json`.
13 组进程场景用时 37.837 秒, 进程已回收、端口可复用、持有的临时目录已删除.
使用 `TSAN_OPTIONS=halt_on_error=1:exitcode=66`, 未增加竞争报告抑制规则. 本次不是新的一小时故障循环.
链接检查曾发现 `utf8_range_DIR` 沿用普通构建缓存, 已在构建入口切换前缀时一并清除并重新链接.
最终 Star/Planet 的 CMake 缓存和链接命令未再引用普通 `linux-gcc16/install` 依赖目录.

## 连接与资源实测

以下为修复后一小时入口的 Release 前置场景, 来自 `peer-cpp-rpc-1789125171274001193.json`:

| 项目 | 实测 |
| --- | --- |
| 本机 TLS/gRPC 控制 RTT, 预热 20 次后 500 次采样 | p50 636.234 μs, p95 846.847 μs, p99 950.452 μs, 最大 1021.560 μs |
| 24 条未完成 TLS 握手 | Star FD 从 11 到 35, 期限后为 8 |
| 不读取响应的 RPC 对端 | 尝试发送 75 个 Ping 后流关闭, 观测耗时 8 ms |
| 已完成 TLS 但不提交 HTTP2 请求的连接 | 2 条均回收; 13.322 秒时观察到关闭, 不是精确关闭时刻 |
| RPC 场景完成后的 Star | RSS 19,452 KiB, 9 个线程, 8 个 FD |
| 独立准入边界测试全程峰值 RSS | 21,496 KiB, `/usr/bin/time -v` 采样, 退出码 0 |

RTT 是闭环 Ping/Pong 往返时间, 不能解释成单向延迟或 Catalog/Registry 更新吞吐.
测试期间另有单任务 TSan 依赖编译, 这些数据只作功能与资源观察, 不作语言性能比较.
准入测试峰值也不是所有合法或恶意 2 MiB 消息的最坏内存上界.

一小时运行使用 4 个 Star、2 个 Planet 和 Go Supervisor, 最低可用内存为 1189 MiB.
以下均为每轮采样, 不是连续监测得到的全时刻上界:

| 角色 | 初始 RSS | 最终 RSS | 采样峰值 RSS | 最终线程 / FD |
| --- | --- | --- | --- | --- |
| Star, 4 个位置 | 18,596..19,064 KiB | 18,584..19,180 KiB | 19,456 KiB | 各 9 线程, 14..16 FD |
| Planet 0 | 17,204 KiB | 18,884 KiB | 19,028 KiB | 9 / 9 |
| Planet 1 | 17,112 KiB | 18,604 KiB | 18,696 KiB | 9 / 9 |
| Supervisor | 16,232 KiB | 14,316 KiB | 16,232 KiB | 6 / 9 |

Star 和 Supervisor 会在循环中重启, 初末样本不代表同一个 PID 的内存变化.
两台 Planet 持续存活, 最终 RSS 比初始分别增加 1680 / 1492 KiB; 仅凭这些采样不判断为泄漏或证明内存恒定.
测试结束后已 join 所有持有进程、确认端口可重用并清理持有目录, 不继续后台运行故障循环.

代码和测试集中在 `peer-cpp/`. Rust 保留在 `peer/`, Supervisor 保持 Go.
Redis SDK、数据存储/复制、Catalog/Registry 转发与 Admin 不属于本轮迁移范围.

## 设计验收项与测试对应

本轮 TSan 已通过, 但它不是整个设计文档的最后一项验收.
连接功能已实现并取得下列证据; 以下缺口按维护者要求暂停, 补齐后才能宣称 M0..M4 全部完成:

| 尚未完整验收的设计项 | 当前差距 |
| --- | --- |
| 第 10.1 节编译期负例 | 生产代码有编译期描述检查, 尚无独立的预期编译失败用例来确认名称冲突、缺失注解和未支持类型的诊断 |
| 第 10.1 节契约关闭对照 | 现有构建启用 enforce; 尚未用关闭契约的相同源码执行行为对照 |
| 第 11.1 节控制面规模与成本 | 已测 4 Star / 2 Planet、控制 RTT、RSS、线程和 FD; 尚未完整记录加入/收敛速率、分配次数/字节及更大规模阶梯 |
| 第 11.2 节独立推流性能夹具 | 尚未完成 C++ 与 Rust 的同条件小消息/扇出/慢下游对照及交错重复; 控制 Ping/Pong 结果不能代替 |
| 默认服务入口与最终收尾 | Rust 默认入口仍保留, 完整质量门槛通过后再处理入口及最终验收状态 |

以上属于骨架自身的验收缺口. 业务存储、Catalog/Registry 同步和 SDK 接入仍属设计明确排除的后续阶段,
不能用后续业务尚未实现解释或掩盖本阶段的验收缺口.

| 设计项 | 当前用例位置与覆盖 |
| --- | --- |
| G1 TLS 与目标验证 | `test_processes.py` 的 TLS 1.3/h2 正例与 TLS 1.2 负例; `admission_test.cpp` 的 SAN、根和过期负例 |
| G2 RPC 前连接清理 | 24 条不完整 TLS、2 条 TLS 后不提交 HTTP2 请求的连接; 停机时另建 8 条不完整握手, 检查退出和端口释放 |
| G3 身份与逻辑会话代次 | `core_test.cpp` 的 epoch/generation 替换; `rpc_probe.cpp` 的同 Channel 重用、重复活动流及独立 subchannel pool 重复拒绝 |
| G4 角色拓扑 | `testkit.services` 的 Star 全互联、Planet 本组优先/跨组切换/单上游; `test_processes.py` 的 Rust/C++ 双方向混合组合 |
| G5 背压与期限 | `session_test.cpp` 的四槽队列、Pong 绝对期限与停滞写; `rpc_probe.cpp` 的慢读者; 单独记录框架流控默认值和实际资源 |
| G6 解码边界 | `identity_test.cpp` 的签名原文和未知字段; `admission_test.cpp` 的超字节/超成员数/空成员与失败不安装状态 |
| G7 准入与拨号取消 | `connect_test.cpp` 的 DNS/TLS 停滞及取消; `admission_test.cpp` 的 Challenge/Register 共用总期限和丢响应重试 |
| G8 回调与缓冲区生命周期 | `session_test.cpp` 的受控关闭及 50,000 次跨线程交付; ASan/UBSan 已通过, 完整 TSan 结果另列 |

上述是测试分组, 不把一次 Ping、一轮故障循环或一次断言分别计成独立测试用例.
当前规模为最多 4 个 Star 和 2 个 Planet; 不能由此声称容量参数允许的全部规模已经得到验证.

## 工程结构与精简

- C++ 生产目录目前为 21 个手写源文件/头文件、2,408 行, Rust 对照的 src 目录为 23 个文件、3,133 行.
  C++ 包含 common/include 公开头文件; 统计包含注释与空行, 排除 generated 和独立 tests 目录.
  两者 TLS 适配与历史测试摆放不同, 不能把差值当作等功能优化比例.
- CLI 解析、必填/范围校验和帮助共用反射注解, 避免并行维护选项分派与帮助表.
- Common 持有配置、身份、准入、会话和生命周期; Star/Planet 各自维护角色规则, 没有复制两套网络服务.
- 八候选使用 `inplace_vector`, 四条待发送控制消息使用固定槽. 消息解析与 gRPC 内部仍可能分配和复制,
  当前没有业务 payload 路径, 不声称端到端零拷贝或分配次数已经全面下降.
- 默认窗口交给 gRPC; 未引入 HTTP2/TLS 私有补丁、自建线程池、通用序列化框架或内存池.

## 本轮审查修复

1. 反射注解必须使用 structural type. CLI 元数据使用内嵌字符数组, 默认值和帮助由同一声明生成.
2. 控制循环先记录唤醒序号再处理事件, 通知和等待使用同一互斥锁, 避免丢失通知导致额外轮询等待.
3. Hello、Ping、Pong 排队/在途写入均有绝对截止; 关闭后停止投递, 最终回调后才回收 reactor.
4. EOF 不立即覆盖远端 gRPC 状态. 服务端先尝试 Finish 返回有限错误码, 停滞写入限时取消.
5. 入站 RPC 未验证拨号方之前不披露服务端 bearer 凭证. Rust 与 C++ 同步修复, 不增加字段或额外鉴权消息.
6. C++ 的响应元数据和凭证分离: 先返回不含凭证的 gRPC 响应头, 允许流式客户端继续提交 Hello.
7. 初始化失败和信号安装部分失败均有清理路径. 取消前尚未启动的会话不再发送 Hello.
8. 数值 Supervisor 地址被拒绝后不能被当作 DNS 名绕过检查; 嵌入 NUL 的 IP 文本显式拒绝.
9. 控制队列同时检查四个槽的截止, 不假定入队次序等于期限次序. 响应头停滞时后入队 Pong 仍按自己的期限取消.
10. Linux 信号可能送到任意 gRPC 线程, 退出标志改为编译期确认无锁的 atomic_bool, 不依赖 volatile 在跨线程间同步.
11. Planet 按已观察的当前 group/UUID 选择候选. Star 重启并更改分组后, 不继续依赖旧名单的存储排序.
12. 同组候选按进程 UUID 加盐选择, 避免所有 Planet 总拨号名单第一项. 这只改变本地选择, 不增加发现消息.
13. GCC 契约违例处理需要自带的 `libstdc++exp`, 已在手写目标的公共构建选项中链接, 不另下载运行库.
14. 真实 TLS 发现旧 Ed25519 测试证书不能与所选 BoringSSL 默认验证算法互通. 公共 TLS 夹具改为
    ECDSA P-256 / SHA-256, 保留 SAN、用途、过期和错误签名语义; 准入密钥、账号及签名算法不变.
15. ASan/UBSan 测试入口显式设置错误即失败, 防止 UBSan 可恢复诊断被成功输出折叠.
    TSan 使用独立前缀重新插桩第三方库和生成代码, 不通过 suppressions 把初次竞争报告隐藏.
16. 服务测试器的预期拒绝要求退出码 1, 不再把任意非零退出当作通过. 持续消费并保留 sanitizer 失败状态,
    即使强制停止也报失败; 正常停止检查已经提前退出的进程. 退出检查失败后仍释放所持有的临时目录.
17. `OnReadDone` 若在控制循环两次加锁之间到达, 旧实现只检查 `read_inflight` 就重新读取,
    尚未消费的 Protobuf 接收对象可能被覆盖. 现在同时要求没有待消费消息, 并在提交前复查失败/取消状态.
    专用并发用例在旧代码上因非空接收缓冲区失败, 修复后完成 50,000 次交付; 日志为 `receive-before.log` / `receive-after.log`.
    先前 ASan 报告定位到 Protobuf `MessageSize`, 缺少完整调用栈, 不把这个位置直接归咎于第三方库;
    新用例独立证实了本项目的接收交接错误, 修复后的最终 sanitizer/长时结果见前述记录.

## 扩散审查

- Bearer 预先发送问题在原 Rust Common 的握手函数中同样存在, 已修改公共路径, Star/Planet 共用修复.
  Go Supervisor 的 Challenge/Register 先进行账号鉴权, SDK 尚无该 PeerTransport 接口, 不受这条握手路径影响.
- TLS 算法差异来自 C++ 配套 BoringSSL 的默认验证列表, Go/Rust 原测试支持 Ed25519 TLS.
  三端共用新的 ECDSA 夹具进行互通回归, 独立的 Ed25519 准入向量保留. 此项没有修改 SDK TLS 配置.
- UBSan 默认可恢复的问题也适用于旧 C++ SDK 的 sanitizer 构建选项, 本轮记录为独立测试入口缺口,
  未修改或宣称完成 SDK 回归. Rust/Go 的现有原生测试没有相同的 UBSan 运行选项路径.
- 预期拒绝误吞异常退出的问题位于三端共用的 Python 服务测试器, 已在公共路径统一修复.
  原 Rust/Go 的 13 组进程回归和 C++ ASan/UBSan、TSan 回归已复测通过; 原生 CTest/Cargo/Go test 并不依赖此退出判定.
- TSan 前缀切换遗留 `utf8_range_DIR` 缓存的问题属于新 C++ 服务的 CMake 构建入口, 已统一修复 Star/Planet.
  Rust/Cargo 和 Go/Supervisor 不使用这套 CMake 包缓存; 现有 C++ SDK 没有本轮 gRPC TSan 双前缀路径.
- 接收交接错误影响共用 `RpcSession` 的 C++ Star 与 Planet. Rust 的 `Streaming::message().await`
  顺序取得独立消息后处理, Go Supervisor 使用 unary 请求, 没有本轮这种双检查之间重用 Protobuf 接收对象的路径.
- 新 C++ 控制队列的期限次序问题不在 Rust 同一位置扩散: Rust 按每次写入的绝对期限等待有界通道,
  没有“只检查队首期限”的数组算法. Go Supervisor 的准入是 unary RPC, 现有 SDK 不维护 Peer Hello/Ping 队列.
- C++ 信号标志问题未在其余服务发现同类共享 volatile 写入: Rust 使用 Tokio signal, Go 使用运行时信号通道;
  SDK 库不接管宿主进程的退出信号.
- Planet 候选次序问题未扩散到 Rust: Rust 每次重试用当前 group 做 min_by_key 选择.
  Go Supervisor 生成新名单时重新排序, SDK 尚无 Planet 候选状态机.
- C++ 新控制循环的通知顺序与 Rust/Tokio、Go channel 的运行模型不同. Rust SDK 的 Notify 等待者已先 enable
  再检查状态; C++ SDK 的订阅栅栏和 mailbox 修改在等待锁内, 未在这些路径发现相同丢失通知窗口.
- 另在旧 C++ Redis SDK `sdk/cpp/src/driver.cpp` 的连接池 shutdown 发现同类风险: closing 在锁外更新并先通知,
  之后才进入 pool_mutex. 极端竞态下等待者可能等到获取连接的 deadline 才观察关闭. 该项独立记录,
  本轮未修改 Redis SDK, 不能写成已修复或已通过 SDK 回归.

## 验证边界

- HTTP2 窗口和 BDP 使用锁定 gRPC 的默认值: 初始流窗口 65,535 字节, 初始帧上限 16,384 字节,
  BDP probe 默认开启且可按估计带宽/内存压力调整. 这是框架初值, 不是 Verdandi 协议要求.
  本轮未关闭流控, 应用消息上限和四槽队列独立生效. 来源为锁定源码的
  `http2_settings.h`, `chttp2_transport.cc` 和 `flow_control.cc`.
- TLS 算法差异可在锁定的 [BoringSSL extensions.cc](https://github.com/google/boringssl/blob/2b44a3701a4788e1ef866ddc7f143060a3d196c9/ssl/extensions.cc)
  中核对: 默认签名列表有 Ed25519, 默认验证列表未列出它. 这里描述的是 TLS 默认协商,
  不表示 BoringSSL 的独立 Ed25519 验签函数不可用.

- 首次上游默认 all 构建在 BoringSSL `crypto/asn1/asn1_test.cc:2328` 遇到 GCC 16.2 的
  `-Werror=array-bounds` 编译诊断. 所需运行库此前已经编译; 准备入口改为明确构建完整安装目标,
  保留原版本、源码和警告规则. 本轮不声称 BoringSSL 自带测试已通过, 也未判断该诊断是否属于编译器误报.
- 核心测试不能证明 gRPC callback 生命周期或真实网络正确性, 因而与网络结果分开记录.
- 普通依赖按 Release/O2 构建, ASan 不重新插桩第三方库. TSan 的全库插桩使用另一套显式准备产物,
  不能用普通依赖的成功构建代替. [TSan 官方说明](https://github.com/google/sanitizers/wiki/threadsanitizercppmanual#non-instrumented-code)
  指出缺少插桩可能造成误报、漏报和不完整堆栈, 因此初次混合构建的报告尚不足以判断真实竞争.
- gRPC ResourceQuota 是框架预算, 不等于整个进程 RSS 或文件描述符的硬上限.
- 准入采用已确认的可复用 bearer 方案, 凭证持有者可以重放它. 当前没有 TLS exporter 绑定、逐节点私钥持有证明或在线吊销;
  本轮修复的是未认证连接直接取得服务端凭证的顺序问题, 不把它描述成消除了 bearer 的重放属性.
- 入站 TLS 由 gRPC 的握手期限限制, 应用 Hello 从 RPC 被接纳开始计时; 不把两段计时伪称为同一个底层 TCP 起点.
- 当前仅完成连接骨架, 未声称 C++ 比 Rust 更快, 也未把配置容量上限当作已验证的部署规模.
