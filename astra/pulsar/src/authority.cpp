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

    // input 独占材料文件的只读流, 实际读取量另行检查以应对文件大小变化.
    std::ifstream input(path, std::ios::binary);
    // bytes 多读一个字节用于识别超限, 不把额外容量误当合法材料.
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
        // c 为第 i 个十六进制字符, 每两字符合并到 output 的一个字节.
        const auto c = value[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                       : c >= 'A' && c <= 'F'   ? c - 'A' + 10
                                                                                : -1;
        if (digit < 0) {
            return false;
        }
        output[i / 2] = static_cast<std::uint8_t>((static_cast<unsigned>(output[i / 2]) << 4) | static_cast<unsigned>(digit));
    }
    return true;
}
} // namespace

// Authority::load 加载签发者全部材料: 证书、签名密钥、账号集合.
// directory 为身份目录; admission/pulse 为两个监听端点 (证书必须同时授权).
// 返回签发者, 材料非法时返回身份错误.
Result<std::unique_ptr<Authority>> Authority::load(const std::filesystem::path& directory, const Endpoint& admission, const Endpoint& pulse) {

    try {
        // identity 验证登记监听端点的证书授权, 成功对象保留供两个服务共用.
        auto identity = Identity::load_server(directory, admission);
        // pulse_identity 额外验证同一证书也授权对时端点, 校验后无需保留第二份 Provider.
        auto pulse_identity = Identity::load_server(directory, pulse);
        if (!identity || !pulse_identity) {
            return Status::identity("Invalid Pulsar TLS identity");
        }

        // result 独占尚未发布的签发者, 任一材料校验失败都会销毁它.
        auto result = std::unique_ptr<Authority>(new Authority);
        result->identity_ = *identity;
        // 读取 PKCS#8 Ed25519 seed, 并确认派生公钥与节点信任的 admission.pub 完全一致.
        auto signing = material(directory / "admission.key");
        bssl::UniquePtr<BIO> input(BIO_new_mem_buf(signing.data(), static_cast<int>(signing.size())));
        bssl::UniquePtr<EVP_PKEY> key(input ? PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr) : nullptr);
        // seed 为 32 字节私钥种子, pub 为派生公钥; seed 使用后清理, pub 用于匹配验证材料.
        std::array<std::uint8_t, 32> seed{}, pub{};
        // length 初始为种子缓冲容量, EVP 调用写回实际长度, 必须恰好 32 字节.
        auto length = seed.size();
        if (!key || EVP_PKEY_id(key.get()) != EVP_PKEY_ED25519 || EVP_PKEY_get_raw_private_key(key.get(), seed.data(), &length) != 1 || length != seed.size()) {
            return Status::identity("Invalid Ed25519 signing key");
        }
        ED25519_keypair_from_seed(pub.data(), result->private_key_.data(), seed.data());
        SHA256(pub.data(), pub.size(), result->key_id_.bytes.data());
        OPENSSL_cleanse(seed.data(), seed.size());
        OPENSSL_cleanse(signing.data(), signing.size());
        // public_key 为磁盘中的验证公钥字节, 必须与私钥派生出的 pub 完全相等.
        const auto public_key = material(directory / "admission.pub");
        if (public_key.size() != pub.size() || CRYPTO_memcmp(pub.data(), public_key.data(), pub.size()) != 0) {
            return Status::identity("Admission key pair does not match");
        }

        // 一次遍历拒绝重复字段和未知字段, 不允许模糊配置在不同语言中产生不同授权.
        auto bytes = material(directory / "accounts.json");
        std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)> doc(yyjson_read(bytes.data(), bytes.size(), 0), yyjson_doc_free);
        // root 借用账号 JSON 根, doc 为空时也安全返回空, 只接受非空有界数组.
        auto* root = doc ? yyjson_doc_get_root(doc.get()) : nullptr;
        if (!yyjson_is_arr(root) || yyjson_arr_size(root) == 0 || yyjson_arr_size(root) > 64) {
            return Status::identity("Invalid account collection");
        }

        // index/maximum 是 yyjson 数组遍历所需的当前位置与数组长度, 均从零初始化.
        std::size_t index{}, maximum{};
        // entry 由遍历宏借出当前账号对象, 不脱离 doc 生命周期.
        yyjson_val* entry{};
        yyjson_arr_foreach(root, index, maximum, entry) {

            if (!yyjson_is_obj(entry) || yyjson_obj_size(entry) != 4) {
                return Status::identity("Invalid account fields");
            }

            // account 临时保存单个账号的盐,摘要和角色, 完整校验后才移入账号表.
            Account account;
            // seen 初始全零, 逐字段记录唯一出现情况, 不接受遗漏或重复字段.
            std::bitset<4> seen;
            // fields 借用当前账号对象的字段迭代器, 不改变 JSON 文档.
            auto fields = yyjson_obj_iter_with(entry);
            while (auto* field = yyjson_obj_iter_next(&fields)) {
                // name 借用字段完整文本, 只匹配 username/salt/hash/roles 四个固定名字.
                const auto name = text(field);
                // value 借用当前字段值, 对应分支先检查类型和边界再存入 account.
                auto* value = yyjson_obj_iter_get_val(field);
                // slot 初始四表示未知字段, 合法索引为 0..3, 先判边界再访问 seen.
                std::size_t slot = 4;
                if (name == "username") {
                    slot = 0;
                    account.username = text(value);
                    if (!Member::valid_name(account.username)) {
                        return Status::identity("Invalid account name");
                    }
                } else if (name == "salt") {
                    slot = 1;
                    if (!unhex(text(value), account.salt)) {
                        return Status::identity("Invalid account salt");
                    }
                } else if (name == "hash") {
                    slot = 2;
                    if (!unhex(text(value), account.hash)) {
                        return Status::identity("Invalid account hash");
                    }
                } else if (name == "roles") {
                    slot = 3;
                    if (!yyjson_is_arr(value) || yyjson_arr_size(value) == 0 || yyjson_arr_size(value) > 4) {
                        return Status::identity("Invalid account roles");
                    }

                    // role_index/role_maximum 用于遍历一至四个角色, 不参与协议编码.
                    std::size_t role_index{}, role_maximum{};
                    // role 借用当前角色字符串, 未知文本和重复授权位都被拒绝.
                    yyjson_val* role{};
                    yyjson_arr_foreach(value, role_index, role_maximum, role) {

                        // bit 使用内部四种独立角色位, 零表示未知角色, 不直接复用 Protobuf 枚举数值.
                        const auto bit = text(role) == "star" ? 1U : text(role) == "planet"  ? 2U
                                                                 : text(role) == "polaris"   ? 4U
                                                                 : text(role) == "astrolabe" ? 8U
                                                                                             : 0U;
                        if (bit == 0 || (account.roles & bit) != 0) {
                            return Status::identity("Invalid account role");
                        }
                        account.roles |= bit;
                    }
                }
                if (slot == 4 || seen.test(slot)) {
                    return Status::identity("Unknown or duplicate account field");
                }
                seen.set(slot);
            }
            if (!seen.all() || std::ranges::any_of(result->accounts_, [&](const auto& item) { return item.username == account.username; })) {
                return Status::identity("Duplicate or incomplete account");
            }
            result->accounts_.push_back(std::move(account));
        }
        return result;
    } catch (...) {
        return Status::identity("Cannot load Pulsar authority");
    }
}

