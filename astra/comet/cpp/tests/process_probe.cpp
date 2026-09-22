#include <charconv>
#include <chrono>
#include <comet/client.hpp>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {
using namespace std::chrono_literals;

// 只读取测试专有秘密文件, 不将 APISECRET 放入命令行/输出, 文件必须为 1..4096 字节.
std::vector<std::uint8_t> secret(const std::filesystem::path& path) {

    const auto size = std::filesystem::file_size(path);
    if (!std::filesystem::is_regular_file(path) || size == 0 || size > 4096) {
        throw std::runtime_error("Invalid private fixture file");
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> result(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(size)) || input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Cannot read private fixture file");
    }
    return result;
}

// 正常/失败都显式关闭 SDK 并等待本进程在途回收; 只覆盖这个测试创建的 Client.
struct Cleanup {
    comet::Client& client; // 借用 main 栈中的 Client, 其寿命长于本守卫.

    ~Cleanup() {
        client.close();
        if (!client.wait(5s)) {
            std::terminate();
        }
    }
};
} // namespace

// 原生 SDK 进程探针, 不包含 Pulsar/Polaris Stub 或服务端代码; 业务内容只用公开的测试数据.
int main(int count, char** arguments) {

    try {
        if (count != 7) {
            throw std::runtime_error("Expected endpoint, CA file, secret file, sector, spectrum, target version");
        }
        const std::string_view requested(arguments[6]); // 最后期待的完整权威版本, 输入严格十进制正整数.
        std::uint64_t target{};
        const auto parsed = std::from_chars(requested.data(), requested.data() + requested.size(), target);
        if (parsed.ec != std::errc{} || parsed.ptr != requested.data() + requested.size() || target == 0) {
            throw std::runtime_error("Invalid target version");
        }

        std::mutex mutex; // 回调仅更新很小的完成状态, 不在此锁内等待 SDK.
        std::condition_variable changed;
        bool complete{};
        comet::Client::Options options; // 使用真实公共 TLS 和 Comet 登录, 没有节点私钥或管理凭证.
        options.endpoints = {arguments[1]};
        options.ca = arguments[2];
        options.key = "integration";
        options.secret = secret(arguments[3]);
        auto opened = comet::Client::open(std::move(options));
        if (!opened) {
            throw std::runtime_error("Cannot create native Client");
        }
        auto client = std::move(*opened);
        const Cleanup cleanup{client}; // 回调借用的互斥量等在守卫清理完成前继续存活.
        comet::Reader::Options reading;
        reading.changed = [&](comet::Reader::View view) {
            if (view.state() != comet::Reader::State::ready || !view.version()) {
                return;
            }
            const auto value = view.find("key"); // 只输出测试配置的有界十六进制, 不读取内部凭据范围.
            constexpr std::string_view digits = "0123456789abcdef";
            std::string encoded;
            if (value) {
                if (value->size() > 64) {
                    throw std::runtime_error("Unexpected probe payload size");
                }
                for (const auto byte : *value) {
                    encoded.push_back(digits[byte >> 4]);
                    encoded.push_back(digits[byte & 15]);
                }
            }
            std::cout << "{\"event\":\"view\",\"version\":" << *view.version() << ",\"present\":" << (value ? "true" : "false") << ",\"value\":\"" << encoded << "\"}" << std::endl;
            const std::lock_guard lock(mutex);
            complete = *view.version() >= target;
            changed.notify_all();
        };
        auto reader = client.reader({arguments[4], arguments[5]}, {}, std::move(reading));
        if (!reader) {
            throw std::runtime_error("Cannot create native Reader");
        }
        {
            std::unique_lock lock(mutex);
            if (!changed.wait_for(lock, 160s, [&] { return complete; })) {
                throw std::runtime_error("Native Reader did not reach complete target");
            }
        }
        reader->close();
        if (!reader->wait(5s) || client.exceptions() != 0) {
            throw std::runtime_error("Native Reader cleanup or callback failed");
        }
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
