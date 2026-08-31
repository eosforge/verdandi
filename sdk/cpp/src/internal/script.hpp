#pragma once

#include "internal/driver.hpp"

#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace verdandi::detail {

struct script_call {
    std::vector<std::string> keys;
    std::vector<std::string> arguments;
};

/// 一个按 Redis 实例惰性恢复 NOSCRIPT 的线程安全 Lua SHA 入口。
class script final {
public:
    explicit script(std::string_view source) noexcept;

    /// 使用固定 KEYS/ARGV 顺序执行脚本；mutation 决定传输不确定性映射。
    [[nodiscard]] result<response> run(driver& transport, std::span<const std::string> keys, std::span<const std::string> arguments, bool mutation);

    /// 在一个 Redis Pipeline 中执行同一脚本的多个独立调用；NOSCRIPT 只整体重载并重试一次。
    [[nodiscard]] result<std::vector<response>> run(driver& transport, std::span<const script_call> calls, bool mutation);

private:
    [[nodiscard]] result<std::string> load(driver& transport);

    std::string_view source_;
    std::mutex mutex_;
    std::string sha_;
};

} // namespace verdandi::detail
