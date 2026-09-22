#include "core.hpp"
#include "beaming.hpp"
#include "observing.hpp"
#include "publishing.hpp"
#include "reading.hpp"
#include "roots.hpp"
#include "subscribing.hpp"
#include <algorithm>
#include <astra/config.hpp>
#include <astra/scope.hpp>
#include <fstream>
#include <grpc/support/time.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/resource_quota.h>
#include <grpcpp/security/tls_certificate_provider.h>
#include <grpcpp/security/tls_credentials_options.h>
#include <limits>
#include <set>

namespace comet::detail {
namespace {
// 只标识本线程正在调用用户观察者, 不保存对象指针或全局 Client 状态.
thread_local bool callback{};

// 外部 CA 文件有界且仅接受普通文件, 不通过 SDK 安装证书或下载根材料.
Result<std::string> certificate(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error || std::filesystem::file_size(path, error) > 1024 * 1024 || error) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    std::ifstream file(path, std::ios::binary);
    std::string bytes(1024 * 1024 + 1, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (file.bad() || (!file.eof() && file.fail()) || file.gcount() == 0 || file.gcount() > 1024 * 1024) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    bytes.resize(static_cast<std::size_t>(file.gcount()));
    if (bytes.contains("PRIVATE KEY")) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    return bytes;
}
} // namespace

class Core::Login final : public grpc::ClientReadReactor<proto::comet::v1::SessionReply> {
public:
    // owner 在途保活, secret 是此尝试捕获的不可变值, 不受 Client::secret 修改.
    Login(std::shared_ptr<Core> owner, std::shared_ptr<Binding> binding, Value secret, Time deadline) : owner_(std::move(owner)), binding_(std::move(binding)), secret_(std::move(secret)), deadline_(deadline), stub_(proto::comet::v1::Gateway::NewStub(binding_->streams)) {
        request_.set_key(owner_->options_.key);
        request_.set_secret(secret_->data(), secret_->size());
    }

    // Core 已持有本对象后才启动, 不给长期 Session 设置错误的整体三秒 deadline.
    void start() {
        const std::lock_guard lock(mutex_); // secret()/close 的取消可能在安排调用后立即到达.
        stub_->async()->Session(&context_, &request_, this);
        AddHold();
        held_ = true;
        StartRead(&reply_);
        StartCall();
    }

    void cancel() { // Set/Cancel 与回调在本对象的 I/O 锁下定序, 不在 Core 锁下调用.
        const std::lock_guard lock(mutex_);
        context_.TryCancel();
        release();
    }

    void OnReadDone(bool ok) override { // 只报告完整消息/EOF, 校验与连接状态更新留给唯一 Core 控制轮.
        const std::lock_guard lock(mutex_);
        available_ = ok;
        ended_ = !ok;
        owner_->wake();
    }

    void OnDone(const grpc::Status& status) override { // context 由本对象拥有, metadata 可在控制轮读取.
        const std::lock_guard lock(mutex_);
        code_ = status.error_code();
        done_ = true;
        owner_->wake();
    }

private:
    friend class Core;

    void release() {
        if (held_) {
            held_ = false;
            RemoveHold();
        }
    } // 已持 I/O 锁, 精确归还一次控制 hold.

