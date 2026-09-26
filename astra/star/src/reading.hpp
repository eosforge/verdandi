#pragma once
#include <astra/profile.hpp>
#include <expected>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

namespace astra {
// 两动态域共用的读取同步边界. State 授予访问权, 保留各自时钟, 到期语义与来源锁类型.
// 这里只合并锁/等待/回收次序, 不统一业务存储, 不新增运行时对象或虚调用.
template <typename State>
class Reading {
public:
    // action 同步且只执行一次, 返回 expected<T, State::Error>, 不重入 State 或返回其受锁保护的裸借用.
    // 错误及异常均先释放域锁再销毁旧记录; 等待来源准备时释放域锁, 然后重采时钟.
    static auto execute(State& state, auto&& action) {

        ASTRA_PROFILE_SCOPE("star.reading.execute");

        using Result = std::invoke_result_t<decltype(action)>; // 与领域错误类型保持一致, 不吞没 clock 等失败.
        {
            ASTRA_PROFILE_BEGIN(profile_lock_20, "star.reading.execute.wait.lock");
            const std::shared_lock lock(*state.gate_); // 整拍内只读不进入来源准备锁.
            ASTRA_PROFILE_END(profile_lock_20);
            const auto stamp = state.reading(); // 保留 State 对本地时钟和单调性的检查.
            if (!stamp)
                return Result(std::unexpected(stamp.error()));
            if (stamp->time < state.due_)
                return std::forward<decltype(action)>(action)();
        }

        ASTRA_PROFILE_COUNT("star.reading.expiry_fallback", 1);
        std::vector<typename State::Retired> retired; // 必须声明在域锁之前, 异常展开也在锁外释放.
        ASTRA_PROFILE_BEGIN(profile_lock_29, "star.reading.execute.wait.lock");
        std::unique_lock lock(*state.gate_); // 重新获取独占锁后不能复用先前的时间读数.
        ASTRA_PROFILE_END(profile_lock_29);
        for (;;) {
            const auto stamp = state.reading(); // 每次来源等待后都重采, 不将旧读数当作追平证据.
            if (!stamp)
                return Result(std::unexpected(stamp.error()));
            auto blocked = state.advance(stamp->time, retired); // 域自己的到期规则, 返回尚忙的来源或空.
            if (!blocked)
                return std::forward<decltype(action)>(action)();
            typename State::Guard waiting(std::move(blocked), lock); // Guard 在等待期间释放域锁, 随后重锁并重试.
        }
    }
};
} // namespace astra
