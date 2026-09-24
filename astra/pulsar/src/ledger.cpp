#include "ledger.hpp"
#include "identity.hpp"
#include <algorithm>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sqlite3.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace astra {

// 仅供 Ledger 使用的连接与语句所有权. 单写锁保护所有运行期 SQL, 读取当前身份不进 SQLite.
struct Ledger::Database {
    // 固定格式只在显式初始化时创建; SQL 原文同时用于拒绝未知 Schema 或额外对象.
    static constexpr std::array<std::string_view, 3> schema{
        "CREATE TABLE metadata (id INTEGER PRIMARY KEY CHECK(id=1), format INTEGER NOT NULL CHECK(format=1), galaxy TEXT NOT NULL, authority TEXT NOT NULL) STRICT",
        "CREATE TABLE members (principal TEXT PRIMARY KEY CHECK(length(principal)=64), body BLOB NOT NULL CHECK(length(body) BETWEEN 1 AND 1024)) STRICT, WITHOUT ROWID",
        "CREATE TABLE starts (request BLOB PRIMARY KEY CHECK(length(request)=32), principal TEXT NOT NULL REFERENCES members(principal), epoch BLOB NOT NULL CHECK(length(epoch)=8), UNIQUE(principal,epoch)) STRICT, WITHOUT ROWID"};

    // 固定错误不拼接 SQL, 路径或 SQLite 原始诊断, 避免泄露部署数据.
    static void require(bool condition) {
        if (!condition) {
            throw std::runtime_error("Invalid or unavailable Pulsar SQLite database");
        }
    }

    // 拥有单个文件描述符, 构造验证普通文件; 只用于打开已授权的库和独立服务锁.
    struct File {
        // value 为有效描述符, 不能复制其关闭责任.
        int value;

        // path 必须位于部署拥有的目录, flags 由调用点明确选择是否排他创建.
        File(const std::filesystem::path& path, int flags) : value(::open(path.c_str(), flags | O_CLOEXEC | O_NOFOLLOW, 0600)) {

            // info 检查实际打开的 inode, 不把 FIFO/目录/符号链接当成状态文件.
            struct stat info{};
            if (value < 0 || ::fstat(value, &info) != 0 || !S_ISREG(info.st_mode)) {
                if (value >= 0) {
                    ::close(value);
                }
                require(false);
            }
        }

        // 释放文件和附着的 flock, 不删除锁文件以免产生两个不同 inode 的锁域.
        ~File() {
            ::close(value);
        }

        // 禁止复制唯一文件所有者.
        File(const File&) = delete;
        // 禁止赋值覆盖描述符.
        File& operator=(const File&) = delete;
    };

    // 连接句柄即使构造后续阶段抛异常也会关闭; 所有语句须先于连接释放.
    struct Connection {
        // value 由 sqlite3_open_v2 填入, 失败也可能返回需关闭的句柄.
        sqlite3* value{};
        // 唯一连接拥有者, 默认空; 打开动作在 Database 内统一进行.
        Connection() = default;

        // close_v2 容忍空句柄, 所有正常路径不留下悬空语句.
        ~Connection() {
            sqlite3_close_v2(value);
        }

        // 禁止复制连接关闭责任.
        Connection(const Connection&) = delete;
        // 禁止覆盖活动连接.
        Connection& operator=(const Connection&) = delete;
    };

    // 一条固定 SQL 的 RAII 预编译语句, 参数类型明确, 不进行动态 SQL 拼接.
    class Query {
    public:
        // database 借用唯一连接, sql 必须静态且以 NUL 结尾, 构造失败无残留语句.
        Query(sqlite3* database, const char* sql) {
            // code 保存预编译结果; SQLite 错误路径也显式清理可能返回的句柄.
            const auto code = sqlite3_prepare_v3(database, sql, -1, 0, &statement_, nullptr);
            if (code != SQLITE_OK) {
                sqlite3_finalize(statement_);
                require(false);
            }
        }

