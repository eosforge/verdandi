#include "database_fault.hpp"
#include "ledger.hpp"
#include "pulsar_test.hpp"
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

using namespace astra;

namespace {
// 测试只用此连接观察或故意破坏本例数据库, 不构成生产绕过服务锁的管理入口.
class SQL {
public:
    // 打开已由 Ledger 显式初始化的库, 失败不创建新文件.
    explicit SQL(const std::filesystem::path& path) {
        if (sqlite3_open_v2(path.c_str(), &database_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
            sqlite3_close_v2(database_);
            throw std::runtime_error("Cannot open owned test database");
        }
    }

    // 析构关闭连接, SQLite 自动回滚测试制造的未提交事务.
    ~SQL() {
        sqlite3_close_v2(database_);
    }

    // 禁止复制数据库句柄.
    SQL(const SQL&) = delete;
    // 禁止覆盖现有测试事务.
    SQL& operator=(const SQL&) = delete;

    // sql 为固定测试语句, 不接受部署输入; 所有注入必须明确成功后继续断言.
    void execute(const char* sql) {
        CHECK(sqlite3_exec(database_, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    // body 为本例构造的完整成员正文, 长度单独绑定, 不经 SQL 字符串插值或 NUL 截断.
    void replace(std::string_view body) {
        sqlite3_stmt* statement{}; // 唯一测试 UPDATE, 绑定寿命覆盖 step, owner 在异常时也会 finalize.
        const auto prepared = sqlite3_prepare_v2(database_, "UPDATE members SET body=?", -1, &statement, nullptr);
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> owner(statement, sqlite3_finalize);
        CHECK(prepared == SQLITE_OK);
        CHECK(sqlite3_bind_blob(statement, 1, body.data(), static_cast<int>(body.size()), SQLITE_STATIC) == SQLITE_OK);
        CHECK(sqlite3_step(statement) == SQLITE_DONE);
    }

    // 单列标量查询返回独立拥有的文本, 可读取 hex(epoch) 验证完整字节表示.
    std::string value(const char* sql) {

        // statement 只属于本次查询, 独立 RAII 在断言抛异常时也能清理.
        sqlite3_stmt* statement{};
        const auto prepared = sqlite3_prepare_v2(database_, sql, -1, &statement, nullptr);
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> owner(statement, sqlite3_finalize);
        CHECK(prepared == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW);
        const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        CHECK(data);
        const std::string result(data, static_cast<std::size_t>(sqlite3_column_bytes(statement, 0)));
        CHECK(sqlite3_step(statement) == SQLITE_DONE);
        return result;
    }

private:
    // database_ 由 SQLite 创建, 测试结束时唯一关闭, 初始为空.
    sqlite3* database_{};
};

// action 必须明确抛异常, 不以服务返回空视图或短路未执行冒充拒绝.
template <class Action>
void rejects(Action&& action) {

    bool rejected{};
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    CHECK(rejected);
}

// 同一个启动请求并发提交只追加一次. 每个线程独占结果槽, 主线程 join 后断言.
static void concurrent_retry(const std::filesystem::path& path, const std::string& authority) {

    // ledger 独占当前测试数据库, 每角色最多 2 个成员, 历史启动最多 4 条.
    Ledger ledger(path, "alpha", authority, 2, 4, true);
    // success 每线程独占一个结果槽, join 后统一读取.
    std::array<bool, 8> success{};
    // ids 每线程保存同一幂等登记最终返回的成员 ID.
    std::array<Id, 8> ids{};
    // clients 拥有八个并发登记线程, clear 时全部 join.
    std::vector<std::jthread> clients;
    // i 为独立结果槽下标, 捕获值固定各线程责任.
    for (std::size_t i = 0; i < success.size(); ++i) {
        clients.emplace_back([&, i] {
            try {
                // candidate 为当前线程独有的候选, 请求 ID 相同而建议成员 ID 不同.
                auto candidate = test::member(1);
                candidate.id = "attempt-" + std::to_string(i);
                success[i] = ledger.register_member(candidate, std::string(32, 'a'), [&](const Member& member, const auto&) { ids[i] = member.id; }).has_value();
            } catch (...) {
                success[i] = false;
            }
        });
    }
    clients.clear();
    // i 为独立结果槽下标, 捕获值固定各线程责任.
    for (std::size_t i = 0; i < success.size(); ++i) {
        CHECK(success[i] && ids[i] == ids.front());
    }
}

// 显式初始化、服务级独占和已知格式边界, 失败不得把无效目标当成新群组.
void initialization(const std::filesystem::path& root, const std::string& authority) {

    const auto path = root / "initialization.db";
    rejects([&] { Ledger missing(path, "alpha", authority, 2, 4); });
    CHECK(!std::filesystem::exists(path));
    {
        Ledger initialized(path, "alpha", authority, 2, 4, true);
        rejects([&] { Ledger locked(path, "alpha", authority, 2, 4); });
    }
    // 已有文件不能显式重建, 新连接恢复正确绑定, 错误 Galaxy/签发者拒绝.
    rejects([&] { Ledger duplicate(path, "alpha", authority, 2, 4, true); });
    rejects([&] { Ledger galaxy(path, "other", authority, 2, 4); });
    rejects([&] { Ledger signer(path, "alpha", std::string(64, 'b'), 2, 4); });
    {
        Ledger restored(path, "alpha", authority, 2, 4);
    }

    // 各 target 在本例独占目录中创建, 不使用部署文件或跟随符号链接.
    const auto empty = root / "empty.db";
    { std::ofstream output(empty); }
    rejects([&] { Ledger invalid(empty, "alpha", authority, 2, 4); });
    const auto legacy = root / "legacy.db";
    {
        std::ofstream output(legacy);
        output << "ASTRA-PULSAR-JOURNAL-1\n";
    }
    rejects([&] { Ledger invalid(legacy, "alpha", authority, 2, 4); });
    const auto link = root / "linked.db";
    std::filesystem::create_symlink(path, link);
    rejects([&] { Ledger invalid(link, "alpha", authority, 2, 4); });
    const auto directory = root / "directory.db";
    std::filesystem::create_directory(directory);
    rejects([&] { Ledger invalid(directory, "alpha", authority, 2, 4); });
    rejects([&] { Ledger invalid(root / "missing-parent/state.db", "alpha", authority, 2, 4, true); });
    const auto sidecar = root / "sidecar.db";
    {
        std::ofstream output(sidecar.string() + "-journal");
        output << "owned recovery evidence";
    }
    rejects([&] { Ledger invalid(sidecar, "alpha", authority, 2, 4, true); });
    CHECK(!std::filesystem::exists(sidecar) && std::filesystem::file_size(sidecar.string() + "-journal") != 0);
}

// 身份幂等、替换、快照寿命、容量及 prepare 异常边界在同库重启前后保持一致.
void registration(const std::filesystem::path& path, const std::string& authority) {

    // committed/count 分别保存 prepare 所见成员和当时目录数, 回调运行不等于提交成功.
    Member committed;
    std::size_t count{};
    const auto prepare = [&](const Member& member, const auto& members) { committed = member; count = members.size(); };
    const std::string first(32, 'a'), second(32, 'b'), next(32, 'c');
    {
        Ledger ledger(path, "alpha", authority, 2, 4, true);
        CHECK(ledger.register_member(test::member(1), first, prepare));
        const auto original = committed;
        const auto snapshot = ledger.snapshot(original);
        CHECK(snapshot && snapshot->size() == 1);
        CHECK(ledger.register_member(test::member(2), second, prepare));
        CHECK(ledger.register_member(test::member(1), first, prepare));
        CHECK(count == 2 && committed == original && snapshot->size() == 1);

        // changed 保留部署身份, 更换进程 ID; 重试改变分组/部署与占满角色均拒绝.
        auto changed = test::member(1);
        changed.group = "other";
        CHECK(!ledger.register_member(changed, first, prepare));
        CHECK(!ledger.register_member(test::member(2), first, prepare));
        CHECK(!ledger.register_member(test::member(3), std::string(32, 'd'), prepare));
        CHECK(!ledger.register_member(test::member(3), std::string(31, 'd'), prepare));
        changed.group = "default";
        changed.id = "replacement";
        rejects([&] { static_cast<void>(ledger.register_member(changed, next, [](const auto&, const auto&) { throw std::bad_alloc{}; })); });
        CHECK(ledger.current(original));
        SQL sql(path);
        CHECK(sql.value("SELECT count(*) FROM starts") == "2");
        CHECK(ledger.register_member(changed, next, prepare));
        CHECK(committed.epoch.value == 2 && ledger.current(committed) && !ledger.current(original));
        CHECK(!ledger.register_member(test::member(1), first, prepare));
        CHECK(sql.value("SELECT hex(epoch) FROM starts ORDER BY epoch DESC LIMIT 1") == "0000000000000002");
        CHECK(sql.value("PRAGMA journal_mode") == "delete");
    }

    Ledger restored(path, "alpha", authority, 2, 4);
    auto retry = test::member(1);
    retry.id = "ignored-new-suggestion";
    CHECK(restored.register_member(retry, next, prepare));
    CHECK(committed.id == "replacement" && committed.epoch.value == 2 && count == 2);
    CHECK(!restored.register_member(test::member(1), first, prepare));
    // 控制角色独立拥有容量, 本例第四条启动成功, 再新建任何角色均触及累计启动上限.
    CHECK(restored.register_member(test::member(3, Member::Role::polaris), std::string(32, 'd'), prepare));
    CHECK(!restored.register_member(test::member(4, Member::Role::astrolabe), std::string(32, 'e'), prepare));
}

// 固定 format=1 的磁盘编码不随 protobuf string/bytes 迁移变化, 恢复保留身份、代次与启动去重证据.
void encoding(const std::filesystem::path& path, const std::string& authority) {

    const auto candidate = test::member(1); // 登记候选的 epoch 必须为零, 由 Ledger 分配.
    auto original = candidate;              // 独立保存首次成功登记预期身份, 不把已有代次再次当作候选提交.
    original.epoch.value = 1;
    const std::string request(32, 'a'); // 首次启动幂等键, 恢复后仍必须返回同一身份.
    {
        Ledger ledger(path, "alpha", authority, 2, 4, true);
        CHECK(ledger.register_member(candidate, request, [](const auto&, const auto&) {}));
    }
    {
        SQL sql(path);
        proto::orbit::v1::Member persisted; // 新写入也必须遵守既有磁盘契约.
        CHECK(persisted.ParseFromString(sql.value("SELECT body FROM members")));
        CHECK(persisted.principal() == original.principal.text());
        persisted.set_principal(original.principal.text());
        sql.replace(persisted.SerializeAsString());
    }
    {
        Ledger restored(path, "alpha", authority, 2, 4);
        CHECK(restored.current(original));
        CHECK(restored.register_member(candidate, request, [&](const Member& member, const auto&) { CHECK(member == original); }));
        auto next = candidate; // 新启动由 Ledger 继承旧部署代次, 不把恢复误判为重新初始化.
        next.id = "next-process";
        CHECK(restored.register_member(next, std::string(32, 'b'), [&](const Member& member, const auto&) { CHECK(member.epoch.value == original.epoch.value + 1); }));
    }

    // 正文与主键不匹配时仍拒绝, 不能通过编码转换绕过原有部署身份检查.
    {
        SQL sql(path);
        proto::orbit::v1::Member persisted;
        CHECK(persisted.ParseFromString(sql.value("SELECT body FROM members")));
        persisted.set_principal(std::string(64, 'f'));
        sql.replace(persisted.SerializeAsString());
    }
    rejects([&] { Ledger invalid(path, "alpha", authority, 2, 4); });
}

// 真实 VFS 一次性写/同步故障, 失败后不发布身份也不继续签发; 关闭后同库恢复旧身份.
void io_failure(const std::filesystem::path& path, const std::string& authority, int error, bool sync, bool main = false, bool persistent = false) {

    test::Fault fault;
    Member original, candidate;
    const auto prepare = [&](const Member& member, const auto&) { candidate = member; };
    {
        Ledger ledger(path, "alpha", authority, 2, 4, true);
        CHECK(ledger.register_member(test::member(1), std::string(32, 'a'), prepare));
        original = candidate;
        auto changed = test::member(1);
        changed.id = "uncommitted";
        fault.arm(error, sync, main, persistent ? 0U : 1U);
        CHECK(!ledger.register_member(changed, std::string(32, 'b'), prepare));
        CHECK(fault.hits() >= 1 && ledger.current(original) && !ledger.current(candidate));
        if (!persistent) {
            CHECK(fault.hits() == 1);
        }
        CHECK(!ledger.register_member(test::member(1), std::string(32, 'a'), prepare));
        fault.clear(); // 生产已经停止签发, 模拟存储故障修复, 之后才关闭并恢复实际文件.
    }

    Ledger restored(path, "alpha", authority, 2, 4);
    CHECK(restored.current(original));
    CHECK(restored.register_member(test::member(1), std::string(32, 'a'), prepare));
    CHECK(candidate == original);
    auto changed = test::member(1);
    changed.id = "recovered";
    CHECK(restored.register_member(changed, std::string(32, 'b'), prepare));
    CHECK(candidate.epoch.value == 2 && restored.current(candidate));
}

// COMMIT 的 deferred FK 失败不是自动回滚证明; 生产流程必须显式回滚两表并隔离新登记.
void commit_failure(const std::filesystem::path& path, const std::string& authority) {

    Member original, candidate;
    {
        Ledger ledger(path, "alpha", authority, 2, 4, true);
        CHECK(ledger.register_member(test::member(1), std::string(32, 'a'), [&](const Member& member, const auto&) { original = member; }));
        SQL sql(path);
        sql.execute("CREATE TABLE failure (principal TEXT REFERENCES members(principal) DEFERRABLE INITIALLY DEFERRED)");
        sql.execute("CREATE TRIGGER commit_failure AFTER INSERT ON starts BEGIN INSERT INTO failure VALUES('missing'); END");
        CHECK(!ledger.register_member(test::member(2), std::string(32, 'b'), [&](const Member& member, const auto&) { candidate = member; }));
        CHECK(ledger.current(original) && !ledger.current(candidate));
        CHECK(sql.value("SELECT count(*) FROM members") == "1" && sql.value("SELECT count(*) FROM starts") == "1");
        CHECK(sql.value("SELECT count(*) FROM failure") == "0");
        sql.execute("DROP TRIGGER commit_failure");
        sql.execute("DROP TABLE failure");
        CHECK(!ledger.register_member(test::member(2), std::string(32, 'b'), [](const auto&, const auto&) {}));
    }
    Ledger restored(path, "alpha", authority, 2, 4);
    CHECK(restored.current(original) && !restored.current(candidate));
}

// 外部文件锁使 SQLite 等待并最终 BUSY, 当前身份读取仍不等待登记互斥或磁盘锁.
void busy(const std::filesystem::path& path, const std::string& authority) {

    Ledger ledger(path, "alpha", authority, 2, 4, true);
    Member original;
    CHECK(ledger.register_member(test::member(1), std::string(32, 'a'), [&](const Member& member, const auto&) { original = member; }));
    SQL sql(path);
    sql.execute("BEGIN EXCLUSIVE");
    // prepared 建立已进入提交前阶段的同步点, outcome 返回登记线程的正常结果或异常.
    std::promise<void> prepared;
    std::promise<bool> outcome;
    auto waiting = prepared.get_future();
    auto result = outcome.get_future();
    const auto begin = Steady::now();
    std::jthread writer([&] {
        try {
            outcome.set_value(ledger.register_member(test::member(2), std::string(32, 'b'), [&](const auto&, const auto&) { prepared.set_value(); }).has_value());
        } catch (...) {
            outcome.set_exception(std::current_exception());
        }
    });
    CHECK(waiting.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
    CHECK(ledger.current(original) && ledger.snapshot(original)->size() == 1);
    CHECK(result.wait_for(std::chrono::seconds(3)) == std::future_status::ready && !result.get());
    CHECK(Steady::now() - begin < std::chrono::seconds(4));
    sql.execute("ROLLBACK");
}

// 每个修改都在独立副本进行. 恢复必须拒绝未知 Schema、丢失幂等证据及 unsigned 边界污染.
void corruption(const std::filesystem::path& root, const std::string& authority) {

    // mutations 为固定的内部损坏样本; 有效 SQL 执行成功后才尝试正常恢复.
    constexpr std::array mutations{
        "CREATE TABLE extra (value TEXT)",
        "CREATE INDEX unexpected ON starts(epoch)",
        "PRAGMA ignore_check_constraints=ON; UPDATE metadata SET format=2",
        "UPDATE metadata SET authority='wrong'",
        "DELETE FROM starts",
        "PRAGMA ignore_check_constraints=ON; UPDATE members SET body=x'ff'",
        "PRAGMA ignore_check_constraints=ON; UPDATE starts SET request=x'00'",
        "UPDATE starts SET epoch=x'0000000000000000'",
        "UPDATE starts SET epoch=x'7fffffffffffffff'",
        "UPDATE starts SET epoch=x'8000000000000000'",
        "UPDATE starts SET epoch=x'ffffffffffffffff'"};
    // index 只用于生成独立文件名, 每个库包含同一条已确认启动身份.
    std::size_t index{};
    for (const auto mutation : mutations) {
        const auto path = root / ("corrupt-" + std::to_string(index++) + ".db");
        {
            Ledger ledger(path, "alpha", authority, 2, 4, true);
            CHECK(ledger.register_member(test::member(1), std::string(32, 'a'), [](const auto&, const auto&) {}));
        }
        {
            SQL sql(path);
            sql.execute(mutation);
        }
        rejects([&] { Ledger invalid(path, "alpha", authority, 2, 4); });
        CHECK(std::filesystem::file_size(path) > 0);
    }

    const auto path = root / "physical.db";
    { Ledger ledger(path, "alpha", authority, 2, 4, true); }
    {
        std::fstream output(path, std::ios::in | std::ios::out | std::ios::binary);
        output.put('\xff');
    }
    rejects([&] { Ledger invalid(path, "alpha", authority, 2, 4); });
}
} // namespace

// 仅由明确授权的测试命令启动, 所有文件、VFS 和连接在异常路径同样释放.
int main() {

    try {
        test::Directory directory;
        const std::string authority(64, 'a');
        initialization(directory.path, authority);
        registration(directory.path / "registration.db", authority);
        encoding(directory.path / "encoding.db", authority);
        concurrent_retry(directory.path / "concurrent.db", authority);
        commit_failure(directory.path / "commit.db", authority);
        busy(directory.path / "busy.db", authority);
        corruption(directory.path, authority);
        // error 覆盖 SQLite 常见写失败类别, 另用 xSync 验证持久化阶段失败.
        for (const int error : {SQLITE_IOERR_WRITE, SQLITE_FULL, SQLITE_NOMEM}) {
            io_failure(directory.path / ("fault-" + std::to_string(error) + ".db"), authority, error, false);
        }
        io_failure(directory.path / "sync.db", authority, SQLITE_IOERR_FSYNC, true);
        io_failure(directory.path / "main-sync.db", authority, SQLITE_IOERR_FSYNC, true, true);
        io_failure(directory.path / "rollback.db", authority, SQLITE_IOERR_WRITE, false, true, true);
        std::cout << "PASS SQLite admission initialization, transactions, retry, fault recovery, bounds and read isolation\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
