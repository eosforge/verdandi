#pragma once
#include <astra/profile.hpp>
#include <astra/scope.hpp>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace astra {
// 动态来源的私有恢复任务, 每步最多安装一个 Scope; 析构仅取消后续工作, 不回滚已公开的范围.
template <typename State>
class Restore {
public:
    using Error = typename State::Error; // 使用所属域的容量/冲突错误, 不创建另一套网络错误码.

    // 恢复任务唯一拥有私有全量候选, 不允许复制并重复推进同一个任务.
    Restore(Restore&&) noexcept = default;
    Restore(const Restore&) = delete;
    Restore& operator=(const Restore&) = delete;
    // Scope 覆盖证据由 State 持有, 放弃任务不使晚到增量覆盖已安装内容.
    ~Restore() = default;

    // true 代表所有范围和来源确认完成, false 代表仍有工作; 失败后只返回原错误, 不继续部分执行.
    std::expected<bool, Error> step() {

        ASTRA_PROFILE_SCOPE("star.restore.step");

        if (failure_)
            return std::unexpected(*failure_);
        if (done_)
            return true;
        if (offset_ < scopes_.size()) {
            const auto& scope = scopes_[offset_]; // 固定范围地址, step 返回后不保留任何提交锁.
            const auto result = owner_->install(id_, draft_, scope);
            if (!result) {
                if (result.error() != Error::capacity || retried_) {
                    failure_ = result.error();
                    return std::unexpected(*failure_);
                }
                deferred_.push_back(std::move(scopes_[offset_])); // 先让其他范围归还容量, 至多重试一遍, 不在热范围上忙等.
            }
            ++offset_;
            return false;
        }

        if (!deferred_.empty()) {
            scopes_.swap(deferred_);
            deferred_.clear();
            offset_ = 0;
            retried_ = true;
            return false;
        }
        const auto result = owner_->finish(id_, draft_.position()); // 只有这一处可确认完整来源位置.
        if (!result) {
            failure_ = result.error();
            return std::unexpected(*failure_);
        }
        done_ = true;
        return true;
    }

private:
    friend State;

    // State 已验证完整候选、身份和容量; scopes 为新旧来源范围的有序并集.
    Restore(State& owner, std::string id, typename State::Source::Draft draft, std::vector<Scope> scopes) : owner_(&owner), id_(std::move(id)), draft_(std::move(draft)), scopes_(std::move(scopes)) {
        deferred_.reserve(scopes_.size()); // 安装后不再因登记容量重试目标而分配失败.
    }

    State* owner_;                        // 所属 State 必须比任务活得更久, 与 Exchange 的原有借用边界一致.
    std::string id_;                      // 固定来源身份, 不跨控制轮借用临时字符串.
    typename State::Source::Draft draft_; // 唯一完整候选, 网络页在创建任务前已全部校验.
    std::vector<Scope> scopes_;           // 每步安装的范围清单, 不复制正文.
    std::vector<Scope> deferred_;         // 容量不足的 Scope, 只在其他范围处理后再尝试一次.
    std::size_t offset_{};                // 下一个 Scope 下标, 初始零.
    std::optional<Error> failure_;        // 首次终止错误, 重复 step 不重试失败任务.
    bool retried_{};                      // 是否已经进入唯一一次容量重试遍历.
    bool done_{};                         // 来源完整确认已发布, 重复 step 幂等返回 true.
};
} // namespace astra
