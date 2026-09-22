#include "access.hpp"
#include "check.hpp"
#include "orbit.pb.h"
#include "receiver.hpp"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <thread>

namespace {
using astra::Access;
using astra::Almanac;

// 创建包含二进制 SECRET 的私有准备, 不把密码作为 UTF-8 或 NUL 结尾文本处理.
Access::Draft prepare(std::uint8_t value = 1) {
    Access::Draft draft; // 本函数每次返回一个全新的凭据快照身份.
    CHECK(draft.set("first", {0, value, 255}));
    CHECK(draft.set("second", {2}));
    return draft;
}

// 单元边界用已完成提交模拟底稿, 真实 Library 的定序另由 receiver 用例验证.
std::expected<bool, Almanac::Error> committed() {
    return true;
}

// 验证一个有效会话, 返回值拥有引用但不延长实际流授权; close 仍能立即移除 token.
std::shared_ptr<Access::Session> login(Access& access, std::uint8_t value = 1) {
    const Almanac::Buffer secret{0, value, 255}; // 覆盖零字节和非文本字节.
    const auto session = access.open("first", secret);
    CHECK(session && (*session)->token().size() == 32 && access.enter((*session)->token()));
    return *session;
}

// 容量包含已经撤销但真实流尚未完成的 Session, 重复关闭不影响后来登录.
void lifecycle() {

    Access access(2);
    CHECK(access.reset(prepare(), committed));
    const auto first = login(access);
    const auto second = login(access);
    CHECK(first->token() != second->token());
    CHECK(access.open("first", Almanac::Buffer{0, 1, 255}).error() == Access::Error::capacity);
    CHECK(!access.open("first", Almanac::Buffer{0, 1}));
    CHECK(!access.open("missing", Almanac::Buffer{2}));
    CHECK(!access.open("first", {}));
    CHECK(!access.open(std::string(129, 'k'), Almanac::Buffer{2}));
    CHECK(!access.enter(""));
    CHECK(!access.enter(std::string(32, 'x')));
    access.close(first);
    CHECK(first->stopped().stop_requested() && !access.enter(first->token()));
    const auto third = login(access);
    access.close(first);
    CHECK(access.enter(second->token()) && access.enter(third->token()));
    access.close(second);
    access.close(third);
}

// 连续提交只撤销实际变化的账号, 完整新快照则必须撤销即使最终字节相同的旧会话.
void replacement() {

    Access access;
    CHECK(access.reset(prepare(), committed));
    const auto first = login(access);
    const auto second = *access.open("second", Almanac::Buffer{2});
    CHECK(access.apply("first", Almanac::Buffer{0, 1, 255}, committed));
    CHECK(access.enter(first->token()) && !first->stopped().stop_requested());
    CHECK(access.apply("first", Almanac::Buffer{0, 3, 255}, committed));
    CHECK(!access.enter(first->token()) && first->stopped().stop_requested());
    CHECK(access.enter(second->token()));
    const auto current = login(access, 3);
    CHECK(access.apply("first", std::nullopt, committed));
    CHECK(!access.enter(current->token()));
    CHECK(access.apply("first", Almanac::Buffer{0, 3, 255}, committed));
    CHECK(!access.enter(current->token()));
    const auto recreated = login(access, 3);
    CHECK(!*access.reset(prepare(), [] { return std::expected<bool, Almanac::Error>(false); }));
    CHECK(access.enter(recreated->token()) && access.enter(second->token()));
    CHECK(access.reset(prepare(3), committed));
    CHECK(!access.enter(recreated->token()) && !access.enter(second->token()));
    CHECK(recreated->stopped().stop_requested() && second->stopped().stop_requested());
    const auto renewed = login(access, 3);
    CHECK(access.enter(renewed->token()));
}

// 底稿准备/提交失败不先撤销权限; 取消回调在锁外执行, 可以重新登录或回收旧 Session.
void failures() {

    Access access;
    CHECK(access.reset(prepare(), committed));
    const auto original = login(access);
    const auto failed = [] { return std::expected<bool, Almanac::Error>(std::unexpected(Almanac::Error::capacity)); };
    CHECK(!access.apply("first", Almanac::Buffer{4}, failed));
    CHECK(!access.reset(prepare(4), failed));
    CHECK(access.enter(original->token()) && !original->stopped().stop_requested());
    bool threw{}; // 明确区分底稿抛出与普通 expected 错误.
    try {
        static_cast<void>(access.apply("first", Almanac::Buffer{4}, []() -> std::expected<bool, Almanac::Error> { throw std::bad_alloc{}; }));
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    CHECK(threw && access.enter(original->token()));
    std::shared_ptr<Access::Session> current; // 回调只保存新会话, 不把原 session 重新授权.
    std::stop_callback callback(original->stopped(), [&] {
        access.close(original);
        current = login(access, 5);
    });
    CHECK(access.apply("first", Almanac::Buffer{0, 5, 255}, committed));
    CHECK(current && access.enter(current->token()) && !access.enter(original->token()));
}

// 最终提交持有共享 Permit 时轮换必须等待, 轮换返回后同一 token 不能再次取得许可.
void ordering() {

    Access access;
    CHECK(access.reset(prepare(), committed));
    const auto session = login(access);
    std::promise<void> started; // 只协调本例两个线程的启动, 不依赖真实网络时序.
    auto arrival = started.get_future();
    std::atomic_bool installed{}; // 由工作线程 release 发布, 主线程 acquire 观察.
    std::jthread writer;
    {
        const auto permit = access.enter(session->token()); // 覆盖实际模拟业务提交的锁.
        CHECK(permit);
        writer = std::jthread([&] {
            started.set_value();
            const auto result = access.apply("first", Almanac::Buffer{9}, committed);
            installed.store(result && *result, std::memory_order_release);
        });
        CHECK(arrival.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK(!installed.load(std::memory_order_acquire));
    }
    writer.join();
    CHECK(installed.load(std::memory_order_acquire) && !access.enter(session->token()));
}

// 构造真实内部凭据快照, 每页都带固定权威版本, complete 之前不改变登录索引.
proto::polaris::v1::Packet snapshot(std::uint64_t version, bool complete, std::string key) {

    proto::polaris::v1::Packet packet;
    auto* page = packet.mutable_snapshot(); // 指向本包拥有的候选页, 不向外保存指针.
    page->mutable_scope()->set_sector("__auth");
    page->mutable_scope()->set_spectrum("comet");
    page->set_version(version);
    page->set_complete(complete);
    if (!key.empty()) {
        proto::orbit::v1::Credential credential; // 非文本 SECRET 原样进入内部 protobuf.
        credential.set_secret(std::string("\0\1\xff", 3));
        auto* entry = page->add_entries();
        entry->set_key(std::move(key));
        entry->set_value(credential.SerializeAsString());
    }
    return packet;
}

// Receiver 不允许底稿版本已前进而登录索引尚未安装, 失败重连也不复活旧 Session.
void receiving() {

    astra::Library library;
    Access access;
    astra::Receiver receiver(library, access);
    static_cast<void>(receiver.next());
    proto::polaris::v1::Packet plan; // 空首轮清单允许随后动态新增内部 Scope.
    plan.mutable_plan()->set_complete(true);
    CHECK(receiver.receive(plan));
    CHECK(receiver.receive(snapshot(1, false, "first")));
    CHECK(!library.find({"__auth", "comet"}) && !access.open("first", Almanac::Buffer{0, 1, 255}));
    CHECK(receiver.receive(snapshot(1, true, "")));
    const auto session = login(access);
    CHECK(library.find({"__auth", "comet"})->usage().version == 1);
    CHECK(receiver.receive(snapshot(1, true, "first")));
    CHECK(access.enter(session->token()));
    CHECK(receiver.receive(snapshot(3, true, "first")));
    CHECK(!access.enter(session->token()) && library.find({"__auth", "comet"})->usage().version == 3);
    const auto current = login(access);
    auto invalid = snapshot(4, true, "first"); // 非法 SECRET 必须在任何新版本发布前拒绝.
    invalid.mutable_snapshot()->mutable_entries(0)->set_value("");
    CHECK(!receiver.receive(invalid));
    CHECK(access.enter(current->token()) && library.find({"__auth", "comet"})->usage().version == 3);
}
} // namespace

// 本入口只被显式测试命令调用, 覆盖纯权限核心与真实 Receiver/Library 安装.
int main() {
    try {
        lifecycle();
        replacement();
        failures();
        ordering();
        receiving();
        std::cout << "PASS business access lifecycle and credential installation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