        // finalize 不抛异常, 未完成的语句不能越过连接生命周期.
        ~Query() {
            sqlite3_finalize(statement_);
        }

        // 禁止复制语句所有权.
        Query(const Query&) = delete;
        // 禁止覆盖未完成语句.
        Query& operator=(const Query&) = delete;

        // 借用 value 到本条语句销毁, 参数不用于 SQL 文本, 位次从 1 开始.
        void text(int index, std::string_view value) {
            require(value.size() <= 2048 && sqlite3_bind_text(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_STATIC) == SQLITE_OK);
        }

        // 借用明确长度的原始字节, 包含 NUL 的启动随机数不会被截短.
        void blob(int index, std::string_view value) {
            require(value.size() <= 2048 && sqlite3_bind_blob(statement_, index, value.data(), static_cast<int>(value.size()), SQLITE_STATIC) == SQLITE_OK);
        }

        // 返回是否得到一行, DONE 是正常结束, 其他状态均抛固定诊断.
        bool next() {
            // code 区分 ROW 与 DONE, 不把 BUSY/损坏/磁盘错误误判成空表.
            const auto code = sqlite3_step(statement_);
            require(code == SQLITE_ROW || code == SQLITE_DONE);
            return code == SQLITE_ROW;
        }

        // 读取当前列的 UTF-8 文本视图, 只在下一次 step/finalize 前有效.
        std::string_view text(int column) const {
            return bytes(column, SQLITE_TEXT);
        }

        // 读取当前列的 BLOB 视图, 调用者在推进语句前完成解析或复制.
        std::string_view blob(int column) const {
            return bytes(column, SQLITE_BLOB);
        }

        // 小型 PRAGMA/元信息使用 INTEGER, 身份代次禁止通过此方法窄化.
        sqlite3_int64 integer(int column) const {
            require(sqlite3_column_type(statement_, column) == SQLITE_INTEGER);
            return sqlite3_column_int64(statement_, column);
        }

    private:
        // column 为零基列号, type 为唯一允许存储类型; 失败不进行隐式类型转换.
        std::string_view bytes(int column, int type) const {
            require(sqlite3_column_type(statement_, column) == type);
            // data 借用 SQLite 本行字节, size 已受连接 LENGTH 上限约束.
            const auto data = static_cast<const char*>(sqlite3_column_blob(statement_, column));
            const auto size = sqlite3_column_bytes(statement_, column);
            require(size >= 0 && (data || size == 0));
            return {data ? data : "", static_cast<std::size_t>(size)};
        }

        // statement_ 由构造接管, 每条语句只在登记锁或启动恢复期间访问.
        sqlite3_stmt* statement_{};
    };

