#include <expected>

#if defined(VERDANDI_PROBE_OPENSSL)
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#endif

int main() {
    // std::expected 是 Verdandi 公共 C++23 API 的实际依赖；仅接受编译器宣称
    // C++23 而标准库尚未提供该类型，会让后续正式构建产生误导性错误。
    const std::expected<int, int> language_probe{42};
    if (!language_probe || *language_probe != 42) {
        return 1;
    }

#if defined(VERDANDI_PROBE_OPENSSL)
    // 同时引用 SSL 初始化入口和 Crypto 版本入口，确保两个导入目标都能完成
    // 真实编译与链接，而不只是让 find_package 找到一个残缺目录。
    if (OPENSSL_init_ssl(0, nullptr) != 1 || OpenSSL_version_num() == 0) {
        return 1;
    }
#endif

    return 0;
}
