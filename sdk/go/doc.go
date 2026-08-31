// Package verdandi 提供一个轻量共享 Redis 根客户端、带容量限制的强类型 Key/Hash 命令、字段编解码契约和稳定错误。
// Zone 身份及全部领域工作协程位于引用同一根 Client 的 registration 与 catalog 子包中。
//
// 应用数据表示为有界顶层字段 map，字段值对 Verdandi 是不透明字节。
// 应用 Attr/Data 类型自行定义跨语言字段编码；包在需要保留调用方数据前会解除可变别名。
package verdandi
