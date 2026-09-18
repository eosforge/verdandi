// 功能: 加载有界身份文件, 验证既有账号格式并执行域隔离签名.
#include "authority.hpp"
#include <algorithm>
#include <bitset>
#include <fstream>
#include <openssl/crypto.h>
#include <openssl/curve25519.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <yyjson.h>

namespace astra {
namespace {
// 只读普通文件且限制实际读入量, 错误文本不包含路径或秘密正文.
std::string material(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 16384) {
        throw std::runtime_error("Invalid authority material");
    }
    std::ifstream input(path, std::ios::binary);
    std::string bytes(16385, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || (!input.eof() && input.fail()) || input.gcount() > 16384) {
        throw std::runtime_error("Cannot read authority material");
    }
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    return bytes;
}

// 提取 JSON 字符串视图, 非字符串返回空; 视图不超过 doc 生命周期.
std::string_view text(yyjson_val* value) {
    return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view{};
}

// 固定长度十六进制解析, 允许账号文件沿用 Go 支持的大写编码.
bool unhex(std::string_view value, std::span<std::uint8_t> output) {
    if (value.size() != output.size() * 2) {
        return false;
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = value[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) {
            return false;
        }
        output[i / 2] = static_cast<std::uint8_t>((static_cast<unsigned>(output[i / 2]) << 4) | static_cast<unsigned>(digit));
    }
    return true;
}
} // namespace

Result<std::unique_ptr<PulsarAuthority>> PulsarAuthority::load(const std::filesystem::path& directory, const Endpoint& admission, const Endpoint& pulse) {
    try {
        auto identity = Identity::load_server(directory, admission);
        auto pulse_identity = Identity::load_server(directory, pulse);
        if (!identity || !pulse_identity) {
            return Error::identity("Invalid Pulsar TLS identity");
        }
        auto result = std::unique_ptr<PulsarAuthority>(new PulsarAuthority);
        result->identity_ = *identity;
        // 读取 PKCS#8 Ed25519 seed, 并确认派生公钥与节点信任的 admission.pub 完全一致.
        auto signing = material(directory / "admission.key");
        bssl::UniquePtr<BIO> input(BIO_new_mem_buf(signing.data(), static_cast<int>(signing.size())));
        bssl::UniquePtr<EVP_PKEY> key(input ? PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr) : nullptr);
        std::array<std::uint8_t, 32> seed{}, pub{};
        auto length = seed.size();
        if (!key || EVP_PKEY_id(key.get()) != EVP_PKEY_ED25519 || EVP_PKEY_get_raw_private_key(key.get(), seed.data(), &length) != 1 || length != seed.size()) {
            return Error::identity("Invalid Ed25519 signing key");
        }
        ED25519_keypair_from_seed(pub.data(), result->private_key_.data(), seed.data());
        SHA256(pub.data(), pub.size(), result->key_id_.bytes.data());
        OPENSSL_cleanse(seed.data(), seed.size());
        OPENSSL_cleanse(signing.data(), signing.size());
        const auto public_key = material(directory / "admission.pub");
        if (public_key.size() != pub.size() || CRYPTO_memcmp(pub.data(), public_key.data(), pub.size()) != 0) {
            return Error::identity("Admission key pair does not match");
        }
        // 一次遍历拒绝重复字段和未知字段, 不允许模糊配置在不同语言中产生不同授权.
        auto bytes = material(directory / "accounts.json");
        std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(yyjson_read(bytes.data(), bytes.size(), 0), yyjson_doc_free);
        auto* root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
        if (!yyjson_is_arr(root) || yyjson_arr_size(root) == 0 || yyjson_arr_size(root) > 64) {
            return Error::identity("Invalid account collection");
        }
        std::size_t index{}, maximum{};
        yyjson_val* entry{};
        yyjson_arr_foreach(root, index, maximum, entry) {
            if (!yyjson_is_obj(entry) || yyjson_obj_size(entry) != 4) {
                return Error::identity("Invalid account fields");
            }
            Account account;
            std::bitset<4> seen;
            auto fields = yyjson_obj_iter_with(entry);
            while (auto* field = yyjson_obj_iter_next(&fields)) {
                const auto name = text(field);
                auto* value = yyjson_obj_iter_get_val(field);
                std::size_t slot = 4;
                if (name == "username") {
                    slot = 0;
                    account.username = text(value);
                    if (!Member::valid_name(account.username)) {
                        return Error::identity("Invalid account name");
                    }
                } else if (name == "salt") {
                    slot = 1;
                    if (!unhex(text(value), account.salt)) {
                        return Error::identity("Invalid account salt");
                    }
                } else if (name == "hash") {
                    slot = 2;
                    if (!unhex(text(value), account.hash)) {
                        return Error::identity("Invalid account hash");
                    }
                } else if (name == "roles") {
                    slot = 3;
                    if (!yyjson_is_arr(value) || yyjson_arr_size(value) == 0 || yyjson_arr_size(value) > 2) {
                        return Error::identity("Invalid account roles");
                    }
                    std::size_t role_index{}, role_maximum{};
                    yyjson_val* role{};
                    yyjson_arr_foreach(value, role_index, role_maximum, role) {
                        const auto bit = text(role) == "star" ? 1U : text(role) == "planet" ? 2U : 0U;
                        if (bit == 0 || (account.roles & bit) != 0) {
                            return Error::identity("Invalid account role");
                        }
                        account.roles |= bit;
                    }
                }
                if (slot == 4 || seen.test(slot)) {
                    return Error::identity("Unknown or duplicate account field");
                }
                seen.set(slot);
            }
            if (!seen.all() || std::ranges::any_of(result->accounts_, [&](const auto& item) { return item.username == account.username; })) {
                return Error::identity("Duplicate or incomplete account");
            }
            result->accounts_.push_back(std::move(account));
        }
        return result;
    } catch (...) {
        return Error::identity("Cannot load Pulsar authority");
    }
}

