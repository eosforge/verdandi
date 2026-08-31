#pragma once

#include "verdandi/error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace verdandi {

/// Redis 数据节点或 Sentinel 使用的一组独立 ACL 凭据。
struct auth_configuration {
    /// ACL 用户名；默认空字符串，表示不显式提供用户名。
    std::string username;
    /// ACL 密码；默认空字符串，表示不显式提供密码。
    std::string password;
};

/// Redis 和 Sentinel 的 TLS 1.2+ 连接配置。
struct tls_configuration {
    /// 是否启用 TLS；默认 false。禁用时其余字段必须保持默认值。
    bool enabled{false};
    /// 是否包含操作系统信任根；默认 true。关闭时必须提供 `ca_file`。
    bool system_roots{true};
    /// Standalone 证书/SNI 名称覆盖；默认空，最多 253 个 UTF-8 字节；Sentinel 模式不允许设置。
    std::string server_name;
    /// 附加 PEM CA 文件；默认空，路径最多 4096 个 UTF-8 字节，读取上限为 1 MiB。
    std::filesystem::path ca_file;
    /// PEM 客户端证书链；默认空，路径最多 4096 个 UTF-8 字节，必须与 `key_file` 同时设置。
    std::filesystem::path cert_file;
    /// 未加密 PEM 客户端私钥；默认空，路径最多 4096 个 UTF-8 字节，必须与 `cert_file` 同时设置。
    std::filesystem::path key_file;

    /// 纯校验字段范围和 TLS 关系，不读取证书文件。
    [[nodiscard]] result<void> check() const;
};

