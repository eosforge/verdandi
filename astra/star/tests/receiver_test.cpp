#include "check.hpp"
#include "receiver.hpp"
#include <iostream>

namespace {
// 声明唯一初始范围及最低版本, 清单在一个完整帧结束.
proto::polaris::v1::Packet plan(std::uint64_t version) {
    proto::polaris::v1::Packet packet; // 本例固定 a/s, 不依赖部署目录.
    auto* page = packet.mutable_plan();
    page->set_complete(true);
    auto* position = page->add_positions();
    position->mutable_scope()->set_sector("a");
    position->mutable_scope()->set_spectrum("s");
    position->set_version(version);
    return packet;
}

// 创建一页 Set 快照, key 为空表示空页, 版本每页都有但 complete 才能安装.
proto::polaris::v1::Packet snapshot(std::uint64_t version, bool complete, std::string key) {

    proto::polaris::v1::Packet packet;
    auto* page = packet.mutable_snapshot();
    page->mutable_scope()->set_sector("a");
    page->mutable_scope()->set_spectrum("s");
    page->set_version(version);
    page->set_complete(complete);
    if (!key.empty()) {
        auto* entry = page->add_entries(); // 零字节 Set 与没有 action 保持区别.
        entry->set_key(std::move(key));
        entry->set_value("");
    }
    return packet;
}

// 一条单 Key 有序补丁, 后续测试可显式修改版本或 action 制造边界输入.
proto::polaris::v1::Packet update(std::uint64_t version) {
    proto::polaris::v1::Packet packet;
    auto* page = packet.mutable_updates();
    page->mutable_scope()->set_sector("a");
    page->mutable_scope()->set_spectrum("s");
    auto* patch = page->add_patches();
    patch->set_version(version);
    patch->mutable_change()->set_key("key");
    patch->mutable_change()->set_value("changed");
    return packet;
}

// 跨页断流保留旧状态, 重连只报告实际安装版本, 完整页后才生成累计确认.
void snapshots() {

    astra::Library library;
    astra::Access access; // 同一个 Library 跨流共享登录索引.
    const astra::Scope scope{"a", "s"};
    {
        astra::Receiver receiver(library, access);
        CHECK(receiver.next()->inventory().complete()); // 空进程也必须发送明确完整清单.
        CHECK(receiver.receive(plan(4)));
        CHECK(receiver.receive(snapshot(4, false, "key")));
        CHECK(!receiver.next() && !library.find(scope));
    }

    astra::Receiver receiver(library, access); // 上一条流半份候选不能遗留到新流.
    CHECK(receiver.next()->inventory().positions().empty());
    CHECK(receiver.receive(plan(4)));
    CHECK(receiver.receive(snapshot(4, false, "key")));
    CHECK(receiver.receive(snapshot(4, true, "other")));
    CHECK(library.find(scope)->view()->size() == 2);
    CHECK(receiver.next()->acknowledged().version() == 4);
    CHECK(receiver.receive(update(5)));
    CHECK(receiver.receive(update(6)));
    CHECK(receiver.next()->acknowledged().version() == 6 && !receiver.next());
    proto::polaris::v1::Packet ready; // 服务端最终 ready 仍需本地范围版本验证.
    ready.mutable_ready();
    CHECK(receiver.receive(ready) && receiver.ready());
    CHECK(!receiver.receive(update(8)) && !receiver.ready());
    CHECK(!receiver.receive(update(7))); // 首次错误封闭流, 不能继续填补后悄悄恢复.
    CHECK(library.find(scope)->usage().version == 6);
}

// 重复 Key、混版本和空基线边界必须在协议层闭合, 失败不安装任何部分候选.
void invalid() {

    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        astra::Library library;
        astra::Access access; // 同一个 Library 跨流共享登录索引.
        astra::Receiver receiver(library, access);
        static_cast<void>(receiver.next());
        CHECK(receiver.receive(plan(2)));
        CHECK(receiver.receive(snapshot(2, false, "key")));
        auto final = snapshot(scenario == 1 ? 3 : 2, true, scenario == 0 ? "key" : "other");
        if (scenario == 2) {
            final.mutable_snapshot()->clear_version();
        }
        if (scenario == 3) {
            final.mutable_snapshot()->mutable_entries(0)->mutable_erase();
        }
        CHECK(!receiver.receive(final));
        CHECK(!library.find({"a", "s"}));
    }

    astra::Library library;
    astra::Access access; // 同一个 Library 跨流共享登录索引.
    astra::Receiver receiver(library, access);
    static_cast<void>(receiver.next());
    CHECK(receiver.receive(plan(0)));
    CHECK(receiver.receive(snapshot(0, true, "")));
    const auto ack = receiver.next(); // 显式零位置也必须得到确认, 不能用零代替无基线.
    CHECK(ack && ack->has_acknowledged() && ack->acknowledged().version() == 0);
    CHECK(library.find({"a", "s"})->usage().version == 0);
}
} // namespace

// 独立协议状态测试, 不通过实际联网或睡眠来碰概率边界.
int main() {
    snapshots();
    invalid();
    {
        // 最小协商预算下遍历完整位置清单, 每页有进度, 无缺项和超限编码.
        astra::Library library;
        astra::Access access; // 同一个 Library 跨流共享登录索引.
        for (unsigned index = 0; index < 20; ++index) {
            auto draft = library.prepare({std::string(128, 's'), std::string(120, 'p') + std::to_string(index)}, index);
            CHECK(draft && library.reset(std::move(*draft)));
        }
        astra::Receiver receiver(library, access);
        std::size_t count{};
        std::size_t pages{};
        bool complete{};
        while (const auto packet = receiver.next(1024)) {
            CHECK(!complete && packet->ByteSizeLong() <= 1024 && !packet->inventory().positions().empty());
            count += static_cast<std::size_t>(packet->inventory().positions_size());
            ++pages;
            complete = packet->inventory().complete();
        }
        CHECK(complete && count == 20 && pages > 1);
    }
    std::cout << "Almanac receiver checks passed\n";
}
