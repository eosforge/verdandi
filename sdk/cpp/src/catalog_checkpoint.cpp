#include "internal/catalog_checkpoint.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace verdandi::catalog::detail {

namespace {

constexpr int checkpoint_application_id = 0x56434154; // `VCAT`。
constexpr int checkpoint_schema_version = 1;
constexpr std::array<std::byte, 4> fields_magic{std::byte{'V'}, std::byte{'C'}, std::byte{'F'}, std::byte{'1'}};

struct statement_deleter {
    void operator()(sqlite3_stmt* value) const noexcept {
        if (value != nullptr) {
            static_cast<void>(sqlite3_finalize(value));
        }
    }
};

using statement = std::unique_ptr<sqlite3_stmt, statement_deleter>;

[[nodiscard]] error sqlite_error(sqlite3* database, const code category = code::unavailable) {
    const char* message = database == nullptr ? nullptr : sqlite3_errmsg(database);
    return error(category, "local_store_path").with_detail(message == nullptr ? "SQLite failure" : std::string(message));
}

[[nodiscard]] result<void> execute(sqlite3* database, const char* sql) {
    char* message{};
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    if (status == SQLITE_OK) {
        return {};
    }
    std::string detail = message == nullptr ? sqlite3_errmsg(database) : message;
    sqlite3_free(message);
    return std::unexpected(error(code::unavailable, "local_store_path").with_detail(std::move(detail)));
}

[[nodiscard]] result<statement> prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* raw{};
    if (sqlite3_prepare_v3(database, sql, -1, SQLITE_PREPARE_PERSISTENT, &raw, nullptr) != SQLITE_OK) {
        return std::unexpected(sqlite_error(database));
    }
    return statement(raw);
}

[[nodiscard]] result<std::int64_t> pragma_integer(sqlite3* database, const char* sql) {
    auto query = prepare(database, sql);
    if (!query) {
        return std::unexpected(query.error());
    }
    if (sqlite3_step(query->get()) != SQLITE_ROW || sqlite3_column_type(query->get(), 0) != SQLITE_INTEGER) {
        return std::unexpected(sqlite_error(database, code::corrupt));
    }
    return sqlite3_column_int64(query->get(), 0);
}

[[nodiscard]] result<void> bind_text(sqlite3* database, sqlite3_stmt* target, const int index, const std::string_view value) {
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        sqlite3_bind_text(target, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        return std::unexpected(sqlite_error(database));
    }
    return {};
}

[[nodiscard]] result<void> bind_blob(sqlite3* database, sqlite3_stmt* target, const int index, const std::span<const std::byte> value) {
    if (value.empty()) {
        if (sqlite3_bind_zeroblob(target, index, 0) != SQLITE_OK) {
            return std::unexpected(sqlite_error(database));
        }
        return {};
    }
    if (sqlite3_bind_blob64(target, index, value.data(), static_cast<sqlite3_uint64>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
        return std::unexpected(sqlite_error(database));
    }
    return {};
}

[[nodiscard]] result<void> bind_integer(sqlite3* database, sqlite3_stmt* target, const int index, const std::uint64_t value) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max()) ||
        sqlite3_bind_int64(target, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
        return std::unexpected(sqlite_error(database));
    }
    return {};
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    output.push_back(static_cast<std::byte>((value >> 24U) & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 16U) & 0xffU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
    output.push_back(static_cast<std::byte>(value & 0xffU));
}

[[nodiscard]] result<std::uint32_t> read_u32(const std::span<const std::byte> source, std::size_t& offset) {
    if (offset > source.size() || source.size() - offset < 4) {
        return std::unexpected(error(code::corrupt, "local_store_path"));
    }
    const auto first = std::to_integer<std::uint32_t>(source[offset]);
    const auto second = std::to_integer<std::uint32_t>(source[offset + 1]);
    const auto third = std::to_integer<std::uint32_t>(source[offset + 2]);
    const auto fourth = std::to_integer<std::uint32_t>(source[offset + 3]);
    offset += 4;
    return (first << 24U) | (second << 16U) | (third << 8U) | fourth;
}

