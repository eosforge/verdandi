# Astra C++26 服务

当前 C++ 目标为 Linux x64 / GCC 16.2.0. 已接入 Pulsar SQLite 成员库、Go Polaris、Star 三域存储/登录/Watch、多 Star 单流恢复、Comet C++ 读写保活、必要 Go Astrolabe、指标与 Admin 管理适配. Planet 继续冻结. 当前实现边界见 [推进进度](../docs/progress.md), 构建与运行结果统一见 [验证记录](../testkit/validation.md).

范围见 [架构](../docs/architecture.md), 源码职责见 [维护指南](CONTRIBUTING.md), 存储见 [三域存储](common/README.md), 时间服务见 [Pulsar](pulsar/README.md). [Polaris](polaris/README.md) 负责持久 Almanac, [Astrolabe](astrolabe/README.md) 提供无自身数据库的 Go 管理入口与实时观测. 首版 SDK 是 [Comet C++](comet/cpp/README.md), [Comet Go](comet/go/README.md) 后续推进, 不作为 C++ 业务闭环的前置条件.

## 运行模式与接线

取消架构级 standalone 分支. 单机与 All-in-One 仅是部署方式, 同样完成 Pulsar 准入及首次校准、Polaris 初始 Almanac、首轮对等连接与动态数据同步. 不因部署同机、材料缺失或依赖离线绕过初始化. 具体启动预算与已运行节点的降级规则只在 [架构](../docs/architecture.md#启动与运行) 定义.

Star 业务认证与 TLS 独立配置, 目标默认均开启, 允许分别显式关闭, 配置失败不自动降级. APIKEY/APISECRET 只验证 Comet 登录, 不建立范围权限或注册所有者; 外部接口始终隔离 `__` Sector. 凭据只从 Polaris 的内部 Almanac 安装, 不再提供 standalone 凭据文件或另一份启动权威.

内部必需材料缺失或非法时明确失败, 暂时不可达时按初始化及退避规则处理. 业务 TLS 开启时必须具有有效的服务端证书和私钥, 关闭 Comet TLS 不取消基础设施链路的保护. 初始 Almanac 的合法空凭据表可以完成同步, 但认证开启时没有合法 Comet 账号就拒绝登录, 不改成匿名.

### 监听与配置入口

业务与内部服务使用两个独立的 gRPC Server/Builder. 不能在注册了内部服务的同一个 Server 上增加业务明文端口, 否则业务 TLS 开关会一并暴露内部 RPC.

| 入口 | 调用者与服务 | 控制规则 |
| --- | --- | --- |
| 外部业务入口 | Comet Session, Almanac 读取, Catalog/Ephemeris 业务 | 独立认证/TLS 配置, 始终隔离 `__` Sector |
| 内部系统入口 | 获准节点的对等流和必要内部控制 | 内部 TLS 与 Pulsar 身份, 按角色限制系统职责 |
| 指标 HTTP 入口 | Astrolabe 读取 /metrics | 独立可选只读监听与资源预算, 不依赖外部监控系统 |

Star 对等节点不能替 Polaris 发布 Almanac, Astrolabe 也不能绕过 Polaris 直接改写该权威数据. APIKEY 只用于普通 Comet 准入, 不等于内部服务角色. 内部监听先于公共业务开放, 以便冷启动恢复, 不等待浏览器登录或尚不可用的 Comet 会话.

Almanac 同步由 Star 主动连接 Pulsar 名单中的唯一 Polaris, 接收其推送并返回安装确认, 不再为 Polaris 入站灌注另开 Star RPC. 该出站同步与上述两套 Server 共用受控存储, 内部身份和 TLS 仍独立于 Comet 开关; 具体规则见 [Polaris 同步流](../proto/README.md#polaris-stream).

两套 Server 共享受控业务存储, 分别限制流数、消息、资源及退出工作. 独立 Server 不等于独占 CPU, 仍需限制快照、解析和发送工作. 鉴权前 Hello 的 4 KiB 预算不等于已鉴权消息上限; 当前内部消息默认上限为 8 MiB, 由双方声明取较小值, 也不因此取消业务预算. 长流继续采用 Callback/异步路径, 不用同步阻塞 Read 占住服务线程.

### 业务监听与材料参数

下列业务配置已接入实现, 具体构建配置和已验证源码身份见 [验证记录](../testkit/validation.md):

| 参数 | 目标含义 |
| --- | --- |
| `--listen` / `--advertise` | 保留内部节点监听和可达登记地址, 不改作 Comet 地址 |
| `--comet=IP:PORT` | 显式启用独立业务监听, 不自动占用内部端口 |
| `--auth=true|false` | 只控制 Comet 登录验证, 目标默认 true |
| `--tls=true|false` | 只控制 Star–Comet TLS, 目标默认 true |
| `--comet-identity=目录` | 外部 TLS 的 cert.pem / key.pem, 不从节点 identity 隐式借用 |
| `--metrics=IP:PORT` | 独立只读 HTTP 指标监听, 缺省不监听, 与 Comet TLS 开关分开 |
| `COMET_CA_FILE` CMake 缓存项 | SDK 可选嵌入的 CA 证书文件, 不是服务端私钥 |

各入口地址冲突直接失败, 外部 TLS 关闭时显式提供 comet-identity 视为冲突配置. 不提供 --standalone、本地 credentials 引导分支或单独指定 Polaris 地址的参数. 目录查找、Almanac 接收及动态域首轮同步均已接入业务开放门, 动态来源超时按已确认的有界降级规则处理.

SDK 信任材料的外部加载、编译嵌入与证书来源见 [Comet TLS](comet/cpp/README.md#tls). 关闭业务 TLS 时凭据和载荷不再获得该链路的机密性, 登录本身不提供加密. 指标的实时抓取边界见 [Astrolabe](astrolabe/README.md#实时观测), 未接入外部监控不影响业务初始化.

## 构建

从仓库根目录使用现有项目工具和缓存, 不隐式下载:

```bash
bash astra/build.sh build --profile debug
bash astra/build.sh build --profile release
```

目标产物位于 `build/astra/<profile>/`: C++ `star`, `planet`, `pulsar`, Go `polaris`, `astrolabe`, 以及 Comet 静态库. 已移除 C++ Astrolabe 占位程序的构建目标; Planet 继续冻结. 各构建配置的实际结果见 [验证记录](../testkit/validation.md).
Windows 可编辑和格式化源码, `build.ps1` 会明确拒绝当前不支持的平台, 不自动远程执行或切换编译器.

普通构建不运行 protoc. [dependencies.lock.json](dependencies.lock.json) 是依赖版本、来源及校验值的唯一清单; 工具在 `build/tools`, 依赖在 `build/deps/astra`.
生成源码保存在 `common/src/generated`, 由 [协议入口](../proto/README.md#generation) 的显式命令更新; 禁止手改生成文件.
gRPC 使用配套 BoringSSL, 不另找系统 OpenSSL. 第三方许可见 [licenses](licenses/README.md).

`build.py` 的非 core-only 构建同时要求项目内 Go 1.27.1 及已批准模块缓存. Go 子进程只使用 build/deps/go 和 build/cache/go, 设置 GOPROXY=off、GOTOOLCHAIN=local 与只读模块模式, 缺依赖明确失败. Pulsar 的 SQLite C 后端仅消费已缓存并校验的 amalgamation, 不影响 Star 的纯内存业务设计.

构建和测试并行度分别通过 `--jobs` / `--test-jobs` 限制, 默认根据本次实际 CPU、可用内存及 cgroup v2 预算选择. 显式上限不会突破估算的资源预算, Go 测试与 CTest 顺序运行. 这些命令仍需本轮明确授权后才能执行.

`prepare_dependencies.py fetch/build` 是单独的依赖准备动作, 须先获得具体下载/构建授权. 正常检查缺少工具时失败, 不代为安装.
TSan 消费独立的 `linux-gcc16-tsan` 已插桩前缀, 不覆盖普通依赖. 正常依赖构建的虚拟内存限制不能机械套用到 TSan shadow 地址空间.

Linux 继续使用 `/home/ubuntu/verdandi` 下的源码、构建树与缓存. `build/deps/astra` 可能指向原 `build/deps/peer-cpp` 安装目录; 不移动安装前缀或清缓存以“整理”名称.
并行度由上述统一入口按实际资源选择, 不因虚拟机配置为 16 核便同时启动 16 个高内存编译任务.

## 启动

身份目录包含 `ca.pem`, `cert.pem`, `key.pem`, `admission.pub`, `login.json`. 密码保存在受保护的 login.json, 不放到命令行或日志. 仓库公开身份仅用于测试.

```bash
build/astra/debug/star --listen=192.168.0.119:7442 --super=192.168.0.119:7440 --galaxy=alpha --group=east --identity=build/deployment/star-a
build/astra/debug/planet --listen=192.168.0.119:7443 --super=192.168.0.119:7440 --galaxy=alpha --group=east --identity=build/deployment/planet-a
```

C++ 当前使用 `--galaxy`, Go Supervisor 使用 `--cluster`; 不把旧文档的参数套到所有程序.
`--super` 指向 Orbit 登记端口. 使用 Pulsar 时响应携带 Pulse 地址, Star 启动采样; Go Supervisor 返回空地址时仅有连接准入, 不获得新有限租约的时间资格.

TLS 1.3/h2, 跨 Go/C++ 测试证书采用 ECDSA P-256/SHA-256, 准入签名采用 Ed25519. TLS 服务端证书与签名 bearer 的职责不同.
通配监听需提供可达 `--advertise`; 实际选项、范围和默认值见配置与 `--help`. 角色由可执行文件决定, `--worker-threads` 不支持.

服务不随 stdin 关闭退出. Linux 接收 SIGINT/SIGTERM, 停止接纳、取消并排空自有 RPC 后退出. 参数错误为 2, 运行错误为 1, 正常退出为 0.
Planet 当前不提供业务下游服务; `initialized` 和 `upstream` 不能作为业务同步就绪标志.

## 验证与交付

测试及前置构建须先获当轮授权, 命令和配置见 [Testkit](../testkit/README.md). 最新结果只在 [验证记录](../testkit/validation.md) 维护.
安装目标保留根许可证和第三方授权文本, 不继承开发机 GCC RPATH. 发布需携带匹配运行库并在目标发行版验证; 当前骨架不承诺跨发行版二进制兼容或生产业务就绪.
