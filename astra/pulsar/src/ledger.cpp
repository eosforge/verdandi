// 功能: 用一条持久记录同时提交身份和幂等绑定, 恢复不依赖其他节点或 Go 进程.
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
    std::size_t used = 0;
    while (used < bytes.size()) {
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
    SHA256_CTX context;
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
    output.set_role(member.role == Role::star ? proto::orbit::v1::ROLE_STAR : proto::orbit::v1::ROLE_PLANET);
    output.set_group(member.group);
}
} // namespace

MembershipLedger::MembershipLedger(const std::filesystem::path& path, std::string galaxy, std::string authority, std::size_t maximum, std::size_t starts)
    : galaxy_(std::move(galaxy)), header_("ASTRA-PULSAR-JOURNAL-1\n" + galaxy_ + "\n" + authority + "\n"), maximum_(maximum), maximum_starts_(starts) {
    if (!Member::valid_name(galaxy_) || !Principal::parse(authority) || maximum == 0 || maximum > 4096 || starts == 0 || starts > 1'000'000) {
        throw std::runtime_error("Invalid registration journal configuration");
    }
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    std::filesystem::create_directories(parent);
    file_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (file_ < 0) {
        throw std::runtime_error("Cannot open registration journal");
    }
    try {
        struct stat info{};
        if (::fstat(file_, &info) != 0 || !S_ISREG(info.st_mode) || ::flock(file_, LOCK_EX | LOCK_NB) != 0) {
            throw std::runtime_error("Registration journal must be regular and exclusively owned");
        }
        if (info.st_size == 0 && (!write_all(file_, header_) || !sync_file(file_))) {
            throw std::runtime_error("Cannot initialize registration journal");
        }
        // 确保目录项在服务开始确认写入前持久化, 不替换用户已有的其他文件.
        const auto directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
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

MembershipLedger::~MembershipLedger() {
    if (file_ >= 0) {
        ::close(file_);
    }
}

Result<void> MembershipLedger::validate(const Member& member, std::string_view request_id, const Members& view) const {
    if (!member.validate() || member.galaxy != galaxy_ || request_id.size() != 32) {
        return Error::configuration("Invalid registration record");
    }
    if (starts_.contains(request_id)) {
        return Error::conflict("Startup request already committed");
    }
    if (starts_.size() >= maximum_starts_) {
        return Error::capacity("Startup history capacity reached");
    }
    const auto principal = member.principal.text();
    const auto old = view.find(principal);
    const auto previous = old == view.end() ? 0 : old->second.epoch.value;
    if (previous == std::numeric_limits<std::uint64_t>::max() || member.epoch.value != previous + 1 ||
        (old != view.end() && (old->second.role != member.role || old->second.address != member.address || old->second.id == member.id))) {
        return Error::conflict("Invalid deployment replacement");
    }
    std::size_t role_count = 0;
    for (const auto& [key, current] : view) {
        if (current.role == member.role) {
            ++role_count;
        }
        if (key != principal && (current.id == member.id || current.address == member.address)) {
            return Error::conflict("Member identity or endpoint already used");
        }
    }
    if (old == view.end() && role_count >= maximum_) {
        return Error::capacity("Member capacity reached");
    }
    return {};
}

Result<void> MembershipLedger::register_member(Member candidate, std::string_view request_id,
                                               const std::function<void(const Member&, const Members&)>& prepare) {
    std::lock_guard lock(mutex_);
    if (!writable_) {
        return Error::transport("Registration journal requires recovery");
    }
    if (request_id.size() != 32 || candidate.epoch.value != 0 || candidate.galaxy != galaxy_) {
        return Error::configuration("Invalid registration input");
    }
    const auto principal = candidate.principal.text();
    const auto view = members_.load();
    const auto old = view->find(principal);
    if (const auto start = starts_.find(request_id); start != starts_.end()) {
        if (old == view->end() || start->second.principal != principal || start->second.epoch != old->second.epoch.value ||
            old->second.role != candidate.role || old->second.group != candidate.group || old->second.address != candidate.address) {
            return Error::conflict("Startup request no longer matches current deployment");
        }
        // 身份保持不变, 名单取当前快照, 不缓存旧 RegistrationResponse.
        prepare(old->second, *view);
        return {};
    }
    if (old != view->end() && old->second.epoch.value == std::numeric_limits<std::uint64_t>::max()) {
        return Error::conflict("Member epoch exhausted");
    }
    candidate.epoch.value = old == view->end() ? 1 : old->second.epoch.value + 1;
    if (auto valid = validate(candidate, request_id, *view); !valid) {
        return valid;
    }
    auto replacement = std::make_shared<Members>(*view);
    replacement->insert_or_assign(principal, candidate);
    // 准备节点句柄和完整应答. 此后的提交不再分配 map 节点或序列化应答.
    std::map<std::string, Start, std::less<>> pending;
    pending.emplace(std::string(request_id), Start{principal, candidate.epoch.value});
    auto node = pending.extract(pending.begin());
    proto::pulsar::v1::RegistrationRecord record;
    encode(candidate, *record.mutable_member());
    record.set_request_id(request_id);
    const auto payload = record.SerializeAsString();
    prepare(candidate, *replacement);
    if (!append(payload)) {
        return Error::transport("Registration durability is uncertain; restart required");
    }
    starts_.insert(std::move(node));
    members_.store(std::move(replacement));
    return {};
}

bool MembershipLedger::current(const Member& member) const {
    const auto snapshot = members_.load();
    const auto found = snapshot->find(member.principal.text());
    return found != snapshot->end() && found->second == member;
}

bool MembershipLedger::append(std::string_view payload) {
    if (payload.empty() || payload.size() > 2048) {
        throw std::runtime_error("Registration record exceeds fixed bounds");
    }
    const auto hash = digest(chain_, payload);
    std::string bytes(4, '\0');
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

void MembershipLedger::restore() {
    // 启动尚未对外发布, 原地构建一次快照, 不为每条历史记录复制整张成员表.
    auto recovered = std::make_shared<Members>();
    std::string header(header_.size(), '\0');
    if (read_into(file_, header) != header.size() || header != header_) {
        throw std::runtime_error("Registration journal header mismatch");
    }
    SHA256(reinterpret_cast<const std::uint8_t*>(header.data()), header.size(), chain_.data());
    auto boundary = ::lseek(file_, 0, SEEK_CUR);
    for (;;) {
        std::array<char, 36> prefix{};
        const auto count = read_into(file_, prefix);
        if (count == 0) {
            break;
        }
        if (count != prefix.size()) {
            break;
        }
        std::uint32_t size = 0;
        for (unsigned i = 0; i < 4; ++i) {
            size = (size << 8) | static_cast<unsigned char>(prefix[i]);
        }
        if (size == 0 || size > 2048) {
            throw std::runtime_error("Registration journal record length invalid");
        }
        std::string payload(size, '\0');
        if (read_into(file_, payload) != payload.size()) {
            break;
        }
        const auto hash = digest(chain_, payload);
        if (!std::equal(hash.begin(), hash.end(), reinterpret_cast<const std::uint8_t*>(prefix.data() + 4))) {
            throw std::runtime_error("Registration journal checksum mismatch");
        }
        proto::pulsar::v1::RegistrationRecord record;
        if (!record.ParseFromString(payload)) {
            throw std::runtime_error("Invalid registration journal encoding");
        }
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
