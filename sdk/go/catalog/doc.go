// Package catalog 通过 Verdandi 同步持久、带版本的结构化值。
//
// Client 把一个 Zone 的 Catalog 脚本和状态附着到共享 verdandi.Client。
// Publisher 原子 Replace、Patch 或 Delete 路径；Subscriber 在内存中维护完整原始值，并由 Entry.Load 投影成应用类型。
package catalog