[[nodiscard]] result<std::vector<std::byte>> encode_fields(const fields& value) {
    if (value.size() > maximum_fields || value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(error(code::capacity, "fields"));
    }
    std::size_t payload{};
    for (const auto& [name, field] : value) {
        if (name.size() > std::numeric_limits<std::uint32_t>::max() || field.size() > std::numeric_limits<std::uint32_t>::max() ||
            name.size() > std::numeric_limits<std::size_t>::max() - payload ||
            field.size() > std::numeric_limits<std::size_t>::max() - payload - name.size() - 8) {
            return std::unexpected(error(code::capacity, "fields"));
        }
        payload += name.size() + field.size() + 8;
    }
    if (payload > std::numeric_limits<std::size_t>::max() - 8) {
        return std::unexpected(error(code::capacity, "fields"));
    }
    std::vector<std::byte> output;
    output.reserve(payload + 8);
    output.insert(output.end(), fields_magic.begin(), fields_magic.end());
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    for (const auto& [name, field] : value) {
        append_u32(output, static_cast<std::uint32_t>(name.size()));
        append_u32(output, static_cast<std::uint32_t>(field.size()));
        const auto* name_begin = reinterpret_cast<const std::byte*>(name.data());
        output.insert(output.end(), name_begin, name_begin + name.size());
        output.insert(output.end(), field.begin(), field.end());
    }
    return output;
}

[[nodiscard]] result<fields> decode_fields(const std::span<const std::byte> source) {
    if (source.size() < 8 || !std::equal(fields_magic.begin(), fields_magic.end(), source.begin())) {
        return std::unexpected(error(code::corrupt, "local_store_path"));
    }
    std::size_t offset = fields_magic.size();
    auto count = read_u32(source, offset);
    if (!count || *count > maximum_fields) {
        return std::unexpected(error(code::corrupt, "local_store_path"));
    }
    fields output;
    for (std::uint32_t index = 0; index < *count; ++index) {
        auto name_size = read_u32(source, offset);
        auto value_size = read_u32(source, offset);
        if (!name_size || !value_size || offset > source.size() || *name_size > source.size() - offset) {
            return std::unexpected(error(code::corrupt, "local_store_path"));
        }
        const auto* name_data = reinterpret_cast<const char*>(source.data() + offset);
        std::string name(name_data, *name_size);
        offset += *name_size;
        if (*value_size > source.size() - offset) {
            return std::unexpected(error(code::corrupt, "local_store_path"));
        }
        bytes field(source.begin() + static_cast<std::ptrdiff_t>(offset), source.begin() + static_cast<std::ptrdiff_t>(offset + *value_size));
        offset += *value_size;
        if (!output.emplace(std::move(name), std::move(field)).second) {
            return std::unexpected(error(code::corrupt, "local_store_path"));
        }
    }
    if (offset != source.size()) {
        return std::unexpected(error(code::corrupt, "local_store_path"));
    }
    return output;
}

[[nodiscard]] int status_number(const status value) noexcept {
    switch (value) {
    case status::present:
        return 1;
    case status::absent:
        return 2;
    case status::deleted:
        return 3;
    case status::synchronizing:
    case status::unavailable:
    case status::closed:
        return 0;
    }
    return 0;
}