    const std::shared_ptr<Core> owner_;                     // 后台引用不计作应用拥有者, 最后应用 close 仍会取消本 RPC.
    const std::shared_ptr<Binding> binding_;                // 尚未公开的本次候选绑定.
    const Value secret_;                                    // 用指针身份识别已经被外部换密替代的尝试, 不增加协议代次.
    const Time deadline_;                                   // 只用于首次确认, confirmed 后不再执行这个超时.
    std::unique_ptr<proto::comet::v1::Gateway::Stub> stub_; // 只使用长期流 Channel.
    grpc::ClientContext context_;                           // 独占 metadata 和取消状态.
    proto::comet::v1::SessionRequest request_;              // 本次独立请求, 包括冻结 SECRET.
    proto::comet::v1::SessionReply reply_;                  // 恰好一个有效确认, 第二条成功消息视为协议错误.
    std::mutex mutex_;                                      // 网络回调与控制轮对消息、hold 和最终状态的边界.
    bool held_{};                                           // 最终完成前保有的外部控制 hold.
    bool available_{};                                      // 一份未消费的完整确认.
    bool ended_{};                                          // Read 已返回 EOF, 需要控制轮归还 hold.
    bool done_{};                                           // OnDone 已到达, 可以释放本对象和恢复额度.
    bool confirmed_{};                                      // 曾经得到过完整合法确认, 与首次登录被拒绝区分.
    grpc::StatusCode code_ = grpc::StatusCode::UNKNOWN;     // 只由 OnDone 发布真实最终状态.
};

Core::Core(Client::Options options, std::shared_ptr<grpc::ChannelCredentials> credentials, Value secret) : options_(std::move(options)), credentials_(std::move(credentials)), secret_(std::move(secret)) {}

Result<std::shared_ptr<Core>> Core::prepare(Client::Options options) {

    const auto invalid = [] { return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}}); };
    if (options.endpoints.empty() || options.endpoints.size() > 64 || options.readers == 0 || options.readers > 4096 || options.beacons == 0 || options.beacons > 65536 || options.bytes < 16384 || options.bytes > 4ULL * 1024 * 1024 * 1024 || options.timeout.count() <= 0 || options.timeout > std::chrono::minutes(1)) {
        return invalid();
    }
    if (options.auth ? (!astra::Scope::text(options.key, 128) || options.secret.empty() || options.secret.size() > 4096) : (!options.key.empty() || !options.secret.empty())) {
        return invalid();
    }
    if (!options.tls && !options.ca.empty()) {
        return invalid();
    }
    std::set<std::string> endpoints; // 冷启动去重与规范化, 不作为运行时逐 RPC 地址选择器.
    for (auto& endpoint : options.endpoints) {
        auto normalized = astra::Config::format_supervisor(endpoint);
        if (!normalized || !endpoints.insert(*normalized).second) {
            return invalid();
        }
        endpoint = std::move(*normalized);
    }

    std::shared_ptr<grpc::ChannelCredentials> credentials;
    if (options.tls) {
        auto ca = options.ca.empty() ? Result<std::string>(std::string(roots())) : certificate(options.ca);
        if (!ca) {
            return std::unexpected(ca.error());
        }
        grpc::experimental::TlsChannelCredentialsOptions tls;
        if (!ca->empty()) {
            auto provider = std::make_shared<grpc::experimental::InMemoryCertificateProvider>();
            if (!provider->UpdateRoot(std::move(*ca)).ok() || !provider->ValidateCredentials().ok()) {
                return invalid();
            }
            tls.set_root_certificate_provider(std::move(provider));
        }
        tls.set_verify_server_certs(true);
        tls.set_check_call_host(true);
        tls.set_min_tls_version(TLS1_3);
        tls.set_max_tls_version(TLS1_3);
        credentials = grpc::experimental::TlsCredentials(tls);
    } else {
        credentials = grpc::InsecureChannelCredentials();
    }
    if (!credentials) {
        return invalid();
    }
    auto secret = std::make_shared<const std::vector<std::uint8_t>>(std::move(options.secret)); // Options 不再保留第二份密码正文.
    return std::shared_ptr<Core>(new Core(std::move(options), std::move(credentials), std::move(secret)));
}

std::shared_ptr<Binding> Core::connect() const {

    auto binding = std::make_shared<Binding>(); // 只有这一个当前端点的两种传输, 不按 Watch 数扩大连接池.
    binding->endpoint = options_.endpoints[endpoint_];
    grpc::ChannelArguments arguments;
    arguments.SetInt("grpc.use_local_subchannel_pool", 1);
    arguments.SetInt("grpc.enable_retries", 0);
    arguments.SetInt("grpc.keepalive_time_ms", 60000);
    arguments.SetInt("grpc.keepalive_timeout_ms", 20000);
    arguments.SetInt("grpc.keepalive_permit_without_calls", 0);
    arguments.SetMaxReceiveMessageSize(8 * 1024 * 1024);
    arguments.SetMaxSendMessageSize(8 * 1024 * 1024);
    grpc::ResourceQuota quota; // 解码与传输另计, 不只限制业务状态里的 Buffer 大小.
    quota.Resize(32 * 1024 * 1024);
    arguments.SetResourceQuota(quota);
    binding->streams = grpc::CreateCustomChannel(binding->endpoint, credentials_, arguments);
    binding->unary = grpc::CreateCustomChannel(binding->endpoint, credentials_, arguments);
    binding->ephemeris = proto::comet::v1::Ephemeris::NewStub(binding->unary);
    binding->catalog = proto::comet::v1::Catalog::NewStub(binding->unary);
    return binding;
}

