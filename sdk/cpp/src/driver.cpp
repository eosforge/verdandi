#include "internal/driver.hpp"

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/redis/connection.hpp>
#include <boost/redis/logger.hpp>
#include <boost/redis/request.hpp>
#include <boost/redis/response.hpp>
#include <boost/redis/src.hpp>
#include <boost/system/error_code.hpp>

#include <openssl/ssl.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>

namespace verdandi::detail {

namespace asio = boost::asio;
namespace redis = boost::redis;
using boost::system::error_code;

namespace {

constexpr std::uint64_t protocol_integer_max = (std::uint64_t{1} << 53U) - 1U;

struct endpoint {
    std::string host;
    std::string port;
};

[[nodiscard]] endpoint split_endpoint(const std::string_view value) {
    if (value.front() == '[') {
        const auto close = value.find(']');
        return {std::string(value.substr(1, close - 1)), std::string(value.substr(close + 2))};
    }
    const auto separator = value.rfind(':');
    return {std::string(value.substr(0, separator)), std::string(value.substr(separator + 1))};
}

void append_setup(redis::request& output, const auth_configuration& auth, const std::uint16_t database) {
    output.clear();
    if (auth.username.empty() && auth.password.empty()) {
        output.hello();
    } else {
        output.hello(auth.username.empty() ? "default" : auth.username, auth.password);
    }
    if (database != 0) {
        output.push("SELECT", database);
    }
}

[[nodiscard]] redis::config make_wire_configuration(const redis_configuration& source) {
    redis::config output;
    const auto primary = split_endpoint(source.addresses.front());

    output.addr = {primary.host, primary.port};
    output.use_ssl = source.tls.enabled;
    output.resolve_timeout = source.connect_timeout;
    output.connect_timeout = source.connect_timeout;
    output.ssl_handshake_timeout = source.connect_timeout;
    output.health_check_interval = source.timeout;
    output.reconnect_wait_interval = source.reconnect.initial_delay;
    output.max_read_size = 256ULL * 1'024ULL * 1'024ULL;
    output.use_setup = true;
    append_setup(output.setup, source.auth, source.database);

    if (source.mode == redis_mode::sentinel) {
        output.sentinel.master_name = source.master_name;
        output.sentinel.server_role = redis::role::master;
        output.sentinel.resolve_timeout = source.connect_timeout;
        output.sentinel.connect_timeout = source.connect_timeout;
        output.sentinel.ssl_handshake_timeout = source.connect_timeout;
        output.sentinel.request_timeout = source.timeout;
        output.sentinel.use_ssl = source.tls.enabled;
        output.sentinel.addresses.reserve(source.addresses.size());
        for (const auto& value : source.addresses) {
            auto parsed = split_endpoint(value);
            output.sentinel.addresses.push_back({std::move(parsed.host), std::move(parsed.port)});
        }
        append_setup(output.sentinel.setup, source.sentinel_auth, 0);
    }
    return output;
}

[[nodiscard]] result<asio::ssl::context> make_ssl_context(const redis_configuration& source) {
    try {
        asio::ssl::context output(asio::ssl::context::tls_client);
        output.set_options(asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
        if (!source.tls.enabled) {
            output.set_verify_mode(asio::ssl::verify_none);
            return output;
        }

        if (source.tls.system_roots) {
            output.set_default_verify_paths();
        }
        if (!source.tls.ca_file.empty()) {
            output.load_verify_file(source.tls.ca_file.string());
        }
        if (!source.tls.cert_file.empty()) {
            output.use_certificate_chain_file(source.tls.cert_file.string());
            output.use_private_key_file(source.tls.key_file.string(), asio::ssl::context::pem);
        }

        const auto parsed = split_endpoint(source.addresses.front());
        const auto name = source.tls.server_name.empty() ? parsed.host : source.tls.server_name;
        output.set_verify_mode(asio::ssl::verify_peer);
        output.set_verify_callback(asio::ssl::host_name_verification(name));
        return output;
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::invalid, "redis.tls").with_detail(exception.what()));
    }
}

[[nodiscard]] result<std::shared_ptr<redis::connection>> make_connection(asio::io_context& io, const redis_configuration& source) {
    auto context = make_ssl_context(source);
    if (!context) {
        return std::unexpected(context.error());
    }

    try {
        auto output = std::make_shared<redis::connection>(io, std::move(*context), redis::logger(redis::logger::level::disabled));
        if (source.tls.enabled) {
            const auto parsed = split_endpoint(source.addresses.front());
            const auto& name = source.tls.server_name.empty() ? parsed.host : source.tls.server_name;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
            if (SSL_set_tlsext_host_name(output->next_layer().native_handle(), name.c_str()) != 1) {
                return std::unexpected(error(code::invalid, "redis.tls.server_name"));
            }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
        }
        return output;
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::unavailable, "redis.connection").with_detail(exception.what()));
    }
}

