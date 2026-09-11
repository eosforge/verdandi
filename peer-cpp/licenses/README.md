# 第三方声明

这里保存 [固定来源锁](../dependencies.lock.json) 对应源码的许可证和 NOTICE 原文.
不修改上游授权文字. 构建分发包时应保留这些文件及本项目 LICENSE.

| 来源 | 原文 |
| --- | --- |
| gRPC, 含其内部第三方声明 | [LICENSE](grpc/LICENSE), [NOTICE](grpc/NOTICE.txt) |
| gRPC 协议子模块 | [LICENSE](grpc-proto/LICENSE) |
| Protobuf | [LICENSE](protobuf/LICENSE) |
| Abseil | [LICENSE](abseil/LICENSE) |
| c-ares | [LICENSE](cares/LICENSE.md) |
| RE2 | [LICENSE](re2/LICENSE) |
| zlib | [LICENSE](zlib/LICENSE) |
| yyjson | [LICENSE](yyjson/LICENSE) |
| BoringSSL, 含其多个上游授权 | [LICENSE](boringssl/LICENSE) |
| GCC 16.2 libstdc++exp, 契约运行库 | [COPYING3](gcc-runtime/COPYING3), [运行库例外](gcc-runtime/COPYING.RUNTIME) |

GCC 运行库授权原文复用项目内 GCC 16.2.0 源码. 动态 libstdc++ 和系统 C 运行库属于部署工具链,
其实际随包分发范围与来源也应随发布包记录.
目前该目录是开发骨架, 尚未承诺跨发行版二进制兼容.