void Core::schedule(Time time) {
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(std::max(time - std::chrono::steady_clock::now(), Time::duration::zero())).count();
    scheduled_ = true;
    alarm_.Set(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_nanos(remaining, GPR_TIMESPAN)), [owner = shared_from_this()](bool) mutable {
        // Alarm 在触发后仍保存闭包. 先把本轮所有权移出, 避免最后一轮形成 Core -> Alarm -> Core 引用环.
        // active 覆盖整个 tick, 后续重新 Set 即使替换旧闭包, 也不会提前销毁本轮所属对象.
        const auto active = std::move(owner);
        active->tick();
    });
}

void Core::start() {
    const std::lock_guard lock(mutex_);
    schedule(std::chrono::steady_clock::now());
}

void Core::wake() noexcept {
    const std::lock_guard lock(mutex_);
    awakened_ = true;
    if (scheduled_) {
        alarm_.Cancel();
    }
}

void Core::release() noexcept {
    const std::lock_guard lock(mutex_);
    if (owners_ != 0 && --owners_ == 0) {
        closing_ = true;
        stopped_.store(true, std::memory_order_release);
    }
    awakened_ = true;
    if (scheduled_) {
        alarm_.Cancel();
    }
}

void Core::close() noexcept {
    const std::lock_guard lock(mutex_);
    stopped_.store(true, std::memory_order_release);
    closing_ = true;
    awakened_ = true;
    if (scheduled_) {
        alarm_.Cancel();
    }
}

bool Core::wait(std::chrono::milliseconds timeout) const {
    if (notifying()) {
        throw std::logic_error("Cannot wait inside a Comet callback");
    }
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, timeout, [this] { return complete_; });
}

bool Core::notifying() noexcept {
    return callback;
}

void Core::notify(bool active) noexcept {
    callback = active;
}

bool Core::notification() const noexcept {
    const std::lock_guard lock(mutex_);
    return !closing_; // 该锁的取得是通知开始点; 后来的 close 只等待其自然结束, 不撤回已开始回调.
}

void Core::exception() noexcept {
    auto count = exceptions_.load(std::memory_order_relaxed); // 诊断不参与数据发布排序, 到 uint64 上限停止增长.
    while (count != std::numeric_limits<std::uint64_t>::max() && !exceptions_.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
    }
}

std::uint64_t Core::exceptions() const noexcept {
    return exceptions_.load(std::memory_order_relaxed);
}

Core::Time Core::session(Time now) {

    std::shared_ptr<Login> login; // 独立保有在途对象, 不在 Core 锁下取得 Login I/O 锁.
    Value secret;
    bool closing;
    std::shared_ptr<const Binding> binding;
    {
        const std::lock_guard lock(mutex_);
        login = login_;
        secret = secret_;
        closing = closing_ && std::ranges::all_of(readers_, [](const auto& weak) { const auto object = weak.lock(); return !object || object->finished(); });
        binding = binding_;
    }
    if (!login) {
        return Time::max();
    }

    const std::lock_guard io(login->mutex_);
    const bool obsolete = !login->confirmed_ && login->secret_ != secret;  // 已更换密码的晚到首次确认不能覆盖新尝试; 已确认 Session 保持有效.
    const bool detached = login->confirmed_ && binding != login->binding_; // 业务失败已触发共享恢复, 不能重复轮转端点.
    if (closing || obsolete || detached || (!login->confirmed_ && now >= login->deadline_)) {
        login->context_.TryCancel();
        login->release();
    }

    if (login->available_ && !closing && !obsolete && !detached && (login->confirmed_ || now < login->deadline_)) {
        login->available_ = false;
        const auto& reply = login->reply_; // 首次确认必须给出完整令牌与身份, 不接受第二次确认改变流所属身份.
        const bool valid = !login->confirmed_ && reply.session().size() == 32 && astra::Scope::text(reply.instance(), 128);
        {
            const std::lock_guard lock(mutex_);
            if (!closing_ && secret_ == login->secret_) {
                if (valid) {
                    login->binding_->instance = reply.instance();
                    login->binding_->session = reply.session();
                    login->confirmed_ = true;
                    binding_ = login->binding_; // 两个完整字符串写入后才公开不可变绑定.
                    blocked_.reset();
                } else {
                    binding_.reset();
                    blocked_ = Error{Error::Code::protocol, Error::Effect::unapplied, {}, {}, {}};
                }
            }
        }
        if (valid) {
            login->reply_.Clear();
            login->StartRead(&login->reply_); // 长流继续读 EOF, 不靠定期空闲探测或 Release RPC 清理.
        } else {
            login->context_.TryCancel();
            login->release();
        }
    }
    if (login->ended_) {
        login->release();
    }
    if (!login->done_) {
        return login->confirmed_ || closing || obsolete || detached || now >= login->deadline_ ? Time::max() : login->deadline_;
    }

    // OnDone 后才能消费 trailing metadata 并释放会话名额. 已确认流被撤销允许重新登录,
    // 只有新登录明确拒绝才暂停等待更新 SECRET, 防止撤销事件直接锁死有效账号.
    const auto error = failure(grpc::Status(login->code_, ""), login->context_);
    const std::lock_guard lock(mutex_);
    if (login_ != login) {
        return Time::max();
    }
    login_.reset();
    if (binding_ == login->binding_) {
        binding_.reset();
    }
    if (closing_ || secret_ != login->secret_ || detached || blocked_) {
        return retry_;
    }
    if (!login->confirmed_ && (error.code == Error::Code::session || error.code == Error::Code::input)) {
        blocked_ = error;
        return Time::max();
    }
    if ((error.code == Error::Code::transport || error.code == Error::Code::timeout || login->code_ == grpc::StatusCode::CANCELLED) && !(login->confirmed_ && login->code_ == grpc::StatusCode::CANCELLED)) {
        connecting_.reset();
        endpoint_ = (endpoint_ + 1) % options_.endpoints.size();
    }
    failures_ = std::min(failures_ + 1, 32U);
    retry_ = now + delay(failures_);
    return retry_;
}

