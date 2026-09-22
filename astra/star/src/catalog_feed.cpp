#include "catalog_feed.hpp"

// 单独实例化 Catalog 类型安全的下行流, 不复制排队/认证/取消实现.
template class astra::Downstream<astra::Catalog>;