[[nodiscard]] error transport_error(const error_code& value, const bool mutation) {
    if (mutation) {
        return error(code::ambiguous, "redis.command").with_detail(value.message());
    }
    if (value == asio::error::operation_aborted || value == asio::error::timed_out) {
        return error(code::deadline, "redis.command").with_detail(value.message());
    }
    return error(code::unavailable, "redis.command").with_detail(value.message());
}

[[nodiscard]] result<response::kind> response_kind(const redis::resp3::type value) {
    using redis::resp3::type;
    switch (value) {
    case type::null:
        return response::kind::null;
    case type::simple_string:
    case type::blob_string:
    case type::verbatim_string:
    case type::big_number:
    case type::doublean:
    case type::streamed_string:
    case type::streamed_string_part:
        return response::kind::string;
    case type::number:
        return response::kind::number;
    case type::boolean:
        return response::kind::boolean;
    case type::array:
        return response::kind::array;
    case type::map:
        return response::kind::map;
    case type::set:
        return response::kind::set;
    case type::push:
        return response::kind::push;
    case type::attribute:
    case type::simple_error:
    case type::blob_error:
    case type::invalid:
        return std::unexpected(error(code::corrupt, "redis.response"));
    }
    return std::unexpected(error(code::corrupt, "redis.response"));
}

[[nodiscard]] result<response> convert_node(const redis::resp3::tree& tree, std::size_t& index) {
    if (index >= tree.size()) {
        return std::unexpected(error(code::corrupt, "redis.response"));
    }
    const auto& input = tree[index++];
    auto type = response_kind(input.data_type);
    if (!type) {
        return std::unexpected(type.error());
    }

    response output{*type, input.value, {}};
    if (!redis::resp3::is_aggregate(input.data_type)) {
        return output;
    }

    const auto multiplicity = redis::resp3::element_multiplicity(input.data_type);
    if (input.aggregate_size > std::numeric_limits<std::size_t>::max() / multiplicity) {
        return std::unexpected(error(code::corrupt, "redis.response"));
    }
    const auto count = input.aggregate_size * multiplicity;
    output.children.reserve(count);
    for (std::size_t child = 0; child < count; ++child) {
        auto converted = convert_node(tree, index);
        if (!converted) {
            return std::unexpected(converted.error());
        }
        output.children.push_back(std::move(*converted));
    }
    return output;
}

[[nodiscard]] result<std::vector<response>> convert_response(const redis::generic_response& source, const std::size_t expected) {
    if (source.has_error()) {
        return std::unexpected(error(code::protocol, "redis.command").with_detail(source.error().diagnostic));
    }

    const auto& tree = source.value();
    std::vector<response> output;
    output.reserve(expected);
    std::size_t index{};
    while (index < tree.size()) {
        auto converted = convert_node(tree, index);
        if (!converted) {
            return std::unexpected(converted.error());
        }
        output.push_back(std::move(*converted));
    }
    if (output.size() != expected) {
        return std::unexpected(error(code::corrupt, "redis.response"));
    }
    return output;
}

