#pragma once
#include <astra/clock.hpp>
#include <astra/profile.hpp>
#include <astra/scope.hpp>
#include <cstddef>
#include <expected>
#include <mutex>

namespace astra {
// 两动态域共用的时钟检查、投影目录与历史记账. 所有数据仍由具体 State 持有,
// 不搬移锁或改变成员析构顺序, 不接管来源提交、期限推进及 Pending/Retired 回滚.
template <typename State>
class Context {
    friend State;                                  // 仅所属 State 调用这些内部机制, 不暴露目录或计费接口.
    using Error = typename State::Error;           // 组合层错误, 包含 clock/input/capacity.
    using Projection = typename State::Projection; // 各域自己的可见内容, 不转换或复制正文.

    // state 的域锁已由调用方持有; timing_ 串行化共享读者的采样与单调检查.
    // 无效读数不更新 observed_, 取时异常原样传播, RAII 释放采样锁.
    static std::expected<Clock::Reading, Error> reading(State& state) {

        ASTRA_PROFILE_BEGIN(waiting, "star.context.reading.wait.timing");
        const std::lock_guard timing(state.timing_); // 不在这里再次获取 gate_ 或 export_.
        ASTRA_PROFILE_END(waiting);
        auto value = state.time_(); // 保持采样与验证在同一临界区, 不接受调用方预先取得的时间.
        if (!value || !value->ready || value->time.time_since_epoch().count() < 0 || (state.observed_ && value->time < *state.observed_)) {
            return std::unexpected(Error::clock);
        }
        state.observed_ = value->time;
        return *value;
    }

    // state 已在域锁保护内, scope 只用于两级查找; 缺项返回空, 不创建目录或占用范围额度.
    // 返回的裸借用不得跨越调用方的锁与 State 寿命, 输入合法性由具体入口校验.
    static Projection* locate(State& state, const Scope& scope) {
        const auto sector = state.scenes_.find(scope.sector); // 不复制 Scope 文本.
        if (sector == state.scenes_.end()) {
            return nullptr;
        }
        const auto spectrum = sector->second.find(scope.spectrum); // 同 Sector 下仍按 Spectrum 独立保留游标.
        return spectrum == sector->second.end() ? nullptr : &spectrum->second;
    }

    // state 已持独占域锁; scope 非法或范围额度耗尽返回明确错误, 已有范围不再计数.
    // 新投影继承原域锁、正文计费函数及容量配置; 构造抛错时撤销本次新建的空 Sector.
    // 成功返回后的业务准备若失败, 仍由具体 State::Pending 撤销新范围.
    static std::expected<Projection*, Error> obtain(State& state, const Scope& scope) {

        if (!scope.valid()) {
            return std::unexpected(Error::input);
        }
        if (auto* scene = locate(state, scope)) {
            return scene;
        }
        if (state.scopes_ == state.limits_.scopes) {
            return std::unexpected(Error::capacity);
        }

        auto [sector, created] = state.scenes_.try_emplace(scope.sector); // created 标记本次应负责回滚的外层目录.
        try {
            auto [spectrum, inserted] = sector->second.try_emplace(scope.spectrum, state.gate_, static_cast<typename Projection::Measure>(&State::measure), state.limits_.projection);
            state.scopes_ += inserted; // 仅成功构造新投影才计数, 失败不消耗名额.
            return &spectrum->second;
        } catch (...) {
            if (created) {
                state.scenes_.erase(sector);
            }
            throw;
        }
    }

    // state 与所属 scene 均受域锁保护; 全域计费包含 scene 的旧历史, 当前范围可复用这部分额度.
    // 调用方在开始候选准备前查询, 历史总量仍须满足 State 的既有预算不变量.
    static std::size_t allowance(const State& state, const Projection& scene) {
        return state.limits_.history - (state.history_ - scene.history());
    }

    // state 已持独占域锁, scene 的候选已提交, before 是准备前的历史字节数.
    // event 是携带名称的单条提交通知; 先更新全域计费, 再调用有界 noexcept 内部收集器.
    // Catalog 整批信封不携带单条名称, 仍由其批次路径使用显式 Scope 完成计费/通知.
    static void publish(State& state, Projection& scene, std::size_t before, const typename Projection::Event& event) noexcept {
        state.history_ = state.history_ - before + scene.history();
        if (state.notify_) {
            state.notify_(state.context_, *event.name->scope, event);
        }
    }
};
} // namespace astra
