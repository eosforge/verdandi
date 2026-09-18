#include "issuer.hpp"
#include <algorithm>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdexcept>

namespace astra {
namespace {
// 向传输层编码拥有数据的成员值, 名单与签名使用同一组字段.
proto::orbit::v1::Member encode(const Member& member) {
    proto::orbit::v1::Member result;
    result.set_galaxy(member.galaxy);
    result.set_id(member.id);
    result.set_principal(member.principal.text());
    result.set_advertise(member.address.text());
    result.set_epoch(member.epoch.value);
    result.set_role(member.role == Member::Role::star ? proto::orbit::v1::ROLE_STAR : proto::orbit::v1::ROLE_PLANET);
    result.set_group(member.group);
    return result;
}

// 数据面沿用 Go 的候选选择规则: 本组/跨组各优先四个, 不足时补齐, 最多八个.
std::vector<const Member*> candidates(const MembershipLedger::Members& members, const Member& local, std::uint32_t round) {
    std::vector<const Member*> stars;
    for (const auto& [key, member] : members) {
        static_cast<void>(key);
        if (member.role == Member::Role::star) {
            stars.push_back(&member);
        }
    }
    std::ranges::sort(stars, {}, [](const Member* item) -> const Id& { return item->id; });
    if (local.role == Member::Role::star) {
        return stars;
    }
    std::vector<const Member*> nearby, remote;
    for (const auto* member : stars) {
        (member->group == local.group ? nearby : remote).push_back(member);
    }
    const auto principal = local.principal.text();
    std::array<std::uint8_t, 32> hash{};
    SHA256(reinterpret_cast<const std::uint8_t*>(principal.data()), principal.size(), hash.data());
    std::uint64_t seed = 0;
    for (unsigned i = 0; i < 8; ++i) {
        seed = (seed << 8) | hash[i];
    }
    const auto rotate = [&](auto& pool) {
        if (!pool.empty()) {
            // 与 Go 保持相同的确定性散列轮转. 这里只分散候选入口, 不承担密码学抽样或公平抽签.
            const auto offset = static_cast<std::size_t>((seed + static_cast<std::uint64_t>(round) * 4) % pool.size());
            std::ranges::rotate(pool, pool.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    };
    rotate(nearby);
    rotate(remote);
    auto remote_count = std::min(std::size_t{4}, remote.size());
    const auto local_count = std::min(nearby.size(), 8 - remote_count);
    remote_count = std::min(remote.size(), 8 - local_count);
    nearby.resize(local_count);
    remote.resize(remote_count);
    std::ranges::sort(nearby, {}, [](const Member* member) -> const Id& { return member->id; });
    std::ranges::sort(remote, {}, [](const Member* member) -> const Id& { return member->id; });
    nearby.insert(nearby.end(), remote.begin(), remote.end());
    return nearby;
}

// 内部错误映射为稳定的 gRPC 分类, 不把诊断正文发送给对端.
grpc::Status rejected(const Status& error) {
    auto code = grpc::StatusCode::UNAVAILABLE;
    switch (error.code) {
    case Status::Code::configuration:
        code = grpc::StatusCode::INVALID_ARGUMENT;
        break;
    case Status::Code::conflict:
        code = grpc::StatusCode::ABORTED;
        break;
    case Status::Code::capacity:
        code = grpc::StatusCode::RESOURCE_EXHAUSTED;
        break;
    default:
        break;
    }
    return grpc::Status(code, "Registration rejected");
}
} // namespace

Issuer::Issuer(std::string galaxy, std::string pulse_endpoint, const PulsarAuthority& authority, MembershipLedger& ledger)
    : galaxy_(std::move(galaxy)), pulse_endpoint_(std::move(pulse_endpoint)), authority_(authority), ledger_(ledger) {}

grpc::Status Issuer::Register(grpc::ServerContext* context, const proto::orbit::v1::RegistrationRequest* request,
                              proto::orbit::v1::RegistrationResponse* response) {
    try {
        if (request->ByteSizeLong() > 4096 || request->request_id().size() != 32 ||
            (request->role() != proto::orbit::v1::ROLE_STAR && request->role() != proto::orbit::v1::ROLE_PLANET)) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid registration bounds or role");
        }
        if (auto authenticated = authority_.authenticate(*context, *request); !authenticated.ok()) {
            return authenticated;
        }
        const auto address = Endpoint::parse(request->advertise());
        if (request->galaxy() != galaxy_ || !address || address->text() != request->advertise() || !Member::valid_name(request->group())) {
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid registration target");
        }
        // 身份由签发端的密码学随机源产生, 不是由客户端控制的 request_id 派生.
        Principal random, principal;
        if (RAND_bytes(random.bytes.data(), random.bytes.size()) != 1) {
            throw std::runtime_error("Identity entropy unavailable");
        }
        const auto fingerprint = request->username() + '\0' + galaxy_ + '\0' + request->advertise();
        SHA256(reinterpret_cast<const std::uint8_t*>(fingerprint.data()), fingerprint.size(), principal.bytes.data());
        Member candidate{galaxy_,
                         "p_" + random.text(),
                         principal,
                         *address,
                         {},
                         request->role() == proto::orbit::v1::ROLE_STAR ? Member::Role::star : Member::Role::planet,
                         request->group()};
        // 应答在持久提交之前完整准备. 提交后 Swap 不分配, 发送失败的客户端安全重试原请求.
        proto::orbit::v1::RegistrationResponse prepared;
        const auto committed = ledger_.register_member(std::move(candidate), request->request_id(), [&](const Member& local, const auto& members) {
            if (context->IsCancelled()) {
                throw std::runtime_error("Registration cancelled");
            }
            authority_.sign(encode(local), prepared);
            prepared.set_pulse_endpoint(pulse_endpoint_);
            for (const auto* member : candidates(members, local, request->candidate_round())) {
                *prepared.add_members() = encode(*member);
            }
            if (prepared.ByteSizeLong() > 2 * 1024 * 1024) {
                throw std::runtime_error("Registration response capacity exceeded");
            }
        });
        if (!committed) {
            return rejected(committed.error());
        }
        response->Swap(&prepared);
        return grpc::Status::OK;
    } catch (const std::bad_alloc&) {
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Registration resources unavailable");
    } catch (...) {
        return grpc::Status(context->IsCancelled() ? grpc::StatusCode::CANCELLED : grpc::StatusCode::INTERNAL, "Registration unavailable");
    }
}
} // namespace astra