    // 单个固定 SQL, 包括事务控制和 PRAGMA; 不允许包含业务正文或凭据.
    void execute(const char* sql) {
        require(sqlite3_exec(connection.value, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
    }

    // 对错误事务只做显式回滚, 返回值用于调用方诊断, 不假设 COMMIT 错误已经回滚.
    bool rollback() noexcept {
        return sqlite3_get_autocommit(connection.value) != 0 || sqlite3_exec(connection.value, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK;
    }

    // 固定宽度大端字节无有符号转换, 可保留 UINT64_MAX, 不落为 SQLite REAL.
    static std::array<char, 8> epoch(std::uint64_t value) {
        // result 的各字节按高位到低位排列, 便于 SQLite BLOB 索引按 unsigned 顺序比较.
        std::array<char, 8> result{};
        for (unsigned index = 0; index < result.size(); ++index) {
            result[index] = static_cast<char>((value >> (56 - index * 8)) & 255);
        }
        return result;
    }

    // 恢复八字节大端代次, 非法宽度或零值均拒绝.
    static std::uint64_t epoch(std::string_view value) {
        require(value.size() == 8);
        // result 从零累计, 固定八步不产生超出 uint64 的位移.
        std::uint64_t result{};
        for (const char byte : value) {
            result = (result << 8) | static_cast<unsigned char>(byte);
        }
        require(result != 0);
        return result;
    }

    // 严格打开或初始化, 不自动创建父目录, 不覆盖文件或导入 journal/bbolt.
    Database(const std::filesystem::path& path, std::string_view galaxy, std::string_view authority, bool initialize) : lock(path.string() + ".lock", O_RDWR | O_CREAT) {

        require(::flock(lock.value, LOCK_EX | LOCK_NB) == 0);
        // 初始化目标包括潜在 sidecar, 不在残留恢复材料旁新建一个空数据库.
        if (initialize) {
            for (const auto suffix : {"-journal", "-wal", "-shm"}) {
                require(std::filesystem::symlink_status(path.string() + suffix).type() == std::filesystem::file_type::not_found);
            }
        }
        // file 固定实际目标, initialize 使用 O_EXCL; 正常启动不创建缺失状态文件.
        const File file(path, O_RDWR | (initialize ? O_CREAT | O_EXCL : 0));
        // info 用于排除正常启动的空文件, 并在 SQLite 打开后再次核对路径未被替换.
        struct stat info{};
        require(::fstat(file.value, &info) == 0 && (initialize || info.st_size > 0));
        require(sqlite3_libversion_number() == 3053004 && sqlite3_threadsafe() != 0);
        require(sqlite3_open_v2(path.c_str(), &connection.value, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr) == SQLITE_OK);
        // actual 来自路径末端且不跟随符号链接, 所有状态文件要求位于受信任部署目录.
        struct stat actual{};
        require(::lstat(path.c_str(), &actual) == 0 && S_ISREG(actual.st_mode) && actual.st_dev == info.st_dev && actual.st_ino == info.st_ino);
        require(sqlite3_busy_timeout(connection.value, 1000) == SQLITE_OK);
        sqlite3_limit(connection.value, SQLITE_LIMIT_LENGTH, 4096);
        sqlite3_limit(connection.value, SQLITE_LIMIT_SQL_LENGTH, 16384);
        // defensive 为 SQLite 实际返回的设置, 不能只发出配置而不确认是否生效.
        int defensive{};
        require(sqlite3_db_config(connection.value, SQLITE_DBCONFIG_DEFENSIVE, 1, &defensive) == SQLITE_OK && defensive == 1);
        execute("PRAGMA trusted_schema=OFF");
        execute("PRAGMA foreign_keys=ON");
        if (initialize) {
            execute("PRAGMA journal_mode=DELETE");
        }
        execute("PRAGMA synchronous=EXTRA");
        {
            // mode 检查已有库也是 DELETE, 不悄悄把其他模式改写后宣称已恢复.
            Query mode(connection.value, "PRAGMA journal_mode");
            require(mode.next() && mode.text(0) == "delete" && !mode.next());
        }
        // sql/expected 分别为连接配置查询和唯一接受的整数值.
        for (const auto& [sql, expected] : std::array<std::pair<const char*, int>, 3>{{{"PRAGMA synchronous", 3}, {"PRAGMA foreign_keys", 1}, {"PRAGMA trusted_schema", 0}}}) {
            Query setting(connection.value, sql);
            require(setting.next() && setting.integer(0) == expected && !setting.next());
        }

        if (initialize) {
            execute("BEGIN IMMEDIATE");
            try {
                for (const auto sql : schema) {
                    execute(sql.data());
                }
                // metadata 的绑定与 Schema 同事务, 半次初始化不会被正常恢复接受.
                Query metadata(connection.value, "INSERT INTO metadata VALUES(1,1,?,?)");
                metadata.text(1, galaxy);
                metadata.text(2, authority);
                require(!metadata.next());
                execute("COMMIT");
            } catch (...) {
                static_cast<void>(rollback());
                throw;
            }
            // 新建数据库自身和父目录都显式同步. 失败保留文件供人工检查, 不自动重建.
            const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
            const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            const bool synced = ::fsync(file.value) == 0 && directory >= 0 && ::fsync(directory) == 0;
            if (directory >= 0) {
                ::close(directory);
            }
            require(synced);
        }

        // 读取物理一致性, 已知格式和身份边界. 不使用 AutoMigrate 或错误修复删除.
        Query check(connection.value, "PRAGMA quick_check");
        require(check.next() && check.text(0) == "ok" && !check.next());
        Query foreign(connection.value, "PRAGMA foreign_key_check");
        require(!foreign.next());
        Query objects(connection.value, "SELECT sql FROM sqlite_schema WHERE sql IS NOT NULL ORDER BY name");
        for (const auto index : {1U, 0U, 2U}) {
            require(objects.next() && objects.text(0) == schema[index]);
        }
        require(!objects.next());
        Query metadata(connection.value, "SELECT id,format,galaxy,authority FROM metadata");
        require(metadata.next() && metadata.integer(0) == 1 && metadata.integer(1) == 1 && metadata.text(2) == galaxy && metadata.text(3) == authority && !metadata.next());
    }

    // 锁先构造后析构, 数据库连接完全关闭前不允许另一签发者接管.
    File lock;
    // 唯一写连接. NOMUTEX 只关闭连接内部互斥, Ledger 外层仍串行化所有 SQL.
    Connection connection;
};

Ledger::Ledger(const std::filesystem::path& path, std::string galaxy, std::string authority, std::size_t maximum, std::size_t starts, bool initialize) : galaxy_(std::move(galaxy)), maximum_(maximum), maximum_starts_(starts) {

    if (!Member::valid_name(galaxy_) || !Principal::parse(authority) || maximum == 0 || maximum > 4096 || starts == 0 || starts > 1'000'000) {
        throw std::runtime_error("Invalid registration database configuration");
    }
    database_ = std::make_unique<Database>(path, galaxy_, authority, initialize);
    restore();
}

// Database 的完整类型已可见, 默认析构按 RAII 顺序关闭连接和进程锁.
Ledger::~Ledger() = default;

Result<void> Ledger::validate(const Member& member, std::string_view request_id, const Members& view) const {

    if (!member.validate() || member.galaxy != galaxy_ || request_id.size() != 32) {
        return Status::configuration("Invalid registration record");
    }
    if (starts_.contains(request_id)) {
        return Status::conflict("Startup request already committed");
    }
    if (starts_.size() >= maximum_starts_) {
        return Status::capacity("Startup history capacity reached");
    }

    // principal 为部署摘要的规范文本键, 与 Members 和 Start 索引保持同一编码.
    const auto principal = member.principal.text();
    // old 指向该部署当前记录, 末尾表示首次登记, 不通过新进程 ID 定位旧代次.
    const auto old = view.find(principal);
    // previous 是已登记代次, 未知部署按零处理, 新记录必须严格为下一代.
    const auto previous = old == view.end() ? 0 : old->second.epoch.value;
    if (previous == std::numeric_limits<std::uint64_t>::max() || member.epoch.value != previous + 1 || (old != view.end() && (old->second.role != member.role || old->second.address != member.address || old->second.id == member.id))) {
        return Status::conflict("Invalid deployment replacement");
    }

    // role_count 从零统计同角色成员数, 新部署不能挤占另一角色的配额.
    std::size_t role_count = 0;
    for (const auto& [key, current] : view) {
        if (current.role == member.role) {
            ++role_count;
        }
        if (key != principal && (current.id == member.id || current.address == member.address)) {
            return Status::conflict("Member identity or endpoint already used");
        }
    }
    if (old == view.end() && role_count >= maximum_) {
        return Status::capacity("Member capacity reached");
    }
    return {};
}

Result<void> Ledger::register_member(Member candidate, std::string_view request_id, const std::function<void(const Member&, const Members&)>& prepare) {

    // lock 串行化登记校验与数据库提交, 只读当前凭证使用独立不可变视图.
    std::lock_guard lock(mutex_);
    if (!writable_) {
        return Status::transport("Registration database requires recovery");
    }
    if (request_id.size() != 32 || candidate.epoch.value != 0 || candidate.galaxy != galaxy_) {
        return Status::configuration("Invalid registration input");
    }

    // principal 为部署摘要的规范文本键, 与 Members 和 Start 索引保持同一编码.
    const auto principal = candidate.principal.text();
    // view 拥有原子发布的不可变名单, 在新名单提交之前保持旧视图有效.
    const auto view = members_.load();
    // old 指向该部署当前记录, 末尾表示首次登记, 不通过新进程 ID 定位旧代次.
    const auto old = view->find(principal);
    if (const auto start = starts_.find(request_id); start != starts_.end()) {
        if (old == view->end() || start->second.principal != principal || start->second.epoch != old->second.epoch.value || old->second.role != candidate.role || old->second.group != candidate.group || old->second.address != candidate.address) {
            return Status::conflict("Startup request no longer matches current deployment");
        }

        // 身份保持不变, 名单取当前快照, 不缓存旧 RegistrationResponse.
        prepare(old->second, *view);
        return {};
    }
    if (old != view->end() && old->second.epoch.value == std::numeric_limits<std::uint64_t>::max()) {
        return Status::conflict("Member epoch exhausted");
    }
    candidate.epoch.value = old == view->end() ? 1 : old->second.epoch.value + 1;
    if (auto valid = validate(candidate, request_id, *view); !valid) {
        return valid;
    }

    // replacement 复制当前名单后只替换候选成员, 应答准备和磁盘提交失败均不发布它.
    auto replacement = std::make_shared<Members>(*view);
    replacement->insert_or_assign(principal, candidate);
    // 准备节点句柄和完整应答. 此后的提交不再分配 map 节点或序列化应答.
    std::map<std::string, Start, std::less<>> pending;
    pending.emplace(std::string(request_id), Start{principal, candidate.epoch.value});
    // node 持有提前分配的启动记录节点, 持久确认后的 map 插入无需再分配.
    auto node = pending.extract(pending.begin());
    // format=1 的持久正文保持 64 字符摘要, 与线上 32 字节编码解耦, 避免协议优化破坏既有库恢复.
    // record 只保存当前身份正文, 启动请求作为独立索引在同一 SQLite 事务提交.
    auto record = Identity::encode(candidate);
    record.set_principal(principal);
    // payload 在应答准备前序列化, COMMIT 成功之后不再分配或序列化成员正文.
    const auto payload = record.SerializeAsString();
    prepare(candidate, *replacement);
    if (!commit(candidate, request_id, payload)) {
        return Status::transport("Registration durability is uncertain; restart required");
    }
    starts_.insert(std::move(node));
    members_.store(std::move(replacement));
    return {};
}

bool Ledger::current(const Member& member) const {
    return static_cast<bool>(snapshot(member));
}

std::shared_ptr<const Ledger::Members> Ledger::snapshot(const Member& member) const {

    // snapshot 保持当前名单存活, 整次只读身份检查使用同一版本.
    const auto snapshot = members_.load();
    // found 按部署摘要查找, 随后完整比较身份字段以拒绝旧进程凭证.
    const auto found = snapshot->find(member.principal.text());
    return found != snapshot->end() && found->second == member ? snapshot : nullptr;
}

bool Ledger::commit(const Member& member, std::string_view request, std::string_view payload) {

    // 先准备语句和全部绑定, 不在成功 COMMIT 与内存发布之间做可失败的准备工作.
    Database::require(!payload.empty() && payload.size() <= 1024);
    const auto principal = member.principal.text();
    const auto epoch = Database::epoch(member.epoch.value);
    try {
        Database::Query current(database_->connection.value, "INSERT INTO members(principal,body) VALUES(?,?) ON CONFLICT(principal) DO UPDATE SET body=excluded.body");
        current.text(1, principal);
        current.blob(2, payload);
        Database::Query startup(database_->connection.value, "INSERT INTO starts(request,principal,epoch) VALUES(?,?,?)");
        startup.blob(1, request);
        startup.text(2, principal);
        startup.blob(3, {epoch.data(), epoch.size()});

        // 两张表要么一起提交, 要么保留旧状态. 回调应答和容器节点已经在调用前准备好.
        database_->execute("BEGIN IMMEDIATE");
        Database::require(!current.next());
        Database::require(!startup.next());
        database_->execute("COMMIT");
        return true;
    } catch (...) {
        // 即使回滚成功也保守停止登记, 不由同一连接猜测 COMMIT 的持久结果.
        // catch 进入前两条语句均已析构, 不让活动语句阻碍事务回滚或重启恢复.
        writable_ = false;
        static_cast<void>(database_->rollback());
        return false;
    }
}

void Ledger::restore() {

    // 恢复事务覆盖整次读取, 不将不同磁盘时刻的成员与启动记录拼为一个身份视图.
    database_->execute("BEGIN");
    try {
        // recovered 为尚未发布的当前成员; ids/addresses 排除跨部署身份和端点别名.
        auto recovered = std::make_shared<Members>();
        std::set<Id> ids;
        std::set<Endpoint> addresses;
        // counts 分角色累计, Role 已经由 decode_member 完整校验, 下标为 0..3.
        std::array<std::size_t, 4> counts{};
        Database::Query members(database_->connection.value, "SELECT principal,body FROM members ORDER BY principal");
        while (members.next()) {
            // principal/body 仅借用本行, 解析与拥有值复制都在下次 step 前完成.
            const auto principal = members.text(0);
            const auto body = members.blob(1);
            Database::require(principal.size() == 64 && !body.empty() && body.size() <= 1024);
            proto::orbit::v1::Member record;
            Database::require(record.ParseFromArray(body.data(), static_cast<int>(body.size())));
            // format=1 明确保存规范文本摘要, 必须和主键一致; 仅在内存中转成当前线协议, 不改写恢复中的数据库.
            const auto digest = Principal::parse(record.principal());
            Database::require(digest && record.principal() == principal);
            record.set_principal(digest->bytes.data(), digest->bytes.size());
            const auto member = decode_member(record);
            Database::require(member && member->galaxy == galaxy_ && member->principal.text() == principal);
            Database::require(++counts[static_cast<std::size_t>(member->role)] <= maximum_ && ids.insert(member->id).second && addresses.insert(member->address).second);
            Database::require(recovered->emplace(principal, *member).second);
        }

        // 每个部署保留从 1 到当前代次的完整启动证据. 递增前显式检查溢出, 不把八字节 BLOB 转为有符号 INTEGER.
        std::string previous;
        std::uint64_t sequence{};
        std::size_t deployments{};
        Database::Query starts(database_->connection.value, "SELECT request,principal,epoch FROM starts ORDER BY principal,epoch");
        while (starts.next()) {
            // request 为固定 32 字节随机幂等键, principal 对应当前成员, epoch 为该请求持久代次.
            const auto request = starts.blob(0);
            const auto principal = starts.text(1);
            const auto epoch = Database::epoch(starts.blob(2));
            const auto member = recovered->find(principal);
            Database::require(request.size() == 32 && member != recovered->end() && starts_.size() < maximum_starts_);
            if (previous != principal) {
                Database::require(previous.empty() || sequence == recovered->at(previous).epoch.value);
                previous = principal;
                sequence = 0;
                ++deployments;
            }
            Database::require(sequence != std::numeric_limits<std::uint64_t>::max() && epoch == sequence + 1 && epoch <= member->second.epoch.value);
            sequence = epoch;
            Database::require(starts_.emplace(std::string(request), Start{std::string(principal), epoch}).second);
        }
        Database::require(deployments == recovered->size() && (previous.empty() || sequence == recovered->at(previous).epoch.value));
        database_->execute("COMMIT");
        members_.store(std::move(recovered));
    } catch (...) {
        static_cast<void>(database_->rollback());
        throw;
    }
}
} // namespace astra