[[nodiscard]] std::optional<status> parse_status(const int value) noexcept {
    switch (value) {
    case 1:
        return status::present;
    case 2:
        return status::absent;
    case 3:
        return status::deleted;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] int kind_number(const kind value) noexcept {
    switch (value) {
    case kind::value:
        return 1;
    case kind::array:
        return 2;
    case kind::map:
        return 3;
    }
    return 0;
}

[[nodiscard]] std::optional<kind> parse_kind(const int value) noexcept {
    switch (value) {
    case 1:
        return kind::value;
    case 2:
        return kind::array;
    case 3:
        return kind::map;
    default:
        return std::nullopt;
    }
}

[[nodiscard]] result<void> validate_state(const entry_state& value, const std::size_t maximum_bytes) {
    if (value.revision > maximum_revision || value.replace_revision > value.revision || value.encoded_bytes > maximum_bytes) {
        return std::unexpected(error(code::corrupt, "local_store_path"));
    }
    if (value.state == status::present) {
        auto actual = validate_catalog_value(value.shape, value.value, maximum_bytes);
        if (value.revision == 0 || value.replace_revision == 0 || !actual || *actual != value.encoded_bytes) {
            return std::unexpected(error(code::corrupt, "local_store_path"));
        }
        return {};
    }
    if ((value.state == status::absent && value.revision != 0) || (value.state == status::deleted && value.revision == 0) ||
        (value.state != status::absent && value.state != status::deleted) || value.replace_revision != 0 || value.encoded_bytes != 0 || !value.value.empty()) {
        return std::unexpected(error(code::corrupt, "local_store_path"));
    }
    return {};
}

[[nodiscard]] result<path> parse_member(const std::string_view value) {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos || value.find(':', separator + 1) != std::string_view::npos) {
        return std::unexpected(error(code::corrupt, "local_store_path"));
    }
    auto target = path::create(std::string(value.substr(0, separator)), std::string(value.substr(separator + 1)));
    return target ? std::move(*target) : result<path>(std::unexpected(error(code::corrupt, "local_store_path")));
}

[[nodiscard]] std::string path_text(const std::filesystem::path& value) {
    const auto encoded = value.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

} // namespace

struct checkpoint_store::implementation {
    explicit implementation(sqlite3* value) noexcept : database(value) {}

    ~implementation() {
        if (database != nullptr) {
            static_cast<void>(sqlite3_close_v2(database));
        }
    }

    sqlite3* database{};
    std::atomic_bool disabled{false};
    std::mutex mutex;
};

checkpoint_store::checkpoint_store(std::unique_ptr<implementation> implementation) noexcept : implementation_(std::move(implementation)) {}

checkpoint_store::~checkpoint_store() = default;

result<std::shared_ptr<checkpoint_store>> checkpoint_store::open(const std::filesystem::path& file, const std::chrono::milliseconds timeout) {
    try {
        std::error_code filesystem_error;
        if (const auto parent = file.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent, filesystem_error);
        }
        if (filesystem_error) {
            return std::unexpected(error(code::unavailable, "local_store_path").with_detail(filesystem_error.message()));
        }
        sqlite3* raw{};
        const auto encoded = path_text(file);
        const int open_status = sqlite3_open_v2(encoded.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
        if (open_status != SQLITE_OK) {
            auto failure = sqlite_error(raw);
            if (raw != nullptr) {
                static_cast<void>(sqlite3_close_v2(raw));
            }
            return std::unexpected(std::move(failure));
        }
        auto state = std::make_unique<implementation>(raw);
        static_cast<void>(sqlite3_extended_result_codes(raw, 1));
        const auto bounded_timeout = std::clamp<std::int64_t>(timeout.count(), 1, std::numeric_limits<int>::max());
        if (sqlite3_busy_timeout(raw, static_cast<int>(bounded_timeout)) != SQLITE_OK) {
            return std::unexpected(sqlite_error(raw));
        }

        auto application_id = pragma_integer(raw, "PRAGMA application_id");
        auto schema_version = pragma_integer(raw, "PRAGMA user_version");
        if (!application_id || !schema_version) {
            return std::unexpected(!application_id ? application_id.error() : schema_version.error());
        }
        if ((*application_id != 0 && *application_id != checkpoint_application_id) || (*schema_version != 0 && *schema_version != checkpoint_schema_version)) {
            return std::unexpected(error(code::protocol, "local_store_path"));
        }
        constexpr auto schema = R"SQL(
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
BEGIN IMMEDIATE;
CREATE TABLE IF NOT EXISTS verdandi_catalog_entries(
    zone TEXT NOT NULL,
    scope BLOB NOT NULL CHECK(length(scope)=32),
    member TEXT NOT NULL,
    observed_revision INTEGER NOT NULL,
    revision INTEGER NOT NULL,
    replace_revision INTEGER NOT NULL,
    status INTEGER NOT NULL,
    kind INTEGER NOT NULL,
    encoded_bytes INTEGER NOT NULL,
    fields BLOB NOT NULL,
    PRIMARY KEY(zone, scope, member)
) WITHOUT ROWID, STRICT;
CREATE TABLE IF NOT EXISTS verdandi_catalog_cursors(
    zone TEXT NOT NULL,
    scope BLOB NOT NULL CHECK(length(scope)=32),
    revision INTEGER NOT NULL,
    PRIMARY KEY(zone, scope)
) WITHOUT ROWID, STRICT;
PRAGMA application_id=1447248212;
PRAGMA user_version=1;
COMMIT;
)SQL";
        if (auto status = execute(raw, schema); !status) {
            static_cast<void>(execute(raw, "ROLLBACK"));
            return std::unexpected(status.error());
        }
        return std::shared_ptr<checkpoint_store>(new checkpoint_store(std::move(state)));
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::unavailable, "local_store_path").with_detail(exception.what()));
    } catch (...) {
        return std::unexpected(error(code::unavailable, "local_store_path"));
    }
}

