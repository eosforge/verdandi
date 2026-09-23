#include "check.hpp"
#include "dispatch.hpp"
#include "landing.hpp"
#include <iostream>

namespace {
using namespace std::chrono_literals;
using astra::Catalog;
using astra::Clock;
using astra::Ephemeris;
using astra::Parcel;
using astra::Scope;

// 二进制测试值不包含协议类型, 空值同样有独立不可变所有者.
Catalog::Value bytes(std::string_view value) {
    return std::make_shared<const Catalog::Buffer>(value.begin(), value.end());
}

// 空前缀、连续装包及累计 ACK 不把 prepare 当作实际网络交接, 不等待每项往返.
void catalog() {

    auto now = 1000ms; // 可控业务时间, 不启动对时或网络线程.
    Catalog::State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    astra::Dispatch<Catalog> stream(state);
    CHECK(!stream.prepare() && !stream.acknowledge(0));
    CHECK(stream.resume(0) && !stream.resume(0));
    auto packet = stream.prepare();
    CHECK(packet && *packet && (*packet)->has_catalog_changes());
    CHECK((*packet)->catalog_changes().head() == 0 && (*packet)->catalog_changes().entries().empty());
    stream.dispatched(); // 空组也明确响应一次, 之后没有业务变化时不制造重复心跳.
    CHECK(stream.prepare() && *stream.prepare() == nullptr);
    CHECK(state.publish({"routes", "one"}, "key", bytes(std::string("a\0b", 3)), 90, 1000));
    CHECK(state.publish({"routes", "two"}, "key", bytes(""), 5, 1000));
    CHECK(state.renew({"routes", "one"}, "key", 90, 2000));
    packet = stream.prepare();
    CHECK(packet && *packet && (*packet)->catalog_changes().entries_size() == 3);
    CHECK(Parcel::valid((*packet)->catalog_changes()));
    CHECK(!stream.acknowledge(3) && stream.sent() == 0);
    CHECK(stream.prepare().value() == *packet); // 重复准备借用同一个冻结包, 不追加或推进位置.
    const auto sent = stream.dispatched();
    const auto& entries = sent.catalog_changes().entries();
    CHECK(entries[0].position() == 1 && entries[0].record().version() == 90);
    CHECK(entries[0].record().value().value() == std::string("a\0b", 3));
    CHECK(entries[1].has_record() && entries[1].record().value().value().empty());
    CHECK(entries[2].has_lease() && !entries[2].has_record() && entries[2].lease().version() == 90);
    CHECK(stream.sent() == 3 && stream.acknowledged() == 0);
    CHECK(stream.acknowledge(2) && stream.acknowledge(3) && stream.acknowledge(3));
    CHECK(!stream.acknowledge(2) && !stream.acknowledge(4));

    // 最后一个写入即使没有后续通知, 再次准备仍从实际来源头部发现它.
    CHECK(state.publish({"routes", "one"}, "key", bytes("next"), 100, 1000));
    packet = stream.prepare();
    CHECK(packet && *packet && (*packet)->catalog_changes().entries_size() == 1);
    CHECK((*packet)->catalog_changes().entries(0).position() == 4);
    astra::Dispatch<Catalog> future(state);
    CHECK(!future.resume(5) && future.resume(4)); // 拒绝超前恢复后仍能正确开始, 不静默重置为零.
}

// 被裁剪历史改用完整组, 共享基线跨 Scope 固定; 同时到来的新事实必须在基线之后发送.
void baseline() {

    auto now = 1000ms;
    Catalog::State::Limits limits;
    limits.source.history = 0;
    Catalog::State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, limits);
    const auto payload = bytes(std::string(400, 'x'));
    CHECK(state.publish({"routes", "one"}, "first", payload, 1, 1000));
    CHECK(state.publish({"routes", "two"}, "second", payload, 7, 1000));
    astra::Dispatch<Catalog> stream(state, 2048, 256); // 单条大于软目标仍可发送, 每页只装一项.
    CHECK(stream.resume(0));
    const auto first = stream.prepare();
    CHECK(first && *first && (*first)->has_catalog_snapshot());
    CHECK((*first)->catalog_snapshot().position() == 2 && (*first)->catalog_snapshot().entries_size() == 1 && !(*first)->catalog_snapshot().complete());
    stream.dispatched();
    CHECK(stream.sent() == 0 && !stream.acknowledge(2));
    CHECK(state.publish({"routes", "three"}, "third", bytes("new"), 8, 1000));
    const auto second = stream.prepare();
    CHECK(second && *second && (*second)->catalog_snapshot().position() == 2 && (*second)->catalog_snapshot().complete());
    CHECK((*second)->catalog_snapshot().entries_size() == 1 && (*second)->catalog_snapshot().entries(0).key() != "third");
    stream.dispatched();
    CHECK(stream.sent() == 2 && stream.acknowledge(2));
    CHECK(stream.prepare().value()->catalog_snapshot().position() == 3); // 新写的历史也禁用, 下一轮重新捕获而非伪装成基线 2.

