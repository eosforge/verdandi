#include "access.hpp"
#include <astra/scope.hpp>
#include <openssl/mem.h>
#include <openssl/rand.h>
#include <stdexcept>

namespace astra {
struct Access::Group {
    // 是否仍能授权, 仅在 Access 锁内读取或修改, 不能用延迟取消通知代替此位.
    bool active = true;
    // 旧快照的取消广播, request_stop 只在 Access 解锁后调用.
    std::stop_source stop;
};

struct Access::Account {
    // 解析后不可再修改的 SECRET, 与接入原始请求没有可写别名.
    Almanac::Buffer secret;
    // 单账号是否仍可授权, 同一 Access 锁保护.
    bool active = true;
    // 单账号轮换/删除的取消广播, 不在业务锁内执行外部回调.
    std::stop_source stop;
};

Access::Access(std::size_t maximum) : maximum_(maximum), group_(std::make_shared<Group>()) {
    if (maximum == 0 || maximum > 65536) {
        throw std::invalid_argument("Invalid business session capacity");
    }
}

Access::~Access() {
    group_->active = false;
    group_->stop.request_stop();
}

Access::Draft::Draft() : group_(std::make_shared<Group>()) {}

bool Access::Draft::set(std::string key, Almanac::Buffer secret) {

    if (!group_ || !Access::valid(key, secret) || accounts_.size() >= 65536 || accounts_.contains(key)) {
        return false;
    }
    // 所有节点与载荷均在独占候选中准备, 失败不改变当前登录表.
    auto account = std::make_shared<Account>();
    account->secret = std::move(secret);
    accounts_.emplace(std::move(key), std::move(account));
    return true;
}

bool Access::valid(std::string_view key, std::span<const std::uint8_t> secret) noexcept {
    return Scope::text(key, 128) && !secret.empty() && secret.size() <= 4096;
}

bool Access::equal(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) noexcept {
    return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

std::size_t Access::Hash::operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
}

void Access::Session::Relay::operator()() const noexcept {
    target.request_stop();
}

Access::Session::Session(std::string token, std::shared_ptr<Account> account, std::shared_ptr<Group> group) : token_(std::move(token)), account_(std::move(account)), group_(std::move(group)), account_stop_(account_->stop.get_token(), Relay{stop_}), group_stop_(group_->stop.get_token(), Relay{stop_}) {}

std::string_view Access::Session::token() const noexcept {
    return token_;
}

std::stop_token Access::Session::stopped() const noexcept {
    return stop_.get_token();
}

Access::Permit::Permit(std::shared_lock<std::shared_mutex> lock, std::stop_token stopped) : lock_(std::move(lock)), stopped_(std::move(stopped)) {}

std::stop_token Access::Permit::stopped() const noexcept {
    return stopped_;
}

std::expected<bool, Almanac::Error> Access::reset(Draft&& draft, Commit commit) {

    if (!draft.group_ || !commit) {
        return std::unexpected(Almanac::Error::input);
    }
    // guard 解开前共同失效旧组, 之后的登录只可能取得新组; 旧通知不会误伤后来创建的 Session.
    {
        const std::unique_lock guard(mutex_);
        const auto result = commit();
        if (!result || !*result) {
            return result;
        }
        group_->active = false;
        group_.swap(draft.group_);
        accounts_.swap(draft.accounts_);
    }
    draft.group_->stop.request_stop();
    draft.group_.reset();
    draft.accounts_.clear();
    return true;
}

std::expected<bool, Almanac::Error> Access::apply(std::string key, std::optional<Almanac::Buffer> secret, Commit commit) {

    if (!Scope::text(key, 128) || (secret && !valid(key, *secret)) || !commit) {
        return std::unexpected(Almanac::Error::input);
    }
    // prepared 仅包含待替换节点; map node_handle 移交不分配, 确保持久底稿提交后不再可能 bad_alloc.
    std::map<std::string, std::shared_ptr<Account>, std::less<>> prepared;
    if (secret) {
        auto account = std::make_shared<Account>(); // 构造停止状态也必须在 commit 之前.
        account->secret = std::move(*secret);
        prepared.emplace(key, std::move(account));
    }
    // retired 必须活过锁作用域, 取消回调可重入 close 或新登录, 不在独占锁内执行.
    std::shared_ptr<Account> retired;
    {
        const std::unique_lock guard(mutex_);
        const auto previous = accounts_.find(key); // 当前值决定同值 Set, 不使用锁外过期观察.
        const bool unchanged = secret && previous != accounts_.end() && equal(previous->second->secret, prepared.begin()->second->secret);
        if (secret && previous == accounts_.end() && accounts_.size() >= 65536) {
            return std::unexpected(Almanac::Error::capacity);
        }
        const auto result = commit(); // 失败或重放不改变原凭据和已有 Session.
        if (!result || !*result || unchanged) {
            return result;
        }
        if (previous != accounts_.end()) {
            retired = std::move(previous->second);
            retired->active = false;
            accounts_.erase(previous);
        }
        if (secret) {
            accounts_.insert(prepared.extract(prepared.begin()));
        }
    }
    if (retired) {
        retired->stop.request_stop();
    }
    return true;
}

std::expected<std::shared_ptr<Access::Session>, Access::Error> Access::open(std::string_view key, std::span<const std::uint8_t> secret) {

    if (!valid(key, secret)) {
        return std::unexpected(Error::denied);
    }
    // 仅随机生成在锁外完成; 不记录令牌, 最终匹配和安装在同一独占边界, 防止轮换后补装旧登录.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        std::string token(32, '\0'); // 原始随机字节, 长度固定为协议的 32 字节.
        if (RAND_bytes(reinterpret_cast<unsigned char*>(token.data()), token.size()) != 1) {
            return std::unexpected(Error::random);
        }
        const std::unique_lock guard(mutex_);
        const auto account = accounts_.find(key);
        if (account == accounts_.end() || !equal(account->second->secret, secret)) {
            return std::unexpected(Error::denied);
        }
        if (sessions_.size() >= maximum_) {
            return std::unexpected(Error::capacity);
        }
        if (sessions_.contains(token)) {
            continue;
        }
        // Session 的停止转发在当前有效组中安装; 组不会在本锁内已经停止或同时开始撤销.
        auto session = std::shared_ptr<Session>(new Session(token, account->second, group_));
        sessions_.emplace(std::move(token), session);
        return session;
    }
    return std::unexpected(Error::random);
}

std::expected<Access::Permit, Access::Error> Access::enter(std::string_view token) const {

    if (token.size() != 32) {
        return std::unexpected(Error::denied);
    }
    std::shared_lock guard(mutex_); // 成功时把锁移交 Permit, 不在函数返回前提前解开.
    const auto session = sessions_.find(token);
    if (session == sessions_.end() || !session->second->account_->active || !session->second->group_->active) {
        return std::unexpected(Error::denied);
    }
    return Permit(std::move(guard), session->second->stopped());
}

void Access::close(const std::shared_ptr<Session>& session) {

    if (!session) {
        return;
    }
    {
        const std::unique_lock guard(mutex_);
        const auto existing = sessions_.find(session->token_);
        if (existing == sessions_.end() || existing->second != session) {
            return;
        }
        sessions_.erase(existing);
    }
    session->stop_.request_stop();
}
} // namespace astra
