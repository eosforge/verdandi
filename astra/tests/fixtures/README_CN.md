# 公开测试身份

[English](README.md) | [简体中文](README_CN.md)

本目录的证书、私钥、账号和签名材料是刻意公开的隔离测试夹具, 不得用于部署. 只有显式加载夹具 CA 的测试信任它们, 不安装到系统信任库.

## 用途

- pulsar: 测试准入签名密钥和公开账号配置.
- star-a 至 star-d: alpha 的不同部署身份.
- planet-a、planet-b: 保留 Planet 测试身份, 不表示该组件已完成.
- wrong-cluster: 错误集群/未知账号场景.
- expired、rogue: 过期证书及签名无效场景.
- untrusted: 独立的不可信证书, 用于验证拒绝非夹具 CA.
- admission-v1.json、admission-v5.json: 共享编码与身份测试向量.

主夹具 TLS 证书使用同一公开测试 CA 的 ECDSA P-256/SHA-256 配置, 与固定版本的 Go 和 C++ gRPC/BoringSSL 配合. 证书有匹配的 SKI/AKI; 不通过关闭严格校验适配不完整证书. untrusted 是独立拒绝样本, 不属于主 CA.

准入签名使用 Ed25519, 与 TLS 证书算法分开. 历史 Verdandi URI SAN 不再授予角色; v5 由公开 login.json 和 Pulsar accounts.json 测试账号授权. Astra 准入签名域为 proto.orbit.v1.admission 后接 NUL, 本目录不保存真实部署的已签名准入凭证.

## 维护

生成入口为 [tools/generate_fixtures.py](../../tools/generate_fixtures.py), 仅在明确需要更新夹具时使用已有项目 Python/cryptography 环境执行. 生成工具与测试运行分开, 不自动下载依赖或刷新证书. 主 CA 私钥在生成后丢弃, 叶证书私钥作为测试输入保留.

目录迁移保持原有身份字节不变. 临时数据库、复制的密钥、进程记录和日志写入忽略的 build/, 不回写公开夹具. 实际验证结果见 [验证记录](../../docs/validation.md).
