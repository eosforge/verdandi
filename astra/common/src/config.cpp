// 功能: 依据 C++26 字段反射解析 CLI, 校验参数并生成帮助和拥有数据的运行配置.
#include <astra/config.hpp>

#include "options.hpp"
#include <bitset>
#include <charconv>
#include <meta>
#include <type_traits>

namespace astra {
namespace {
// 检查数值默认值是否落在 option 的包含式范围内, 供编译期元数据校验使用, 不修改输入.
constexpr bool valid_default(std::uint64_t value, const detail::Option& option) {
    return value >= option.minimum && value <= option.maximum;
}
// 只检查字符串默认值的通用 4096 字节上限; 必填和字段业务格式在实际 CLI 解析时校验.
constexpr bool valid_default(const std::string& value, const detail::Option&) {
    return value.size() <= 4096;
}
// 反射查询在编译期完成, 每次解析只使用固定 seen 位集, 不动态建立字段注册表.
// clang-format off
constexpr auto fields = std::define_static_array(std::meta::nonstatic_data_members_of(^^detail::Options, std::meta::access_context::current()));
// 提取 field 唯一的 Option 注解, 数量或类型不匹配会阻止常量求值通过, 不生成运行时注册项.
consteval detail::Option descriptor(std::meta::info field) {
    auto tags = std::meta::annotations_of(field);
    if (tags.size() != 1) { throw "Each CLI field requires exactly one annotation"; }
    return std::meta::extract<detail::Option>(tags[0]);
}
// 检查名称唯一, 帮助非空, 范围有序和默认值边界; 返回 false 由 static_assert 阻止构建.
consteval bool valid_descriptors() {
    for (std::size_t i = 0; i < fields.size(); ++i) {
        auto tag = descriptor(fields[i]);
        if (tag.name[0] == '\0' || tag.description[0] == '\0' || tag.minimum > tag.maximum) { return false; }
        for (std::size_t j = 0; j < i; ++j) {
            if (std::string_view(descriptor(fields[j]).name) == tag.name) { return false; }
        }
    }
    const detail::Options defaults;
    bool valid = true;
    template for (constexpr auto field : fields) {
        valid = valid && valid_default(defaults.[:field:], descriptor(field));
    }
    return valid;
}
static_assert(valid_descriptors());
// clang-format on

// 只接受完整十进制数字, 范围取自注解; 不使用会忽略尾部文本的转换函数.
Result<void> assign(std::uint64_t& output, std::string_view text, const detail::Option& option) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value < option.minimum || value > option.maximum) {
        return std::unexpected(Error{ErrorCode::configuration, "Invalid numeric option: --" + std::string(option.name)});
    }
    output = value;
    return {};
}

// 字符串先转为拥有的值, 地址/名称关系在解析完全部字段后检查.
Result<void> assign(std::string& output, std::string_view text, const detail::Option&) {
    output = text;
    return {};
}

// 帮助使用普通重载处理字段值, 反射仅负责枚举, 不扩展异构模板分支.
std::string default_help(const std::string& value, const detail::Option&) {
    return value.empty() ? std::string{} : "; default=" + value;
}
// 使用数值 value 和注解 option 返回包含式范围与默认值文本, 不参与运行时参数赋值.
std::string default_help(std::uint64_t value, const detail::Option& option) {
    return " [" + std::to_string(option.minimum) + ", " + std::to_string(option.maximum) + "]; default=" + std::to_string(value);
}
} // namespace

