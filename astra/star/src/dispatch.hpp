#pragma once
#include "catalog_state.hpp"
#include "ephemeris_state.hpp"
#include "parcel.hpp"
#include <bit>
#include <concepts>

namespace astra {
// 一条已验证对等流的一个发送域. 只持有当前位置、至多一个编码包和一个完整基线根, 不复制业务 FIFO.
// 所有方法由所属流的控制循环串行调用; Domain 只允许两个动态域, 不把 Almanac 纳入相同数据规则.
template <typename Domain>
    requires(std::same_as<Domain, Catalog> || std::same_as<Domain, Ephemeris>)
class Dispatch {
public:
    using State = typename Domain::State;           // 借用真实提交器, 寿命必须覆盖本发送器.
    using Source = typename State::Source;          // 只读自有来源, 绝不转播远端来源.
    using Packet = proto::astra::v1::SessionPacket; // 最终直接交给现有双向流, 不加第二层字节帧.
    using Error = typename State::Error;            // 保留历史/时钟/容量错误供流控制器决定终止原因.

    using Reserve = bool (*)(void*, std::size_t) noexcept; // 仅在冻结根保留期间占用全局工作区.
    using Release = void (*)(void*, std::size_t) noexcept; // 对称归还, 不执行用户代码或网络.

    // capacity 为本流协商的单消息硬上限, target 是装包目标; 至少保留 64 字节封装空间.
    explicit Dispatch(State& state, std::size_t capacity = 8 * 1024 * 1024, std::size_t target = 256 * 1024, void* context = nullptr, Reserve reserve = nullptr, Release release = nullptr) : state_(state), capacity_(capacity), target_(target), context_(context), reserve_(reserve), release_(release) {
        if (capacity < 64 || capacity > 8 * 1024 * 1024 || target == 0 || target > capacity || static_cast<bool>(reserve) != static_cast<bool>(release)) {
            throw std::invalid_argument("Invalid peer delivery limits");
        }
    }

    // 断流释放冻结根, 不留下为已结束发送保留的全局预算.
    ~Dispatch() {
        baseline_.reset();
        if (release_) {
            release_(context_, retained_);
        }
    }

    // 冻结根的额度只归一个发送器, 不复制或移动一个活动发送责任.
    Dispatch(const Dispatch&) = delete;
    Dispatch& operator=(const Dispatch&) = delete;

    // 首次收到对端连续前缀后启用, 同一流不允许第二次重置发送位置或回退 ACK.
    std::expected<void, Error> resume(std::uint64_t position) {

        if (opened_) {
            return std::unexpected(Error::input);
        }
        const auto delivery = state_.deliver(position, 1, capacity_); // 同边界核对位置不超前, 不把它当作新写入.
        if (!delivery) {
            return std::unexpected(delivery.error());
        }
        sent_ = acknowledged_ = position;
        opened_ = true;
        return {};
    }

    // 返回当前准备包的借用, nullptr 为暂时没有尾部. 只有 dispatched 才推进发送位置.
    // 每次从来源真实头部重新判断, 唤醒合并不能丢失最后一次提交; 不等 ACK 才发送下一包.
    std::expected<const Packet*, Error> prepare() {

        if (!opened_) {
            return std::unexpected(Error::input);
        }
        if (packet_) {
            return &*packet_;
        }
        if (!baseline_) {
            auto delivery = state_.deliver(sent_, 256, capacity_);
            if (!delivery) {
                return std::unexpected(delivery.error());
            }
            if (delivery->baseline) {
                const auto retained = delivery->baseline->bytes(); // 保守按每条流记完整逻辑引用, 不假设不同捕获根永远共享.
                if (reserve_ && !reserve_(context_, retained)) {
                    return std::unexpected(Error::capacity);
                }
                retained_ = retained;
                baseline_.emplace(std::move(*delivery->baseline));
                offset_ = 0;
            } else {
                if (!initial_ && delivery->events.empty()) {
                    return nullptr;
                }
                auto result = changes(*delivery);
                if (!result) {
                    return std::unexpected(result.error());
                }
                return &*packet_;
            }
        }
        const auto result = snapshot(); // 一个来源快照未结束时不插入本域增量, 另一域仍由外层公平调度.
        return result ? std::expected<const Packet*, Error>(&*packet_) : std::unexpected(result.error());
    }

    // 仅在调用方马上把本包交给唯一 StartWrite 时移出. 被排队或准备好的包不能提前调用.
    // 返回对象由会话保持到 OnWriteDone, 取消后也不能提早释放实际在途字节.
    Packet dispatched() {

        if (!packet_) {
            throw std::logic_error("No prepared peer packet");
        }
        auto packet = std::move(*packet_); // Protobuf 同 Arena 的移动交接, 不另建每目标事件日志.
        packet_.reset();
        initial_ = false;
        if (baseline_) {
            offset_ = offset_next_;
            if (offset_ == baseline_->size()) {
                sent_ = baseline_->position();
                baseline_.reset();
                if (release_) {
                    release_(context_, retained_);
                }
                retained_ = 0;
            }
        } else {
            sent_ = position_next_;
        }
        return packet;
    }

    // 累计确认不能回退/超过已交给 gRPC 的完整边界, 重复确认无副作用且不触发 ACK 的 ACK.
    bool acknowledge(std::uint64_t position) noexcept {
        if (!opened_ || position < acknowledged_ || position > sent_) {
            return false;
        }
        acknowledged_ = position;
        return true;
    }

