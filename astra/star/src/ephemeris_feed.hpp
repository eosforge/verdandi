#pragma once
#include "downstream.hpp"
#include "ephemeris_edition.hpp"

namespace astra {
// Ephemeris 只选择自己的记录编码; 流管理、背压、认证和取消由共享实现负责.
class Ephemeris::Feed final : public Downstream<Ephemeris> {
public:
    using Downstream<Ephemeris>::Downstream; // 构造参数和生命周期契约保持一致, 不另加状态.
};
} // namespace astra