Result<Config> parse_options(std::span<const std::string_view> arguments, Role role) {
    detail::Options options;
    std::bitset<fields.size()> seen;
    // 先统一 --name=value 与 --name value, 再分派字段; 借用的参数只在本次调用中有效.
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto argument = arguments[i];
        if (!argument.starts_with("--")) {
            return std::unexpected(Error{ErrorCode::configuration, "Expected a named option"});
        }
        const auto separator = argument.find('=');
        const auto name = argument.substr(2, separator == std::string_view::npos ? separator : separator - 2);
        if (name == "worker-threads") {
            return std::unexpected(Error{ErrorCode::configuration, "--worker-threads is unsupported: gRPC owns I/O workers"});
        }
        std::string_view value;
        if (separator != std::string_view::npos) {
            value = argument.substr(separator + 1);
        } else if (i + 1 < arguments.size()) {
            value = arguments[++i];
        }
        if (value.empty() || value.starts_with("--") || value.size() > 4096) {
            return std::unexpected(Error{ErrorCode::configuration, "Missing or oversized option value"});
        }
        bool found = false;
        std::size_t index = 0;
        // 仅此字段分派使用 splice; 把异构访问展开为普通比较和赋值, 不引入虚函数或成员指针表.
        // clang-format off
        template for (constexpr auto field : fields) {
            constexpr auto option = descriptor(field);
            if (name == option.name) {
                if (seen.test(index)) { return std::unexpected(Error{ErrorCode::configuration, "Duplicate option"}); }
                auto result = assign(options.[:field:], value, option);
                if (!result) { return std::unexpected(result.error()); }
                seen.set(index);
                found = true;
            }
            ++index;
        }
        // clang-format on
        if (!found) {
            return std::unexpected(Error{ErrorCode::configuration, "Unknown option"});
        }
    }
    std::size_t index = 0;
    // 必填字段检查来自相同注解, 新增字段无需修改第二份条件列表.
    // clang-format off
    template for (constexpr auto field : fields) {
        if (descriptor(field).required && !seen.test(index)) { return std::unexpected(Error{ErrorCode::configuration, "Missing required option"}); }
        ++index;
    }
    // clang-format on

    // 跨字段约束保持普通代码, 不把地址关系塞入元编程规则引擎.
    auto listen = Endpoint::parse(options.listen, true);
    auto supervisor = supervisor_address(options.supervisor);
    if (!listen || !supervisor || !valid_name(options.cluster) || !valid_name(options.group) || options.identity.empty()) {
        return std::unexpected(Error{ErrorCode::configuration, "Invalid endpoint, name or identity path"});
    }
    auto advertise = options.advertise.empty() ? listen : Endpoint::parse(options.advertise);
    if (!advertise || (listen->wildcard && options.advertise.empty())) {
        return std::unexpected(Error{ErrorCode::configuration, "A concrete advertise endpoint is required"});
    }
    // 全部输入和跨字段约束通过后才形成运行配置, 派生入站预算并将 CLI 秒数统一转换为毫秒.
    Config result;
    result.role = role;
    result.cluster = std::move(options.cluster);
    result.group = std::move(options.group);
    result.listen = std::move(*listen);
    result.advertise = std::move(*advertise);
    result.supervisor = std::move(*supervisor);
    result.identity = std::move(options.identity);
    result.max_members = options.maximum;
    result.max_inbound = options.maximum * 2;
    result.heartbeat_interval = Milliseconds(options.heartbeat);
    result.pong_timeout = Milliseconds(options.pong);
    result.shutdown_timeout = std::chrono::seconds(options.shutdown);
    result.status_interval = std::chrono::seconds(options.status);
    return result;
}

std::string option_help(Role role) {
    std::string result = "Usage: " + std::string(role == Role::star ? "star" : "planet") + " --listen=IP:PORT --super=HOST:PORT --cluster=ID [options]\n";
    const detail::Options defaults;
    // 默认值直接读取同一结构, 修改初始化器会同步改变实例和帮助.
    // clang-format off
    template for (constexpr auto field : fields) {
        constexpr auto option = descriptor(field);
        result += "  --" + std::string(option.name) + "  " + std::string(option.description);
        result += default_help(defaults.[:field:], option);
        result += option.required ? " (required)\n" : "\n";
    }
    // clang-format on
    return result + "  --help / --version\nTLS and Supervisor account login are required. Stop with SIGINT or SIGTERM.\n";
}
} // namespace astra