    // 发送位置与对端完整安装位置分别暴露给受控监控, 不互相冒充业务复制完成.
    std::uint64_t sent() const noexcept {
        return sent_;
    }

    std::uint64_t acknowledged() const noexcept {
        return acknowledged_;
    }

private:
    // 单个 repeated 子消息的准确长度开销; 元素标签均为单字节, size 有 8 MiB 上限.
    static std::size_t encoded(std::size_t size) noexcept {
        return 1 + static_cast<std::size_t>(std::max(1, (std::bit_width(size) + 6) / 7)) + size;
    }

    // 有界前缀按实际序列化大小装包, 超目标的合法单条独占一包, 不跳过或拆分原子提交.
    std::expected<void, Error> changes(const typename Source::Delivery& delivery) {

        Packet packet; // 未完成编码前不占用 packet_, 异常保留原 sent_ 和基线位置.
        auto* body = [&] { if constexpr (std::same_as<Domain, Catalog>) return packet.mutable_catalog_changes(); else return packet.mutable_ephemeris_changes(); }();
        body->set_head(delivery.head);
        std::size_t bytes = 32; // SessionPacket/头部保守预留, 每条正文用准确编码成本.
        auto position = sent_;
        for (const auto& event : delivery.events) {
            std::remove_cvref_t<decltype(*body->add_entries())> entry; // 只由 decltype 推导, 不执行 add_entries.
            Parcel::delta(entry, event);
            const auto cost = encoded(entry.ByteSizeLong());
            if (body->entries_size() != 0 && cost > target_ - std::min(bytes, target_)) {
                break;
            }
            if (cost > capacity_ - bytes) {
                return std::unexpected(Error::capacity);
            }
            body->add_entries()->Swap(&entry);
            bytes += cost;
            position = event.position;
        }
        if (packet.ByteSizeLong() > capacity_) {
            return std::unexpected(Error::capacity);
        }
        packet_.emplace(std::move(packet));
        position_next_ = position;
        return {};
    }

    // 通过带行数索引的稳定根分页, 每页不重扫已有前缀; 所有页都使用同一基线位置.
    std::expected<void, Error> snapshot() {

        Packet packet;
        auto* body = [&] { if constexpr (std::same_as<Domain, Catalog>) return packet.mutable_catalog_snapshot(); else return packet.mutable_ephemeris_snapshot(); }();
        body->set_position(baseline_->position());
        std::size_t bytes = 32; // 预留基线, complete 及外层消息的编码空间.
        bool exceeded{};        // 区分正常达到软目标与无法装入单条完整记录的硬失败.
        const auto count = baseline_->page(offset_, 256, [&](const Scope& scope, const std::string& key, const typename Domain::Record& value) {
            std::remove_cvref_t<decltype(*body->add_entries())> entry;
            Parcel::scope(*entry.mutable_scope(), scope);
            if constexpr (std::same_as<Domain, Catalog>) {
                entry.set_key(key);
            } else {
                entry.set_uuid(key);
            }
            Parcel::record(*entry.mutable_record(), value);
            const auto cost = encoded(entry.ByteSizeLong());
            if (body->entries_size() != 0 && cost > target_ - std::min(bytes, target_)) {
                return false;
            }
            if (cost > capacity_ - bytes) {
                exceeded = true;
                return false;
            }
            body->add_entries()->Swap(&entry);
            bytes += cost;
            return true;
        });
        if (exceeded) {
            return std::unexpected(Error::capacity);
        }
        body->set_complete(offset_ + count == baseline_->size());
        if (packet.ByteSizeLong() > capacity_) {
            return std::unexpected(Error::capacity);
        }
        packet_.emplace(std::move(packet));
        offset_next_ = offset_ + count;
        return {};
    }

    State& state_;                                  // 固定借用自有域提交器, 无额外工作线程.
    const std::size_t capacity_;                    // 本流单消息硬预算, 64..8 MiB.
    const std::size_t target_;                      // 装包软目标, 不阻止较大单条合法正文.
    std::optional<typename Source::View> baseline_; // 至多一个来源根, 完成整份发送后释放.
    std::optional<Packet> packet_;                  // 至多一个已编码尚未交接包, 异常不改变连续位置.
    std::uint64_t sent_{};                          // 已交给 gRPC 的完整位置, 部分快照页不增加它.
    std::uint64_t acknowledged_{};                  // 对端累计完整安装位置, 不超过 sent_.
    std::uint64_t position_next_{};                 // packet_ 完整交接后才允许发布的位置.
    std::size_t offset_{};                          // 当前基线已交接行数, 初始零.
    std::size_t offset_next_{};                     // 已准备页的后继偏移, 准备异常/取消不影响 offset_.
    void* context_{};                               // 可选的 Runtime 全局预算, 比发送器活得更久.
    Reserve reserve_{};                             // 两个回调同时存在或同时为空.
    Release release_{};                             // 归还冻结根额度, noexcept.
    std::size_t retained_{};                        // 本发送器已保留根的逻辑字节.
    bool opened_{};                                 // 是否已经接受本流唯一的恢复起点.
    bool initial_ = true;                           // 空组/已追平的首次恢复仍必须返回一次确认包.
};
} // namespace astra
