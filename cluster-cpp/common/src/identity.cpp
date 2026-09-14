// 功能: 加载并校验本地身份材料, 计算部署摘要, 验证准入签名并构造 TLS 凭证.
#include "identity.hpp"

#include <fstream>
#include <grpcpp/security/tls_credentials_options.h>
#include <openssl/curve25519.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
#include <yyjson.h>

namespace verdandi::cluster {
namespace {
// 文件大小和实际读取量同时限制, 只读取普通文件, 不把文件内容作为错误文本返回.
Result<std::string> read_identity(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error || std::filesystem::file_size(path, error) > 16384 || error) {
        return std::unexpected(Error{ErrorCode::identity, "Identity file must be regular and at most 16 KiB"});
    }
    std::ifstream file(path, std::ios::binary);
    std::string bytes(16385, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (file.bad() || (!file.eof() && file.fail()) || file.gcount() > 16384) {
        return std::unexpected(Error{ErrorCode::identity, "Cannot read bounded identity file"});
    }
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    return bytes;
}

// 公布端点必须拥有有效服务端证书, 在向 Supervisor 登记前检查链, 用途, IP SAN 和私钥匹配.
Result<void> validate_certificate(const std::string& ca, const std::string& cert, const std::string& key, const Endpoint& endpoint) {
    bssl::UniquePtr<BIO> roots(BIO_new_mem_buf(ca.data(), static_cast<int>(ca.size())));
    bssl::UniquePtr<BIO> certificates(BIO_new_mem_buf(cert.data(), static_cast<int>(cert.size())));
    bssl::UniquePtr<BIO> private_bytes(BIO_new_mem_buf(key.data(), static_cast<int>(key.size())));
    bssl::UniquePtr<X509_STORE> store(X509_STORE_new());
    if (!roots || !certificates || !private_bytes || !store) {
        return std::unexpected(Error{ErrorCode::internal, "Cannot allocate certificate validation context"});
    }
    std::vector<bssl::UniquePtr<X509>> chain;
    while (auto* item = PEM_read_bio_X509(certificates.get(), nullptr, nullptr, nullptr)) {
        chain.emplace_back(item);
    }
    bssl::UniquePtr<EVP_PKEY> private_key(PEM_read_bio_PrivateKey(private_bytes.get(), nullptr, nullptr, nullptr));
    std::size_t root_count = 0;
    while (auto* item = PEM_read_bio_X509(roots.get(), nullptr, nullptr, nullptr)) {
        bssl::UniquePtr<X509> root(item);
        if (X509_STORE_add_cert(store.get(), root.get()) != 1) {
            return std::unexpected(Error{ErrorCode::identity, "Invalid trust root"});
        }
        ++root_count;
    }
    if (chain.empty() || root_count == 0 || !private_key || X509_check_private_key(chain[0].get(), private_key.get()) != 1) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid certificate or matching private key"});
    }
    // stack 通过 up_ref 保留自己的证书引用; 验证 context 在其后声明, 保证析构顺序正确.
    bssl::UniquePtr<STACK_OF(X509)> intermediates(sk_X509_new_null());
    if (!intermediates) {
        return std::unexpected(Error{ErrorCode::internal, "Cannot allocate certificate chain"});
    }
    for (std::size_t i = 1; i < chain.size(); ++i) {
        X509_up_ref(chain[i].get());
        if (!sk_X509_push(intermediates.get(), chain[i].get())) {
            X509_free(chain[i].get());
            return std::unexpected(Error{ErrorCode::internal, "Cannot retain certificate chain"});
        }
    }
    bssl::UniquePtr<X509_STORE_CTX> context(X509_STORE_CTX_new());
    if (!context || X509_STORE_CTX_init(context.get(), store.get(), chain[0].get(), intermediates.get()) != 1 ||
        X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_SSL_SERVER) != 1 ||
        X509_VERIFY_PARAM_set1_ip_asc(X509_STORE_CTX_get0_param(context.get()), endpoint.host.c_str()) != 1 || X509_verify_cert(context.get()) != 1) {
        return std::unexpected(Error{ErrorCode::identity, "Local certificate does not authorize the advertised endpoint"});
    }
    // 清理借用关系后才允许临时链销毁.
    X509_STORE_CTX_cleanup(context.get());
    return {};
}
} // namespace

Result<Member> decode_member(const wire::RegistrationResponse::Member& value) {
    const auto& id = value.id();
    auto principal = Principal::parse(value.principal());
    auto address = Endpoint::parse(value.advertise());
    if (!valid_id(id) || !principal || !address || address->text() != value.advertise() ||
        (value.role() != wire::ROLE_STAR && value.role() != wire::ROLE_PLANET)) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid encoded member"});
    }
    Member result{value.cluster_id(), id, *principal, *address, MemberEpoch{value.epoch()}, value.role() == wire::ROLE_STAR ? Role::star : Role::planet,
                  value.group()};
    if (auto valid = validate_member(result); !valid) {
        return std::unexpected(valid.error());
    }
    return result;
}

