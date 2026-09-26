#pragma once
#include <astra/profile.hpp>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace astra {
// 远端来源的独占准备责任. 对象寿命独立于域目录, 等待来源时不占住公共提交锁.
template <typename Replica>
class Borrowing {
public:
    // 调用方已持 domain, owner 来自该锁保护的目录; 空 owner 表示来源不存在.
    Borrowing(std::shared_ptr<Replica> owner, std::unique_lock<std::shared_mutex>& domain) : owner_(std::move(owner)) {

        if (!owner_)
            return;
        lock_ = std::unique_lock(owner_->mutex, std::try_to_lock);
        if (!lock_.owns_lock()) {
            outside(domain, [&] { ASTRA_PROFILE_SCOPE("star.borrowing.wait.source"); lock_.lock(); }); // 等待远端准备结束时允许本地 RPC 提交.
        }
    }

    // 禁止复制独占责任, 默认移动仅移交所有权和锁, 不移动来源对象.
    Borrowing(Borrowing&&) noexcept = default;
    Borrowing(const Borrowing&) = delete;
    Borrowing& operator=(const Borrowing&) = delete;

    // 先释放来源互斥量再归还拥有权, 避免来源已从目录移除时先销毁仍加锁的 mutex.
    ~Borrowing() = default;

    // 借用只覆盖本守卫寿命, 目录身份须由外层在重新取得 domain 后再次核实.
    Replica* get() const noexcept {
        return owner_.get();
    }

    // 仅执行不会访问共享域的无返回准备步骤, 正常/异常均先恢复 domain 再离开.
    // action 的私有候选在外层构造, 不得在释放域锁期间发布共享投影、期限或计费.
    static void outside(std::unique_lock<std::shared_mutex>& domain, auto&& action) {

        ASTRA_PROFILE_SCOPE("star.borrowing.outside");
        domain.unlock();
        try {
            ASTRA_PROFILE_SCOPE("star.borrowing.prepare");
            std::forward<decltype(action)>(action)();
        } catch (...) {
            ASTRA_PROFILE_SCOPE("star.borrowing.wait.domain.failure");
            domain.lock();
            throw;
        }
        ASTRA_PROFILE_SCOPE("star.borrowing.wait.domain");
        domain.lock();
    }

private:
    std::shared_ptr<Replica> owner_;    // 保活先于锁构造, 最后释放; 不以目录迭代器跨解锁借用.
    std::unique_lock<std::mutex> lock_; // 同来源串行, 不阻止其他来源的准备或本地来源提交.
};
} // namespace astra
