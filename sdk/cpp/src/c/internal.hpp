#pragma once

#include "verdandi/c/verdandi.h"
#include "verdandi/catalog/catalog.hpp"
#include "verdandi/client.hpp"
#include "verdandi/configuration.hpp"
#include "verdandi/registration/selector.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// C ABI 拥有的连续二进制结果。
struct verdandi_blob {
    verdandi::bytes value;
};

/// C ABI 拥有的完整字段结果和一次建立的 O(1) 顺序索引。
struct verdandi_field_set {
    explicit verdandi_field_set(verdandi::fields source) : value(std::move(source)) {
        ordered.reserve(value.size());
        for (const auto& field : value) {
            ordered.push_back(&field);
        }
    }

    verdandi::fields value;
    std::vector<const verdandi::fields::value_type*> ordered;
};

/// 保存规范配置和根传输，使子域可以从同一 JSON 延迟构造。
struct verdandi_client {
    verdandi::configuration configuration;
    verdandi::client value;
};

/// C ABI Registration 子域句柄。
struct verdandi_registration_client {
    verdandi::registration::client value;
};

/// C ABI raw-Fields Registration 句柄。
struct verdandi_registration {
    verdandi::registration::registration<verdandi::fields, verdandi::fields> value;
};

/// C ABI raw-Fields Selector 句柄。
struct verdandi_selector {
    std::unique_ptr<verdandi::registration::selector<verdandi::fields, verdandi::fields>> value;
};

/// 策略调用期间借用的原生 Candidates。
struct verdandi_candidates {
    verdandi::registration::candidates<verdandi::fields, verdandi::fields>* value{};
};

/// 策略调用期间构造 One 或 Any 结果，避免预分配候选总量大小的数组。
struct verdandi_selection {
    verdandi_candidates* candidates{};
    bool one{};
    std::optional<verdandi::registration::choice> selected;
    std::vector<verdandi::registration::choice> many;
};

/// Selector 事务脱离后由 C ABI 独占的候选列表。
struct verdandi_candidate_list {
    std::vector<verdandi::registration::candidate<verdandi::fields, verdandi::fields>> values;
};

/// Selector 完整脱离视图。
struct verdandi_selector_snapshot {
    verdandi::registration::selection_snapshot<verdandi::fields, verdandi::fields> value;
};

/// C ABI Catalog 子域句柄。
struct verdandi_catalog_client {
    verdandi::catalog::client value;
};

/// C ABI Catalog Publisher 句柄。
struct verdandi_catalog_publisher {
    verdandi::catalog::publisher value;
};

/// C ABI Catalog Subscriber 句柄。
struct verdandi_catalog_subscriber {
    std::unique_ptr<verdandi::catalog::subscriber> value;
};

/// C ABI 稳定 Catalog Entry 句柄。
struct verdandi_catalog_entry {
    std::shared_ptr<verdandi::catalog::entry> value;
};

namespace verdandi::c_api {

/// 清空可选错误输出。
void clear_error(verdandi_error* output) noexcept;

/// 把原生错误有界复制到 C ABI 拥有型结果。
void write_error(verdandi_error* output, const error& value) noexcept;

/// 把策略回调填写的稳定字符串错误转回原生错误。
[[nodiscard]] error read_callback_error(const verdandi_error& value);

/// 校验一段借用文本的指针/长度关系。
[[nodiscard]] result<std::string_view> read_text(verdandi_string_view value, std::string_view field);

/// 校验一段借用二进制的指针/长度关系。
[[nodiscard]] result<std::span<const std::byte>> read_bytes(verdandi_bytes_view value, std::string_view field);

/// 深拷贝并拒绝重复字段名。
[[nodiscard]] result<fields> read_fields(verdandi_fields_view value, std::string_view field);

/// 把无符号毫秒安全转换为原生精确持续时间。
[[nodiscard]] result<std::chrono::milliseconds> read_duration(std::uint64_t value, std::string_view field);

/// 用一个回调遍历 Fields，不允许异常或零返回逃出边界。
[[nodiscard]] result<void> visit_fields(const fields& value, verdandi_field_visitor visitor, void* context);

/// 把原生元数据映射为只借用 UUID 的 C ABI 结构。
void write_metadata(const registration::metadata& value, verdandi_registration_metadata* output) noexcept;

/// 返回稳定 Catalog 状态名称。
[[nodiscard]] const char* catalog_status(catalog::status value) noexcept;

/// 统一把所有异常和原生 expected 失败转换到 C ABI 的零/非零约定。
template <class Callback>
[[nodiscard]] int boundary(verdandi_error* output, Callback&& callback) noexcept {
    clear_error(output);
    try {
        auto status = std::invoke(std::forward<Callback>(callback));
        if (!status) {
            write_error(output, status.error());
            return 0;
        }
        return 1;
    } catch (const std::bad_alloc& exception) {
        write_error(output, error(code::capacity, "c_abi").with_detail(exception.what()));
    } catch (const std::exception& exception) {
        write_error(output, error(code::unavailable, "c_abi").with_detail(exception.what()));
    } catch (...) {
        write_error(output, error(code::corrupt, "c_abi"));
    }
    return 0;
}

} // namespace verdandi::c_api
