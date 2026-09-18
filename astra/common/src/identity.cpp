// 详细说明: 提供 identity.hpp 中 Identity 类的具体实现。使用 OpenSSL 库进行证书（X.509）和
// 签名（Ed25519）的强校验，使用 yyjson 库安全解析 JSON，使用 gRPC API 生成并包装网络凭证。
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

namespace astra {
namespace {
// 文件大小和实际读取量同时限制, 只读取普通文件, 不把文件内容作为错误文本返回.
// - path (const std::filesystem::path&): 欲读取的凭证文件路径。
// 返回值: 读取成功则返回文件二进制内容的 string 对象，否则返回 unexpected 错误封装。
Result<std::string> read_identity(const std::filesystem::path& path) {
    std::error_code error;
    // 确保这是一个普通文件（排除目录、管道等），并且其大小不超过 16384 字节 (16 KiB)
    if (!std::filesystem::is_regular_file(path, error) || error || std::filesystem::file_size(path, error) > 16384 || error) {
        return Status::identity("Identity file must be regular and at most 16 KiB");
    }
    std::ifstream file(path, std::ios::binary);
    // 多预留一字节给结尾
    std::string bytes(16385, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    // 读取过程中出现严重错误 (bad)，或者没读到文件尾部 (eof) 就失败了 (fail)，或读到了超过限制的字节数。
    if (file.bad() || (!file.eof() && file.fail()) || file.gcount() > 16384) {
        return Status::identity("Cannot read bounded identity file");
    }
    // 将字符串精确截断到实际读取到的字节数。
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    return bytes;
}

// 公布端点必须拥有有效服务端证书, 在向 Supervisor 登记前检查链, 用途, IP SAN 和私钥匹配.
// - ca (const std::string&): CA 证书内容。
// - cert (const std::string&): 服务端公钥证书内容。
// - key (const std::string&): 服务端私钥内容。
// - endpoint (const Endpoint&): 当前应用向外公开暴露的网络地址及端口。
// 返回值: 返回空的成功态，或者包含错误的 unexpected。
Result<void> validate_certificate(const std::string& ca, const std::string& cert, const std::string& key, const Endpoint& endpoint) {
    // 利用 OpenSSL 包装内存中的 PEM 字符串流
    bssl::UniquePtr<BIO> roots(BIO_new_mem_buf(ca.data(), static_cast<int>(ca.size())));
    bssl::UniquePtr<BIO> certificates(BIO_new_mem_buf(cert.data(), static_cast<int>(cert.size())));
    bssl::UniquePtr<BIO> private_bytes(BIO_new_mem_buf(key.data(), static_cast<int>(key.size())));
    bssl::UniquePtr<X509_STORE> store(X509_STORE_new()); // 创建 X509 信任存储区

    // 如果内存分配或初始化失败
    if (!roots || !certificates || !private_bytes || !store) {
        return Status::internal("Cannot allocate certificate validation context");
    }

    // 解析证书链：遍历 certificates 缓冲并依次加入 chain
    std::vector<bssl::UniquePtr<X509>> chain;
    while (auto* item = PEM_read_bio_X509(certificates.get(), nullptr, nullptr, nullptr)) {
        chain.emplace_back(item);
    }
    // 解析私钥
    bssl::UniquePtr<EVP_PKEY> private_key(PEM_read_bio_PrivateKey(private_bytes.get(), nullptr, nullptr, nullptr));

    std::size_t root_count = 0;
    // 将解析出的 CA 根证书加入到可信 store 中
    while (auto* item = PEM_read_bio_X509(roots.get(), nullptr, nullptr, nullptr)) {
        bssl::UniquePtr<X509> root(item);
        if (X509_STORE_add_cert(store.get(), root.get()) != 1) {
            return Status::identity("Invalid trust root");
        }
        ++root_count;
    }

    // 校验：必须至少有一个证书，至少一个根，私钥必须成功解析，且首个证书（叶子证书）和私钥必须匹配。
    if (chain.empty() || root_count == 0 || !private_key || X509_check_private_key(chain[0].get(), private_key.get()) != 1) {
        return Status::identity("Invalid certificate or matching private key");
    }

    // stack 通过 up_ref 保留自己的证书引用; 验证 context 在其后声明, 保证析构顺序正确.
    bssl::UniquePtr<STACK_OF(X509)> intermediates(sk_X509_new_null());
    if (!intermediates) {
        return Status::internal("Cannot allocate certificate chain");
    }

    // 组装中间证书链 (忽略 chain[0] 因为它是叶子证书)
    for (std::size_t i = 1; i < chain.size(); ++i) {
        X509_up_ref(chain[i].get()); // 增加引用计数
        if (!sk_X509_push(intermediates.get(), chain[i].get())) {
            X509_free(chain[i].get()); // 推入失败则手工减引用并退出
            return Status::internal("Cannot retain certificate chain");
        }
    }

    // 建立证书验证上下文
    bssl::UniquePtr<X509_STORE_CTX> context(X509_STORE_CTX_new());
    if (!context || X509_STORE_CTX_init(context.get(), store.get(), chain[0].get(), intermediates.get()) != 1 || // 挂载待验证叶子、store 和中间链
        X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_SSL_SERVER) != 1 ||                               // 限制用途：必须能作为 SSL Server
        X509_VERIFY_PARAM_set1_ip_asc(X509_STORE_CTX_get0_param(context.get()), endpoint.host.c_str()) != 1 ||   // 配置期望的 IP，用于校验 SAN
        X509_verify_cert(context.get()) != 1) {                                                                  // 执行校验逻辑
        return Status::identity("Local certificate does not authorize the advertised endpoint");
    }
    // 清理借用关系后才允许临时链销毁.
    X509_STORE_CTX_cleanup(context.get());
    return {};
}
} // namespace

// decode_member 方法实现
// 详细说明: 尝试把传输层的 proto 类型转换为内存中的强类型 `Member`，并执行范围以及逻辑检查。
Result<Member> decode_member(const proto::orbit::v1::Member& value) {
    const auto& id = value.id();
    auto principal = Principal::parse(value.principal());
    auto address = Endpoint::parse(value.advertise());

    // 各种边界与合法性检查
    if (!Member::valid_id(id) || !principal || !address || address->text() != value.advertise() ||
        (value.role() != proto::orbit::v1::ROLE_STAR && value.role() != proto::orbit::v1::ROLE_PLANET)) {
        return Status::identity("Invalid encoded member");
    }

    // 初始化 Member 数据结构
    Member result{value.galaxy(),
                  id,
                  *principal,
                  *address,
                  MemberEpoch{value.epoch()},
                  value.role() == proto::orbit::v1::ROLE_STAR ? Member::Role::star : Member::Role::planet,
                  value.group()};

    // 再次调用验证函数校验结构整体的合理性
    if (auto valid = result.validate(); !valid) {
        return std::unexpected(valid.error());
    }
    return result;
}

// Identity::load 方法实现
// 详细说明: 读取磁盘特定目录下的几个固定文件，进行验证，并封装返回。
Result<std::shared_ptr<Identity>> Identity::load(const std::filesystem::path& directory, const Endpoint& advertise) {
    auto loaded = load_server(directory, advertise);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    auto result = *loaded;
    auto login = read_identity(directory / "login.json");
    if (!login) {
        return std::unexpected(login.error());
    }
    // 登录 JSON 只接受两个唯一字符串字段, 不把未知内容, 密码或解析输入带入错误文本.
    // 使用 yyjson 库安全且快速地解析 JSON 内容。
    std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> document(yyjson_read(login->data(), login->size(), 0), yyjson_doc_free);
    auto* object = document ? yyjson_doc_get_root(document.get()) : nullptr;
    if (!yyjson_is_obj(object) || yyjson_obj_size(object) != 2) {
        return Status::identity("login.json requires username and password");
    }

    bool seen_user = false, seen_password = false;
    yyjson_obj_iter iterator = yyjson_obj_iter_with(object);
    while (auto* field = yyjson_obj_iter_next(&iterator)) {
        auto* value = yyjson_obj_iter_get_val(field);
        const std::string_view name(yyjson_get_str(field), yyjson_get_len(field));
        // 所有值必须都是字符串类型
        if (!yyjson_is_str(value)) {
            return Status::identity("Login fields must be strings");
        }
        // 解析和匹配 username 字段
        if (name == "username" && !seen_user) {
            result->username_.assign(yyjson_get_str(value), yyjson_get_len(value));
            seen_user = true;
        }
        // 解析和匹配 password 字段
        else if (name == "password" && !seen_password) {
            result->password_.assign(yyjson_get_str(value), yyjson_get_len(value));
            seen_password = true;
        } else {
            return Status::identity("Unknown or duplicate login field");
        }
    }

    // 对读取到的密码和账号名进行有效性以及长度过滤
    if (!Member::valid_name(result->username_) || result->password_.empty() || result->password_.size() > 1024) {
        return Status::identity("Invalid login field bounds");
    }
    return result;
}

Result<std::shared_ptr<Identity>> Identity::load_server(const std::filesystem::path& directory, const Endpoint& advertise) {
    // 先读取固定文件集并验证本地 TLS 身份, 任一材料无效都不发布半初始化的 Identity.
    auto ca = read_identity(directory / "ca.pem");
    auto cert = read_identity(directory / "cert.pem");
    auto key = read_identity(directory / "key.pem");
    auto authority = read_identity(directory / "admission.pub");

    // 文件是否存在、是否正确读取完毕，并且准入公钥长度是否满足要求的 32 字节。
    if (!ca || !cert || !key || !authority || authority->size() != 32) {
        return Status::identity("Missing or invalid identity materials");
    }
    // 执行严格的 OpenSSL 证书链验证
    if (auto valid = validate_certificate(*ca, *cert, *key, advertise); !valid) {
        return std::unexpected(valid.error());
    }

    auto result = std::make_shared<Identity>();
    // 把读取到的准入公钥 copy 到固定数组里
    std::copy(authority->begin(), authority->end(), result->authority_.begin());

    // 配置 gRPC 内置的 TLS 证书 Provider
    result->provider_ = std::make_shared<grpc::experimental::InMemoryCertificateProvider>();
    std::vector<grpc::experimental::IdentityKeyOrSignerCertPair> pair{{*key, *cert}};
    if (!result->provider_->UpdateRoot(*ca).ok() || !result->provider_->UpdateIdentityKeyCertPair(std::move(pair)).ok() ||
        !result->provider_->ValidateCredentials().ok()) {
        return Status::identity("Cannot prepare TLS certificate provider");
    }

    return result;
}

// principal 方法实现
// 详细说明: 计算 SHA256 摘要作为访问指纹。
Principal Identity::principal(std::string_view cluster, std::string_view endpoint) const {
    // 构造拼接：username + '\0' + cluster + '\0' + endpoint
    std::string input = username_;
    input += '\0';
    input += cluster;
    input += '\0';
    input += endpoint;
    Principal result;
    // 将拼接好的字符串作为输入，经过 SHA256，然后直接输出到 result.bytes 内部。
    SHA256(reinterpret_cast<const std::uint8_t*>(input.data()), input.size(), result.bytes.data());
    return result;
}

// verify 方法实现
// 详细说明: 校验并反序列化成员的加入（Admission）签名字节数据。
Result<Member> Identity::verify(std::span<const std::uint8_t> value, std::span<const std::uint8_t> signature) const {
    // 防御过大的包或是非法长度的签名
    if (value.size() > 1024 || signature.size() != 64) {
        return Status::identity("Invalid admission credential bounds");
    }

    // 签名输入包含协议域及末尾 NUL 再拼接原始 value, 与签发方保持字节级一致.
    std::string input(admission_signature_domain);
    input += '\0';
    input.append(reinterpret_cast<const char*>(value.data()), value.size());

    // 调用 OpenSSL 提供的 ED25519_verify 函数，参数为(消息、消息长度、待验签名、验签公钥)。返回值 1 即为成功。
    if (ED25519_verify(reinterpret_cast<const std::uint8_t*>(input.data()), input.size(), signature.data(), authority_.data()) != 1) {
        return Status::identity("Invalid admission signature");
    }

    // 验签通过后，尝试将 value 字节反序列化到 protobuf 消息里。
    proto::orbit::v1::Member member;
    if (!member.ParseFromArray(value.data(), static_cast<int>(value.size()))) {
        return Status::identity("Invalid signed member encoding");
    }

    // 转交给 decode_member 执行数据边界与逻辑检查。
    return decode_member(member);
}

// channel_credentials 方法实现
// 详细说明: 生成严格的客户端 TLS 1.3 凭据选项。
std::shared_ptr<grpc::ChannelCredentials> Identity::channel_credentials() const {
    grpc::experimental::TlsChannelCredentialsOptions options;
    options.set_root_certificate_provider(provider_); // 提供内存中的根 CA
    options.set_verify_server_certs(true);            // 开启服务端证书校验
    options.set_check_call_host(true);                // 校验目标的主机名
    options.set_min_tls_version(TLS1_3);              // 限定最小 TLS 版本 1.3
    options.set_max_tls_version(TLS1_3);              // 限定最大 TLS 版本 1.3
    return grpc::experimental::TlsCredentials(options);
}

// server_credentials 方法实现
// 详细说明: 生成严格的服务端 TLS 1.3 凭据选项。
std::shared_ptr<grpc::ServerCredentials> Identity::server_credentials() const {
    auto result = grpc::experimental::TlsServerCredentialsOptions::Create(provider_);
    if (!result.ok()) {
        return nullptr; // 创建底层选项失败
    }
    result->set_min_tls_version(TLS1_3); // 限定最小 TLS 版本 1.3
    result->set_max_tls_version(TLS1_3); // 限定最大 TLS 版本 1.3
    // 指定不要主动索求客户端的 TLS 证书。
    result->set_cert_request_type(GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE);
    return grpc::experimental::TlsServerCredentials(*result);
}

// username 方法实现
const std::string& Identity::username() const {
    return username_;
}

// password 方法实现
const std::string& Identity::password() const {
    return password_;
}
} // namespace astra
