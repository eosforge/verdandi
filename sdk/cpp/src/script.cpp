#include "internal/script.hpp"

#include <algorithm>
#include <utility>

namespace verdandi::detail {

namespace {

[[nodiscard]] bool no_script(const error& value) noexcept {
    return value.category() == code::protocol && value.detail().starts_with("NOSCRIPT");
}

[[nodiscard]] command invocation(const std::string_view sha, const std::span<const std::string> keys, const std::span<const std::string> arguments) {
    command output("EVALSHA");
    output.add(sha).add(static_cast<std::uint64_t>(keys.size()));
    for (const auto& key : keys) {
        output.add(key);
    }
    for (const auto& argument : arguments) {
        output.add(argument);
    }
    return output;
}

} // namespace

script::script(const std::string_view source) noexcept : source_(source) {}

result<response> script::run(driver& transport, const std::span<const std::string> keys, const std::span<const std::string> arguments, const bool mutation) {
    std::string sha;
    {
        std::lock_guard lock(mutex_);
        sha = sha_;
    }
    if (sha.empty()) {
        auto loaded = load(transport);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        sha = std::move(*loaded);
    }

    auto result = transport.execute(invocation(sha, keys, arguments), mutation);
    if (result || !no_script(result.error())) {
        return result;
    }

    auto loaded = load(transport);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    return transport.execute(invocation(*loaded, keys, arguments), mutation);
}

result<std::vector<response>> script::run(driver& transport, const std::span<const script_call> calls, const bool mutation) {
    if (calls.empty()) {
        return std::vector<response>{};
    }
    std::string sha;
    {
        std::lock_guard lock(mutex_);
        sha = sha_;
    }
    if (sha.empty()) {
        auto loaded = load(transport);
        if (!loaded) {
            return std::unexpected(loaded.error());
        }
        sha = std::move(*loaded);
    }
    const auto build = [&](const std::string_view digest) {
        std::vector<command> commands;
        commands.reserve(calls.size());
        for (const auto& call : calls) {
            commands.push_back(invocation(digest, call.keys, call.arguments));
        }
        return commands;
    };
    auto commands = build(sha);
    auto result = transport.execute(commands, mutation);
    if (result || !no_script(result.error())) {
        return result;
    }
    auto loaded = load(transport);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    commands = build(*loaded);
    return transport.execute(commands, mutation);
}

result<std::string> script::load(driver& transport) {
    std::lock_guard lock(mutex_);
    command command("SCRIPT");
    command.add("LOAD").add(source_);
    auto loaded = transport.execute(command);
    if (!loaded) {
        return std::unexpected(loaded.error());
    }
    auto value = loaded->text();
    if (!value || value->size() != 40 ||
        !std::ranges::all_of(*value, [](const char character) { return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f'); })) {
        return std::unexpected(error(code::corrupt, "SCRIPT LOAD"));
    }
    sha_.assign(*value);
    return sha_;
}

} // namespace verdandi::detail
