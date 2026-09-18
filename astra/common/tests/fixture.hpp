#pragma once
#include "identity.hpp"

#include <fstream>
#include <openssl/curve25519.h>
#include <openssl/pem.h>
#include <stdexcept>

namespace astra::test {
// 仅使用公开测试夹具. 不从环境变量或开发者部署目录读取签发私钥.
inline std::string read(const std::filesystem::path& path) {

    // input 只读 path 指定的公开夹具, 读取失败抛异常, 不自动生成或下载材料.
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read public test fixture");
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// 用公开测试私钥签署 value 原始字节, domain 默认准入域; 返回拥有的 64 字节签名.
inline std::string sign(std::string_view value, std::string_view domain = admission_signature_domain) {

    // pem 持有仓库公开 Ed25519 私钥文本, 不读取部署秘密或环境变量.
    const auto pem = read(std::filesystem::path(ASTRA_FIXTURES) / "supervisor/admission.key");
    // input 借用 pem 建立只读解析流, 其寿命严格短于 pem.
    bssl::UniquePtr<BIO> input(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    // key 独占解析后的密钥对象, 无效格式由后续检查拒绝.
    bssl::UniquePtr<EVP_PKEY> key(input ? PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr) : nullptr);
    // seed 接收 32 字节私钥种子, pub 接收派生公钥, 两者仅属于公开测试材料.
    std::array<std::uint8_t, 32> seed{}, pub{};
    // secret 为 BoringSSL 签名所需的 seed+公钥布局, 只在本次调用内使用.
    std::array<std::uint8_t, 64> secret{};
    // size 初始为 seed 容量, 解析后必须仍为 32, 不接受截短私钥.
    auto size = seed.size();
    if (!key || EVP_PKEY_get_raw_private_key(key.get(), seed.data(), &size) != 1 || size != seed.size()) {
        throw std::runtime_error("Invalid public signing fixture");
    }
    ED25519_keypair_from_seed(pub.data(), secret.data(), seed.data());
    // bytes 按域,NUL,原始正文拼接, 与生产签发和验证端保持同一格式.
    const auto bytes = std::string(domain) + '\0' + std::string(value);
    // signature 预留 Ed25519 固定输出长度, 成功后按值返回.
    std::string signature(64, '\0');
    if (ED25519_sign(reinterpret_cast<std::uint8_t*>(signature.data()), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), secret.data()) != 1) {
        throw std::runtime_error("Public fixture signing failed");
    }
    return signature;
}
} // namespace astra::test
