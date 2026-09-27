#pragma once
#include "check.hpp"
#include <array>
#include <astra/clock.hpp>
#include <astra/scope.hpp>
#include <chrono>
#include <optional>
#include <string>
#include <utility>

namespace {
// 已安装两个范围后放弃恢复, 第二次安装不能丢弃第一次的覆盖证据; 两域使用相同来源顺序.
template <typename Domain>
void coverage(typename Domain::Record old, typename Domain::Record newer, std::array<std::string, 3> keys) {

    using State = typename Domain::State;
    using namespace std::chrono_literals;
    State state([] { return std::optional(astra::Clock::Reading{.time = astra::Clock::Time(1s), .ready = true}); }, {});
    const std::array<astra::Scope, 3> scopes{{{"a", "main"}, {"b", "main"}, {"c", "main"}}}; // 恢复按地址排序逐 Scope 推进.
    CHECK(state.admit("peer"));
    for (std::size_t index = 0; index < scopes.size(); ++index)
        CHECK(state.apply("peer", index + 1, scopes[index], keys[index], old, State::Source::Form::record));
    {
        auto draft = state.prepare("peer", 6);
        CHECK(draft);
        for (std::size_t index = 0; index < scopes.size(); ++index)
            CHECK(draft->set(scopes[index], keys[index], newer));
        auto task = state.restore("peer", std::move(*draft));
        CHECK(task && task->step() == false && task->step() == false);
        CHECK(state.received("peer") == 3); // 两个局部范围不冒充完整来源确认.
    }
    const auto first = state.capture(scopes[0]), second = state.capture(scopes[1]); // 已公开的新内容游标, 后续旧后缀不得推进它们.
    CHECK(first && second);
    CHECK(state.covered("peer", 4, scopes[0], keys[0]) == true);
    CHECK(state.apply("peer", 5, scopes[1], keys[1], old, State::Source::Form::record));
    CHECK(state.capture(scopes[0])->version() == first->version() && state.capture(scopes[1])->version() == second->version());
    CHECK(state.apply("peer", 6, scopes[2], keys[2], newer, State::Source::Form::record));
    CHECK(state.received("peer") == 6 && state.prepare("peer", 6)); // 连续前缀追平后覆盖可以回收.
}

// 精确回补的 128 项预算横跨 Scope 计数, 已有目标可原位更新, 失败不能推进连续来源位置.
template <typename Domain>
void capacity(typename Domain::Record record) {

    using State = typename Domain::State; // 使用真实领域提交器, 不复制 coverage 的实现作为测试替身.
    using namespace std::chrono_literals;
    State state([] { return std::optional(astra::Clock::Reading{.time = astra::Clock::Time(1s), .ready = true}); }, {}); // 固定可用时钟避免记录在容量填充期间过期.
    const std::array<astra::Scope, 2> scopes{{{"a", "main"}, {"b", "main"}}};                                            // 同来源不同范围共享精确回补配额.
    std::array<std::string, 129> keys;                                                                                   // 最后一项用于验证满额拒绝, 其余填满实际预算.
    // index 是测试目标编号, Catalog 使用十进制 Key, Ephemeris 使用真实生成器的合法 UUID.
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if constexpr (requires { Domain::uuid(); })
            keys[index] = Domain::uuid();
        else
            keys[index] = std::to_string(index);
    }
    CHECK(state.admit("peer"));

    for (std::size_t index = 0; index < 128; ++index)
        CHECK(state.repair("peer", index + 2, scopes[index % 2], keys[index], record));
    CHECK(state.received("peer") == 0);                                          // 精确回补不能假装跨过其他目标的历史.
    CHECK(state.repair("peer", 200, scopes[0], keys[0], record));                // 已有目标原位更新不再占一项.
    const auto denied = state.repair("peer", 201, scopes[1], keys[128], record); // 第 129 个目标不能越过全来源预算.
    CHECK(!denied && denied.error() == State::Error::capacity);
    CHECK(state.received("peer") == 0 && !state.replica("peer", scopes[1], keys[128])->has_value());
    CHECK(state.apply("peer", 1, scopes[0], keys[0], record, State::Source::Form::record)); // 被覆盖的旧事件仍按连续顺序确认.
    CHECK(state.received("peer") == 1);
}
} // namespace
