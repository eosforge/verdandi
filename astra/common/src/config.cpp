// 详细说明: 这是一个前沿的 C++26 代码文件, 利用新标准中的 `std::meta` 编译期反射机制, 通过探测
// `detail::Options` 内部成员与其上挂载的注解, 实现一套无需在运行时使用繁杂的映射表/注册逻辑
// 的零开销或极低开销的命令行参数(CLI)解析器.最终将各个参数合法化并汇聚成为统一且所有权清晰的 `Config` 配置.
#include <astra/config.hpp>

#include "options.hpp"
#include <bitset>
#include <charconv>
#include <meta>
#include <type_traits>

namespace astra {
namespace {
// 检查数值默认值是否落在 option 的包含式范围内, 供编译期元数据校验使用, 不修改输入.
// - value (std::uint64_t): 结构体定义中预设的默认数字值.
// - option (const detail::Option&): 反射抓取到的属性配置.
// 返回值: 默认值是否有效(即是否大于等于最小值, 小于等于最大值).
constexpr bool valid_default(std::uint64_t value, const detail::Option& option) {
    return value >= option.minimum && value <= option.maximum;
}

// 检查字符串默认值是否有效.
// 只检查字符串默认值的通用 4096 字节上限; 必填和字段业务格式在实际 CLI 解析时校验.
// - value (const std::string&): 结构体定义中预设的默认文本.
// - (const detail::Option&): 当前参数未用到注解内容.
constexpr bool valid_default(const std::string& value, const detail::Option&) {
    return value.size() <= 4096;
}

// 反射查询在编译期完成, 每次解析只使用固定 seen 位集, 不动态建立字段注册表.
// clang-format off
// 详细说明: 使用 C++26 (P2996) `^` 操作符获取 `detail::Options` 的类型的元信息.
// `std::meta::nonstatic_data_members_of` 可以抽取出其所有非静态成员变量.最后用 `std::define_static_array` 变为 constexpr 数组.
constexpr auto fields = std::define_static_array(std::meta::nonstatic_data_members_of(^^detail::Options, std::meta::access_context::current()));

// 提取 field 唯一的 Option 注解, 数量或类型不匹配会阻止常量求值通过, 不生成运行时注册项.
// - field (std::meta::info): 一个成员变量的元信息结构.
// 返回值: 返回该字段上挂载的属于 detail::Option 的注解实例.
consteval detail::Option descriptor(std::meta::info field) {

    // tags 为 field 的编译期注解集合, 必须恰好包含一个 Option.
    auto tags = std::meta::annotations_of(field);
    if (tags.size() != 1) { throw "Each CLI field requires exactly one annotation"; } // 只允许严格打一个注解
    return std::meta::extract<detail::Option>(tags[0]);
}

// 检查名称唯一, 帮助非空, 范围有序和默认值边界; 返回 false 由 static_assert 阻止构建.
// 详细说明: 这是一个在编译期运行的校验函数.如果注解写得不对, 例如有两个同样的 CLI 选项名, 或者范围配反了, 代码根本编译不过.
consteval bool valid_descriptors() {

    for (std::size_t i = 0; i < fields.size(); ++i) {
        // tag 拥有当前字段的编译期选项定义, 用于检查名称,说明与边界.
        auto tag = descriptor(fields[i]);
        // 名字不能为空, 描述不能为空, 最小值不能大于最大值.
        if (tag.name[0] == '\0' || tag.description[0] == '\0' || tag.minimum > tag.maximum) { return false; }

        // 与排在前面的字段对比名称是否冲撞
        for (std::size_t j = 0; j < i; ++j) {
            if (std::string_view(descriptor(fields[j]).name) == tag.name) { return false; }
        }
    }

    // defaults 使用字段初始化器提供真实默认值, 不维护另一份默认参数表.
    const detail::Options defaults;
    // valid 初始为 true, 编译期合并全部默认值的范围检查结果.
    bool valid = true;

    // 利用 C++26 `template for` 编译期展开.
    // 使用 `[:field:]` 发起成员访问反射.
    template for (constexpr auto field : fields) {
        valid = valid && valid_default(defaults.[:field:], descriptor(field));
    }
    return valid;
}
static_assert(valid_descriptors()); // 触发并在此确保所有的反射约束在编译时满足
// clang-format on

// 只接受完整十进制数字, 范围取自注解; 不使用会忽略尾部文本的转换函数.
// - output (std::uint64_t&): 转换成功后, 结果回写的地址.
// - text (std::string_view): 命令行输入提取到的值文本.
// - option (const detail::Option&): 对应选项的定义配置.
// 详细说明: 负责把传入的字符串用严格的方式转换成数字, 且要求值落在所给定的极值范围内.
Result<void> assign(std::uint64_t& output, std::string_view text, const detail::Option& option) {

    // value 暂存十进制解析结果, 完整消费且满足注解边界后才写入 output.
    std::uint64_t value = 0;
    // end 是首个未消费字符, error 是转换错误码, 两者均成功才接受完整数字.
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    // 判断是否有转换错误, 或者是还有余留未能转换完毕的字符, 或是最终数字越界.
    if (error != std::errc{} || end != text.data() + text.size() || value < option.minimum || value > option.maximum) {
        return Status::configuration("Invalid numeric option: --" + std::string(option.name));
    }
    output = value;
    return {};
}

// 字符串先转为拥有的值, 地址/名称关系在解析完全部字段后检查.
// - output (std::string&): 存储提取到的字符串引用.
// - text (std::string_view): 命令行提取的文本.
Result<void> assign(std::string& output, std::string_view text, const detail::Option&) {
    output = text;
    return {};
}

// 帮助使用普通重载处理字段值, 反射仅负责枚举, 不扩展异构模板分支.
// 生成字符串参数结尾的默认值说明信息.为空则不输出默认值.
std::string default_help(const std::string& value, const detail::Option&) {
    return value.empty() ? std::string{} : "; default=" + value;
}

// 使用数值 value 和注解 option 返回包含式范围与默认值文本, 不参与运行时参数赋值.
// 生成数字型参数的取值范围和默认值说明.
std::string default_help(std::uint64_t value, const detail::Option& option) {
    return " [" + std::to_string(option.minimum) + ", " + std::to_string(option.maximum) + "]; default=" + std::to_string(value);
}
} // namespace

// parse_options 函数实现
// - arguments (std::span<const std::string_view>): 操作系统的 argv 参数列表切片, 丢弃了 arg[0](自身程序名).
// - role (Member::Role): 明确启动服务的类型.
// 返回值: 成功返回完整检查且赋值通过的配置 Config, 失败返回 unexpected 和原因.
Result<Config> Config::parse(std::span<const std::string_view> arguments, Member::Role role) {

    detail::Options options;
    std::bitset<fields.size()> seen; // 用于追踪每个字段是否在命令行出现过, 避免重复赋.

    // 先统一 --name=value 与 --name value, 再分派字段; 借用的参数只在本次调用中有效.
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        // argument 借用当前 argv 项, i 指示当前索引, 分隔式参数会额外消费下一项.
        const auto argument = arguments[i];
        if (!argument.starts_with("--")) {
            return Status::configuration("Expected a named option");
        }

        // 尝试分离键和值
        const auto separator = argument.find('=');
        // 如果是 --name=value, 则截取 -- 后到 = 的部分.否则截取 -- 后的所有.
        const auto name = argument.substr(2, separator == std::string_view::npos ? separator : separator - 2);

        if (name == "worker-threads") {
            return Status::configuration("--worker-threads is unsupported: gRPC owns I/O workers");
        }

        // value 借用同项等号后文本或下一参数, 初始为空用于识别缺值.
        std::string_view value;
        // 支持 `--name=value` 格式
        if (separator != std::string_view::npos) {
            value = argument.substr(separator + 1);
        }

        // 支持 `--name value` 格式, 此时消耗下一个 argv 参数.
        else if (i + 1 < arguments.size()) {
            value = arguments[++i];
        }

        // 值为空, 值似乎被错写为另外一个旗帜形参, 值太长均报错.
        if (value.empty() || value.starts_with("--") || value.size() > 4096) {
            return Status::configuration("Missing or oversized option value");
        }

        // found 初始为 false, 仅在已声明的选项名匹配后设置.
        bool found = false;
        // index 从零对应 fields 与 seen 的同一字段顺序, 不作为用户输入.
        std::size_t index = 0;

        // 仅此字段分派使用 splice; 把异构访问展开为普通比较和赋值, 不引入虚函数或成员指针表.
        // 详细说明: 这里利用了 C++26 的反射, 在编译期生成了长串的 if (name == option.name) 来挨个匹配目标字段.
        // clang-format off
        template for (constexpr auto field : fields) {

            // option 是当前反射字段的编译期 CLI 描述, 用于共享校验及帮助信息.
            constexpr auto option = descriptor(field);
            if (name == option.name) {
                // 如果发现某项参数在 bitset 中已激活过, 说明其被传入了两次.
                if (seen.test(index)) { return Status::configuration("Duplicate option"); }
                // 使用 `[:field:]` 访问 options 对象里的目标变量, 调用之前的 assign 重载.
                auto result = assign(options.[:field:], value, option);
                if (!result) { return std::unexpected(result.error()); }
                seen.set(index);
                found = true;
            }
            ++index;
        }

        // clang-format on

        // 如果上面一大串展开的 if 没能匹配上任何注册的项, 就报错未知的选项.
        if (!found) {
            return Status::configuration("Unknown option");
        }
    }

