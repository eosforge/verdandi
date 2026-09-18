#include "ledger.hpp"
#include "identity.hpp"
#include "pulsar.pb.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <openssl/sha.h>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace astra {
namespace {
// 普通文件 I/O 处理 EINTR 和短写, 不动态分配; 已写一部分仍失败由调用者隔离账本.
bool write_all(int file, std::string_view bytes) {

    while (!bytes.empty()) {
        // count 为本次实际写入字节数, 短写推进剩余视图, EINTR 不丢失原位置.
        const auto count = ::write(file, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return false;
        }
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    return true;
}

// 同步普通文件内容, 被信号打断时重试.
bool sync_file(int file) {

    while (::fdatasync(file) != 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

// 精确读入目标或到达 EOF; 抛错与末尾短读区分, 不把 I/O 错误当作可截断尾部.
std::size_t read_into(int file, std::span<char> bytes) {

    // used 从零累计读入量, 始终不超过 bytes.size(), EOF 可返回不足长度.
    std::size_t used = 0;
    while (used < bytes.size()) {
        // count 为当前读取结果, 负数区分 EINTR 与设施失败, 零只代表 EOF.
        const auto count = ::read(file, bytes.data() + used, bytes.size() - used);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            throw std::runtime_error("Cannot read registration journal");
        }
        if (count == 0) {
            break;
        }
        used += static_cast<std::size_t>(count);
    }
    return used;
}

// 用前一记录摘要绑定追加顺序, 防止静默删除或重排中间记录.
std::array<std::uint8_t, 32> digest(const std::array<std::uint8_t, 32>& previous, std::string_view payload) {

    // context 为本次链式摘要状态, Init 后依次加入前序摘要和当前载荷.
    SHA256_CTX context;
    // hash 拥有固定 32 字节 SHA-256 结果, 不借用输入数据.
    std::array<std::uint8_t, 32> hash{};
    SHA256_Init(&context);
    SHA256_Update(&context, previous.data(), previous.size());
    SHA256_Update(&context, payload.data(), payload.size());
    SHA256_Final(hash.data(), &context);
    return hash;
}

// 填充唯一的 Member 编码, 供日志与应答共用字段语义.
void encode(const Member& member, proto::orbit::v1::Member& output) {

    output.set_galaxy(member.galaxy);
    output.set_id(member.id);
    output.set_principal(member.principal.text());
    output.set_advertise(member.address.text());
    output.set_epoch(member.epoch.value);
    output.set_role(member.role == Member::Role::star ? proto::orbit::v1::ROLE_STAR : proto::orbit::v1::ROLE_PLANET);
    output.set_group(member.group);
}
} // namespace

Ledger::Ledger(const std::filesystem::path& path, std::string galaxy, std::string authority, std::size_t maximum, std::size_t starts) : galaxy_(std::move(galaxy)), header_("ASTRA-PULSAR-JOURNAL-1\n" + galaxy_ + "\n" + authority + "\n"), maximum_(maximum), maximum_starts_(starts) {

    if (!Member::valid_name(galaxy_) || !Principal::parse(authority) || maximum == 0 || maximum > 4096 || starts == 0 || starts > 1'000'000) {
        throw std::runtime_error("Invalid registration journal configuration");
    }

    // parent 为日志所在目录, 无显式父路径时使用当前目录, 后续同步目录项.
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    std::filesystem::create_directories(parent);
    file_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file_ < 0) {
        throw std::runtime_error("Cannot open registration journal");
    }
    try {
        // info 接收实际已打开文件的元数据, 不仅依赖打开之前的路径检查.
        struct stat info{};
        if (::fstat(file_, &info) != 0 || !S_ISREG(info.st_mode) || ::flock(file_, LOCK_EX | LOCK_NB) != 0) {
            throw std::runtime_error("Registration journal must be regular and exclusively owned");
        }
        if (info.st_size == 0 && (!write_all(file_, header_) || !sync_file(file_))) {
            throw std::runtime_error("Cannot initialize registration journal");
        }

        // 确保目录项在服务开始确认写入前持久化, 不替换用户已有的其他文件.
        const auto directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        // synced 记录目录项是否成功持久化, 无论成功与否都先关闭目录描述符.
        const auto synced = directory >= 0 && ::fsync(directory) == 0;
        if (directory >= 0) {
            ::close(directory);
        }
        if (!synced || ::lseek(file_, 0, SEEK_SET) < 0) {
            throw std::runtime_error("Cannot prepare registration journal");
        }
        restore();
    } catch (...) {
        ::close(file_);
        file_ = -1;
        throw;
    }
}

Ledger::~Ledger() {

    if (file_ >= 0) {
        ::close(file_);
    }
}

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

