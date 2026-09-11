#include <sqlite3.h>

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
int rows_before_failure = -1;
int fault_step(sqlite3_stmt* statement) {
    const std::string_view sql(sqlite3_sql(statement));
    if (sql.starts_with("SELECT member,")) {
        if (rows_before_failure == 0) {
            return SQLITE_IOERR;
        }
        if (rows_before_failure > 0) {
            --rows_before_failure;
        }
    }
    return sqlite3_step(statement);
}
} // namespace

// 只在数据库遍历边界注入驱动失败；其余 SQL、序列化、事务与校验全部执行真实实现。
#define sqlite3_step fault_step
#include "../src/catalog_checkpoint.cpp"
#undef sqlite3_step

int main() {
    using namespace verdandi::catalog;
    using namespace verdandi::catalog::detail;
    int failures{};
    const std::string scope(32, 's');
    for (const int fail_after : {-1, 0, 1}) {
        rows_before_failure = -1;
        auto store = checkpoint_store::open(":memory:", std::chrono::milliseconds(100));
        const auto first = path::create("p", "a");
        const auto second = path::create("p", "b");
        auto state = std::make_shared<const entry_state>(entry_state{1, 1, status::present, kind::map, 0, {}});
        if (!store || !first || !second) {
            return 1;
        }
        const std::array entries{checkpoint_entry{*first, state}, checkpoint_entry{*second, state}};
        if (!(*store)->save("Zone", scope, entries, 1, 1024)) {
            return 1;
        }
        rows_before_failure = fail_after;
        const auto loaded = (*store)->load("Zone", scope, 1024);
        const bool valid = fail_after < 0 ? loaded && loaded->entries.size() == 2 && loaded->cursor == 1 : !loaded && (*store)->disabled();
        if (!valid) {
            ++failures;
            std::cerr << "checkpoint returned partial success after " << fail_after << " rows\n";
        }
    }
    return failures == 0 ? 0 : 1;
}
