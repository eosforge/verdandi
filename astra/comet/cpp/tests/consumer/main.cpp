#include <chrono>
#include <comet/client.hpp>
#include <filesystem>
#include <string>
#include <type_traits>

// 独立消费工程检查公开类型、链接与 TLS 配置. endpoint 为夹具占有但不监听的回环端口, invalid 为缺失 CA 路径.
int main(int count, char** arguments) {

    static_assert(std::is_copy_constructible_v<comet::Client>);
    static_assert(!std::is_copy_constructible_v<comet::Reader>);
    static_assert(std::is_copy_constructible_v<comet::Observer::Item>);
    static_assert(std::is_same_v<decltype(std::declval<comet::Beacon&>().update(comet::Value{})), comet::Result<comet::Beacon::Receipt>>);

    // 空句柄验证完整新接口的公开声明与实际链接, 不为包消费检查另外创建网络业务.
    comet::Beacon beacon;
    comet::Reader reader;
    comet::Subscriber subscriber;
    comet::Observer observer;
    if (beacon.tick(std::chrono::milliseconds(1), [] { return comet::Value{}; }) || beacon.changed([](comet::Beacon::State) {}) || reader.watch([](comet::Reader::Map) {}) || subscriber.watch([](std::string, std::optional<comet::Value>) {}) || reader.changed([](comet::Reader::View) {}) || subscriber.changed([](comet::Subscriber::View) {})) {
        return 1;
    }
    const auto selected = observer.one([](const comet::Observer::Pool& pool) -> std::optional<comet::Observer::Item> {
        pool.each([](comet::Observer::Item item) { static_cast<void>(item.record()); });
        return pool.find("missing");
    });
    if (selected || selected.error().code != comet::Error::Code::closed) {
        return 1;
    }
    beacon.destroy();
    reader.stop();
    subscriber.stop();
    observer.stop();
    if (count != 3) {
        return 1;
    }
    const auto invalid = comet::Client::open({}); // 空端点必须明确拒绝, 同时解析真实 SDK 入口与私有依赖.
    if (invalid || invalid.error().code != comet::Error::Code::input) {
        return 1;
    }

    comet::Client::Options options; // 无认证, 保留默认 TLS; 包测试通过 CMake 配置将公开 CA 编入 SDK.
    options.auth = false;
    options.endpoints = {arguments[1]};
    auto encrypted = comet::Client::open(options); // 到达实际根证书提供器校验, 不停在空参数拒绝之前.
    if (!encrypted) {
        return 1;
    }
    encrypted->close();
    if (!encrypted->wait(std::chrono::seconds(5))) {
        return 1;
    }

    options.ca = std::filesystem::path(arguments[2]); // 显式外部路径必须优先, 不能读失败后默默使用嵌入 CA.
    const auto missing = comet::Client::open(options);
    if (missing || missing.error().code != comet::Error::Code::input) {
        return 1;
    }
    options.ca.clear();
    options.tls = false; // 明确关闭公共 TLS 仍允许本地创建和完整清理, 不影响内部链接.
    auto plaintext = comet::Client::open(std::move(options));
    if (!plaintext) {
        return 1;
    }
    plaintext->close();
    return plaintext->wait(std::chrono::seconds(5)) ? 0 : 1;
}