    // index 从零对应 fields 与 seen 的同一字段顺序, 不作为用户输入.
    std::size_t index = 0;

    // 必填字段检查来自相同注解, 新增字段无需修改第二份条件列表.
    // 详细说明: 再次编译期展开所有字段, 看一看带有 required: true 标记的字段对应的 seen 状态是不是没有激活.
    // clang-format off
    template for (constexpr auto field : fields) {
        if (descriptor(field).required && !seen.test(index)) { return Status::configuration("Missing required option"); }
        ++index;
    }

    // clang-format on

    // 跨字段约束保持普通代码, 不把地址关系塞入元编程规则引擎.
    // 将解析出的临时 option 值, 代入类型化的验证结构里.
    auto listen = Endpoint::parse(options.listen, true);
    // supervisor 拥有校验后的控制面地址, 支持合规 DNS 名称或数值 IP.
    auto supervisor = Config::format_supervisor(options.supervisor);
    if (!listen || !supervisor || !Member::valid_name(options.galaxy) || !Member::valid_name(options.group) || options.identity.empty()) {
        return Status::configuration("Invalid endpoint, name or identity path");
    }

    // 生成对外的宣告地址(如果不传则降级采用 listen 地址).
    auto advertise = options.advertise.empty() ? listen : Endpoint::parse(options.advertise);
    // 对外宣告的端点绝对不可以含有 0.0.0.0 或者 :: 这样指向模糊的通配符.必须能被别人明确寻址.
    if (!advertise || (listen->wildcard && options.advertise.empty())) {
        return Status::configuration("A concrete advertise endpoint is required");
    }

