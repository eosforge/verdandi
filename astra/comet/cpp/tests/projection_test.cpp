#include "check.hpp"
#include "projection.hpp"
#include <atomic>
#include <iostream>
#include <thread>

namespace {
using comet::detail::Projection;
using Reply = proto::comet::v1::AlmanacWatchReply;

// 构造真实生成消息, 完整字段只在 complete=true 时出现.
Reply page(proto::comet::v1::Mode mode, bool complete = false, std::uint64_t version = 0, std::string instance = "star-test") {
    Reply reply;
    reply.set_mode(mode);
    reply.set_complete(complete);
    if (complete) {
        reply.set_instance(std::move(instance));
        reply.set_version(version);
    }
    return reply;
}

// 一个完整 Set, 包含空 value 时也设置正确 oneof 分支.
void set(Reply& reply, std::string key, std::string value = {}) {
    auto* change = reply.add_changes();
    change->set_key(std::move(key));
    change->set_value(std::move(value));
}

// 原子发布与旧 View 独立存活, 半份快照/增量不进入当前读取结果.
void lifecycle() {

    comet::Reader::View held;
    {
        Projection projection({"routes", "main"}, {}, 65536, 1000);
        CHECK(!projection.view(comet::Reader::State::waiting).version());
        projection.begin();
        auto first = page(proto::comet::v1::MODE_RESET);
        set(first, "first", "before");
        auto partial = projection.accept(first);
        CHECK(partial && !*partial && !projection.view(comet::Reader::State::waiting).version());
        auto last = page(proto::comet::v1::MODE_RESET, true, 1);
        set(last, "empty");
        auto completed = projection.accept(last);
        CHECK(completed && *completed);
        held = **completed;
        CHECK(held.version() == 1 && held.size() == 2 && held.find("empty") && held.find("empty")->empty());
        auto update = page(proto::comet::v1::MODE_APPLY, true, 2);
        set(update, "first", "after");
        auto changed = projection.accept(update);
        CHECK(changed && *changed && (**changed).find("first") != held.find("first"));
        CHECK((**changed).find("empty") == held.find("empty"));
        auto erase = page(proto::comet::v1::MODE_APPLY, true, 3);
        erase.add_changes()->set_key("first");
        erase.mutable_changes(0)->mutable_erase();
        CHECK(projection.accept(erase));
        CHECK(!projection.view(comet::Reader::State::ready).find("first"));
        CHECK(held.state() == comet::Reader::State::ready);
    }
    CHECK(held.find("first") && std::string(held.find("first")->begin(), held.find("first")->end()) == "before");
    std::size_t records{}; // 回调在 SDK 对象全部销毁后仍可遍历内容.
    held.each([&](std::string_view key, const comet::Value& value) { CHECK(!key.empty() && value); ++records; });
    CHECK(records == 2);
}

// 跨页重复、非法字段及 mode/instance 变化必须废弃整批, 完整版本不受失败污染.
void malformed() {

    Projection projection({"routes", "main"}, {}, 65536, 1000);
    projection.begin();
    auto empty = page(proto::comet::v1::MODE_RESET, true, 1);
    CHECK(projection.accept(empty));
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        projection.begin("star-test");
        auto candidate = page(proto::comet::v1::MODE_RESET);
        set(candidate, "kept", "private");
        CHECK(projection.accept(candidate));
        auto bad = page(proto::comet::v1::MODE_RESET, true, 2);
        switch (scenario) {
        case 0:
            set(bad, "kept");
            break;
        case 1:
            bad.set_mode(proto::comet::v1::MODE_APPLY);
            break;
        case 2:
            bad.clear_version();
            break;
        case 3:
            bad.set_instance("different");
            break;
        case 4:
            bad.add_changes()->set_key("no-action");
            break;
        case 5:
            bad.add_changes()->mutable_erase();
            break;
        case 6:
            bad.set_version(0);
            break;
        case 7:
            set(bad, std::string("bad\0key", 7));
            break;
        }
        const auto rejected = projection.accept(bad);
        CHECK(!rejected && rejected.error().code == comet::Error::Code::protocol);
        CHECK(projection.view(comet::Reader::State::stale).version() == 1 && projection.view(comet::Reader::State::stale).size() == 0);
        CHECK(!projection.accept(empty)); // 错误后不能在同一流继续拼接新页面.
    }
}