    // lock 串行化登记校验与日志提交, 只读当前凭证使用独立不可变视图.
    std::lock_guard lock(mutex_);
    if (!writable_) {
        return Status::transport("Registration journal requires recovery");
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
    // record 拥有日志记录的协议字段, 序列化或解析失败不得推进已提交的链尾.
    proto::pulsar::v1::RegistrationRecord record;
    encode(candidate, *record.mutable_member());
    record.set_request_id(request_id);
    // payload 为准备完整的日志正文, 仅在应答也准备好之后进入 append.
    const auto payload = record.SerializeAsString();
    prepare(candidate, *replacement);
    if (!append(payload)) {
        return Status::transport("Registration durability is uncertain; restart required");
    }
    starts_.insert(std::move(node));
    members_.store(std::move(replacement));
    return {};
}

bool Ledger::current(const Member& member) const {

    // snapshot 保持当前名单存活, 整次只读身份检查使用同一版本.
    const auto snapshot = members_.load();
    // found 按部署摘要查找, 随后完整比较身份字段以拒绝旧进程凭证.
    const auto found = snapshot->find(member.principal.text());
    return found != snapshot->end() && found->second == member;
}

bool Ledger::append(std::string_view payload) {

    if (payload.empty() || payload.size() > 2048) {
        throw std::runtime_error("Registration record exceeds fixed bounds");
    }

    // hash 将本条载荷绑定到当前 chain_, 只有完整写入并同步后才成为新链尾.
    const auto hash = digest(chain_, payload);
    // bytes 先容纳四字节大端长度, 随后拼接摘要和载荷, 全部准备后才写文件.
    std::string bytes(4, '\0');
    // size 已受 2048 字节上限约束, 可安全编码到四字节长度头.
    const auto size = static_cast<std::uint32_t>(payload.size());
    for (unsigned i = 0; i < 4; ++i) {
        bytes[i] = static_cast<char>((size >> (24 - i * 8)) & 255);
    }
    bytes.append(reinterpret_cast<const char*>(hash.data()), hash.size());
    bytes += payload;
    if (!write_all(file_, bytes) || !sync_file(file_)) {
        writable_ = false;
        return false;
    }
    chain_ = hash;
    return true;
}

void Ledger::restore() {

    // 启动尚未对外发布, 原地构建一次快照, 不为每条历史记录复制整张成员表.
    auto recovered = std::make_shared<Members>();
    // header 为与期望长度一致的读取缓冲, 必须逐字节匹配 Galaxy 和签发权威.
    std::string header(header_.size(), '\0');
    if (read_into(file_, header) != header.size() || header != header_) {
        throw std::runtime_error("Registration journal header mismatch");
    }
    SHA256(reinterpret_cast<const std::uint8_t*>(header.data()), header.size(), chain_.data());
    // boundary 记录最后一条完整记录的文件位置, 只允许截断这个位置之后的不完整尾部.
    auto boundary = ::lseek(file_, 0, SEEK_CUR);
    for (;;) {
        // prefix 接收四字节长度和 32 字节链摘要, 短读只允许作为末尾不完整记录.
        std::array<char, 36> prefix{};
        // count 是头部实际读入量, 零为正常 EOF, 中途设施错误由 read_into 抛出.
        const auto count = read_into(file_, prefix);
        if (count == 0) {
            break;
        }
        if (count != prefix.size()) {
            break;
        }

        // size 从零按大端解码四字节长度, 通过非零和 2048 上限后才分配载荷.
        std::uint32_t size = 0;
        for (unsigned i = 0; i < 4; ++i) {
            size = (size << 8) | static_cast<unsigned char>(prefix[i]);
        }
        if (size == 0 || size > 2048) {
            throw std::runtime_error("Registration journal record length invalid");
        }

        // payload 独立拥有当前记录正文, 必须完整读入,验摘要及验证状态转换.
        std::string payload(size, '\0');
        if (read_into(file_, payload) != payload.size()) {
            break;
        }

        // hash 将本条载荷绑定到当前 chain_, 只有完整写入并同步后才成为新链尾.
        const auto hash = digest(chain_, payload);
        if (!std::equal(hash.begin(), hash.end(), reinterpret_cast<const std::uint8_t*>(prefix.data() + 4))) {
            throw std::runtime_error("Registration journal checksum mismatch");
        }

        // record 拥有日志记录的协议字段, 序列化或解析失败不得推进已提交的链尾.
        proto::pulsar::v1::RegistrationRecord record;
        if (!record.ParseFromString(payload)) {
            throw std::runtime_error("Invalid registration journal encoding");
        }

        // member 为解码并校验后的拥有值, 未通过转换检查不能写入恢复名单.
        auto member = decode_member(record.member());
        if (!member) {
            throw std::runtime_error("Invalid registration journal member");
        }
        if (!validate(*member, record.request_id(), *recovered)) {
            throw std::runtime_error("Invalid registration journal transition");
        }
        starts_.emplace(record.request_id(), Start{member->principal.text(), member->epoch.value});
        recovered->insert_or_assign(member->principal.text(), std::move(*member));
        chain_ = hash;
        boundary = ::lseek(file_, 0, SEEK_CUR);
    }

    // 完整的末条记录可能来自丢失应答, 必须保留; 只丢弃尚未形成完整记录的尾巴.
    if (boundary < 0 || ::ftruncate(file_, boundary) != 0 || !sync_file(file_)) {
        throw std::runtime_error("Cannot recover registration journal tail");
    }
    members_.store(std::move(recovered));
}
} // namespace astra
