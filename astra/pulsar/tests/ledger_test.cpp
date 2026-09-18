// 功能: 验证真正的登记持久提交, 重试/替换, 容量, 排他打开和损坏恢复边界.
#include "ledger.hpp"
#include "pulsar_test.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace astra;

// 只改变本测试进程的软文件上限, 模拟内核短写; 析构恢复原限制和信号处理器.
class FileLimit {
public:
    explicit FileLimit(rlim_t bytes) {
        CHECK(::getrlimit(RLIMIT_FSIZE, &previous_) == 0);
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        CHECK(::sigemptyset(&ignored.sa_mask) == 0);
        CHECK(::sigaction(SIGXFSZ, &ignored, &signal_) == 0);
        auto limit = previous_;
        limit.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            static_cast<void>(::sigaction(SIGXFSZ, &signal_, nullptr));
            throw std::runtime_error("Cannot install test file limit");
        }
    }
    ~FileLimit() {
        static_cast<void>(::setrlimit(RLIMIT_FSIZE, &previous_));
        static_cast<void>(::sigaction(SIGXFSZ, &signal_, nullptr));
    }
    FileLimit(const FileLimit&) = delete;
    FileLimit& operator=(const FileLimit&) = delete;

private:
    struct rlimit previous_{};
    struct sigaction signal_{};
};

// 同一个启动请求并发提交只追加一次. 每个线程独占结果槽, 主线程 join 后断言.
static void concurrent_retry(const std::filesystem::path& path, const std::string& authority) {
    MembershipLedger ledger(path, "alpha", authority, 2, 4);
    std::array<bool, 8> success{};
    std::array<Id, 8> ids{};
    std::vector<std::jthread> clients;
    for (std::size_t i = 0; i < success.size(); ++i) {
        clients.emplace_back([&, i] {
            try {
                auto candidate = test::member(1);
                candidate.id = "attempt-" + std::to_string(i);
                success[i] =
                    ledger.register_member(candidate, std::string(32, 'a'), [&](const Member& member, const auto&) { ids[i] = member.id; }).has_value();
            } catch (...) {
                success[i] = false;
            }
        });
    }
    clients.clear();
    for (std::size_t i = 0; i < success.size(); ++i) {
        CHECK(success[i] && ids[i] == ids.front());
    }
}

// 一条记录只写入前八个字节就失败, 当前视图保持原状; 重启丢弃残尾后允许重试.
static void partial_write(const std::filesystem::path& path, const std::string& authority) {
    Member committed;
    const auto prepare = [&](const Member& member, const auto&) { committed = member; };
    std::uintmax_t boundary{};
    {
        MembershipLedger ledger(path, "alpha", authority, 2, 4);
        CHECK(ledger.register_member(test::member(1), std::string(32, 'a'), prepare));
        const auto original = committed;
        boundary = std::filesystem::file_size(path);
        {
            FileLimit limited(static_cast<rlim_t>(boundary + 8));
            CHECK(!ledger.register_member(test::member(2), std::string(32, 'b'), prepare));
        }
        CHECK(std::filesystem::file_size(path) == boundary + 8);
        CHECK(ledger.current(original) && !ledger.current(committed));
        CHECK(!ledger.register_member(test::member(1), std::string(32, 'a'), prepare));
    }
    MembershipLedger restored(path, "alpha", authority, 2, 4);
    CHECK(std::filesystem::file_size(path) == boundary);
    CHECK(restored.register_member(test::member(2), std::string(32, 'b'), prepare));
    CHECK(committed.epoch.value == 1 && restored.current(committed));
}

int main() {
    try {
        test::Directory directory;
        const auto path = directory.path / "membership.journal";
        const std::string authority(64, 'a'), first(32, 'a'), second(32, 'b'), replacement(32, 'c');
        Member committed;
        std::size_t count{};
        const auto prepare = [&](const Member& local, const auto& members) {
            committed = local;
            count = members.size();
        };
        {
            MembershipLedger ledger(path, "alpha", authority, 2, 4);
            CHECK(ledger.register_member(test::member(1), first, prepare));
            CHECK(committed.epoch.value == 1 && ledger.current(committed));
            bool locked = false;
            try {
                MembershipLedger duplicate(path, "alpha", authority, 2, 4);
            } catch (const std::exception&) {
                locked = true;
            }
            CHECK(locked);
            CHECK(ledger.register_member(test::member(2), second, prepare));
            CHECK(ledger.register_member(test::member(1), first, prepare));
            CHECK(count == 2 && committed.id == "node-1");
            auto changed = test::member(1);
            changed.group = "other";
            CHECK(!ledger.register_member(changed, first, prepare));
            CHECK(!ledger.register_member(test::member(2), first, prepare));
            CHECK(!ledger.register_member(test::member(3), std::string(32, 'd'), prepare));
            const auto size = std::filesystem::file_size(path);
            bool failed = false;
            changed = test::member(1);
            changed.id = "replacement";
            try {
                static_cast<void>(ledger.register_member(changed, replacement, [](const auto&, const auto&) { throw std::bad_alloc{}; }));
            } catch (const std::bad_alloc&) {
                failed = true;
            }
            CHECK(failed && std::filesystem::file_size(path) == size);
            CHECK(ledger.register_member(changed, replacement, prepare));
            CHECK(committed.epoch.value == 2 && count == 2 && ledger.current(committed));
            CHECK(!ledger.register_member(test::member(1), first, prepare));
        }
        const auto size = std::filesystem::file_size(path);
        {
            std::ofstream tail(path, std::ios::binary | std::ios::app);
            tail.write("\0\0", 2);
        }
        {
            MembershipLedger restored(path, "alpha", authority, 2, 4);
            auto changed = test::member(1);
            changed.id = "ignored-on-retry";
            CHECK(restored.register_member(changed, replacement, prepare));
            CHECK(committed.id == "replacement" && committed.epoch.value == 2 && count == 2);
            CHECK(!restored.register_member(test::member(1), first, prepare));
            CHECK(std::filesystem::file_size(path) == size);
            auto planet = test::member(3, Role::planet);
            CHECK(restored.register_member(planet, std::string(32, 'd'), prepare));
            planet = test::member(4, Role::planet);
            CHECK(!restored.register_member(planet, std::string(32, 'e'), prepare));
        }
        // 完整记录的损坏不能按掉电尾巴丢弃, 也不能把别的 Galaxy/签发公钥套在现有账本上.
        for (const auto& galaxy : {std::string("other"), std::string("alpha")}) {
            bool rejected = false;
            try {
                MembershipLedger invalid(path, galaxy, std::string(64, 'b'), 2, 4);
            } catch (const std::exception&) {
                rejected = true;
            }
            CHECK(rejected);
        }
        {
            std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
            corrupt.seekp(-1, std::ios::end);
            corrupt.put('\xff');
        }
        bool rejected = false;
        try {
            MembershipLedger invalid(path, "alpha", authority, 2, 4);
        } catch (const std::exception&) {
            rejected = true;
        }
        CHECK(rejected);
        concurrent_retry(directory.path / "concurrent.journal", authority);
        partial_write(directory.path / "short-write.journal", authority);
        std::cout << "PASS durable registration, fresh retry snapshots, replacement, capacity, rollback, locking and recovery\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
