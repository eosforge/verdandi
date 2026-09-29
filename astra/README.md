# Astra

Astra 是分布式服务发现与状态同步项目, 独立仓库为 `github.com/eosforge/astra`.
Star 在 Galaxy 内交换来源状态, Pulsar 提供准入登记和连续时间, Polaris 管理权威数据, Astrolabe 与 Admin 提供管理入口.

当前实现包含 Almanac、Catalog、Ephemeris 三域, Comet C++ SDK, 多键原子提交及多 Star 恢复. 服务端使用 Linux/GCC 16.2 和 C++26; Comet C++ 使用 C++23, 支持 Linux/GCC 与 Windows/MSVC. Comet Go、Moon 及完整 Planet 能力仍未完成. 代码存在、以前通过回归和正式发布就绪是不同结论, 以 [最新验证](docs/validation.md) 的源码身份与边界为准.

## 开始使用

以下命令从本仓库根目录运行, 只消费已有工具和依赖:

```bash
bash build.sh build --profile release
```

工具和依赖默认位于 `build/tools`、`build/deps`, 新编译产物位于 `build/cmake/<profile>`, 原始测试报告位于 `build/results`. `build/` 不进入 Git 或源码发布包. 依赖缺失时明确失败, 不自动下载.

迁移中的开发工作区可以显式设置 `ASTRA_CACHE_ROOT` 复用旧工具和依赖缓存; 新产物仍写入本仓库 `build/`, 不搬用旧 CMakeCache 或把旧测试结果记作新路径已验证. 详细配置见 [构建指南](docs/build.md). 运行测试及其前置构建遵循 [项目约定](AGENTS.md).

## 目录

| 路径 | 职责 |
| --- | --- |
| `common/`, `star/`, `pulsar/`, `polaris/`, `astrolabe/` | 共享基础及服务实现 |
| `comet/` | 原生 SDK, C++ 可独立配置和安装 |
| `admin/` | 管理前端 |
| `proto/`, `internal/generated/` | 协议输入与 Go 生成代码; C++ 生成代码在 `common/src/generated/` |
| `tools/` | 构建、协议生成、依赖准备及离线诊断工具 |
| `tests/` | 共享测试身份、跨进程场景和 Python 用例; 组件单元测试留在组件内 |
| `bench/` | 独立性能负载; Redis 对照显式指定外部旧 SDK |
| `docs/` | 当前设计、使用说明及唯一最新验证记录 |
| `licenses/` | 随依赖版本保留的第三方许可及 NOTICE 原文 |

文档入口为 [docs/README.md](docs/README.md), SDK 接口见 [Comet C++](comet/cpp/README.md), 测试入口见 [tests/README.md](tests/README.md).
常规构建不依赖父目录、旧 Redis SDK 或旧 Rust 服务. `planet/` 仍包含当前 CMake 使用的策略代码; `moon/` 尚无实现, 不代表完整功能已经交付.

## 许可证

Astra 自有代码采用 [MIT](LICENSE). 第三方代码、运行库及其声明继续遵循各自许可证, 见 [第三方声明](licenses/README.md); 不以本项目 MIT 覆盖上游授权.