struct execution {
    redis::request request;
    redis::generic_response response;
    std::promise<error_code> completed;
};

} // namespace

command::command(const std::string_view name) {
    if (name.empty()) {
        throw std::invalid_argument("Redis command name is empty");
    }
    arguments.emplace_back(name);
}

command& command::add(const std::string_view value) {
    arguments.emplace_back(value);
    return *this;
}

command& command::add(const std::span<const std::byte> value) {
    arguments.emplace_back(reinterpret_cast<const char*>(value.data()), value.size());
    return *this;
}

command& command::add(const std::uint64_t value) {
    arguments.push_back(std::to_string(value));
    return *this;
}

result<std::string_view> response::text() const {
    if (type != kind::string && type != kind::number && type != kind::boolean) {
        return std::unexpected(error(code::corrupt, "redis.response"));
    }
    return value;
}

struct driver::implementation {
    struct connection_slot {
        redis::config wire;
        std::shared_ptr<redis::connection> connection;
        bool busy{false};
        std::chrono::steady_clock::time_point idle_since{std::chrono::steady_clock::now()};
    };

    explicit implementation(redis_configuration source) : configuration(std::move(source)), work(asio::make_work_guard(io)), reactor([this] { io.run(); }) {}

    ~implementation() noexcept {
        shutdown();
    }

    [[nodiscard]] result<std::shared_ptr<connection_slot>> create_slot() {
        auto connection = make_connection(io, configuration);
        if (!connection) {
            return std::unexpected(connection.error());
        }

        auto slot = std::make_shared<connection_slot>();
        slot->wire = make_wire_configuration(configuration);
        slot->connection = std::move(*connection);
        asio::post(io, [slot] { slot->connection->async_run(slot->wire, asio::consign(asio::detached, slot)); });
        return slot;
    }

    [[nodiscard]] result<std::shared_ptr<connection_slot>> acquire(const std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(pool_mutex);
        for (;;) {
            if (closing.load(std::memory_order_acquire)) {
                return std::unexpected(error(code::closed));
            }

            const auto now = std::chrono::steady_clock::now();
            for (auto iterator = slots.begin(); slots.size() > configuration.pool.min_connections && iterator != slots.end();) {
                const auto& slot = *iterator;
                if (!slot->busy && now - slot->idle_since >= configuration.pool.idle_timeout) {
                    auto retired = slot->connection;
                    iterator = slots.erase(iterator);
                    asio::post(io, [retired] { retired->cancel(); });
                } else {
                    ++iterator;
                }
            }

            const auto available = std::ranges::find_if(slots, [](const auto& slot) { return !slot->busy; });
            if (available != slots.end()) {
                (*available)->busy = true;
                return *available;
            }

            if (slots.size() + creating < configuration.pool.max_connections) {
                ++creating;
                lock.unlock();
                auto created = create_slot();
                lock.lock();
                --creating;
                pool_changed.notify_all();
                if (!created) {
                    return std::unexpected(created.error());
                }
                if (closing.load(std::memory_order_acquire)) {
                    auto retired = (*created)->connection;
                    lock.unlock();
                    asio::post(io, [retired] { retired->cancel(); });
                    return std::unexpected(error(code::closed));
                }
                (*created)->busy = true;
                slots.push_back(*created);
                return *created;
            }

            if (!pool_changed.wait_until(lock, deadline, [this] {
                    return closing.load(std::memory_order_acquire) || std::ranges::any_of(slots, [](const auto& slot) { return !slot->busy; }) ||
                           slots.size() + creating < configuration.pool.max_connections;
                })) {
                return std::unexpected(error(code::deadline, "redis.pool"));
            }
        }
    }

