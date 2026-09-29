# 冻结 SDK 测试工具

本目录只服务 [旧 Redis SDK](../README.md), 不用于 Astra/Comet 验收. 旧 Star/Planet/Supervisor、cluster-cpp 和 Rust 服务对照入口已经移除, 不再保留其运行说明. 本页维护仍存在的执行入口和验证边界, 不累计历史通过记录.

## 运行入口

以下命令从 sdk/ 根目录执行, 须先取得当前范围的构建和测试授权. 包装器只选择已有 Python, 调度逻辑位于 [run.py](run.py).

Windows:

```powershell
.\testkit\run.ps1 regression --targets local --languages go,rust,cpp,csharp
.\testkit\run.ps1 soak --targets local --duration 2h
```

Linux:

```bash
bash testkit/run.sh regression --targets local
bash testkit/run.sh soak --targets local --duration 2h
```

Windows 可用 --targets linux 调度 Ubuntu 副本, 或 local,linux 依次覆盖两端; Linux 直接使用 local. --languages 可限定 go,rust,cpp,csharp, 缩小范围的结果不证明其他语言通过. --plan 只输出计划, --preflight-only 只检查前提, 都不能作为回归结果.

soak 的 --duration 范围为 210s..24h, 默认 2h, 指每个业务域、每个目标的有效负载时间. Registration 和 Catalog 串行, 因而单目标 2h 至少需要四小时负载, 准备和清理另计. 专用持续负载以当前 Go 驱动为主, 不能宣称四种语言都经过同样时长的耐久测试.

## 环境与配置

配置示例见 [config.example.json](config.example.json), 本地副本放在忽略的 build/testkit/config.json. remote_project 必须指向 Ubuntu 的 sdk 根目录, 例如 /home/ubuntu/verdandi/sdk. 凭据不写入公开配置或日志; 密码交互输入, 无人值守可使用运行器支持的进程环境变量. known_hosts 位于 build/testkit/.

需要已有 Python 及 [requirements.txt](requirements.txt)、选中语言工具链和依赖. C# 需要对应的原生共享库; Redis 场景需要夹具主机已有 Docker 和指定镜像. 缺失工具、依赖或镜像必须单独取得下载授权, 不由测试命令隐式补齐. 具体 C++ 构建选项见 [构建指南](../cpp/BUILD.md).

运行器有项目锁、内存检查、容器限额和所属进程清理. 只在隔离测试资源上运行, 不把部署中的 Redis 或系统服务当作可清空、强杀的夹具. 不改变系统部署、用户工具链或全局缓存设置.

## 场景与证据

- [conformance](conformance/README.md): 各语言共用的配置、Catalog 事件向量.
- [lua](lua): Lua 生成、一致性检查及获准后执行的专用负载.
- standalone、sentinel、interop、catalog: 独立 Redis、故障切换、跨语言协议及 Catalog 场景.
- [tls](tls/README.md): 公开隔离测试身份, 不用于部署.
- cpp 与各语言 tests: 本语言和绑定用例; 不以一个语言的结果代替另一个语言.

每次运行在忽略的 build/testkit/runs/<run_id>/ 保存源码身份、报告、日志、覆盖缺口和清理状态. 迁移前导入的原始结果保留在 build/results/imported/, 只解释其原源码与环境. 当前目录迁移尚无运行验收, 不从历史报告推定通过.

运行器明确保留 live mTLS、部分语言专用长期负载以及直接 C++ 双次 Sentinel 切换等覆盖缺口. 中断或失败必须保留报告, 清理本任务资源; 不自动重启. 历史测试过程使用 Git 查询, 本页不再链接已删除服务或失效的旧命令.