Result<std::shared_ptr<Identity>> Identity::load(const std::filesystem::path& directory, const Endpoint& advertise) {
    // 先读取固定文件集并验证本地 TLS 身份, 任一材料无效都不发布半初始化的 Identity.
    auto ca = read_identity(directory / "ca.pem");
    auto cert = read_identity(directory / "cert.pem");
    auto key = read_identity(directory / "key.pem");
    auto authority = read_identity(directory / "admission.pub");
    auto login = read_identity(directory / "login.json");
    if (!ca || !cert || !key || !authority || !login || authority->size() != 32) {
        return std::unexpected(Error{ErrorCode::identity, "Missing or invalid identity materials"});
    }
    if (auto valid = validate_certificate(*ca, *cert, *key, advertise); !valid) {
        return std::unexpected(valid.error());
    }
    // 登录 JSON 只接受两个唯一字符串字段, 不把未知内容, 密码或解析输入带入错误文本.
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(yyjson_read(login->data(), login->size(), 0), yyjson_doc_free);
    auto* object = document ? yyjson_doc_get_root(document.get()) : nullptr;
    if (!yyjson_is_obj(object) || yyjson_obj_size(object) != 2) {
        return std::unexpected(Error{ErrorCode::identity, "login.json requires username and password"});
    }
    auto result = std::make_shared<Identity>();
    bool seen_user = false, seen_password = false;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    while (auto* field = yyjson_obj_iter_next(&iterator)) {
        auto* value = yyjson_obj_iter_get_val(field);
        const std::string_view name(yyjson_get_str(field), yyjson_get_len(field));
        if (!yyjson_is_str(value)) {
            return std::unexpected(Error{ErrorCode::identity, "Login fields must be strings"});
        }
        if (name == "username" && !seen_user) {
            result->username_.assign(yyjson_get_str(value), yyjson_get_len(value));
            seen_user = true;
        } else if (name == "password" && !seen_password) {
            result->password_.assign(yyjson_get_str(value), yyjson_get_len(value));
            seen_password = true;
        } else {
            return std::unexpected(Error{ErrorCode::identity, "Unknown or duplicate login field"});
        }
    }
    if (!valid_name(result->username_) || result->password_.empty() || result->password_.size() > 1024) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid login field bounds"});
    }
    std::copy(authority->begin(), authority->end(), result->authority_.begin());
    result->provider_ = std::make_shared<grpc::experimental::InMemoryCertificateProvider>();
    std::vector<grpc::experimental::IdentityKeyOrSignerCertPair> pair{{*key, *cert}};
    if (!result->provider_->UpdateRoot(*ca).ok() || !result->provider_->UpdateIdentityKeyCertPair(std::move(pair)).ok() ||
        !result->provider_->ValidateCredentials().ok()) {
        return std::unexpected(Error{ErrorCode::identity, "Cannot prepare TLS certificate provider"});
    }
    return result;
}

Principal Identity::principal(std::string_view cluster, std::string_view endpoint) const {
    std::string input = username_;
    input += '\0';
    input += cluster;
    input += '\0';
    input += endpoint;
    Principal result;
    SHA256(reinterpret_cast<const std::uint8_t*>(input.data()), input.size(), result.bytes.data());
    return result;
}

Result<Member> Identity::verify(std::span<const std::uint8_t> payload, std::span<const std::uint8_t> signature) const {
    if (payload.size() > 1024 || signature.size() != 64) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid admission credential bounds"});
    }
    // 签名输入包含协议域及末尾 NUL 再拼接原始 payload, 与签发方保持字节级一致.
    std::string input("verdandi-admission-v6");
    input += '\0';
    input.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    if (ED25519_verify(reinterpret_cast<const std::uint8_t*>(input.data()), input.size(), signature.data(), authority_.data()) != 1) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid admission signature"});
    }
    wire::RegistrationResponse::Member member;
    if (!member.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return std::unexpected(Error{ErrorCode::identity, "Invalid signed member encoding"});
    }
    return decode_member(member);
}

std::shared_ptr<grpc::ChannelCredentials> Identity::channel_credentials() const {
    grpc::experimental::TlsChannelCredentialsOptions options;
    options.set_root_certificate_provider(provider_);
    options.set_verify_server_certs(true);
    options.set_check_call_host(true);
    options.set_min_tls_version(TLS1_3);
    options.set_max_tls_version(TLS1_3);
    return grpc::experimental::TlsCredentials(options);
}

std::shared_ptr<grpc::ServerCredentials> Identity::server_credentials() const {
    auto result = grpc::experimental::TlsServerCredentialsOptions::Create(provider_);
    if (!result.ok()) {
        return nullptr;
    }
    result->set_min_tls_version(TLS1_3);
    result->set_max_tls_version(TLS1_3);
    result->set_cert_request_type(GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
    return grpc::experimental::TlsServerCredentials(*result);
}

const std::string& Identity::username() const {
    return username_;
}
const std::string& Identity::password() const {
    return password_;
}
} // namespace verdandi::cluster
