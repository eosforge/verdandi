#pragma once

#include "internal/catalog.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <span>
#include <string_view>

namespace verdandi::catalog::detail {

/// 从一个一致 SQLite 读事务恢复的作用域游标和完整 Entry 集合。
struct checkpoint_snapshot {
    std::uint64_t cursor{};
    std::map<path, std::shared_ptr<const entry_state>> entries;
};

/// 一次检查点事务要保存的稳定 Path/状态对。
struct checkpoint_entry {
    path target;
    std::shared_ptr<const entry_state> state;
};

/// Catalog 可丢弃 SQLite 检查点；Redis 始终是权威数据源。
class checkpoint_store final {
public:
    checkpoint_store(const checkpoint_store&) = delete;
    checkpoint_store& operator=(const checkpoint_store&) = delete;
    ~checkpoint_store();

    /// 创建父目录、打开数据库、设置有界锁等待并原子建立 v1 表结构。
    [[nodiscard]] static result<std::shared_ptr<checkpoint_store>> open(const std::filesystem::path& file, std::chrono::milliseconds timeout);

    /// 在一个一致读事务中恢复 `zone`/`scope`；每条记录都会按当前容量策略重新校验。
    [[nodiscard]] result<checkpoint_snapshot> load(std::string_view zone, std::string_view scope, std::size_t maximum_bytes);

    /// 在一个写事务中先单调保存 Entry，再单调推进作用域 cursor；空 Entry 集合仍可推进 cursor。
    [[nodiscard]] result<void> save(std::string_view zone, std::string_view scope, std::span<const checkpoint_entry> entries, std::uint64_t cursor,
                                    std::size_t maximum_bytes);

    /// 报告本 Client 世代是否已因一次检查点错误永久停用。
    [[nodiscard]] bool disabled() const noexcept;

    /// 永久停用当前检查点；后续读写成为空成功，Redis 同步继续运行。
    void disable() noexcept;

private:
    struct implementation;

    explicit checkpoint_store(std::unique_ptr<implementation> implementation) noexcept;

    std::unique_ptr<implementation> implementation_;
};

} // namespace verdandi::catalog::detail