Authority::~Authority() {
    OPENSSL_cleanse(private_key_.data(), private_key_.size());
}

grpc::Status Authority::authenticate(grpc::ServerContext& context, const proto::orbit::v1::RegistrationRequest& request) const {

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
        // slots 借用已取得的 KDF 配额池, 当前栈对象只归还自己占用的一个许可.
        std::counting_semaphore<4>& slots;

        // 任意返回路径均归还本次 KDF 配额, 不负责取消已经完成的密码计算.
        ~Permit() {
            slots.release();
        }
    } permit{passwords_};

    // found 是只读账号查找结果, 未找到仍执行同样的 KDF 工作.
    const auto found = std::ranges::find(accounts_, request.username(), &Account::username);
    // dummy 提供固定长度的占位盐和摘要, 避免未知账号跳过密码派生步骤.
    const Account dummy;
    // account 借用真实账号或本栈占位账号, 借用覆盖整个认证过程.
    const auto& account = found == accounts_.end() ? dummy : *found;
    // derived 接收本次密码派生值, 比较完成后显式清理, 不保留明文密码副本.
    std::array<std::uint8_t, 32> derived{};
    // ok 为 KDF 是否成功的返回码, 只有 1 表示成功, 不因摘要偶然一致而忽略失败.
    const auto ok = PKCS5_PBKDF2_HMAC(request.password().data(), request.password().size(), account.salt.data(), account.salt.size(), 600000, EVP_sha256(), derived.size(), derived.data());
    // matches 使用固定长度比较结果, 最终仍需确认账号存在和角色被授权.
    const auto matches = CRYPTO_memcmp(derived.data(), account.hash.data(), derived.size()) == 0;
    OPENSSL_cleanse(derived.data(), derived.size());
    if (context.IsCancelled()) {
        return grpc::Status(grpc::StatusCode::CANCELLED, "Registration cancelled");
    }
    if (ok != 1 || !matches || found == accounts_.end()) {
        return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "Invalid login");
    }

    // role 将已请求的角色映射为内部授权位, 未知协议值映射零而不能获准.
    const auto role = request.role() == proto::orbit::v1::ROLE_STAR ? 1U : request.role() == proto::orbit::v1::ROLE_PLANET  ? 2U
                                                                       : request.role() == proto::orbit::v1::ROLE_POLARIS   ? 4U
                                                                       : request.role() == proto::orbit::v1::ROLE_ASTROLABE ? 8U
                                                                                                                            : 0U;
    return (account.roles & role) != 0 ? grpc::Status::OK : grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Role not authorized");
}

// Authority::sign 用 Ed25519 私钥签署登记应答, 签名覆盖应答全部字节.
// member/response 为成员与待签应答; 签名失败直接抛异常, 不返回半签应答.
void Authority::sign(const proto::orbit::v1::Member& member, proto::orbit::v1::RegistrationResponse& response) const {

    if (!member.SerializeToString(response.mutable_admission())) {
        throw std::runtime_error("Admission encoding failed");
    }

    // input 绑定签名域,NUL 分隔符和原始准入正文, 与验证端字节级一致.
    const auto input = std::string(admission_signature_domain) + '\0' + response.admission();
    // signature 拥有 Ed25519 固定 64 字节输出, 成功后移入独占的 response.
    std::string signature(64, '\0');
    if (ED25519_sign(reinterpret_cast<std::uint8_t*>(signature.data()), reinterpret_cast<const std::uint8_t*>(input.data()), input.size(), private_key_.data()) != 1) {
        throw std::runtime_error("Admission signing failed");
    }
    response.set_signature(std::move(signature));
}

const Identity& Authority::identity() const {
    return *identity_;
}

// Authority::key_id 返回签名公钥的 SHA256 十六进制, 供目录核对签发者身份.
// 返回值独立拥有, 调用后长期有效.
std::string Authority::key_id() const {
    return key_id_.text();
}
} // namespace astra
