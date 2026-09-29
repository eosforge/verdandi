# Astra 测试

共享测试身份和跨进程场景统一在本目录; C++ 组件单元测试保留在各组件 `tests/`, Go 用例与被测包相邻, Admin 用例位于 `admin/tests/`.
最新测试结论只维护在 [docs/validation.md](../docs/validation.md), 原始日志、JSON 和源码/二进制摘要写入忽略的 `build/results/`. 用例定义和通过结论分别维护, 迁移前结果不自动证明迁移后工作区通过.

## 显式入口

以下命令均从 Astra 根目录执行, 构建和测试前须获得 [AGENTS.md](../AGENTS.md) 规定的当轮授权:

```bash
bash build.sh regression --profile release
python3 -B tests/test_build.py
python3 -B tests/test_pulsar_harness.py
python3 -B tests/test_soak.py
```

`regression` 顺序执行离线构建、Go 用例、CTest 和生成源码比较, 尊重资源限制和 CTest 的串行约束. Sanitizer、性能和长期测试分别授权, 不作为普通回归的隐式附加项. 缺少工具或依赖不自动下载.

`fixtures/` 中的密钥、账号和证书仅用于公开的隔离测试身份, 不得用于部署. 服务测试创建自身临时目录、端口与进程, 退出时负责清理; 不影响系统部署服务.

## 方法与证据

- [Comet 场景](../docs/comet.md): 业务契约及用例映射.
- [三 Star 长测](../docs/soak.md): 每台接受本地写入、跨节点传播和故障恢复, 入口为 `tests/soak.py`.
- [诊断探针](../docs/profile.md): 采集边界与 `tools/profile.py` 离线分析; 所属进程退出后才解析.
- [性能负载](../bench/README.md) 与 [可选 Redis 对照](../bench/baseline/README.md): 不属于常规业务回归.

本次目录整理不启动这些命令, 也不恢复此前已停止的长测.