    // Catalog 全部到期后仍通过快照发送正版本水位, 不能编码成删除或零字节 value.
    now = 5s;
    state.tick();
    astra::Dispatch<Catalog> expired(state);
    CHECK(expired.resume(0));
    const auto empty = expired.prepare();
    CHECK(empty && *empty && (*empty)->catalog_snapshot().complete());
    CHECK((*empty)->catalog_snapshot().entries_size() == 3);
    for (const auto& entry : (*empty)->catalog_snapshot().entries()) {
        CHECK(entry.record().has_watermark() && !entry.record().has_value());
        CHECK(Parcel::record(entry.record())->version > 0);
    }
}

// Ephemeris Data/Renew 使用独立轻量分支, 大 Attr 不会随每次变化广播.
void ephemeris() {

    auto now = 1000ms;
    Ephemeris::State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    const Scope scope{"services", "main"};
    const auto created = state.create(scope, bytes(std::string(65536, 'a')), bytes("old"), 1000);
    CHECK(created);
    CHECK(state.update(scope, created->uuid, bytes("new"), 10));
    CHECK(state.renew(scope, created->uuid, 20));
    CHECK(state.remove(scope, created->uuid));
    astra::Dispatch<Ephemeris> stream(state);
    CHECK(stream.resume(0));
    const auto prepared = stream.prepare();
    CHECK(prepared && *prepared && Parcel::valid((*prepared)->ephemeris_changes()));
    const auto packet = stream.dispatched();
    const auto& entries = packet.ephemeris_changes().entries();
    CHECK(entries.size() == 4 && entries[0].has_record() && entries[0].record().attr().size() == 65536);
    CHECK(entries[1].has_data() && entries[1].data().order() == 10 && entries[1].ByteSizeLong() < 128);
    CHECK(entries[2].has_lease() && entries[2].lease().order() == 20 && entries[2].ByteSizeLong() < 128);
    CHECK(entries[3].has_erase() && stream.sent() == 4);

    // 原生解码不把远端过去的截止重新加 TTL, 网络恢复的到期判定留给实际安装边界.
    const auto decoded = Parcel::record(entries[0].record());
    CHECK(decoded && decoded->deadline == Clock::Time(2s) && decoded->ttl == 1000);
    CHECK(decoded->attr->size() == 65536 && decoded->update == 0 && decoded->renewal == 0);
}