result<checkpoint_snapshot> checkpoint_store::load(const std::string_view zone, const std::string_view scope, const std::size_t maximum_bytes) {
    if (disabled()) {
        return checkpoint_snapshot{};
    }
    try {
        std::lock_guard lock(implementation_->mutex);
        sqlite3* database = implementation_->database;
        if (scope.size() != 32) {
            disable();
            return std::unexpected(error(code::corrupt, "local_store_path"));
        }
        if (auto status = execute(database, "BEGIN"); !status) {
            disable();
            return std::unexpected(status.error());
        }
        const auto rollback = [&] { static_cast<void>(execute(database, "ROLLBACK")); };
        checkpoint_snapshot output;
        auto cursor_query = prepare(database, "SELECT revision FROM verdandi_catalog_cursors WHERE zone=?1 AND scope=?2");
        if (!cursor_query || !bind_text(database, cursor_query->get(), 1, zone) ||
            !bind_blob(database, cursor_query->get(), 2, std::as_bytes(std::span(scope)))) {
            auto failure = cursor_query ? sqlite_error(database) : cursor_query.error();
            rollback();
            disable();
            return std::unexpected(std::move(failure));
        }
        const int cursor_status = sqlite3_step(cursor_query->get());
        if (cursor_status == SQLITE_ROW) {
            const auto value = sqlite3_column_int64(cursor_query->get(), 0);
            if (value < 0 || static_cast<std::uint64_t>(value) > maximum_revision || sqlite3_step(cursor_query->get()) != SQLITE_DONE) {
                rollback();
                disable();
                return std::unexpected(error(code::corrupt, "local_store_path"));
            }
            output.cursor = static_cast<std::uint64_t>(value);
        } else if (cursor_status != SQLITE_DONE) {
            auto failure = sqlite_error(database);
            rollback();
            disable();
            return std::unexpected(std::move(failure));
        }

        auto entries_query = prepare(database, "SELECT member,observed_revision,revision,replace_revision,status,kind,encoded_bytes,fields "
                                               "FROM verdandi_catalog_entries WHERE zone=?1 AND scope=?2 ORDER BY member");
        if (!entries_query || !bind_text(database, entries_query->get(), 1, zone) ||
            !bind_blob(database, entries_query->get(), 2, std::as_bytes(std::span(scope)))) {
            auto failure = entries_query ? sqlite_error(database) : entries_query.error();
            rollback();
            disable();
            return std::unexpected(std::move(failure));
        }
        for (int step = sqlite3_step(entries_query->get()); step == SQLITE_ROW; step = sqlite3_step(entries_query->get())) {
            const auto* member_data = reinterpret_cast<const char*>(sqlite3_column_text(entries_query->get(), 0));
            const int member_size = sqlite3_column_bytes(entries_query->get(), 0);
            const auto observed = sqlite3_column_int64(entries_query->get(), 1);
            const auto revision = sqlite3_column_int64(entries_query->get(), 2);
            const auto replace_revision = sqlite3_column_int64(entries_query->get(), 3);
            const auto stored_status = parse_status(sqlite3_column_int(entries_query->get(), 4));
            const auto stored_kind = parse_kind(sqlite3_column_int(entries_query->get(), 5));
            const auto encoded_bytes = sqlite3_column_int64(entries_query->get(), 6);
            const auto* field_data = static_cast<const std::byte*>(sqlite3_column_blob(entries_query->get(), 7));
            const int field_size = sqlite3_column_bytes(entries_query->get(), 7);
            if (member_data == nullptr || member_size <= 0 || observed < 0 || revision < 0 || replace_revision < 0 || encoded_bytes < 0 || field_size < 0 ||
                (!stored_status) || static_cast<std::uint64_t>(observed) > output.cursor || static_cast<std::uint64_t>(revision) > maximum_revision ||
                static_cast<std::uint64_t>(replace_revision) > maximum_revision || static_cast<std::uint64_t>(encoded_bytes) > maximum_bytes ||
                (field_size != 0 && field_data == nullptr)) {
                rollback();
                disable();
                return std::unexpected(error(code::corrupt, "local_store_path"));
            }
            auto target = parse_member(std::string_view(member_data, static_cast<std::size_t>(member_size)));
            auto decoded = decode_fields(std::span(field_data, static_cast<std::size_t>(field_size)));
            if (!target || !decoded) {
                rollback();
                disable();
                return std::unexpected(error(code::corrupt, "local_store_path"));
            }
            entry_state state{static_cast<std::uint64_t>(revision), static_cast<std::uint64_t>(replace_revision), *stored_status,
                              stored_kind.value_or(kind::value),    static_cast<std::size_t>(encoded_bytes),      std::move(*decoded)};
            if ((state.state == status::present && !stored_kind) || (state.state != status::present && sqlite3_column_int(entries_query->get(), 5) != 0) ||
                !validate_state(state, maximum_bytes) ||
                !output.entries.emplace(std::move(*target), std::make_shared<const entry_state>(std::move(state))).second) {
                rollback();
                disable();
                return std::unexpected(error(code::corrupt, "local_store_path"));
            }
        }
        if (sqlite3_errcode(database) != SQLITE_OK || !execute(database, "COMMIT")) {
            auto failure = sqlite_error(database);
            rollback();
            disable();
            return std::unexpected(std::move(failure));
        }
        return output;
    } catch (const std::exception& exception) {
        disable();
        return std::unexpected(error(code::unavailable, "local_store_path").with_detail(exception.what()));
    } catch (...) {
        disable();
        return std::unexpected(error(code::unavailable, "local_store_path"));
    }
}

