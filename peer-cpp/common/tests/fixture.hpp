#pragma once
#include "identity.hpp"

#include <fstream>
#include <openssl/curve25519.h>
#include <openssl/pem.h>
#include <stdexcept>

namespace verdandi::peer::test {
// 仅使用公开测试夹具. 不从环境变量或开发者部署目录读取签发私钥.
inline std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot read public test fixture");
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

inline std::string sign(std::string_view payload, std::string_view domain = "verdandi-admission-v4") {
    const auto pem = read(std::filesystem::path(VERDANDI_FIXTURES) / "supervisor/admission.key");
    bssl::UniquePtr<BIO> input(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    bssl::UniquePtr<EVP_PKEY> key(input ? PEM_read_bio_PrivateKey(input.get(), nullptr, nullptr, nullptr) : nullptr);
    std::array<std::uint8_t, 32> seed{}, pub{};
    std::array<std::uint8_t, 64> secret{};
    auto size = seed.size();
    if (!key || EVP_PKEY_get_raw_private_key(key.get(), seed.data(), &size) != 1 || size != seed.size()) {
        throw std::runtime_error("Invalid public signing fixture");
    }
    ED25519_keypair_from_seed(pub.data(), secret.data(), seed.data());
    const auto bytes = std::string(domain) + '\0' + std::string(payload);
    std::string signature(64, '\0');
    if (ED25519_sign(reinterpret_cast<std::uint8_t*>(signature.data()), reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), secret.data()) !=
        1) {
        throw std::runtime_error("Public fixture signing failed");
    }
    return signature;
}
} // namespace verdandi::peer::test