// 有限消息/记录/时钟边界必须拒绝, 不发生 uint64 到有符号时间的回绕或不完整 oneof 回退.
void boundaries() {

    proto::astra::v1::CatalogRecord catalog;
    catalog.set_version(1);
    CHECK(!Parcel::record(catalog));
    catalog.mutable_watermark();
    CHECK(Parcel::record(catalog) && !Parcel::record(catalog)->value);
    catalog.mutable_value()->set_deadline(UINT64_MAX);
    CHECK(!Parcel::record(catalog));
    catalog.mutable_value()->set_deadline(1);
    CHECK(Parcel::record(catalog)->value && Parcel::record(catalog)->value->empty());
    catalog.mutable_value()->set_value(std::string(1024 * 1024 + 1, 'x'));
    CHECK(!Parcel::record(catalog));
    CHECK(!Parcel::time(0) && !Parcel::time(UINT64_MAX) && Parcel::time(INT64_MAX));
    proto::astra::v1::CatalogChanges changes;
    changes.set_head(3);
    for (const auto position : {1U, 3U}) {
        auto* entry = changes.add_entries();
        entry->set_position(position);
        Parcel::scope(*entry->mutable_scope(), {"routes", "main"});
        entry->set_key("key");
        entry->mutable_record()->set_version(position);
        entry->mutable_record()->mutable_watermark();
    }
    CHECK(!Parcel::valid(changes));
    changes.mutable_entries(1)->set_position(2);
    CHECK(Parcel::valid(changes));
    changes.mutable_entries(1)->set_key(std::string("k\0x", 3));
    CHECK(!Parcel::valid(changes));

    auto now = 1000ms;
    Catalog::State state([&] { return std::optional(Clock::Reading{.time = Clock::Time(now), .ready = true}); }, {});
    CHECK(state.publish({"routes", "main"}, "key", bytes(std::string(4096, 'x')), 1, 1000));
    astra::Dispatch<Catalog> small(state, 256, 256);
    CHECK(!small.resume(0)); // 原生引用预算也受限, 不能无限准备后再依靠网络流控.
    CHECK(small.sent() == 0 && small.acknowledged() == 0);
}

// 私有全量候选跨 Scope 收齐后才可交付; 坏页/重复目标/最终位置变化不覆盖已安装来源.
void landing() {

    using Source = astra::Origin<Catalog::Record>;
    const auto gate = std::make_shared<std::shared_mutex>();
    Source source(gate, [](const Catalog::Record& value) noexcept { return value.value ? value.value->size() : 0; }, false);
    astra::Landing<Catalog> receiver(source);
    proto::astra::v1::CatalogSnapshot page;
    page.set_position(5);
    auto* entry = page.add_entries();
    Parcel::scope(*entry->mutable_scope(), {"routes", "one"});
    entry->set_key("key");
    Parcel::record(*entry->mutable_record(), Catalog::Record{20, bytes("body"), Clock::Time(2s)});
    CHECK(receiver.append(page, 0) && !receiver.complete());
    {
        const std::lock_guard lock(*gate);
        CHECK(source.position() == 0 && source.capture().size() == 0);
    }
    CHECK(!receiver.append(page, 0)); // 跨页重复 Key 不能最后一项覆盖, 整份候选丢弃.
    CHECK(!receiver.complete() && receiver.position() == 0);
    CHECK(receiver.append(page, 0));
    page.clear_entries();
    page.set_complete(true);
    CHECK(receiver.append(page, 0) && receiver.complete());
    CHECK(!receiver.take(6)); // 收集之后的更高覆盖位置也必须在最终交接前复查.
    CHECK(source.position() == 0);
    CHECK(receiver.append(page, 0)); // 空来源基线是明确替换, 仍有位置 5.
    auto candidate = receiver.take(0);
    CHECK(candidate);
    std::optional<Source::Replaced> retired;
    {
        const std::lock_guard lock(*gate);
        auto installed = source.reset(std::move(*candidate));
        CHECK(installed);
        retired.emplace(std::move(*installed));
        CHECK(source.position() == 5 && source.capture().size() == 0);
    }

    // 旧基线/位置零非空/总预算超限都不能推进源端位置, 原有完整空基线仍可查询.
    page.set_position(4);
    CHECK(!receiver.append(page, 5));
    page.set_position(0);
    entry = page.add_entries();
    Parcel::scope(*entry->mutable_scope(), {"routes", "one"});
    entry->set_key("key");
    Parcel::record(*entry->mutable_record(), Catalog::Record{20, {}, {}});
    CHECK(!receiver.append(page, 0));
    page.set_position(6);
    astra::Landing<Catalog> tiny(source, 1);
    CHECK(!tiny.append(page, 5) && !tiny.complete());
    CHECK(source.position() == 5);
}
} // namespace

// 确定性编码/调度组件用例, 不启动端口或外部服务, 仍须获得测试授权才执行.
int main() {
    try {
        catalog();
        baseline();
        ephemeris();
        boundaries();
        landing();
        std::cout << "source delivery: ok\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
