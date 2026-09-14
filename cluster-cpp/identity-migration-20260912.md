# 不透明 id 与 Star/Planet 命名迁移验证

本轮实现以[身份契约](../cluster/identity-contract.md)为准. 旧 Rust 服务不参与 v5 验收.

| 范围 | 结果 |
| --- | --- |
| Go Supervisor | Windows 格式、模块验证、vet、全包测试及构建通过; Linux 全包 race 通过 |
| 协议生成 | Go/C++ 生成一致性通过; 生成工具 Clippy 与 4 项编号分配测试通过 |
| C++26 | Linux/GCC 16.2 Debug 编译通过, 含隔离推流夹具; 8 个 CTest 套件全部通过 |
| 真实 TLS/gRPC | 6 组场景通过, 包括握手、限额、重复会话、候选刷新和关闭回收 |
| 真实服务进程 | 13 组场景通过, 包括 Supervisor 重启、Star 替换、Planet 换绑与端口重用 |
| Python 服务测试器 | 13 项测试通过, 废弃 Rust 实现选项会被拒绝 |
| 管理端 | 53 项测试、Vue/TypeScript 检查、模块边界、格式和 Vite 生产构建通过 |

新增身份回归覆盖: 不透明 Unicode/特殊字符 id、签名域隔离、字段篡改、响应丢失重试、
数据库重开后的幂等、首次基线竞争和旧票据不能覆盖新实例. C++ 日志会转义控制字符.
race 首次执行揭示测试助手把 Challenge/Register 两次鉴权合用旧 3 秒预算;
将测试助手预算改为覆盖两次 RPC 后, 全包 race 复跑通过. 没有改变生产服务的握手期限.

机器报告:

- [TLS/RPC](../testkit/results/cluster-identity-rpc-20260912.json)
- [组网进程](../testkit/results/cluster-identity-services-20260912.json)

验证使用虚拟机独立目录 `build/identity-review/source`, 复用已有工具与依赖, 使用测试持有的新数据库.
原有长时测试、部署进程和数据库未被替换. 报告摘要对应其中记录的实际测试产物;
随后生成的 enum 注释与历史编号追加不改变控制协议消息行为, 已单独检查生成一致性和编号测试.

本轮未重做性能、ASan/UBSan、TSan 或耐久验收, 旧 v4 成绩不能转记为 v5 成绩.
旧推流比较依赖已废弃的 Rust v4 接收器, `benchmark` 当前明确拒绝执行; v5 接收器尚未实现.