    void release(const std::shared_ptr<connection_slot>& slot) {
        {
            std::lock_guard lock(pool_mutex);
            slot->busy = false;
            slot->idle_since = std::chrono::steady_clock::now();
        }
        pool_changed.notify_one();
    }

    [[nodiscard]] result<std::vector<response>> execute(const std::span<const command> values, const bool mutation) {
        if (values.empty()) {
            return std::unexpected(error(code::invalid, "redis.commands"));
        }
        const auto deadline = std::chrono::steady_clock::now() + configuration.timeout;
        auto acquired = acquire(deadline);
        if (!acquired) {
            return std::unexpected(acquired.error());
        }
        const auto slot = *acquired;

        auto operation = std::make_shared<execution>();
        for (const auto& value : values) {
            if (value.arguments.empty()) {
                release(slot);
                return std::unexpected(error(code::invalid, "redis.command"));
            }
            // Boost.Redis 会忽略空区间；零参数命令必须走 push，才能真正写入请求。
            if (value.arguments.size() == 1) {
                operation->request.push(value.arguments.front());
            } else {
                operation->request.push_range(value.arguments.front(), std::next(value.arguments.begin()), value.arguments.end());
            }
        }

        auto completed = operation->completed.get_future();
        const auto remaining =
            std::max(std::chrono::milliseconds{1}, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
        asio::post(io, [slot, operation, remaining] {
            slot->connection->async_exec(
                operation->request, operation->response,
                asio::cancel_after(remaining, [operation](const error_code status, std::size_t) { operation->completed.set_value(status); }));
        });

        if (completed.wait_until(deadline + std::chrono::milliseconds{100}) != std::future_status::ready) {
            asio::post(io, [connection = slot->connection] { connection->cancel(); });
            release(slot);
            return std::unexpected(error(mutation ? code::ambiguous : code::deadline, "redis.command"));
        }
        const auto status = completed.get();
        release(slot);
        if (status) {
            return std::unexpected(transport_error(status, mutation));
        }
        return convert_response(operation->response, values.size());
    }

    void shutdown() noexcept {
        if (closing.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        pool_changed.notify_all();

        try {
            std::vector<std::shared_ptr<redis::connection>> connections;
            {
                std::lock_guard lock(pool_mutex);
                connections.reserve(slots.size());
                for (const auto& slot : slots) {
                    connections.push_back(slot->connection);
                }
            }
            if (reactor.get_id() == std::this_thread::get_id()) {
                for (const auto& connection : connections) {
                    connection->cancel();
                }
            } else {
                auto cancelled = std::make_shared<std::promise<void>>();
                auto completed = cancelled->get_future();
                asio::post(io, [this, connections = std::move(connections), cancelled] {
                    for (const auto& connection : connections) {
                        connection->cancel();
                    }
                    // 再排入一个 reactor 标记，让 cancel 触发的完成处理器先获得一次执行机会。
                    asio::post(io, [cancelled] { cancelled->set_value(); });
                });
                static_cast<void>(completed.wait_for(configuration.timeout));
            }
        } catch (...) {
            // 关闭路径不得让分配、调度或底层取消异常越过析构边界；停止 reactor
            // 会销毁尚未运行的处理器并释放其拥有对象。
            io.stop();
        }

        work.reset();
        io.stop();
        if (reactor.joinable()) {
            try {
                if (reactor.get_id() == std::this_thread::get_id()) {
                    reactor.detach();
                } else {
                    reactor.join();
                }
            } catch (...) {
                // 一个仍 joinable 的 std::thread 无法安全析构；此处已经没有可恢复路径。
                std::terminate();
            }
        }
    }

    redis_configuration configuration;
    asio::io_context io{1};
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::thread reactor;
    std::atomic_bool closing{false};
    std::mutex pool_mutex;
    std::condition_variable pool_changed;
    std::vector<std::shared_ptr<connection_slot>> slots;
    std::size_t creating{};
    std::mutex subscriptions_mutex;
    std::vector<std::weak_ptr<subscription>> subscriptions;
};

struct subscription::implementation {
    struct start_operation {
        redis::request request;
        redis::generic_response response;
    };

    implementation(asio::io_context& owner, redis_configuration source, redis::config wire_configuration, std::shared_ptr<redis::connection> wire_connection,
                   const std::size_t queue_capacity, const std::size_t confirmations)
        : io(owner), configuration(std::move(source)), wire(std::move(wire_configuration)), connection(std::move(wire_connection)), capacity(queue_capacity),
          expected_confirmations(confirmations) {}

    void start(const std::shared_ptr<subscription>& owner, const std::vector<std::string>& channels, const std::vector<std::string>& patterns) {
        connection->set_receive_response(receive);
        arm_receive(owner);

        auto operation = std::make_shared<start_operation>();
        if (!channels.empty()) {
            operation->request.subscribe(channels);
        }
        if (!patterns.empty()) {
            operation->request.psubscribe(patterns);
        }
        const auto timeout = configuration.timeout;
        asio::post(io, [owner, operation, timeout] {
            auto& state = *owner->implementation_;
            state.connection->async_exec(
                operation->request, operation->response, asio::cancel_after(timeout, [owner, operation](const error_code status, std::size_t) {
                    auto& state = *owner->implementation_;
                    if (status) {
                        state.enqueue({subscription_item::kind::failure, {}, {}, {}, 0, transport_error(status, false)});
                    } else if (operation->response.has_error()) {
                        state.enqueue({subscription_item::kind::failure,
                                       {},
                                       {},
                                       {},
                                       0,
                                       error(code::unavailable, "redis.subscription").with_detail(operation->response.error().diagnostic)});
                    }
                }));
            state.connection->async_run(state.wire, asio::consign(asio::detached, owner));
        });
    }

    void arm_receive(const std::shared_ptr<subscription>& owner) {
        connection->async_receive2([owner](const error_code status) {
            auto& state = *owner->implementation_;
            if (state.closed.load(std::memory_order_acquire)) {
                return;
            }
            if (status) {
                state.enqueue({subscription_item::kind::failure, {}, {}, {}, 0, transport_error(status, false)});
                if (state.connection->will_reconnect()) {
                    state.arm_receive(owner);
                }
                return;
            }
            state.consume_pushes();
            state.receive.value().clear();
            state.arm_receive(owner);
        });
    }

    void consume_pushes() {
        if (receive.has_error()) {
            enqueue({subscription_item::kind::failure, {}, {}, {}, 0, error(code::protocol, "redis.subscription").with_detail(receive.error().diagnostic)});
            return;
        }
        const auto& nodes = receive.value();
        for (std::size_t index = 0; index < nodes.size();) {
            const auto root = index++;
            while (index < nodes.size() && nodes[index].depth != 0) {
                ++index;
            }
            if (nodes[root].data_type != redis::resp3::type::push || root + 1 >= index) {
                continue;
            }
            const auto& kind = nodes[root + 1].value;
            if ((kind == "subscribe" || kind == "psubscribe") && root + 2 < index) {
                const auto token = std::string(kind) + '\0' + std::string(nodes[root + 2].value);
                confirmations.insert(token);
                if (confirmations.size() == expected_confirmations) {
                    confirmations.clear();
                    enqueue({subscription_item::kind::reconnected, {}, {}, std::nullopt, 0, std::nullopt});
                }
                continue;
            }
            if (kind == "message" && root + 3 < index) {
                enqueue(
                    {subscription_item::kind::message, std::string(nodes[root + 2].value), std::string(nodes[root + 3].value), std::nullopt, 0, std::nullopt});
                continue;
            }
            if (kind == "pmessage" && root + 4 < index) {
                enqueue({subscription_item::kind::message, std::string(nodes[root + 3].value), std::string(nodes[root + 4].value),
                         std::string(nodes[root + 2].value), 0, std::nullopt});
            }
        }
    }

    void enqueue(subscription_item item) {
        std::lock_guard lock(mutex);
        if (closed.load(std::memory_order_acquire) && item.type != subscription_item::kind::closed) {
            return;
        }

        if (item.type == subscription_item::kind::message && queue.size() >= capacity) {
            if (!lagged) {
                queue.clear();
                queue.push_back({subscription_item::kind::lagged, {}, {}, std::nullopt, 0, std::nullopt});
                lagged = true;
                changed.notify_all();
            }
            return;
        }
        if (queue.size() >= capacity) {
            queue.clear();
        }
        queue.push_back(std::move(item));
        changed.notify_all();
    }

    /// 终止订阅且唤醒本地等待者；任何调度失败均在关闭边界内降级处理。
    void shutdown() noexcept {
        if (closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        {
            std::lock_guard lock(mutex);
            queue.clear();
            lagged = false;
        }
        changed.notify_all();
        try {
            asio::post(io, [wire_connection = connection] { wire_connection->cancel(); });
        } catch (...) {
            // io_context 拒绝调度时直接停止接收；Boost.Redis cancel 不分配协议数据。
            connection->cancel();
        }
    }

    asio::io_context& io;
    redis_configuration configuration;
    redis::config wire;
    std::shared_ptr<redis::connection> connection;
    redis::generic_flat_response receive;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<subscription_item> queue;
    std::size_t capacity;
    std::size_t expected_confirmations;
    std::unordered_set<std::string> confirmations;
    std::uint64_t next_fence{};
    bool lagged{false};
    std::atomic_bool closed{false};
};

driver::driver(std::unique_ptr<implementation> implementation) noexcept : implementation_(std::move(implementation)) {}

driver::~driver() noexcept {
    static_cast<void>(close());
}

result<std::shared_ptr<driver>> driver::open(const redis_configuration& configuration) {
    if (const auto status = configuration.check(); !status) {
        return std::unexpected(status.error());
    }
    if (configuration.mode == redis_mode::sentinel && configuration.tls.enabled) {
        return std::unexpected(error(code::unavailable, "redis.tls").with_detail("Boost.Redis cannot securely verify dynamic Sentinel targets"));
    }

    try {
        auto state = std::make_unique<implementation>(configuration);
        for (std::size_t index = 0; index < configuration.pool.min_connections; ++index) {
            auto slot = state->create_slot();
            if (!slot) {
                state->shutdown();
                return std::unexpected(slot.error());
            }
            state->slots.push_back(std::move(*slot));
        }
        auto output = std::shared_ptr<driver>(new driver(std::move(state)));
        auto ping = output->execute(command("PING"));
        if (!ping) {
            static_cast<void>(output->close());
            return std::unexpected(ping.error());
        }
        auto pong = ping->text();
        if (!pong || *pong != "PONG") {
            static_cast<void>(output->close());
            return std::unexpected(error(code::corrupt, "PING"));
        }
        return output;
    } catch (const std::exception& exception) {
        return std::unexpected(error(code::unavailable, "redis.client").with_detail(exception.what()));
    }
}

result<response> driver::execute(const command& value, const bool mutation) {
    auto values = implementation_->execute(std::span<const command>(&value, 1), mutation);
    if (!values) {
        return std::unexpected(values.error());
    }
    return std::move(values->front());
}

result<std::vector<response>> driver::execute(const std::span<const command> values, const bool mutation) {
    if (!implementation_) {
        return std::unexpected(error(code::closed));
    }
    return implementation_->execute(values, mutation);
}

result<std::shared_ptr<subscription>> driver::subscribe(std::vector<std::string> channels, std::vector<std::string> patterns, const std::size_t capacity) {
    if (!implementation_ || !open()) {
        return std::unexpected(error(code::closed));
    }
    if ((channels.empty() && patterns.empty()) || capacity == 0 || std::ranges::any_of(channels, [](const auto& value) { return value.empty(); }) ||
        std::ranges::any_of(patterns, [](const auto& value) { return value.empty(); })) {
        return std::unexpected(error(code::invalid, "redis.subscription"));
    }

    auto connection = make_connection(implementation_->io, implementation_->configuration);
    if (!connection) {
        return std::unexpected(connection.error());
    }
    auto state = std::make_unique<subscription::implementation>(implementation_->io, implementation_->configuration,
                                                                make_wire_configuration(implementation_->configuration), std::move(*connection), capacity,
                                                                channels.size() + patterns.size());
    auto output = std::shared_ptr<subscription>(new subscription(shared_from_this(), std::move(state)));
    {
        std::lock_guard lock(implementation_->subscriptions_mutex);
        implementation_->subscriptions.erase(
            std::remove_if(implementation_->subscriptions.begin(), implementation_->subscriptions.end(), [](const auto& value) { return value.expired(); }),
            implementation_->subscriptions.end());
        implementation_->subscriptions.push_back(output);
    }
    output->implementation_->start(output, channels, patterns);
    return output;
}

result<void> driver::close() noexcept {
    if (!implementation_) {
        return {};
    }
    {
        std::lock_guard lock(implementation_->subscriptions_mutex);
        for (const auto& weak : implementation_->subscriptions) {
            if (auto value = weak.lock()) {
                static_cast<void>(value->close());
            }
        }
        implementation_->subscriptions.clear();
    }
    implementation_->shutdown();
    return {};
}

bool driver::open() const noexcept {
    return implementation_ && !implementation_->closing.load(std::memory_order_acquire);
}

std::chrono::milliseconds driver::timeout() const noexcept {
    return implementation_ ? implementation_->configuration.timeout : std::chrono::milliseconds::zero();
}

const redis_configuration& driver::configuration() const noexcept {
    return implementation_->configuration;
}

subscription::subscription(std::shared_ptr<driver> owner, std::unique_ptr<implementation> implementation) noexcept
    : owner_(std::move(owner)), implementation_(std::move(implementation)) {}

subscription::~subscription() noexcept {
    static_cast<void>(close());
}

subscription_item subscription::next(const std::stop_token& stop) {
    if (!implementation_) {
        return {subscription_item::kind::closed, {}, {}, std::nullopt, 0, std::nullopt};
    }
    auto& state = *implementation_;
    std::unique_lock lock(state.mutex);
    if (!state.changed.wait(lock, stop, [&state] { return !state.queue.empty() || state.closed.load(std::memory_order_acquire); })) {
        return {subscription_item::kind::closed, {}, {}, std::nullopt, 0, std::nullopt};
    }
    if (state.queue.empty()) {
        return {subscription_item::kind::closed, {}, {}, std::nullopt, 0, std::nullopt};
    }
    auto output = std::move(state.queue.front());
    state.queue.pop_front();
    if (output.type == subscription_item::kind::lagged) {
        state.lagged = false;
    }
    return output;
}

subscription_item subscription::next(const std::stop_token& stop, const std::chrono::milliseconds wait) {
    if (!implementation_) {
        return {subscription_item::kind::closed, {}, {}, std::nullopt, 0, std::nullopt};
    }
    if (wait <= std::chrono::milliseconds::zero()) {
        return {subscription_item::kind::idle, {}, {}, std::nullopt, 0, std::nullopt};
    }
    auto& state = *implementation_;
    std::unique_lock lock(state.mutex);
    const auto deadline = std::chrono::steady_clock::now() + wait;
    if (!state.changed.wait_until(lock, stop, deadline, [&state] { return !state.queue.empty() || state.closed.load(std::memory_order_acquire); })) {
        return {subscription_item::kind::idle, {}, {}, std::nullopt, 0, std::nullopt};
    }
    if (state.queue.empty()) {
        return {subscription_item::kind::closed, {}, {}, std::nullopt, 0, std::nullopt};
    }
    auto output = std::move(state.queue.front());
    state.queue.pop_front();
    if (output.type == subscription_item::kind::lagged) {
        state.lagged = false;
    }
    return output;
}

result<std::uint64_t> subscription::fence() {
    if (!implementation_) {
        return std::unexpected(error(code::closed));
    }
    auto owner = shared_from_this();
    auto& state = *implementation_;
    std::uint64_t identifier{};
    {
        std::lock_guard lock(state.mutex);
        if (state.closed.load(std::memory_order_acquire)) {
            return std::unexpected(error(code::closed));
        }
        identifier = ++state.next_fence;
    }

    struct fence_operation {
        redis::request request;
        redis::response<std::string> response;
        std::promise<error_code> completed;
    };
    auto operation = std::make_shared<fence_operation>();
    operation->request.push("PING", "verdandi-fence-" + std::to_string(identifier));
    auto completed = operation->completed.get_future();
    asio::post(state.io, [owner, identifier, operation] {
        auto& state = *owner->implementation_;
        state.connection->async_exec(operation->request, operation->response,
                                     asio::cancel_after(state.configuration.timeout, [owner, identifier, operation](const error_code status, std::size_t) {
                                         auto& state = *owner->implementation_;
                                         if (status) {
                                             operation->completed.set_value(status);
                                             return;
                                         }
                                         asio::post(state.io, [owner, identifier, operation] {
                                             owner->implementation_->enqueue({subscription_item::kind::fence, {}, {}, std::nullopt, identifier, std::nullopt});
                                             operation->completed.set_value({});
                                         });
                                     }));
    });

    if (completed.wait_for(state.configuration.timeout + std::chrono::milliseconds{100}) != std::future_status::ready) {
        asio::post(state.io, [connection = state.connection] { connection->cancel(); });
        return std::unexpected(error(code::deadline, "redis.subscription.fence"));
    }
    const auto status = completed.get();
    if (status) {
        return std::unexpected(transport_error(status, false));
    }
    return identifier;
}

result<void> subscription::close() noexcept {
    if (!implementation_) {
        return {};
    }
    implementation_->shutdown();
    return {};
}

result<std::uint64_t> parse_unsigned(const std::string_view value, const std::string_view field, const bool allow_zero) {
    if (value.empty() || value.size() > 16 || (value.size() > 1 && value.front() == '0')) {
        return std::unexpected(error(code::corrupt, std::string(field)));
    }
    std::uint64_t output{};
    const auto [end, status] = std::from_chars(value.data(), value.data() + value.size(), output);
    if (status != std::errc{} || end != value.data() + value.size() || output > protocol_integer_max || (!allow_zero && output == 0)) {
        return std::unexpected(error(code::corrupt, std::string(field)));
    }
    return output;
}

result<std::vector<std::pair<std::string_view, std::string_view>>> named_pairs(const response& value) {
    if (value.type != response::kind::array && value.type != response::kind::map) {
        return std::unexpected(error(code::corrupt, "redis.response"));
    }
    if (value.children.size() % 2 != 0) {
        return std::unexpected(error(code::corrupt, "redis.response"));
    }

    std::vector<std::pair<std::string_view, std::string_view>> output;
    output.reserve(value.children.size() / 2);
    std::unordered_set<std::string_view> names;
    names.reserve(value.children.size() / 2);
    for (std::size_t index = 0; index < value.children.size(); index += 2) {
        auto name = value.children[index].text();
        auto member = value.children[index + 1].text();
        if (!name || !member || !names.insert(*name).second) {
            return std::unexpected(error(code::corrupt, "redis.response"));
        }
        output.emplace_back(*name, *member);
    }
    return output;
}

} // namespace verdandi::detail