PulsarAuthority::~PulsarAuthority() {
    OPENSSL_cleanse(private_key_.data(), private_key_.size());
}

grpc::Status PulsarAuthority::authenticate(grpc::ServerContext& context, const proto::orbit::v1::RegistrationRequest& request) const {
    if (context.IsCancelled()) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "Registration cancelled");
    }
    if (!Member::valid_name(request.username()) || request.password().empty() || request.password().size() > 1024) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid login");
    }
    if (!passwords_.try_acquire()) {
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "Authentication capacity reached");
    }
    // 作用域退出总会归还 KDF 配额, 不跨登记锁持有.
    struct Permit {
        std::counting_semaphore<4>& slots;
        ~Permit() {
            slots.release();
        }
    } permit{passwords_};
    const auto found = std::ranges::find(accounts_, request.username(), &Account::username);
    const Account dummy;
    const auto& account = found == accounts_.end() ? dummy : *found;
    std::array<std::uint8_t, 32> derived{};
    const auto ok = PKCS5_PBKDF2_HMAC(request.password().data(), request.password().size(), account.salt.data(), account.salt.size(), 600000, EVP_sha256(),
                                      derived.size(), derived.data());
    const auto matches = CRYPTO_memcmp(derived.data(), account.hash.data(), derived.size()) == 0;
    OPENSSL_cleanse(derived.data(), derived.size());
    if (context.IsCancelled()) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "Registration cancelled");
    }
    if (ok != 1 || !matches || found == accounts_.end()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid login");
    }
    const auto role = request.role() == proto::orbit::v1::ROLE_STAR ? 1U : request.role() == proto::orbit::v1::ROLE_PLANET ? 2U : 0U;
    return (account.roles & role) != 0 ? grpc::Status::OK : grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Role not authorized");
}

void PulsarAuthority::sign(const proto::orbit::v1::Member& member, proto::orbit::v1::RegistrationResponse& response) const {
    if (!member.SerializeToString(response.mutable_admission())) {
        throw std::runtime_error("Admission encoding failed");
    }
    const auto input = std::string(admission_signature_domain) + '\0' + response.admission();
    std::string signature(64, '\0');
    if (ED25519_sign(reinterpret_cast<std::uint8_t*>(signature.data()), reinterpret_cast<const std::uint8_t*>(input.data()), input.size(),
                     private_key_.data()) != 1) {
        throw std::runtime_error("Admission signing failed");
    }
    response.set_signature(std::move(signature));
}

const Identity& PulsarAuthority::identity() const {
    return *identity_;
}

std::string PulsarAuthority::key_id() const {
    return key_id_.text();
}
} // namespace astra
