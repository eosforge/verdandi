// Package registration 基于共享 verdandi.Client 传输，提供按 Zone 隔离的强类型 Registration 发布、租约续期、Registry 同步和本地 Selector 事务。
//
// 应用 Attr/Data 值类型实现 verdandi.Encoder，指针类型实现 verdandi.Decoder。
// 需要直接使用原始字段 map 的调用方可使用同时实现两侧契约的 verdandi.Fields。
package registration