/// 根 Redis 连接池配置。
struct pool_configuration {
    /// 启动时保持的最少连接数；默认 1，范围 1..1024。
    std::size_t min_connections{1};
    /// 并发压力下允许的最多连接数；默认 4，范围 1..1024，且不得小于最小值。
    std::size_t max_connections{4};
    /// 空闲连接回收时间；默认 10,000ms，范围 1,000..3,600,000ms。
    std::chrono::milliseconds idle_timeout{10'000};

    /// 校验连接数、持续时间及最小/最大关系。
    [[nodiscard]] result<void> check() const;
};

/// Redis 建连与故障恢复退避配置。
struct reconnect_configuration {
    /// 首次恢复失败后的基础延迟；默认 100ms，范围 10..5,000ms。
    std::chrono::milliseconds initial_delay{100};
    /// 连续失败退避上限；默认 5,000ms，范围 100..30,000ms，且不得小于基础延迟。
    std::chrono::milliseconds max_delay{5'000};
    /// 指数退避倍数；默认 2，范围 1..8。
    std::uint32_t multiplier{2};
    /// 随机抖动百分比；默认 10，范围 0..50，零表示不增加抖动。
    std::uint8_t jitter_percent{10};

    /// 校验退避字段范围及延迟关系。
    [[nodiscard]] result<void> check(std::string_view prefix = "redis.reconnect") const;
};

/// 根传输支持的 Redis 拓扑；Cluster 在 v1 中没有支持计划。
enum class redis_mode : std::uint8_t {
    standalone,
    sentinel,
};

/// 由根 Client 独占的 Redis 传输配置。
struct redis_configuration {
    /// Redis 拓扑；外部 JSON 必须显式设置 `standalone` 或 `sentinel`。
    redis_mode mode{redis_mode::standalone};
    /// 数据节点或 Sentinel 的 `host:port`；Standalone 恰好一个，Sentinel 至少一个。
    std::vector<std::string> addresses{"127.0.0.1:6379"};
    /// Sentinel 监控的服务名；Standalone 必须为空，Sentinel 必须为非空规范字符串。
    std::string master_name;
    /// Redis 数据节点 ACL；默认空。
    auth_configuration auth;
    /// Sentinel 自身 ACL；默认空，Standalone 必须保持为空。
    auth_configuration sentinel_auth;
    /// Redis 逻辑数据库编号；默认 0，范围 0..255。
    std::uint16_t database{0};
    /// Redis 与 Sentinel TLS 配置；默认禁用但保留系统根标志。
    tls_configuration tls;
    /// 普通 Redis 命令总等待上限；默认 2,000ms，范围 10..15,000ms。
    std::chrono::milliseconds timeout{2'000};
    /// 单次 TCP 建连及 TLS 握手上限；默认 5,000ms，范围 20..30,000ms。
    std::chrono::milliseconds connect_timeout{5'000};
    /// 跨语言一致的连接池控制。
    pool_configuration pool;
    /// 建连与恢复退避控制。
    reconnect_configuration reconnect;

    /// 校验完整拓扑、地址、ACL/TLS 关系以及所有数值边界。
    [[nodiscard]] result<void> check() const;
};

/// Redis 中 Registration Zone 策略的启动默认值和协议上限内初值。
struct registration_policy_configuration {
    /// Attr 顶层字段数；默认 16，范围 1..128。
    std::size_t attr_max_fields{16};
    /// Data 顶层字段数；默认 32，范围 1..128。
    std::size_t data_max_fields{32};
    /// 应用字段名 UTF-8 字节数；默认 64，范围 1..64。
    std::size_t field_name_max_bytes{64};
    /// 单个 Attr 字段值字节数；默认 128，范围 1..16,384。
    std::size_t attr_value_max_bytes{128};
    /// 单个 Data 字段值字节数；默认 128，范围 1..16,384。
    std::size_t data_value_max_bytes{128};
    /// 完整 Registration 估算字节数；默认 16,384，范围 1..65,536。
    std::size_t record_max_bytes{16'384};
    /// SDK 重新读取 Redis 策略的周期；默认 30,000ms，范围 1,000..86,400,000ms。
    std::chrono::milliseconds refresh_interval{30'000};

    /// 校验全部策略值是否落在协议上限内。
    [[nodiscard]] result<void> check() const;
};

/// Selector 本地视图、对时及恢复配置。
struct selector_configuration {
    /// 单次 HSCAN 目标条目数；默认 256，范围 1..1,024。
    std::size_t scan_page_size{256};
    /// 半同步期间最多合并的 UUID 数；默认 4,096，范围 1..65,536。
    std::size_t max_pending_entries{4'096};
    /// 半同步事件估算字节预算；默认 64MiB，范围 1..1GiB。
    std::size_t max_pending_bytes{64ULL * 1'024ULL * 1'024ULL};
    /// 两次不可变视图发布的最短间隔；默认 10ms，范围 0..1,000ms，零表示立即发布。
    std::chrono::milliseconds view_publish_interval{10};
    /// 一代完整或定向同步总上限；默认 30,000ms，范围 100..3,600,000ms。
    std::chrono::milliseconds sync_timeout{30'000};
    /// 活动候选视图估算字节预算；默认 256MiB，范围 1..1GiB。
    std::size_t max_active_bytes{256ULL * 1'024ULL * 1'024ULL};
    /// retained 视图估算字节预算；默认 64MiB，范围 0..1GiB，零表示禁用 retained。
    std::size_t max_retained_bytes{64ULL * 1'024ULL * 1'024ULL};
    /// 连接级 RedisClock 周期校准间隔；默认 30,000ms，范围 1,000..3,600,000ms。
    std::chrono::milliseconds clock_refresh_interval{30'000};
    /// 每次 RedisClock 样本附加的保守误差；默认 1ms，范围 0..1,000ms。
    std::chrono::milliseconds clock_uncertainty{1};
    /// 有界异步诊断容量；默认 16，范围 1..1,024。
    std::size_t error_buffer_capacity{16};
    /// Selector 恢复退避；默认 100ms/5,000ms/2/50%。
    reconnect_configuration recovery{std::chrono::milliseconds{100}, std::chrono::milliseconds{5'000}, 2, 50};

    /// 校验视图、同步、对时、诊断和恢复字段。
    [[nodiscard]] result<void> check() const;
};

/// Registration/Selector 子域配置；Zone 不属于根 Redis Client。
struct registration_configuration {
    /// 管理隔离 Zone；必须匹配 `[A-Za-z]{1,32}`。
    std::string zone;
    /// 单个 Registration Fields 邮箱同时接纳的结果等待者；默认 8，范围 1..256。
    std::size_t buffer_capacity{8};
    /// Registration 域异步诊断容量；默认 16，范围 1..1,024。
    std::size_t error_buffer_capacity{16};
    /// 允许配置的最短显式或自动续期间隔；默认 100ms，范围 10..60,000ms。
    std::chrono::milliseconds min_renew_interval{100};
    /// 自动续期间隔抖动；默认 10%，范围 0..50%。
    std::uint8_t renew_jitter_percent{10};
    /// Redis 策略刷新周期抖动；默认 10%，范围 0..50%。
    std::uint8_t policy_refresh_jitter_percent{10};
    /// Redis Zone 策略缺失字段的启动默认值。
    registration_policy_configuration policy;
    /// 每个 Selector 的本地运行配置。
    selector_configuration selector;

    /// 校验 Zone 及所有 Registration/Selector 本地参数。
    [[nodiscard]] result<void> check() const;
};

/// Catalog Publisher/Subscriber 子域配置；与 Redis 和 Registration 配置分离。
struct catalog_configuration {
    /// 管理隔离 Zone；必须匹配 `[A-Za-z]{1,32}`。
    std::string zone;
    /// 初始同步或修复总等待上限；默认 30,000ms，范围 100..3,600,000ms。
    std::chrono::milliseconds sync_timeout{30'000};
    /// 索引扫描页目标条目数；默认 256，范围 1..4,096。
    std::size_t scan_page_size{256};
    /// 同步期允许并行的权威读取数；默认 32，范围 1..256。
    std::size_t max_inflight_reads{32};
    /// 事件交接及精确修复合并容量；默认 256，范围 1..65,536。
    std::size_t event_buffer_capacity{256};
    /// 有界异步诊断容量；默认 64，范围 1..4,096。
    std::size_t error_buffer_capacity{64};
    /// 完整内存视图的应用编码字节上限；默认 0，范围 0..64GiB，零表示不额外限制。
    std::uint64_t max_view_bytes{0};
    /// 单条 Catalog 完整结构化值上限；默认 512KiB，范围 1..4MiB。
    std::size_t max_record_bytes{512ULL * 1'024ULL};
    /// Catalog 恢复退避；默认 250ms/5,000ms/2/10%。
    reconnect_configuration recovery{std::chrono::milliseconds{250}, std::chrono::milliseconds{5'000}, 2, 10};
    /// 可丢弃本地检查点路径；空路径禁用，非空路径最多 4096 个 UTF-8 字节且不得含 NUL。
    std::filesystem::path local_store_path;

    /// 校验 Zone、同步/容量、恢复以及可选检查点路径。
    [[nodiscard]] result<void> check() const;
};

/// Verdandi v1 唯一跨语言外部 JSON 结构及其语言原生值。
struct configuration {
    /// 外部配置协议版本；只接受 `v1`。
    std::string version{"v1"};
    /// 必需的根 Redis 传输配置。
    redis_configuration redis;
    /// 可选 Registration/Selector 子域；`enabled=false` 表示 JSON 未配置该域。
    bool registration_enabled{false};
    registration_configuration registration;
    /// 可选 Catalog 子域；`enabled=false` 表示 JSON 未配置该域。
    bool catalog_enabled{false};
    catalog_configuration catalog;

    /// 从不超过 1MiB 的严格 UTF-8 JSON `source` 加载完整配置，不执行网络或文件型 TLS I/O。
    [[nodiscard]] static result<configuration> from_json(std::span<const std::byte> source);

    /// 从 `path` 有界读取并严格解析配置；文件超过 1MiB 时拒绝。
    [[nodiscard]] static result<configuration> load_json(const std::filesystem::path& path);

    /// 校验版本、根 Redis 以及所有已启用子域的原生字段和关系。
    [[nodiscard]] result<void> check() const;
};

} // namespace verdandi
