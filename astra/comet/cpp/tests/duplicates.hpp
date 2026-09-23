#pragma once
#include "check.hpp"
#include "comet.pb.h"
#include <algorithm>
#include <array>
#include <comet/types.hpp>
#include <initializer_list>
#include <string_view>

namespace comet::test {
// 三域共同的跨页去重边界: 满额后重复项仍报协议错误, 新项报容量错误, 二者都不发布半批.
// erase 只填写对应域的键和 Delete 分支; Policy 使用各自真实生成消息和安装器.
template <class Policy>
void duplicates(auto&& erase) {

    constexpr std::array<std::string_view, 3> keys{"00000000-0000-4000-8000-000000000001", "00000000-0000-4000-8000-000000000002", "00000000-0000-4000-8000-000000000003"}; // 同时是合法普通 Key 和 UUID.
    std::array<std::size_t, 2> peaks{};                                                                                                                                     // 同一缺失 Delete, 分别按单页完整批次和跨页批次比较准备计费.
    for (const bool split : {false, true}) {
        std::size_t peak{}; // 只记录共享预算真正观察到的峰值, 不读取安装器私有字段.
        Policy projection({"test", "dedup"}, {}, 32768, 1, [&](std::size_t bytes) noexcept { peak = std::max(peak, bytes); return true; });
        projection.begin("star-test");
        typename Policy::Reply initial; // 空基线允许对未知 Key 提交幂等 Delete.
        initial.set_mode(proto::comet::v1::MODE_RESET);
        initial.set_complete(true);
        initial.set_instance("star-test");
        initial.set_version(1);
        CHECK(projection.accept(initial));
        peak = 0;

        typename Policy::Reply page; // split=true 时首项先到, 末页仅确认完整位置.
        page.set_mode(proto::comet::v1::MODE_APPLY);
        if (!split) {
            page.set_complete(true);
            page.set_instance("star-test");
            page.set_version(2);
        }
        erase(*page.add_changes(), keys[0]);
        const auto first = projection.accept(page);
        CHECK(first && static_cast<bool>(*first) == !split);
        if (split) {
            page.clear_changes();
            page.set_complete(true);
            page.set_instance("star-test");
            page.set_version(2);
            const auto completed = projection.accept(page);
            CHECK(completed && *completed);
        }
        peaks[static_cast<std::size_t>(split)] = peak;
        CHECK(projection.view(Policy::API::State::ready).version() == 2);

        // 完成后不再保留去重信息; 下一批可以再次出现同一 Key.
        page.clear_changes();
        page.set_version(3);
        erase(*page.add_changes(), keys[0]);
        const auto next = projection.accept(page);
        CHECK(next && *next && (**next).version() == 3);

        // 只有尾页为单项不代表整批唯一: 非满额时也必须拒绝跨页重复并保留旧根.
        page.clear_version();
        page.clear_instance();
        page.set_complete(false);
        CHECK(projection.accept(page));
        page.set_complete(true);
        page.set_instance("star-test");
        page.set_version(4);
        const auto repeated = projection.accept(page);
        CHECK(!repeated && repeated.error().code == comet::Error::Code::protocol);
        CHECK(projection.view(Policy::API::State::stale).version() == 3);
    }
    CHECK(peaks[0] < peaks[1]); // 单项批次不分配去重集合, 跨页仍为其拥有式名称/桶计费.

    for (const bool repeated : {true, false}) {
        Policy projection({"test", "dedup"}, {}, 32768, 1); // 最终最多一项, 批次允许两个不同键的原子替换.
        projection.begin("star-test");
        typename Policy::Reply initial; // 先建立版本 1 的完整空基线, 才允许后续增量.
        initial.set_mode(proto::comet::v1::MODE_RESET);
        initial.set_complete(true);
        initial.set_instance("star-test");
        initial.set_version(1);
        CHECK(projection.accept(initial));

        typename Policy::Reply head; // 两个缺失 Delete 不增加记录数, 但确实占满本批的去重容量.
        head.set_mode(proto::comet::v1::MODE_APPLY);
        erase(*head.add_changes(), keys[0]);
        erase(*head.add_changes(), keys[1]);
        const auto partial = projection.accept(head);
        CHECK(partial && !*partial);
        typename Policy::Reply tail; // 最终页或重复旧键, 或提交第三个键, 不依赖先发生逻辑记录超额.
        tail.set_mode(proto::comet::v1::MODE_APPLY);
        tail.set_complete(true);
        tail.set_instance("star-test");
        tail.set_version(2);
        erase(*tail.add_changes(), keys[repeated ? 0 : 2]);
        const auto rejected = projection.accept(tail);
        CHECK(!rejected && rejected.error().code == (repeated ? comet::Error::Code::protocol : comet::Error::Code::limit));
        CHECK(projection.view(Policy::API::State::stale).version() == 1 && projection.view(Policy::API::State::stale).size() == 0);

        // 失败已丢弃候选和去重状态, 下一条流可以从原完整位置接受合法空确认.
        projection.begin("star-test");
        initial.set_mode(proto::comet::v1::MODE_APPLY);
        CHECK(projection.accept(initial));
    }
}
} // namespace comet::test