void Core::recovered(const std::shared_ptr<const Binding>& binding) {
    const std::lock_guard lock(mutex_);
    if (binding_ == binding && !closing_) {
        failures_ = 0;
    }
}

Core::Time Core::dial(Time now) {

    std::shared_ptr<Binding> connecting; // Channel 创建在锁外完成; 只有本控制轮选择下一端点.
    {
        const std::lock_guard lock(mutex_);
        if (closing_ || blocked_ || login_ || binding_) {
            return Time::max();
        }
        if (now < retry_) {
            return retry_;
        }
        connecting = connecting_;
    }
    if (!connecting) {
        connecting = connect();
        const std::lock_guard lock(mutex_);
        if (closing_) {
            return Time::max();
        }
        connecting_ = connecting;
        connecting_until_ = now + options_.timeout;
    }

    const auto state = connecting->streams->GetState(true); // 非阻塞推进连接, 不占 gRPC callback 线程等待 TCP/TLS.
    if (state != GRPC_CHANNEL_READY) {
        const std::lock_guard lock(mutex_);
        if (now >= connecting_until_) {
            connecting_.reset();
            endpoint_ = (endpoint_ + 1) % options_.endpoints.size();
            failures_ = std::min(failures_ + 1, 32U);
            retry_ = now + delay(failures_);
            return retry_;
        }
        return std::min(connecting_until_, now + std::chrono::milliseconds(20));
    }

    std::shared_ptr<Login> login;
    {
        const std::lock_guard lock(mutex_);
        if (closing_ || blocked_ || login_ || binding_) {
            return Time::max();
        }
        if (!options_.auth) {
            binding_ = connecting;
            return Time::max();
        }
        auto binding = std::make_shared<Binding>(*connecting); // 只共享 Channel, 不改写旧 Watch 仍持有的已确认 Session 字符串.
        login = std::make_shared<Login>(shared_from_this(), std::move(binding), secret_, now + options_.timeout);
        login_ = login; // 先由 Core 保活, 后在锁外让 this 对 gRPC 可见.
    }
    login->start();
    return now + options_.timeout;
}

