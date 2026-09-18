// 该文件包含用于命令行参数解析的核心字段结构体和相关宏或结构体配置, 集中化管理参数说明以实现DRY(不要重复自己).
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace astra::detail {
// 注解信息结构体, 只负责描述当前 CLI 使用的可视化字符串信息和最基本的无符号数值约束限制,
// 它的目的并非为了构建一个庞大臃肿的通用配置反射框架, 而是精准满足现有需求.
struct Option {
    // 根据 C++ 的结构化绑定以及 constexpr 需求, 注解类型必须是 structural type 结构类型.
    // 所以用内嵌定长字符数组直接原位保存文本数据, 绝不通过悬空指针去持有临时的 string_view 内存.

    // name: 命令行 CLI 使用的长选项名字符串, 设计上不包含前导的双横杠 `--`;
    // 支持最大容纳 47 字节外加末尾隐式的 NUL 字符, 对象初始化采用零初始化来保证自动保留合规终止符.
    char name[48]{};

    // description: 命令行输出展示的对应参数帮助说明信息.
    // 最多支持保存 159 字节长文本外加 NUL 结束符; 此处存放的内容必须是固化在代码中的公开帮助文本, 绝不能无意包含运行时的动态数据或身份敏感材料.
    char description[160]{};

    // minimum: 数值选项专属的包含式有效下界, 缺省值为 0;
    // 若当前处理的是字符串类型选项, 则解析器不会使用此数值字段作为长度约束.
    std::uint64_t minimum = 0;

    // maximum: 数值选项专属的包含式有效上界, 缺省值为 0;
    // 字符串类型的选项格式或长度必须由专门的业务层解析器在后续进行另行检查.
    std::uint64_t maximum = 0;

    // required: 标记当前标志项是否必须在 CLI 命令行中通过手动显式出现输入, 默认是否(false);
    // 当值为 true 时, 不能仅仅依赖字段定义的初始默认值去糊弄通过强制必填的业务层检查.
    bool required = false;

    // 在编译期进行验证并直接复制 option_name 及 help 文本,
    // 参数 lower 与 upper 明确定义了对包含式数值边界范围的划定, mandatory 参数用于控制是否开启选项强制必填的强约束检查.
    // 如果输入的字面量文本过长超出了预设的内部数组空间时, 将会立即触发 throw 致使常量求值失败, 从而使得错误在编译阶段无处遁形;
    // 内部完成字符拷贝操作后, 原传入的 string_view 借用随之废弃解绑, 无悬挂风险.
    consteval Option(std::string_view option_name, std::string_view help, std::uint64_t lower = 0, std::uint64_t upper = 0, bool mandatory = false) : minimum(lower), maximum(upper), required(mandatory) {

        if (option_name.size() >= sizeof(name) || help.size() >= sizeof(description)) {
            // 在编译期通过抛出异常中止常量求值.
            throw "CLI annotation text exceeds its compile-time capacity";
        }

        // 在完成长度阈值检查安全后, 执行拷贝复制文本数据.
        // 未被文本实际覆盖的那部分尾部区域因为零初始化的机制将自动作为 NUL 结束符存在,
        // 从而完美避免了在使用反射读取元数据时去危险地借用临时缓冲.
        std::ranges::copy(option_name, name);
        std::ranges::copy(help, description);
    }
};

struct Options {
    // 整个配置集每一个具体选项的名称定义, 有效数值范围, 物理单位以及说明文档文本, 必须只在当前代码段中进行独家声明定义.
    // 各字段本身的 C++ 成员默认值将会依旧严格保留原先在字段初始化器中的自然语义.
    // 因为目前的 clang-format 格式化工具版本尚未能完美识别 C++ 标准外的特殊反射注解语法标记,
    // 为了防止破坏格式, 特地在此保护这些特定的属性声明代码段, 但并没有粗暴地禁用整个文件的自动格式化.
    // clang-format off

    // galaxy: Galaxy 拓扑标识原文配置, 强制必填项且设计上不留任何空默认值;
    // 解析执行后系统会严格要求内容必须是 1..64 字节长度的安全 ASCII 字符.
    [[=Option{"galaxy", "Galaxy identifier: 1..64 safe ASCII bytes", 0, 0, true}]] std::string galaxy;

    // listen: 网络监听端点原文配置, 强制必填项;
    // 在地址上允许输入数值型的通配 IP(如 0.0.0.0)和特殊的表示系统自动分配的端口 0, 但坚决不支持任何形式的 DNS 域名解析.
    [[=Option{"listen", "Local numeric IP:PORT; wildcard requires advertise", 0, 0, true}]] std::string listen;

    // supervisor: 远端 Supervisor 系统拨号地址原文配置, 强制必填项;
    // 地址部分允许接受常规 DNS 域名格式或者是纯数值 IP 形式, 但在端口方面必须提供真实有效的非零端口参数.
    [[=Option{"super", "Supervisor HOST:PORT", 0, 0, true}]] std::string supervisor;

    // advertise: 外网公开公布的网络端点原文配置, 若缺省未填则默认为空字符串, 系统运行时将自动回退使用监听端点值;
    // 若显式填写该值, 则所输入地址必须具有真实的网络可达性, 并必须确切使用非零真实网络端口.
    [[=Option{"advertise", "Reachable numeric IP:PORT; defaults to bound listener"}]] std::string advertise;

    // identity: 安全认证身份目录原文配置, 如果不指定则隐式默认取值为当前目录下的 "identity";
    // 处理时所有的相对路径解析操作都会严格依赖于当前工作目录为基准进行解释执行, 本 CLI 层解析阶段纯粹处理文本, 绝不主动去发起实质的文件读写动作.
    [[=Option{"identity", "Directory containing TLS, admission.pub and login.json"}]] std::string identity = "identity";

    // group: 分布式网络中的连接偏好逻辑分组名称, 缺省未填则默认为 "default";
    // 字符串要求必须满足 1..64 字节长度的安全 ASCII 集合规范, 注意该属性仅作为路由调度倾向依据, 绝对不能当作任何的安全权限验证范围.
    [[=Option{"group", "Preferred connection group; 1..64 safe ASCII bytes"}]] std::string group = "default";

    // maximum: 针对每一网络类角色的实体连接成员数量容量限制约束, 系统默认 64 个, 在线设定范围必须落在 1..4096 闭区间内;
    // 这个参数项在转译生成 Config 对象实例时, 将会同时负责并关联派生系统相关的入站 RPC 操作底层预算限制配额.
    [[=Option{"max-members", "Capacity per role; Star capacity includes self", 1, 4096}]] std::uint64_t maximum = 64;

    // heartbeat: 周期性发送下一次内部 Ping 保活心跳前的延期待机时间间隔, 默认配置为 10000 毫秒 (10秒),
    // 给定的取值规范范围必须处在 1..86400000 毫秒(24小时)之间, 参数若是输入零则视为非法无效被驳回.
    [[=Option{"heartbeat-interval-ms", "Idle delay before Ping, milliseconds", 1, 86400000}]] std::uint64_t heartbeat = 10000;

    // pong: 针对发送 Ping 报文时排队进出, 内核网络 IO 写入操作以及漫长等待收到对应回复 Pong 的整体合并全生命周期的时间超期预算,
    // 默认配置时间为 5000 毫秒, 输入的设定容许规范范围为 1..86400000 毫秒.
    [[=Option{"pong-timeout-ms", "Ping queue/write/Pong total deadline, milliseconds", 1, 86400000}]] std::uint64_t pong = 5000;

    // shutdown: 所有线程及资源的统一共享优雅关闭超时预算时长, 默认时长为 5 秒钟, 在此提供范围设定需处于 1..60 秒之间;
    // 在后期转为正式业务 Config 结构体时, 会由框架执行数据单位统一转换将其直接转化成为毫秒.
    [[=Option{"shutdown-timeout-seconds", "Owned shutdown deadline, seconds", 1, 60}]] std::uint64_t shutdown = 5;

    // status: 程序周期性在后台按时快照并输出当前连接状态日志的时间间隔节奏, 默认值为 0 代表程序会选择全局静音禁用状态输出功能;
    // 如果启用了此项(值非零), 那么设定的区间需处于 1..3600 秒(1小时内)之间;
    // 后续转换正式 Config 处理时同样也会做一层底层毫秒数据转换对接.
    [[=Option{"status-interval-seconds", "Actual-state snapshot interval; zero disables", 0, 3600}]] std::uint64_t status = 0;

    // max_admission_request_bytes: 连接 Supervisor 发起准入请求时, 最大允许发送的消息字节数上限.
    // 默认配置 4096 (4KB), 允许范围从 1 到 1 MB.极小的封顶足以拦截本地异常超大请求发出的风险.
    [[=Option{"max-admission-request-bytes", "Max admission request bytes", 1, 1024 * 1024}]] std::uint64_t max_admission_request_bytes = 4 * 1024;

    // max_admission_response_bytes: 接收 Supervisor 准入响应时, 最大允许接收的消息字节数上限.
    // 默认配置 2097152 (2MB), 允许范围从 1 KB 到 256 MB.足以容纳系统硬上限的拓扑名单回包而不触发内存 OOM.
    [[=Option{"max-admission-response-bytes", "Max admission response bytes", 1024, 256 * 1024 * 1024}]] std::uint64_t max_admission_response_bytes = 2 * 1024 * 1024;

    // clang-format on
};
} // namespace astra::detail
