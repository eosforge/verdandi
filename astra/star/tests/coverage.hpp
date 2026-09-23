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
} // namespace
