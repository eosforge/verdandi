#pragma once
#include "catalog_edition.hpp"
#include "downstream.hpp"

namespace astra {
// Catalog 的动态内容流, 共用有界生命周期, 不与 Ephemeris 混合消息.
class Catalog::Feed final : public Downstream<Catalog> {
public:
    using Downstream<Catalog>::Downstream; // 复用 State/Gateway/共享唤醒器和有限预算.
};
} // namespace astra