void Core::tick() {

    // Alarm 的回调只用于一条控制路径. wake 在本轮执行时合并为 awakened_, 不并发进入另一轮.
    {
        const std::lock_guard lock(mutex_);
        scheduled_ = false;
        running_ = true;
        awakened_ = false;
    }
    auto next = std::chrono::steady_clock::now() + std::chrono::seconds(1); // 空闲兜底, 实际网络事件会立即取消 Alarm 唤醒.
    bool finished = false;
    try {
        const auto now = std::chrono::steady_clock::now(); // 本轮统一单调时间, 不读 Pulsar 或系统墙钟.
        next = std::min(next, session(now));
        next = std::min(next, dial(now));
        std::vector<std::shared_ptr<Activity>> readers; // 最多两倍配置对象数, 防止已关闭但未清理对象无界积累.
        {
            const std::lock_guard lock(mutex_);
            std::erase_if(readers_, [](const auto& weak) { const auto reader = weak.lock(); return !reader || reader->finished(); });
            readers.reserve(readers_.size());
            for (const auto& weak : readers_) {
                if (auto reader = weak.lock()) {
                    readers.push_back(std::move(reader));
                }
            }
        }
        // 清理/自动保活先推进, 再处理可能触发用户回调的 Watch; 不创建另一套线程池或任务队列.
        std::ranges::partition(readers, [](const auto& object) { return !object->streaming(); });
        for (const auto& reader : readers) {
            std::shared_ptr<const Binding> binding;
            std::optional<Error> error;
            bool closing;
            {
                const std::lock_guard lock(mutex_);
                // 前一个 Reader 可能已经报告全局会话失效, 后续对象不能使用轮首的陈旧绑定.
                binding = binding_;
                error = blocked_;
                closing = closing_;
            }
            next = std::min(next, reader->poll(now, binding, error, closing));
        }
        {
            const std::lock_guard lock(mutex_);
            // 回调期间工厂可能接纳了本轮快照之外的新对象. 必须检查实际目录, 否则 close
            // 可能在这些对象尚未清理时错误地完成, 使它们的 wait 永久等不到通知.
            const bool cleaned = std::ranges::all_of(readers_, [](const auto& weak) { const auto reader = weak.lock(); return !reader || reader->finished(); });
            finished = closing_ && !login_ && calls_.load(std::memory_order_relaxed) == 0 && unary_.load(std::memory_order_relaxed) == 0 && maintenance_.load(std::memory_order_relaxed) == 0 && recovery_.load(std::memory_order_relaxed) == 0 && admitted_.load(std::memory_order_relaxed) == 0 && cleaned;
            if (finished) {
                connecting_.reset();
                binding_.reset();
                readers_.clear();
                complete_ = true;
                condition_.notify_all();
            }
        }
    } catch (...) {
        // 内部准备失败不能越过 gRPC callback 边界. 停止接纳并在后续控制轮继续取消和归还,
        // 不把仍有 RPC 的异常路径伪报为完成; 持续无法分配时也不承诺能够恢复业务.
        close();
        next = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    }

    const std::lock_guard lock(mutex_);
    running_ = false;
    if (!finished) {
        schedule(awakened_ ? std::chrono::steady_clock::now() : next);
    }
}

std::chrono::milliseconds Core::delay(unsigned failures) const noexcept {
    const auto base = std::min<std::uint64_t>(5000, std::uint64_t{100} << std::min(failures, 6U));
    const auto jitter = std::hash<std::string>{}(options_.endpoints[endpoint_]) % (base / 4 + 1);
    return std::chrono::milliseconds(std::min<std::uint64_t>(5000, base + jitter));
}

Result<std::shared_ptr<Reading>> Core::reader(Scope scope, std::string target, Reader::Options options) {

    const astra::Scope address{scope.sector, scope.spectrum}; // 只复用文本验证, 不把服务端 Scope 类型暴露在公共 ABI.
    if (!address.valid() || address.internal() || (!target.empty() && !astra::Scope::text(target, 1024)) || options.bytes < 16384 || options.bytes > 1024ULL * 1024 * 1024 || options.records == 0 || options.records > 65536) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    auto reading = std::make_shared<Reading>(shared_from_this(), std::move(scope), std::move(target), std::move(options));
    auto accepted = accept(reading); // 未接纳对象不发 RPC, 失败析构只释放准备资源.
    if (!accepted) {
        return std::unexpected(accepted.error());
    }
    return reading;
}

Result<std::shared_ptr<Subscribing>> Core::subscriber(Scope scope, std::string target, Subscriber::Options options) {

    const astra::Scope address{scope.sector, scope.spectrum}; // 只复用文本验证, 不把服务端 Scope 类型暴露在公共 ABI.
    if (!address.valid() || address.internal() || (!target.empty() && !astra::Scope::text(target, 1024)) || options.bytes < 16384 || options.bytes > 1024ULL * 1024 * 1024 || options.records == 0 || options.records > 65536) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    auto reading = std::make_shared<Subscribing>(shared_from_this(), std::move(scope), std::move(target), std::move(options));
    auto accepted = accept(reading); // 未接纳对象不发 RPC, 失败析构只释放准备资源.
    if (!accepted) {
        return std::unexpected(accepted.error());
    }
    return reading;
}

