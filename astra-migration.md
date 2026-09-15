# Astra 更名与 Linux 测试迁移

> 本文件记录更名时点的结果. 后续协议与命名已由 [Orbit/Astra/Comet v1](protocol-v1.md) 修订, 下文旧名称不再是当前实现要求.

2026-09-15. 本次迁移只改变活动项目命名和目录组织, 不实现业务同步或新的 SDK.

## 活动范围

| 项目 | 当前值 |
| --- | --- |
| 产品代号 | Astra |
| C++ 服务源码 | `astra/`, 原 `cluster-cpp/` |
| C++ 公共头文件 / 命名空间 | `astra/cluster/*.hpp` / `astra::cluster` |
| CMake 项目 / 选项 | `astra_star_cpp` / `ASTRA_*` |
| C++ 产物 / 缓存入口 | `build/astra/` / `build/deps/astra/` |
| gRPC 包 | `astra.cluster.v1` |
| 准入签名域 | `astra-admission-v6` + NUL + 原始 Member 字节 |
| 协议生成器 | `astra-protocol-generator` |
| 管理界面包 | `astra-admin` |
| 安装许可目录 | `share/astra` |

`star`、`planet`、`supervisor` 可执行文件仍按角色命名. 命令行配置含义、控制协议主版本 6、字段号及业务版本规则不变.
Go module、import 和 proto `go_package` 暂留 `github.com/eosforge/verdandi/supervisor`, 对应尚未迁移的仓库地址.

## 保留边界

- 最外层工作目录仍是 `D:\projects\verdandi` 和 `/home/ubuntu/verdandi`.
- Git origin 仍是 `git@github.com:eosforge/verdandi.git`, 本轮不修改远端或执行 push.
- `sdk/`、Lua、Registration/Catalog Redis 契约与旧测试冻结为 Verdandi 版本, 不迁移为 Astra SDK.
- 已退休 Rust 服务、`testkit/transport/` 旧实验和防火墙实验脚本保留原有命名.
- 日期化报告、历史结果 JSON 和补丁保留原始内容及旧路径, 不把旧成绩写成 Astra 新测试.
- 公开 TLS 证书、密码与密码哈希保持字节不变; 旧证书中的 Verdandi URI 不参与现行授权.
- `testkit/support.py` 的历史 SDK 变量与清理属性继续保留, 共享测试辅助不构成 Astra API.
- Windows 旧 `cluster-cpp/.vs` 若被 Visual Studio 占用, 留作被 Git 忽略的本地 IDE 缓存; 活动源码已迁入 `astra/`.

根目录 [legacy-sdk.md](legacy-sdk.md) 保留旧 SDK 说明, [testkit/legacy-sdk.md](testkit/legacy-sdk.md) 保留旧测试说明.
活动 C++ 实验协议独立移至 `astra/bench/proto/probe.proto`, 不改动退休 Rust 实验的 schema 和生成源码.

## 协议与升级

gRPC 完整方法名由包名组成, 因而 Astra 与 Verdandi v6 不互通. 准入签名域同时隔离, 即使用同一密钥签署相同正文,
旧签名也不能作为 Astra 凭证. Supervisor、Star 和保留的 Planet 必须一起升级并重新登记; 不增加双协议兼容分支.

现有账号、TLS 材料、准入签名密钥和成员数据库无需清空. Member 字段布局及持久结构未变, 新进程仍由原来的登记事务分配代次.
旧进程应停止后再部署新服务, 不能将更名视为滚动混版本互通.

`message-ids.lock` 的 1..28 数值完整保留. 1..12 是退休 peer 协议的保留名;
13..28 只替换包名前缀, 包括已退役但仍占用编号的消息. gRPC 当前不通过该表分派消息.
Go 和 C++ 均由已安装的 protoc / 插件真实重新生成, 未手改生成字节.

## Linux 缓存复用

活动测试现在直接在 `/home/ubuntu/verdandi` 根目录运行. 原隔离测试目录
`build/cluster-sync-validation-20260914/source/build/cluster-cpp/` 中的 Debug、Release、ASan、TSan
及 core-debug 构建树迁入根目录 `build/astra/`.

迁移修复 CMake 的源目录、输出目录、依赖文件路径及选项缓存, 随后显式重新配置.
不删除对象文件或重建第三方依赖. 名称改变涉及的项目源码和 Protobuf 目标必须重新编译, 不能复用旧二进制冒充 Astra.

依赖原始安装前缀保留在 `build/deps/peer-cpp/`, 新入口 `build/deps/astra` 通过项目内符号链接引用它.
这样保持第三方 CMake 导出目标的绝对路径及已插桩的 TSan 库, 避免重编 gRPC、BoringSSL、Protobuf 等.
源码展开标记改为 `.astra-extracted`, 其 commit 内容不变. 项目工具与 Go/Rust 缓存保持原位置.

根目录中更早遗留的 `peer.proto` 和 Go 生成文件已归档到 `build/astra-migration/retired-sources/`,
避免同时编译新旧类型. 同步前被替换的源码备份位于 `build/astra-migration/sources-before.zip`.
缓存迁移前记录了普通/TSan 安装前缀内 3420 个文件的大小与修改时间, 测试后再次核对.

```bash
cd /home/ubuntu/verdandi
bash astra/build.sh regression --profile debug
bash scripts/test-services.sh
```

本次不运行无限时耐久测试. 具体验证结果如下, 不以历史报告替代.

## 本轮验证

| 范围 | 结果 |
| --- | --- |
| Linux Debug | 10 项 CTest + 6 项真实 RPC + 13 项进程场景通过 |
| Linux Release | 10 项 CTest + 6 项真实 RPC + 13 项进程场景通过; 独立存储/推流探针编译通过 |
| Go Supervisor | Windows/Linux 格式、模块一致性、vet、测试、构建通过; Linux race 通过 |
| 协议生成器 | 两端离线生成一致性、fmt、Clippy 和 4 项测试通过 |
| Python 测试辅助 | Windows 44 项通过; Linux 43 项通过, 1 项平台条件跳过 |
| Admin | 53 项测试、格式、目录边界、类型检查及生产构建通过 |
| ASan / TSan / core-debug | 原构建树迁移并重新配置通过; 本轮没有重跑 sanitizer 运行测试 |
| 文件与缓存 | 232 个两端活动文件哈希相同; 704 个冻结文件未改; 原 C++ 目录 99 个文件均有迁移对应项 |
| 协议编号 | 28 个 MessageID 数值保持不变 |
| 第三方依赖 | 两个安装前缀中 3420 个文件大小及修改时间相同, 未重新下载或编译 |

测试覆盖旧产品准入签名被拒绝、新 Go/C++ 准入互通、互联、重启、上游切换和资源清理.
公开测试密码及其哈希保留旧值; 升级不要求修改部署账号密码.
生成器输出中的末尾空行保持原样; 新探针 schema 固定 LF, 避免 Windows 换行进入生成注释造成两端字节差异.
所有进程回归均有限完成, 本轮没有启动持续后台测试.

结构化证据: [astra-rename-20260915.json](testkit/results/astra-rename-20260915.json).
本报告验证更名和测试入口迁移, 不构成业务同步实现或生产就绪声明.

## 后续仓库与最外层目录迁移

另行一次完成仓库地址、Go module/import/go_package 和两端最外层目录迁移, 再显式生成协议并检查.
CMake、Python 虚拟环境启动脚本和工具中的绝对路径也需审查, 不能只对根目录执行重命名.
第三方产物可继续保留兼容路径或单独迁移导出前缀, 无需为了项目代号重新下载.
