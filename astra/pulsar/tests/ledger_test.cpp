#include "ledger.hpp"
#include "pulsar_test.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <thread>
#include <vector>

using namespace astra;

namespace {

// 只改变本测试进程的软文件上限, 模拟内核短写; 析构恢复原限制和信号处理器.
class Limit {
public:
    // bytes 为临时文件软限制, 保存旧限制和 SIGXFSZ 处理器, 失败回滚信号设置.
    explicit Limit(rlim_t bytes) {

        CHECK(::getrlimit(RLIMIT_FSIZE, &previous_) == 0);
        // ignored 临时忽略超限信号, 让写调用返回错误供账本处理.
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        CHECK(::sigemptyset(&ignored.sa_mask) == 0);
        CHECK(::sigaction(SIGXFSZ, &ignored, &signal_) == 0);
        // limit 复制旧硬限制, 仅替换软限制, 不提升系统权限.
        auto limit = previous_;
        limit.rlim_cur = bytes;
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            static_cast<void>(::sigaction(SIGXFSZ, &signal_, nullptr));
            throw std::runtime_error("Cannot install test file limit");
        }
    }

    // 恢复进入作用域前的文件限制和信号处理器, 析构不抛异常.
    ~Limit() {
        static_cast<void>(::setrlimit(RLIMIT_FSIZE, &previous_));
        static_cast<void>(::sigaction(SIGXFSZ, &signal_, nullptr));
    }

    // 禁止复制进程级限制的恢复责任.
    Limit(const Limit&) = delete;
    // 禁止覆盖已经生效的恢复状态.
    Limit& operator=(const Limit&) = delete;

private:
    // previous_ 保存进程原始软硬文件限制.
    struct rlimit previous_{};
    // signal_ 保存原始 SIGXFSZ 处理方式.
    struct sigaction signal_{};
};

// 同一个启动请求并发提交只追加一次. 每个线程独占结果槽, 主线程 join 后断言.
static void concurrent_retry(const std::filesystem::path& path, const std::string& authority) {

    // ledger 独占当前测试日志, 每角色最多 2 个成员, 历史启动最多 4 条.
    Ledger ledger(path, "alpha", authority, 2, 4);
    // success 每线程独占一个结果槽, join 后统一读取.
    std::array<bool, 8> success{};
    // ids 每线程保存同一幂等登记最终返回的成员 ID.
    std::array<Id, 8> ids{};
    // clients 拥有八个并发登记线程, clear 时全部 join.
    std::vector<std::jthread> clients;
    // i 为独立结果槽下标, 捕获值固定各线程责任.
    for (std::size_t i = 0; i < success.size(); ++i) {
        clients.emplace_back([&, i] {
            try {
                // candidate 为当前线程独有的候选, 请求 ID 相同而建议成员 ID 不同.
                auto candidate = test::member(1);
                candidate.id = "attempt-" + std::to_string(i);
                success[i] = ledger.register_member(candidate, std::string(32, 'a'), [&](const Member& member, const auto&) { ids[i] = member.id; }).has_value();
            } catch (...) {
                success[i] = false;
            }
        });
    }
    clients.clear();
    // i 为独立结果槽下标, 捕获值固定各线程责任.
    for (std::size_t i = 0; i < success.size(); ++i) {
        CHECK(success[i] && ids[i] == ids.front());
    }
}