result<void> checkpoint_store::save(const std::string_view zone, const std::string_view scope, const std::span<const checkpoint_entry> entries,
                                    const std::uint64_t cursor, const std::size_t maximum_bytes) {
    if (disabled()) {
        return {};
    }
    try {
        if (scope.size() != 32 || cursor > maximum_revision) {
            disable();
            return std::unexpected(error(code::corrupt, "local_store_path"));
        }
        std::vector<std::vector<std::byte>> encoded;
        encoded.reserve(entries.size());
        for (const auto& item : entries) {
            if (!item.target.valid() || !item.state || !validate_state(*item.state, maximum_bytes)) {
                disable();
                return std::unexpected(error(code::corrupt, "local_store_path"));
            }
            auto value = encode_fields(item.state->value);
            if (!value) {
                disable();
                return std::unexpected(value.error());
            }
            encoded.push_back(std::move(*value));
        }

        std::lock_guard lock(implementation_->mutex);
        sqlite3* database = implementation_->database;
        if (auto status = execute(database, "BEGIN IMMEDIATE"); !status) {
            disable();
            return status;
        }
        const auto rollback = [&] { static_cast<void>(execute(database, "ROLLBACK")); };
        auto write_entry =
            prepare(database,
                    "INSERT INTO verdandi_catalog_entries(zone,scope,member,observed_revision,revision,replace_revision,status,kind,encoded_bytes,fields) "
                    "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) ON CONFLICT(zone,scope,member) DO UPDATE SET "
                    "observed_revision=excluded.observed_revision,revision=excluded.revision,replace_revision=excluded.replace_revision,status=excluded.status,"
                    "kind=excluded.kind,encoded_bytes=excluded.encoded_bytes,fields=excluded.fields WHERE "
                    "excluded.observed_revision>verdandi_catalog_entries.observed_revision OR "
                    "(excluded.observed_revision=verdandi_catalog_entries.observed_revision AND excluded.revision>verdandi_catalog_entries.revision)");
        if (!write_entry) {
            rollback();
            disable();
            return std::unexpected(write_entry.error());
        }
        for (std::size_t index = 0; index < entries.size(); ++index) {
            sqlite3_stmt* statement_value = write_entry->get();
            const auto& item = entries[index];
            const auto member = item.target.member();
            const std::array<result<void>, 10> bindings{
                bind_text(database, statement_value, 1, zone),
                bind_blob(database, statement_value, 2, std::as_bytes(std::span(scope))),
                bind_text(database, statement_value, 3, member),
                bind_integer(database, statement_value, 4, cursor),
                bind_integer(database, statement_value, 5, item.state->revision),
                bind_integer(database, statement_value, 6, item.state->replace_revision),
                sqlite3_bind_int(statement_value, 7, status_number(item.state->state)) == SQLITE_OK ? result<void>{}
                                                                                                    : result<void>{std::unexpected(sqlite_error(database))},
                sqlite3_bind_int(statement_value, 8, item.state->state == status::present ? kind_number(item.state->shape) : 0) == SQLITE_OK
                    ? result<void>{}
                    : result<void>{std::unexpected(sqlite_error(database))},
                bind_integer(database, statement_value, 9, item.state->encoded_bytes),
                bind_blob(database, statement_value, 10, encoded[index]),
            };
            const auto failed = std::ranges::find_if(bindings, [](const result<void>& value) { return !value; });
            if (failed != bindings.end() || sqlite3_step(statement_value) != SQLITE_DONE) {
                auto failure = failed != bindings.end() ? failed->error() : sqlite_error(database);
                rollback();
                disable();
                return std::unexpected(std::move(failure));
            }
            static_cast<void>(sqlite3_reset(statement_value));
            static_cast<void>(sqlite3_clear_bindings(statement_value));
        }

        auto write_cursor = prepare(database, "INSERT INTO verdandi_catalog_cursors(zone,scope,revision) VALUES(?1,?2,?3) "
                                              "ON CONFLICT(zone,scope) DO UPDATE SET revision=excluded.revision "
                                              "WHERE excluded.revision>verdandi_catalog_cursors.revision");
        if (!write_cursor || !bind_text(database, write_cursor->get(), 1, zone) ||
            !bind_blob(database, write_cursor->get(), 2, std::as_bytes(std::span(scope))) || !bind_integer(database, write_cursor->get(), 3, cursor) ||
            sqlite3_step(write_cursor->get()) != SQLITE_DONE) {
            auto failure = write_cursor ? sqlite_error(database) : write_cursor.error();
            rollback();
            disable();
            return std::unexpected(std::move(failure));
        }
        if (auto status = execute(database, "COMMIT"); !status) {
            rollback();
            disable();
            return status;
        }
        return {};
    } catch (const std::exception& exception) {
        disable();
        return std::unexpected(error(code::unavailable, "local_store_path").with_detail(exception.what()));
    } catch (...) {
        disable();
        return std::unexpected(error(code::unavailable, "local_store_path"));
    }
}

bool checkpoint_store::disabled() const noexcept {
    return !implementation_ || implementation_->disabled.load(std::memory_order_acquire);
}

void checkpoint_store::disable() noexcept {
    if (implementation_) {
        implementation_->disabled.store(true, std::memory_order_release);
    }
}

} // namespace verdandi::catalog::detail
