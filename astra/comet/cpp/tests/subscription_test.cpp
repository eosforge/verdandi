#include "check.hpp"
#include "duplicates.hpp"
#include "subscription.hpp"
#include <iostream>

namespace {
using comet::Subscriber;
using comet::detail::Subscription;
using Reply = proto::comet::v1::CatalogWatchReply;

// 真实生成消息, 正内容版本与本地视图游标故意取不同数值.
Reply page(proto::comet::v1::Mode mode, std::uint64_t cursor, std::string instance = "star-a") {
    Reply result;
    result.set_mode(mode);
    result.set_complete(true);
    result.set_version(cursor);
    result.set_instance(std::move(instance));
    return result;
}

// 零字节 value 仍是明确存在的 action, 不与删除混淆.
void value(Reply& page, std::string key, std::uint64_t version, std::string text) {
    auto* item = page.add_changes();
    item->set_key(std::move(key));
    item->set_version(version);
    item->set_value(std::move(text));
}

// 换 Star 完整视图允许较低游标/内容版本, 原视图仍保有完整旧值, 不按 Key 拼接两张视图.
void lifecycle() {

    Subscriber::View held;
    {
        Subscription subscription({"dynamic", "main"}, {}, 65536, 10);
        subscription.begin("star-a");
        auto initial = page(proto::comet::v1::MODE_RESET, 20);
        value(initial, "left", 100, "one");
        value(initial, "right", 7, "");
        auto received = subscription.accept(initial);
        CHECK(received && *received && (**received).version() == 20);
        held = **received;
        CHECK(held.find("right")->value->empty());
        auto update = page(proto::comet::v1::MODE_APPLY, 21);
        value(update, "left", 1000, "two");
        CHECK(subscription.accept(update));
        auto erase = page(proto::comet::v1::MODE_APPLY, 22);
        auto* item = erase.add_changes();
        item->set_key("right");
        item->mutable_erase();
        CHECK(subscription.accept(erase));
        CHECK(!subscription.view(Subscriber::State::ready).find("right"));

        subscription.begin("star-b");
        auto replacement = page(proto::comet::v1::MODE_RESET, 1, "star-b");
        value(replacement, "left", 5, "older");
        received = subscription.accept(replacement);
        CHECK(received && *received && (**received).version() == 1 && (**received).size() == 1);
        CHECK((**received).find("left")->version == 5 && !((**received).find("right")));
        auto rollback = page(proto::comet::v1::MODE_APPLY, 0, "star-b");
        CHECK(!subscription.accept(rollback)); // 同实例视图游标不能回退.
    }
    std::size_t count{}; // 退出安装器后应用视图仍可遍历.
    held.each([&](std::string_view key, const Subscriber::Record& record) { CHECK(!key.empty() && record.version > 0 && record.value); ++count; });
    CHECK(count == 2 && held.find("left")->version == 100);
}

// 无版本正文、有版本删除、跨页重复及未完整页失败都不能发布半批或覆盖旧根.
void malformed() {

    Subscription subscription({"dynamic", "main"}, {}, 65536, 2);
    subscription.begin("star-a");
    auto initial = page(proto::comet::v1::MODE_RESET, 1);
    value(initial, "key", 1, "initial");
    CHECK(subscription.accept(initial));
    auto zero = page(proto::comet::v1::MODE_APPLY, 2);
    value(zero, "key", 0, "invalid");
    CHECK(!subscription.accept(zero));
    CHECK(subscription.view(Subscriber::State::failed).find("key")->version == 1);
    subscription.begin("star-a");
    auto erased = page(proto::comet::v1::MODE_APPLY, 2);
    auto* item = erased.add_changes();
    item->set_key("key");
    item->set_version(1);
    item->mutable_erase();
    CHECK(!subscription.accept(erased));

    subscription.begin("star-a");
    auto head = page(proto::comet::v1::MODE_RESET, 3);
    head.set_complete(false);
    head.clear_version();
    head.clear_instance();
    value(head, "key", 2, "partial");
    const auto accepted = subscription.accept(head);
    CHECK(accepted && !*accepted && subscription.view(Subscriber::State::stale).find("key")->version == 1);
    auto tail = page(proto::comet::v1::MODE_RESET, 3);
    value(tail, "key", 3, "duplicate");
    CHECK(!subscription.accept(tail));
    CHECK(subscription.view(Subscriber::State::failed).find("key")->version == 1);
}
} // namespace

// 纯安装用例只消费生成消息, 不启动 gRPC 或访问物理时间.
int main() {
    try {
        lifecycle();
        malformed();
        comet::test::duplicates<Subscription>([](auto& change, std::string_view key) { change.set_key(key); change.mutable_erase(); }); // Catalog Delete 不携带业务版本.
        std::cout << "Catalog subscription tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