Result<std::shared_ptr<Observing>> Core::observer(Scope scope, std::string target, Observer::Options options) {

    const astra::Scope address{scope.sector, scope.spectrum}; // 只复用文本验证, 不把服务端 Scope 类型暴露在公共 ABI.
    if (!address.valid() || address.internal() || (!target.empty() && !Selection::valid(target)) || options.bytes < 16384 || options.bytes > 1024ULL * 1024 * 1024 || options.records == 0 || options.records > 65536) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    auto reading = std::make_shared<Observing>(shared_from_this(), std::move(scope), std::move(target), std::move(options));
    auto accepted = accept(reading); // 未接纳对象不发 RPC, 失败析构只释放准备资源.
    if (!accepted) {
        return std::unexpected(accepted.error());
    }
    return reading;
}

Result<std::shared_ptr<Beaming>> Core::beacon(Scope scope, Value attr, Value data, std::chrono::milliseconds ttl, Beacon::Options options) {

    const astra::Scope address{scope.sector, scope.spectrum};
    if (!address.valid() || address.internal() || !attr || !data || attr->size() > 1024 * 1024 || data->size() > 1024 * 1024 || ttl < std::chrono::seconds(1) || ttl > std::chrono::minutes(10)) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    std::shared_ptr<Beaming> object; // 构造时的共享预算不足按接纳失败处理, 不修改其他对象.
    try {
        object = std::make_shared<Beaming>(shared_from_this(), std::move(scope), std::move(attr), std::move(data), ttl, std::move(options));
    } catch (const std::length_error&) {
        return std::unexpected(Error{Error::Code::busy, Error::Effect::unapplied, {}, {}, {}});
    }
    auto accepted = accept(object);
    if (!accepted) {
        return std::unexpected(accepted.error());
    }
    return object;
}

Result<std::shared_ptr<Publishing>> Core::publisher(Scope scope, std::string key, std::chrono::milliseconds ttl, Publisher::Options options) {

    const astra::Scope address{scope.sector, scope.spectrum};
    if (!address.valid() || address.internal() || !astra::Scope::text(key, 1024) || ttl < std::chrono::seconds(1) || ttl > std::chrono::minutes(10)) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    auto object = std::make_shared<Publishing>(shared_from_this(), std::move(scope), std::move(key), ttl, std::move(options));
    auto accepted = accept(object);
    if (!accepted) {
        return std::unexpected(accepted.error());
    }
    return object;
}