// Almanac 跨 Star 仍有权威版本下限, 同实例空 apply 可用于首次恢复已追平的位置.
void versions() {

    Projection projection({"routes", "main"}, "key", 65536, 16);
    projection.begin();
    CHECK(!projection.accept(page(proto::comet::v1::MODE_APPLY, true, 1)));
    projection.begin("first");
    CHECK(projection.accept(page(proto::comet::v1::MODE_RESET, true, 5, "first")));
    projection.begin("second");
    CHECK(!projection.accept(page(proto::comet::v1::MODE_RESET, true, 4, "second")));
    projection.begin("second");
    CHECK(projection.accept(page(proto::comet::v1::MODE_RESET, true, 5, "second")));
    projection.begin("second");
    CHECK(projection.accept(page(proto::comet::v1::MODE_APPLY, true, 5, "second")));
    CHECK(!projection.accept(page(proto::comet::v1::MODE_APPLY, true, 5, "second")));
    projection.begin("second");
    auto wrong = page(proto::comet::v1::MODE_RESET, true, 6, "second");
    set(wrong, "another-key");
    CHECK(!projection.accept(wrong));
    CHECK(projection.view(comet::Reader::State::stale).version() == 5);
}

// 最终容量与临时替换次序分开, 先 Set 再 Delete 不会把合法的同容量替换错误拒绝.
void capacity() {

    Projection projection({"routes", "main"}, {}, 32768, 1);
    projection.begin();
    auto initial = page(proto::comet::v1::MODE_RESET, true, 1);
    set(initial, "old", "value");
    CHECK(projection.accept(initial));
    auto update = page(proto::comet::v1::MODE_APPLY, true, 2);
    set(update, "new", "value");
    auto* erased = update.add_changes();
    erased->set_key("old");
    erased->mutable_erase();
    CHECK(projection.accept(update));
    CHECK(projection.view(comet::Reader::State::ready).size() == 1);
    auto large = page(proto::comet::v1::MODE_APPLY, true, 3);
    set(large, "new", std::string(65537, 'x'));
    const auto failed = projection.accept(large);
    CHECK(!failed && failed.error().code == comet::Error::Code::limit);
    CHECK(projection.view(comet::Reader::State::stale).version() == 2);
}

// 全局预算拒绝必须发生在发布之前, 准备丢弃后只保留原当前根计费, 析构再归零.
void shared_budget() {

    std::size_t held{}; // 模拟共享 Client 已承诺给这个 Reading 的字节, 不凭句柄大小计量.
    bool blocked{};     // 后续增长被其他对象占满, 缩小和归零仍必须成功.
    {
        Projection projection({"routes", "main"}, {}, 65536, 128, [&](std::size_t bytes) noexcept {
            if (blocked && bytes > held) {
                return false;
            }
            held = bytes;
            return true;
        });
        projection.begin();
        auto initial = page(proto::comet::v1::MODE_RESET, true, 1);
        set(initial, "old", "value");
        CHECK(projection.accept(initial));
        const auto original = held; // 内容、根、页、节点和桶均占用受控计费.
        CHECK(original > 8);
        blocked = true;
        auto next = page(proto::comet::v1::MODE_APPLY, true, 2);
        set(next, "new", "value");
        const auto rejected = projection.accept(next);
        CHECK(!rejected && rejected.error().code == comet::Error::Code::limit);
        CHECK(held == original && projection.view(comet::Reader::State::stale).version() == 1);
    }
    CHECK(held == 0);
}

// 已发布页面从不改写, 并发读取和最后释放旧根不需要进入 Client 锁或读完成屏障.
void concurrent() {

    Projection projection({"routes", "main"}, {}, 1024 * 1024, 4096);
    projection.begin();
    auto initial = page(proto::comet::v1::MODE_RESET, true, 1);
    for (unsigned index = 0; index < 2000; ++index) {
        set(initial, "key-" + std::to_string(index), "original");
    }
    auto result = projection.accept(initial);
    CHECK(result && *result);
    auto held = **result; // 只把不可变视图移入读线程, Projection 始终单写者.
    std::atomic_bool valid{true};
    std::jthread reader([held, &valid] {
        for (unsigned pass = 0; pass < 100; ++pass) {
            held.each([&valid](std::string_view, const comet::Value& value) { if (!value || std::string(value->begin(), value->end()) != "original") { valid = false; } });
        }
    });
    for (std::uint64_t version = 2; version < 200; ++version) {
        auto delta = page(proto::comet::v1::MODE_APPLY, true, version);
        set(delta, "key-" + std::to_string(version), "changed");
        CHECK(projection.accept(delta));
    }
    reader.join();
    CHECK(valid && held.size() == 2000 && held.version() == 1);
}
} // namespace

// 仅构造本地生成消息, 不启动服务或连接真实部署; 编写用例不代表已经执行.
int main() {
    try {
        lifecycle();
        malformed();
        versions();
        capacity();
        shared_budget();
        concurrent();
        std::cout << "Comet projection tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