// 一条记录只写入前八个字节就失败, 当前视图保持原状; 重启丢弃残尾后允许重试.
static void partial_write(const std::filesystem::path& path, const std::string& authority) {

    // committed 记录 prepare 回调提供的候选, 不等同于已经持久提交成功.
    Member committed;
    // prepare 借用结果变量记录应答准备阶段的候选, 不代表落盘已成功.
    const auto prepare = [&](const Member& member, const auto&) { committed = member; };
    // boundary 记录完整日志尾位置, 后续短写和恢复都与此比较.
    std::uintmax_t boundary{};
    {
        // ledger 独占当前测试日志, 每角色最多 2 个成员, 历史启动最多 4 条.
        Ledger ledger(path, "alpha", authority, 2, 4);
        CHECK(ledger.register_member(test::member(1), std::string(32, 'a'), prepare));
        // original 保存短写前已提交的成员身份, 用于验证当前视图未被污染.
        const auto original = committed;
        boundary = std::filesystem::file_size(path);
        {
            // limited 仅允许追加八字节, 析构后恢复文件上限.
            Limit limited(static_cast<rlim_t>(boundary + 8));
            CHECK(!ledger.register_member(test::member(2), std::string(32, 'b'), prepare));
        }
        CHECK(std::filesystem::file_size(path) == boundary + 8);
        CHECK(ledger.current(original) && !ledger.current(committed));
        CHECK(!ledger.register_member(test::member(1), std::string(32, 'a'), prepare));
    }

    // restored 重放同一日志, 丢弃不完整尾部后提供当前视图.
    Ledger restored(path, "alpha", authority, 2, 4);
    CHECK(std::filesystem::file_size(path) == boundary);
    CHECK(restored.register_member(test::member(2), std::string(32, 'b'), prepare));
    CHECK(committed.epoch.value == 1 && restored.current(committed));
}

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
int main() {

    try {
        test::Directory directory;
        // path 位于本测试独占的临时目录, 不使用部署日志.
        const auto path = directory.path / "membership.journal";
        // authority 是模拟签发公钥摘要; first/second/replacement 为不同启动请求 ID.
        const std::string authority(64, 'a'), first(32, 'a'), second(32, 'b'), replacement(32, 'c');
        // committed 记录 prepare 回调提供的候选, 不等同于已经持久提交成功.
        Member committed;
        // count 记录 prepare 所见成员数, 初始零.
        std::size_t count{};
        // prepare 借用结果变量记录应答准备阶段的候选, 不代表落盘已成功.
        const auto prepare = [&](const Member& local, const auto& members) {
            committed = local;
            count = members.size();
        };
        {
            // ledger 独占当前测试日志, 每角色最多 2 个成员, 历史启动最多 4 条.
            Ledger ledger(path, "alpha", authority, 2, 4);
            CHECK(ledger.register_member(test::member(1), first, prepare));
            CHECK(committed.epoch.value == 1 && ledger.current(committed));
            // locked 初始 false, 同路径二次打开被拒绝后置 true.
            bool locked = false;
            try {
                // duplicate 故意争用已有独占日志, 构造必须失败.
                Ledger duplicate(path, "alpha", authority, 2, 4);
            } catch (const std::exception&) {
                locked = true;
            }
            CHECK(locked);
            CHECK(ledger.register_member(test::member(2), second, prepare));
            CHECK(ledger.register_member(test::member(1), first, prepare));
            CHECK(count == 2 && committed.id == "node-1");
            // changed 独立复制测试成员, 修改分组或 ID 来验证幂等绑定和替换.
            auto changed = test::member(1);
            changed.group = "other";
            CHECK(!ledger.register_member(changed, first, prepare));
            CHECK(!ledger.register_member(test::member(2), first, prepare));
            CHECK(!ledger.register_member(test::member(3), std::string(32, 'd'), prepare));
            // size 保存变更前文件长度, 用于确认失败没有追加或恢复已截尾.
            const auto size = std::filesystem::file_size(path);
            // failed 只接受预期分配异常, 防止误把其他拒绝当作回滚成功.
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

        // size 保存变更前文件长度, 用于确认失败没有追加或恢复已截尾.
        const auto size = std::filesystem::file_size(path);
        {
            // tail 追加两字节不完整记录头, 模拟写入中断.
            std::ofstream tail(path, std::ios::binary | std::ios::app);
            tail.write("\0\0", 2);
        }
        {
            // restored 重放同一日志, 丢弃不完整尾部后提供当前视图.
            Ledger restored(path, "alpha", authority, 2, 4);
            // changed 独立复制测试成员, 修改分组或 ID 来验证幂等绑定和替换.
            auto changed = test::member(1);
            changed.id = "ignored-on-retry";
            CHECK(restored.register_member(changed, replacement, prepare));
            CHECK(committed.id == "replacement" && committed.epoch.value == 2 && count == 2);
            CHECK(!restored.register_member(test::member(1), first, prepare));
            CHECK(std::filesystem::file_size(path) == size);
            // planet 单独验证第二角色的容量和累计启动容量.
            auto planet = test::member(3, Member::Role::planet);
            CHECK(restored.register_member(planet, std::string(32, 'd'), prepare));
            planet = test::member(4, Member::Role::planet);
            CHECK(!restored.register_member(planet, std::string(32, 'e'), prepare));
        }

        // 完整记录的损坏不能按掉电尾巴丢弃, 也不能把别的 Galaxy/签发公钥套在现有账本上.
        for (const auto& galaxy : {std::string("other"), std::string("alpha")}) {
            // rejected 初始 false, 仅日志构造明确抛异常才置 true.
            bool rejected = false;
            try {
                // invalid 故意使用损坏或用途不匹配的日志, 不得成功恢复.
                Ledger invalid(path, galaxy, std::string(64, 'b'), 2, 4);
            } catch (const std::exception&) {
                rejected = true;
            }
            CHECK(rejected);
        }
        {
            // corrupt 只改本测试文件的最后一字节, 模拟完整记录摘要损坏.
            std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
            corrupt.seekp(-1, std::ios::end);
            corrupt.put('\xff');
        }

        // rejected 初始 false, 仅日志构造明确抛异常才置 true.
        bool rejected = false;
        try {
            // invalid 故意使用损坏或用途不匹配的日志, 不得成功恢复.
            Ledger invalid(path, "alpha", authority, 2, 4);
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