bool Core::outgoing(bool priority, bool automatic) noexcept {
    auto& counter = priority ? maintenance_ : automatic ? recovery_
                                                        : unary_; // 后台恢复不占尽保活 16 槽.
    const std::size_t maximum = priority ? 16 : automatic ? 48
                                                          : 256;
    auto count = counter.load(std::memory_order_relaxed);
    while (count < maximum) {
        if (counter.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void Core::returning(bool priority, bool automatic) noexcept {
    (priority ? maintenance_ : automatic ? recovery_
                                         : unary_)
        .fetch_sub(1, std::memory_order_relaxed);
    wake(); // 真正 callback 释放时才归还, 取消不提前释放容量.
}

bool Core::admitting() noexcept {
    auto count = admitted_.load(std::memory_order_relaxed);
    while (count < 256) {
        if (admitted_.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void Core::settled() noexcept {
    admitted_.fetch_sub(1, std::memory_order_relaxed);
}

Result<void> Core::accept(const std::shared_ptr<Activity>& activity) {

    const std::lock_guard lock(mutex_);
    if (closing_) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    std::erase_if(readers_, [](const auto& weak) { const auto reader = weak.lock(); return !reader || reader->finished(); });
    std::size_t count{};    // 关闭尚未清理对象不占应用额度, 其实际 RPC 仍由 calls_ 计费.
    std::size_t retained{}; // 本类已关闭但实际尚未清理对象同样有独立上限.
    for (const auto& reader : readers_) {
        if (const auto owned = reader.lock(); owned && owned->streaming() == activity->streaming()) {
            ++retained;
            count += static_cast<std::size_t>(!owned->closed());
        }
    }
    if (count >= (activity->streaming() ? options_.readers : options_.beacons) || retained >= 2 * (activity->streaming() ? options_.readers : options_.beacons)) {
        return std::unexpected(Error{Error::Code::busy, Error::Effect::unapplied, {}, {}, {}});
    }
    readers_.push_back(activity);
    ++owners_;
    awakened_ = true;
    if (scheduled_) {
        alarm_.Cancel();
    }
    return {};
}

Result<void> Core::secret(std::vector<std::uint8_t> value) {

    if (!options_.auth || value.empty() || value.size() > 4096) {
        return std::unexpected(Error{Error::Code::input, Error::Effect::unapplied, {}, {}, {}});
    }
    auto secret = std::make_shared<const std::vector<std::uint8_t>>(std::move(value));
    std::shared_ptr<Login> login;
    {
        const std::lock_guard lock(mutex_);
        if (closing_) {
            return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
        }
        secret_ = std::move(secret);
        blocked_.reset();
        if (!binding_) {
            login = login_; // 只有未确认登录需要取消, 当前有效身份按服务器撤销或断链结束.
        }
        retry_ = Time{};
    }
    if (login) {
        login->cancel();
    }
    wake();
    return {};
}

bool Core::claim() noexcept {
    auto count = calls_.load(std::memory_order_relaxed);
    while (count < options_.readers) {
        if (calls_.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void Core::relinquish() noexcept {
    calls_.fetch_sub(1, std::memory_order_relaxed);
}

bool Core::resize(std::size_t previous, std::size_t requested) noexcept {
    auto total = bytes_.load(std::memory_order_relaxed);
    do {
        if (total < previous || requested > options_.bytes - (total - previous)) {
            return false;
        }
    } while (!bytes_.compare_exchange_weak(total, total - previous + requested, std::memory_order_relaxed));
    return true;
}

void Core::lost(const std::shared_ptr<const Binding>& binding, Error error) {

    std::shared_ptr<Login> login;
    {
        const std::lock_guard lock(mutex_);
        if (binding_ != binding || closing_) {
            return;
        }
        binding_.reset();
        login = login_;
        if (!options_.auth && error.code == Error::Code::session) {
            blocked_ = error; // 匿名配置被服务端拒绝, 不循环重建相同匿名绑定或偷偷改成认证登录.
        }
        if (error.code == Error::Code::transport) {
            connecting_.reset();
            endpoint_ = (endpoint_ + 1) % options_.endpoints.size();
        }
        failures_ = std::min(failures_ + 1, 32U);
        retry_ = std::chrono::steady_clock::now() + delay(failures_);
    }
    if (login) {
        login->cancel();
    }
    wake();
}

Error Core::failure(const grpc::Status& status, const grpc::ClientContext& context) {

    Error error; // 没有可信细节时, 网络关闭不能证明任何写入未提交.
    switch (status.error_code()) {
    case grpc::StatusCode::UNAUTHENTICATED:
        error.code = Error::Code::session;
        break;
    case grpc::StatusCode::INVALID_ARGUMENT:
        error.code = Error::Code::input;
        break;
    case grpc::StatusCode::RESOURCE_EXHAUSTED:
        error.code = Error::Code::busy;
        break;
    case grpc::StatusCode::DEADLINE_EXCEEDED:
        error.code = Error::Code::timeout;
        break;
    default:
        error.code = Error::Code::transport;
        break;
    }
    const auto& metadata = context.GetServerTrailingMetadata();
    const auto range = metadata.equal_range("comet-error-bin");
    if (range.first == range.second || std::next(range.first) != range.second || range.first->second.size() > 4096) {
        return error;
    }
    proto::comet::v1::Failure failure;
    if (!failure.ParseFromArray(range.first->second.data(), static_cast<int>(range.first->second.size()))) {
        return error;
    }
    switch (failure.reason()) {
    case proto::comet::v1::REASON_DENIED: // 公共固定边界拒绝, 不能因未识别而按传输失败无限重试.
    case proto::comet::v1::REASON_INPUT:
        error.code = Error::Code::input;
        break;
    case proto::comet::v1::REASON_SESSION:
        error.code = Error::Code::session;
        break;
    case proto::comet::v1::REASON_VERSION:
        error.code = Error::Code::version;
        break;
    case proto::comet::v1::REASON_ENDED:
        error.code = Error::Code::ended;
        break;
    case proto::comet::v1::REASON_OBSOLETE:
        error.code = Error::Code::obsolete;
        break;
    case proto::comet::v1::REASON_HISTORY:
        error.code = Error::Code::history;
        break;
    case proto::comet::v1::REASON_LIMIT:
        error.code = Error::Code::limit;
        break;
    case proto::comet::v1::REASON_BUSY:
        error.code = Error::Code::busy;
        break;
    case proto::comet::v1::REASON_INSTANCE:
        error.code = Error::Code::instance;
        break;
    case proto::comet::v1::REASON_CLOCK:
        error.code = Error::Code::clock;
        break;
    default:
        return error;
    }
    if (failure.effect() == proto::comet::v1::EFFECT_UNAPPLIED) {
        error.effect = Error::Effect::unapplied;
    }
    if (astra::Scope::text(failure.instance(), 128)) {
        error.instance = failure.instance();
    }
    if (failure.has_version()) {
        error.version = failure.version();
    }
    if (failure.has_retry_ms()) {
        error.retry = std::chrono::milliseconds(std::min(failure.retry_ms(), 5000U));
    }
    return error;
}
} // namespace comet::detail

namespace comet {
struct Client::Owner {
    std::shared_ptr<detail::Core> core; // 只计算应用 Client 拥有者, 与内部 RPC 引用分开.

    ~Owner() {
        core->release();
    } // 最后 Client 消失时, 活跃 Reader 仍有自己的应用拥有数.
};

Client::Client(std::shared_ptr<Owner> owner) : owner_(std::move(owner)) {}

Result<Client> Client::open(Options options) {
    auto core = detail::Core::prepare(std::move(options));
    if (!core) {
        return std::unexpected(core.error());
    }
    auto owner = std::make_shared<Owner>(*core); // Owner 分配成功后才启动后台任务, 失败不会留下自我保活的会话.
    (*core)->start();
    return Client(std::move(owner));
}

Result<void> Client::secret(std::vector<std::uint8_t> value) {
    if (!owner_) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    return owner_->core->secret(std::move(value));
}

Result<Reader> Client::reader(Scope scope, std::string target, Reader::Options options) {
    if (!owner_) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    auto reading = owner_->core->reader(std::move(scope), std::move(target), std::move(options));
    if (!reading) {
        return std::unexpected(reading.error());
    }
    return Reader(std::move(*reading));
}

Result<Subscriber> Client::subscriber(Scope scope, std::string target, Subscriber::Options options) {
    if (!owner_) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    auto reading = owner_->core->subscriber(std::move(scope), std::move(target), std::move(options));
    if (!reading) {
        return std::unexpected(reading.error());
    }
    return Subscriber(std::move(*reading));
}

Result<Observer> Client::observer(Scope scope, std::string target, Observer::Options options) {
    if (!owner_) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    auto reading = owner_->core->observer(std::move(scope), std::move(target), std::move(options));
    if (!reading) {
        return std::unexpected(reading.error());
    }
    return Observer(std::move(*reading));
}

Result<Beacon> Client::beacon(Scope scope, std::vector<std::uint8_t> attr, std::vector<std::uint8_t> data, std::chrono::milliseconds ttl, Beacon::Options options) {
    if (!owner_) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    auto created = owner_->core->beacon(std::move(scope), std::make_shared<const std::vector<std::uint8_t>>(std::move(attr)), std::make_shared<const std::vector<std::uint8_t>>(std::move(data)), ttl, std::move(options));
    if (!created) {
        return std::unexpected(created.error());
    }
    return Beacon(std::move(*created));
}

Result<Publisher> Client::publisher(Scope scope, std::string key, std::chrono::milliseconds ttl, Publisher::Options options) {
    if (!owner_) {
        return std::unexpected(Error{Error::Code::closed, Error::Effect::unapplied, {}, {}, {}});
    }
    auto object = owner_->core->publisher(std::move(scope), std::move(key), ttl, std::move(options));
    if (!object) {
        return std::unexpected(object.error());
    }
    return Publisher(std::move(*object));
}

void Client::close() noexcept {
    if (owner_) {
        owner_->core->close();
    }
}

bool Client::wait(std::chrono::milliseconds timeout) const {
    return !owner_ || owner_->core->wait(timeout);
}

std::uint64_t Client::exceptions() const noexcept {
    return owner_ ? owner_->core->exceptions() : 0;
}
} // namespace comet
