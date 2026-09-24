#pragma once
#include "parcel.hpp"
#include <concepts>

namespace astra {
// 单来源/域的私有全量接收候选. 只收集完整基线, 不发布来源根/可见投影或 ACK.
// 外层在 take 后准备合并视图/期限调度并最终复查身份/位置, 再调用 Origin::reset 一起提交.
template <typename Domain>
    requires(std::same_as<Domain, Catalog> || std::same_as<Domain, Ephemeris>)
class Landing {
public:
    using Source = Origin<typename Domain::Record, std::same_as<Domain, Ephemeris>>; // 接收者持有的远端来源, 不允许借来本机发送来源.
    using Page = std::conditional_t<std::same_as<Domain, Catalog>, proto::astra::v1::CatalogSnapshot, proto::astra::v1::EphemerisSnapshot>;
    using Error = typename Source::Error; // 结构/重复/容量/位置区分, 不用错误文本决定恢复策略.

    // 来源及其静态容量在本任务期间存活; bytes 包含全部已收分片编码, 原生索引另由 Draft 限额约束.
    explicit Landing(Source& source, std::size_t bytes = 64 * 1024 * 1024) : source_(&source), maximum_(bytes) {
        if (bytes == 0) {
            throw std::invalid_argument("Snapshot receive budget must be positive");
        }
    }

    // State 已验证身份/当前位置后交入私有 Draft, 不向网络层暴露真实来源对象.
    explicit Landing(typename Source::Draft&& draft, std::size_t bytes = 64 * 1024 * 1024) : maximum_(bytes), draft_(std::move(draft)), position_(draft_->position()) {
        if (bytes == 0) {
            throw std::invalid_argument("Snapshot receive budget must be positive");
        }
    }

    // minimum 是当前连续位置及仍有效精确回补覆盖位置的最大值, 每页及最终安装都须重新检查.
    // 任何错误/异常均丢弃私有半份基线, 已安装来源及公共 View 不受影响.
    std::expected<void, Error> append(const Page& page, std::uint64_t minimum) {

        try {
            const auto bytes = page.ByteSizeLong(); // 单页预算与整个候选预算分别控制, 不依赖 HTTP/2 窗口.
            if (complete_ || page.position() < minimum || (draft_ && page.position() != position_)) {
                return reject(Error::version);
            }
            if (page.entries_size() > 256 || bytes > 8 * 1024 * 1024 || bytes > maximum_ - received_) {
                return reject(Error::capacity);
            }
            if ((!page.complete() && page.entries().empty()) || (page.position() == 0 && !page.entries().empty())) {
                return reject(Error::input);
            }
            if (!draft_) {
                if (!source_) {
                    return reject(Error::input); // 转入的 Draft 已放弃后必须由 State 重新核对并建立下一候选.
                }
                draft_.emplace(source_->prepare(page.position())); // 只读取固定预算, 不改变真实远端来源.
                position_ = page.position();
            }
            for (const auto& entry : page.entries()) {
                const auto scope = Parcel::scope(entry.scope());
                const auto& key = [&]() -> const std::string& { if constexpr (std::same_as<Domain, Catalog>) return entry.key(); else return entry.uuid(); }();
                const bool valid = [&] { if constexpr (std::same_as<Domain, Catalog>) return Scope::text(key, 1024); else return Ephemeris::valid(key); }();
                if (!scope || !valid || !entry.has_record()) {
                    return reject(Error::input);
                }
                auto record = Parcel::record(entry.record()); // 大正文仅复制一次成为真正不可变原生所有者.
                if (!record) {
                    return reject(Error::input);
                }
                const auto added = draft_->set(*scope, key, std::move(*record));
                if (!added) {
                    return reject(added.error());
                }
            }
            received_ += bytes;
            complete_ = page.complete();
            return {};
        } catch (...) {
            clear(); // bad_alloc 也不能保留成功插入前半页的候选供之后错误续接.
            throw;
        }
    }

    // complete 只表示基线正文收齐, 不表示来源已安装或可以 ACK.
    bool complete() const noexcept {
        return complete_;
    }

    // 当前候选声称的位置, 尚未开始时为零, 不能作为接收方连续位置对外报告.
    std::uint64_t position() const noexcept {
        return position_;
    }

    // 只在 complete 后转交候选, 外层最终提交失败由 Draft 析构回收, 不保留半公开状态.
    std::expected<typename Source::Draft, Error> take(std::uint64_t minimum) {

        if (!complete_ || !draft_ || position_ < minimum) {
            const auto error = position_ < minimum ? Error::version : Error::input;
            clear();
            return std::unexpected(error);
        }
        auto candidate = std::move(*draft_);
        clear();
        return candidate;
    }

    // 断流、替换或取消只释放本候选, 不重新编号或删除远端来源已安装数据.
    void clear() noexcept {
        draft_.reset();
        position_ = 0;
        received_ = 0;
        complete_ = false;
    }

private:
    // 清空后返回明确失败, 避免重复错误分支忘记销毁候选.
    std::expected<void, Error> reject(Error error) {
        clear();
        return std::unexpected(error);
    }

    Source* source_{};                            // 寿命由受控复制任务保持, 不借用 SessionPacket.
    const std::size_t maximum_;                   // 整组在建编码计费上限, 正数.
    std::optional<typename Source::Draft> draft_; // 未公开原生候选, 可以含多个 Scope/水位.
    std::uint64_t position_{};                    // 全部分片必须一致, 默认零.
    std::size_t received_{};                      // 已收完整页的编码计费, 失败即归零.
    bool complete_{};                             // complete 页成功处理之后才为真.
};
} // namespace astra
