#include "check.hpp"
#include "duplicates.hpp"
#include "selection.hpp"
#include <iostream>

namespace {
using comet::Observer;
using comet::detail::Selection;
using Reply = proto::comet::v1::EphemerisWatchReply;
constexpr std::string_view first = "00000000-0000-4000-8000-000000000001"; // 两个固定合法 UUID, 不依赖随机发生器.
constexpr std::string_view second = "00000000-0000-4000-8000-000000000002";

// 使用真实生成消息, 非 complete 页没有可安装位置.
Reply page(proto::comet::v1::Mode mode, bool complete = true, std::uint64_t version = 1, std::string instance = "star-a") {
    Reply result;
    result.set_mode(mode);
    result.set_complete(complete);
    if (complete) {
        result.set_version(version);
        result.set_instance(std::move(instance));
    }
    return result;
}

// 完整记录即使两个字段均为空也仍是存在, 不用空载荷代替 Delete.
void record(Reply& page, std::string_view uuid, std::string attr = {}, std::string data = {}) {
    auto* change = page.add_changes();
    change->set_uuid(uuid);
    change->mutable_record()->set_attr(std::move(attr));
    change->mutable_record()->set_data(std::move(data));
}

// Data-only 不包含 Attr, 必须由已有完整视图提供不可变属性.
void data(Reply& page, std::string_view uuid, std::string value = {}) {
    auto* change = page.add_changes();
    change->set_uuid(uuid);
    change->set_data(std::move(value));
}

// 只有最终完整页才安装, 高频 Data 复用 Attr, 旧 View 不依赖 Selection 的生命周期.
void lifecycle() {

    Observer::View held;
    {
        Selection selection({"service", "main"}, {}, 65536, 100);
        selection.begin("star-a");
        auto head = page(proto::comet::v1::MODE_RESET, false);
        record(head, first, std::string(8, static_cast<char>(0xff)), "old");
        auto accepted = selection.accept(head);
        CHECK(accepted && !*accepted && !selection.view(Observer::State::waiting).version());
        auto tail = page(proto::comet::v1::MODE_RESET);
        record(tail, second);
        accepted = selection.accept(tail);
        CHECK(accepted && *accepted && (**accepted).size() == 2);
        held = **accepted;
        CHECK(held.find(second)->attr->empty() && held.find(second)->data->empty());
        auto update = page(proto::comet::v1::MODE_APPLY, true, 2);
        data(update, first);
        accepted = selection.accept(update);
        CHECK(accepted && *accepted);
        CHECK((**accepted).find(first)->attr == held.find(first)->attr);
        CHECK((**accepted).find(first)->data->empty() && !held.find(first)->data->empty());
        auto complete = page(proto::comet::v1::MODE_APPLY, true, 3);
        record(complete, first, std::string(8, static_cast<char>(0xff)), "new");
        CHECK(selection.accept(complete)); // 有符号 char 的高位 Attr 也应精确按字节相等.
        auto erase = page(proto::comet::v1::MODE_APPLY, true, 4);
        erase.add_changes()->set_uuid(first);
        erase.mutable_changes(0)->mutable_erase();
        CHECK(selection.accept(erase));
        CHECK(!selection.view(Observer::State::ready).find(first));
    }
    CHECK(held.find(first) && held.find(first)->data->size() == 3);
    std::size_t count{}; // 脱离网络与投影对象后仍能完整遍历.
    held.each([&](std::string_view uuid, const Observer::Record& value) { CHECK(Selection::valid(uuid) && value.attr && value.data); ++count; });
    CHECK(count == 2);
}

// 缺 Attr 不发布半记录, 本对象最多一次无版本新快照, 成功后再次缺失也不得无限恢复.
void repair() {

    Selection selection({"service", "main"}, {}, 65536, 100);
    selection.begin("star-a");
    CHECK(selection.accept(page(proto::comet::v1::MODE_RESET)));
    auto missing = page(proto::comet::v1::MODE_APPLY, true, 2);
    data(missing, first, "partial");
    auto result = selection.accept(missing);
    CHECK(!result && result.error().code == comet::Error::Code::history);
    CHECK(selection.view(Observer::State::stale).version() == 1 && selection.view(Observer::State::stale).size() == 0);
    CHECK(selection.repair(result.error().code) && !selection.resume());
    selection.begin("star-a");
    CHECK(!selection.accept(page(proto::comet::v1::MODE_APPLY, true, 2))); // 强制快照时不承接旧容器的 apply.
    selection.begin("star-a");
    auto reset = page(proto::comet::v1::MODE_RESET, true, 2);
    record(reset, first);
    CHECK(selection.accept(reset) && selection.resume());
    auto repeated = page(proto::comet::v1::MODE_APPLY, true, 3);
    data(repeated, second);
    result = selection.accept(repeated);
    CHECK(!result && !selection.repair(result.error().code));
    CHECK(selection.view(Observer::State::stale).version() == 2);
}

// 游标属于实际 Star; 新 Star 可以从较低本地游标完整替换, 同 Star 不可回退.
void identity() {

    Selection selection({"service", "main"}, {}, 65536, 100);
    selection.begin("star-a");
    auto old = page(proto::comet::v1::MODE_RESET, true, 100);
    record(old, first);
    CHECK(selection.accept(old));
    selection.begin("star-a");
    CHECK(!selection.accept(page(proto::comet::v1::MODE_RESET, true, 99)));
    selection.begin("star-b");
    auto reset = page(proto::comet::v1::MODE_RESET, true, 1, "star-b");
    record(reset, second);
    CHECK(selection.accept(reset));
    auto view = selection.view(Observer::State::ready);
    CHECK(view.instance() == "star-b" && view.version() == 1 && !view.find(first) && view.find(second));
    selection.begin("star-c");
    CHECK(!selection.accept(page(proto::comet::v1::MODE_APPLY, true, 2, "star-c")));
}

// 畸形批次和可见预算失败都不能发布已准备的早期合法记录.
void malformed() {

    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        Selection selection({"service", "main"}, {}, 65536, 100);
        selection.begin("star-a");
        auto base = page(proto::comet::v1::MODE_RESET);
        record(base, first, "fixed");
        CHECK(selection.accept(base));
        auto bad = page(proto::comet::v1::MODE_APPLY, true, 2);
        switch (scenario) {
        case 0:
            record(bad, first, "changed");
            break;
        case 1:
            data(bad, "invalid");
            break;
        case 2:
            data(bad, first);
            data(bad, first);
            break;
        case 3:
            bad.add_changes()->set_uuid(first);
            break;
        case 4:
            data(bad, first);
            bad.set_version(0);
            break;
        case 5:
            data(bad, first);
            bad.clear_instance();
            break;
        case 6:
            data(bad, first, std::string(65536, 'x'));
            break;
        case 7:
            data(bad, first);
            bad.set_mode(proto::comet::v1::MODE_RESET);
            break;
        }
        CHECK(!selection.accept(bad));
        CHECK(selection.view(Observer::State::ready).version() == 1 && selection.view(Observer::State::ready).find(first)->data->empty());
    }
    Selection exact({"service", "main"}, std::string(first), 65536, 100);
    exact.begin("star-a");
    auto wrong = page(proto::comet::v1::MODE_RESET);
    record(wrong, second);
    CHECK(!exact.accept(wrong));
    for (auto mode : {proto::comet::v1::MODE_RESET, proto::comet::v1::MODE_APPLY}) {
        Selection empty({"service", "main"}, {}, 65536, 100);
        empty.begin("star-a");
        auto missing = page(mode);
        data(missing, first);
        CHECK(!empty.accept(missing)); // reset 禁止 data; 首批 apply 没有完整基线.
    }
}
} // namespace

int main() {
    try {
        lifecycle();
        repair();
        identity();
        malformed();
        comet::test::duplicates<Selection>([](auto& change, std::string_view key) { change.set_uuid(key); change.mutable_erase(); }); // Ephemeris 使用 UUID, 不能只覆盖普通 Key 域.
        std::cout << "Ephemeris selection cases passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
