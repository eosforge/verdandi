#pragma once
#include "catalog_state.hpp"
#include "pagination.hpp"

namespace astra {
// Catalog 公开字段策略, 不持有锁, 分页位置或网络状态; 仅供本域 Edition 使用.
class Catalog::Encoding {
public:
    using Projection = State::Projection;              // 本域公开根及可见事件.
    using Content = State::Content;                    // 不携带租约和来源序号的完整内容.
    using Reply = proto::comet::v1::CatalogWatchReply; // 独立线消息, 不混入其他域.

    // 精确目标的格式检查, 无分配; 全范围空串由外层处理.
    static bool target(std::string_view value) noexcept {
        return Scope::text(value, 1024);
    }

    // 精确点查的完整内容计费, record 已由 State 确保完整.
    static std::size_t measure(const Content& record) noexcept {
        return record.value->size();
    }

    // 合并未发送事件, 保留本域的完整内容/Attr 基线规则.
    static Projection::Event merge(const Projection::Event& previous, Projection::Event current) noexcept;

    // 校验公开行并返回本次线载荷长度, data 只对 Ephemeris 的增量分支有意义.
    static std::size_t payload(std::string_view key, const Content* record, bool data) {
        static_cast<void>(data); // Catalog 增量始终携带完整内容, 不存在 Data-only 编码.
        if (!Scope::text(key, 1024) || (record && (!record->value || record->version == 0))) {
            throw std::invalid_argument("Invalid Catalog projection record");
        }
        return record ? record->value->size() : 0; // 期限不进入公开视图.
    }

    // 预算已由 Pagination 核对, 这里只追加该域字段; 分配失败由整页回滚位置.
    static void append(Reply& page, std::string_view key, const Content* record, bool data);
};

// 保留领域专属类型, 分页, 压缩, 冻结所有权及完成游标由同一实现维护.
class Catalog::Edition : public Pagination<Encoding> {
public:
    // 沿用整范围, 精确点查和增量构造, 不增加包装对象或运行时策略选择.
    using Pagination<Encoding>::Pagination;
};
} // namespace astra