    // 全部输入和跨字段约束通过后才形成运行配置, 派生入站预算并将 CLI 秒数统一转换为毫秒.
    Config result;
    result.role = role;
    result.galaxy = std::move(options.galaxy);
    result.group = std::move(options.group);
    result.listen = std::move(*listen);
    result.advertise = std::move(*advertise);
    result.supervisor = std::move(*supervisor);
    result.identity = std::move(options.identity);
    // 成员预算设置
    result.max_members = options.maximum;
    result.max_inbound = options.maximum * 2;
    // 心跳周期, 超时以及状态发布周期做类型化时间转换
    result.heartbeat_interval = Milliseconds(options.heartbeat);
    result.pong_timeout = Milliseconds(options.pong);
    result.shutdown_timeout = std::chrono::seconds(options.shutdown);
    result.status_interval = std::chrono::seconds(options.status);

    // 注入新暴露的准入通道尺寸约束配置
    result.max_admission_request_bytes = static_cast<std::uint32_t>(options.max_admission_request_bytes);
    result.max_admission_response_bytes = static_cast<std::uint32_t>(options.max_admission_response_bytes);

    return result;
}

// option_help 函数实现
// - role (Member::Role): 明确启动服务角色以便于在帮助输出的标头动态打印可执行程序的名称.
// 返回值: 帮助文档文本.
std::string Config::help(Member::Role role) {

    // result 拥有累计的帮助文本, 仅包含固定选项和公开默认值.
    std::string result = "Usage: " + std::string(role == Member::Role::star ? "star" : "planet") + " --listen=IP:PORT --super=HOST:PORT --galaxy=ID [options]\n";
    // defaults 使用字段初始化器提供真实默认值, 不维护另一份默认参数表.
    const detail::Options defaults;

    // 默认值直接读取同一结构, 修改初始化器会同步改变实例和帮助.
    // 详细说明: 依然利用反射技术, 自动将每个字段的注解名, 描述, 默认范围, 以及标志性字串拼接成为格式化良好的命令帮助.
    // clang-format off
    template for (constexpr auto field : fields) {

        // option 是当前反射字段的编译期 CLI 描述, 用于共享校验及帮助信息.
        constexpr auto option = descriptor(field);
        result += "  --" + std::string(option.name) + "  " + std::string(option.description);
        result += default_help(defaults.[:field:], option); // 填入范围和默认值
        result += option.required ? " (required)\n" : "\n"; // 若为必填则增加对应提醒
    }

    // clang-format on
    // 在已有缓冲追加结尾, 返回时允许移动或消除复制, 不再复制整段帮助正文.
    result += "  --help / --version\nTLS and Supervisor account login are required. Stop with SIGINT or SIGTERM.\n";
    return result;
}
} // namespace astra
