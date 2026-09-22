#include "pulsar_test.hpp"
#include "pulse_client.hpp"
#include "server.hpp"
#include "store.hpp"
#include <atomic>
#include <grpc/support/time.h>
#include <grpcpp/create_channel.h>
#include <iostream>
#include <thread>

using namespace astra;
using namespace std::chrono_literals;

namespace {

// 本例拥有一条已鉴权但随后静默的流; 独立子通道避免 HTTP/2 单连接流上限把测试串行化.
struct Idle {
    // context 独占当前 RPC 的截止, 凭证元数据和取消状态, 寿命覆盖流.
    grpc::ClientContext context;
    // stub 拥有对时服务存根, 与 stream 的调用寿命一起保留.
    std::unique_ptr<proto::pulsar::v1::Pulse::Stub> stub;
    // stream 拥有当前同步对时流, 析构前先取消并 Finish.
    std::unique_ptr<grpc::ClientReaderWriter<proto::pulsar::v1::Ping, proto::pulsar::v1::Pong>> stream;
    // finished 初始 false, 记录是否已经取得最终状态, 防止重复 Finish.
    bool finished{};

    // 创建合法流; endpoint/identity/hello 仅在构造期间借用, 默认不设客户端截止.
    Idle(const std::string& endpoint, const Identity& identity, const proto::astra::v1::Hello& hello, bool long_deadline = false) {

        if (long_deadline) {
            context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(3600, GPR_TIMESPAN)));
        }
        context.AddMetadata("astra-admission-bin", hello.admission());
        context.AddMetadata("astra-signature-bin", hello.admission_signature());
        // arguments 强制独立子通道, 避免并发流被同一 HTTP/2 连接上限串行化.
        grpc::ChannelArguments arguments;
        arguments.SetInt("grpc.use_local_subchannel_pool", 1);
        stub = proto::pulsar::v1::Pulse::NewStub(grpc::CreateCustomChannel(endpoint, identity.channel_credentials(), arguments));
        stream = stub->Bounce(&context);
    }

    // 取消并排空未结束流, 保证异常路径不泄漏测试连接.
    ~Idle() {

        if (!finished) {
            context.TryCancel();
            static_cast<void>(stream->Finish());
        }
    }

    // 先取得真实 Pong 确认服务已接纳, 再停止发送, 不把尚未到达服务的请求当作慢流.
    void ping() {

        // ping 保存本次本地发送计数, 与响应中的 t0 核对.
        proto::pulsar::v1::Ping ping;
        // pong 为独占响应缓冲, 失败场景不得收到有效响应.
        proto::pulsar::v1::Pong pong;
        ping.set_t0(static_cast<std::uint64_t>(elapsed_ns(Clock::Elapsed::now())));
        CHECK(stream->Write(ping) && stream->Read(&pong));
        CHECK(pong.t0() == ping.t0() && pong.precision_ns() >= 1 && pong.precision_ns() <= 20'000'000);
        CHECK(pong.synchronized() && pong.t2() >= pong.t1() && pong.t1() >= 1'800'000'000'000'000'000ULL);
    }

    // 故意不 half-close. 只能由服务端自己的单调截止结束; CTest 另有整例超时.
    grpc::Status wait_expiry() {

        // pong 为独占响应缓冲, 失败场景不得收到有效响应.
        proto::pulsar::v1::Pong pong;
        CHECK(!stream->Read(&pong));
        // status 保存 Finish 的最终 RPC 结果, 不再继续使用流.
        auto status = stream->Finish();
        finished = true;
        return status;
    }
};

// 使用公开测试材料的登记客户端, 每次调用独立设置有界截止.
struct Client {
    // identity 共享持有合法测试身份, 供 TLS 通道及对时客户端复用.
    std::shared_ptr<Identity> identity;
    // stub 拥有登记服务存根, 不保留上一次调用上下文.
    std::unique_ptr<proto::orbit::v1::Admission::Stub> stub;

    // 加载 star-a 测试身份并连接 endpoint, 公开材料不含部署秘密.
    explicit Client(const std::string& endpoint) {
        identity = *Identity::load(std::filesystem::path(ASTRA_FIXTURES) / "star-a", *Endpoint::parse("127.0.0.1:7443"));
        stub = proto::orbit::v1::Admission::NewStub(grpc::CreateChannel(endpoint, identity->channel_credentials()));
    }

    // startup 生成固定 32 字节测试启动 ID, port 默认 7443, 返回拥有字段的请求.
    proto::orbit::v1::RegistrationRequest request(char startup, std::uint16_t port = 7443) const {

        // request 是当前调用独有的登记消息, 不与其他请求共享可写字段.
        proto::orbit::v1::RegistrationRequest request;
        request.set_username(identity->username());
        request.set_password(identity->password());
        request.set_galaxy("alpha");
        request.set_group("default");
        request.set_role(proto::orbit::v1::ROLE_STAR);
        request.set_advertise("127.0.0.1:" + std::to_string(port));
        request.set_request_id(std::string(32, startup));
        return request;
    }

    // 借用 request, 清空并填充 response; 十秒后终止, 返回原始最终状态供断言.
    grpc::Status call(const proto::orbit::v1::RegistrationRequest& request, proto::orbit::v1::RegistrationResponse& response) {

        // context 独占当前 RPC 的截止, 凭证元数据和取消状态, 寿命覆盖流.
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + 10s);
        response.Clear();
        return stub->Register(&context, request, &response);
    }

    // 使用已有原始凭证查询目录; duplicate 用于构造重复 metadata 负例, 默认不重复.
    grpc::Status list(std::string_view admission, std::string_view signature, proto::orbit::v1::DirectoryResponse& response, bool duplicate = false) {

        // context 独占此有界查询, 不发送账号或启动幂等键.
        grpc::ClientContext context;
        context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(3, GPR_TIMESPAN)));
        if (!admission.empty()) {
            context.AddMetadata("astra-admission-bin", std::string(admission));
            context.AddMetadata("astra-signature-bin", std::string(signature));
            if (duplicate) {
                context.AddMetadata("astra-admission-bin", std::string(admission));
            }
        }
        // request 明确为空, 查询不能被当作重新登记或代次刷新.
        const proto::orbit::v1::DirectoryRequest request;
        response.Clear();
        return stub->List(&context, request, &response);
    }
};

// 等待 clock 同步质量达标且采样新于 after, after 默认零; 15 s 内不满足则测试失败.
static Clock::Reading wait_clock(const Clock& clock, ElapsedTime after = {}) {

    // deadline 是本轮等待的本地单调上界, 不受被测时钟校正影响.
    const auto deadline = Steady::now() + 15s;
    while (Steady::now() < deadline) {
        // value 独立保存读取快照, 只有新鲜且就绪时才交给调用者.
        if (auto value = clock.now(); value && value->synchronized && value->sampled > after) {
            return *value;
        }
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("Star did not calibrate within deadline");
}

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
int main() {

    try {
        // directory 独占日志临时目录, 所有服务退出后才清理.
        test::Directory directory;
        // config 使用回环动态端口和临时账本, 不接触部署服务.
        Server::Config config;
        config.listen = *Endpoint::parse("127.0.0.1:0", true);
        config.pulse_listen = *Endpoint::parse("127.0.0.1:0", true);
        config.galaxy = "alpha";
        config.identity = std::filesystem::path(ASTRA_FIXTURES) / "supervisor";
        config.state = directory.path / "membership.db";
        config.initialize = true;
        // RPC 测试注入独立参考源, 不要求测试主机联网对时, 也不更改系统墙钟或生产配置.
        std::atomic_bool source_available{false};
        // provider 只借用 source_available, 生产没有此测试参考源开关.
        const auto provider = [&]() -> std::optional<Clock::Estimate> {
            if (!source_available.load()) {
                return std::nullopt;
            }

            // local 与合成 Unix 时间成对采样, 形成一次完整参考观测.
            const auto local = Clock::Elapsed::now();
            return Clock::Estimate{Clock::Time(1'800'000'000s) + local.time_since_epoch(), local, 1000, 0};
        };
        // server 独占 Pulsar 实例, 允许在场景内停止和重新构造.
        auto server = std::make_unique<Server>(config, provider);
        server->start();
        // 后续同库重启只允许恢复, 初始化权限不随 Config 重用自动延续.
        config.initialize = false;
        config.listen = *Endpoint::parse(server->admission_endpoint());
        config.pulse_listen = *Endpoint::parse(server->pulse_endpoint());
        CHECK(config.listen != config.pulse_listen);
        {
            // 配置错误直接拒绝; 同端口第二个服务即使用另一日志也不能启动.
            const auto parse = [](std::initializer_list<std::string_view> args) { return Server::Config::parse({args.begin(), args.size()}); };
            CHECK(parse({"--listen=127.0.0.1:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha"}));
            CHECK(!parse({"--listen=127.0.0.1:1", "--pulse-listen=127.0.0.1:1", "--galaxy=alpha"}));
            CHECK(!parse({"--listen=0.0.0.0:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha"}));
            CHECK(!parse({"--listen=127.0.0.1:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha", "--galaxy=alpha"}));
            CHECK(!parse({"--listen=127.0.0.1:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha", "--max-starts=0"}));
            CHECK(!parse({"--listen=127.0.0.1:0", "--galaxy=alpha"}));
            CHECK(!parse({"--listen=127.0.0.1:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha", "--init=1"}));
            CHECK(!parse({"--listen=127.0.0.1:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha", "--init=true", "--init=false"}));
            // initialized 显式启用新群组初始化, 普通解析的默认值必须为 false.
            const auto initialized = parse({"--listen=127.0.0.1:0", "--pulse-listen=127.0.0.1:0", "--galaxy=alpha", "--init=true"});
            CHECK(initialized && initialized->initialize);
            // duplicate_config 仅替换日志路径, 保留同端口来验证监听冲突.
            auto duplicate_config = config;
            duplicate_config.state = directory.path / "duplicate.db";
            duplicate_config.initialize = true;
            // refused 初始 false, 构造或监听抛异常才表示端口冲突被拒绝.
            bool refused = false;
            try {
                // duplicate 故意争用正在监听的端口, 不得建立第二个服务.
                Server duplicate(duplicate_config);
                duplicate.start();
            } catch (const std::exception&) {
                refused = true;
            }
            CHECK(refused);
        }

        // client 在实际分配的登记端点上执行下述身份场景.
        Client client(server->admission_endpoint());
        // response 在每次调用前清空, 成功字段不会污染后续失败结果.
        proto::orbit::v1::RegistrationResponse response;
        // first 保留最早的启动请求, 用于重试和旧启动淘汰检查.
        auto first = client.request('a');
        CHECK(client.call(first, response).ok());
        CHECK(response.members_size() == 1 && response.pulse_endpoint() == server->pulse_endpoint());
        // original 保存首个进程签发的准入正文, 替换后用于验证旧凭证失效.
        const auto original = response.admission();
        // original_signature 与 original 配对, 区分真实旧凭证和伪造签名.
        const auto original_signature = response.signature();
        CHECK(client.call(client.request('b', 7444), response).ok());
        CHECK(client.call(first, response).ok());
        CHECK(response.admission() == original && response.members_size() == 2);
        CHECK(response.members(0).id() < response.members(1).id());
        // directory_response 只拥有当前查询结果; 目录读取不改变原实例或持久启动次数.
        proto::orbit::v1::DirectoryResponse directory_response;
        CHECK(client.list(original, original_signature, directory_response).ok());
        CHECK(directory_response.members_size() == 2 && directory_response.members(0).id() < directory_response.members(1).id());
        CHECK(client.list({}, {}, directory_response).error_code() == grpc::StatusCode::UNAUTHENTICATED);
        CHECK(client.list(original, std::string(64, '\0'), directory_response).error_code() == grpc::StatusCode::UNAUTHENTICATED);
        CHECK(client.list(original, original_signature, directory_response, true).error_code() == grpc::StatusCode::UNAUTHENTICATED);
        // bad 独立复制合法请求, 每个负例只变更一个条件.
        auto bad = first;
        bad.set_password("not-the-fixture-password");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::UNAUTHENTICATED);
        bad = first;
        bad.set_username("missing-account");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::UNAUTHENTICATED);
        bad = first;
        bad.set_role(proto::orbit::v1::ROLE_PLANET);
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::PERMISSION_DENIED);
        bad = first;
        bad.set_galaxy("other");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
        bad = first;
        bad.set_advertise("127.0.0.1:7445");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::ABORTED);
        bad = first;
        bad.set_request_id("short");
        CHECK(client.call(bad, response).error_code() == grpc::StatusCode::INVALID_ARGUMENT);

        // 同部署新启动替换旧实例, 旧启动请求不能再取得成功凭证.
        auto replacement = client.request('c');
        CHECK(client.call(replacement, response).ok());
        // member 解码签发正文并核对服务端分配的第二代身份.
        proto::orbit::v1::Member member;
        CHECK(member.ParseFromString(response.admission()) && member.epoch() == 2);
        // hello 共享持有新进程的准入凭证, 寿命覆盖全部对时流.
        auto hello = std::make_shared<proto::astra::v1::Hello>();
        hello->set_admission(response.admission());
        hello->set_admission_signature(response.signature());
        // committed 保存新启动的准入正文, 重启回放后应保持一致.
        const auto committed = response.admission();
        CHECK(client.call(first, response).error_code() == grpc::StatusCode::ABORTED);
        CHECK(client.list(original, original_signature, directory_response).error_code() == grpc::StatusCode::UNAUTHENTICATED);
        CHECK(client.list(hello->admission(), hello->admission_signature(), directory_response).ok());
        CHECK(directory_response.members_size() == 2);

        // pulse_stub 使用独立对时端点, 后续拒绝场景复用此存根.
        auto pulse_stub = proto::pulsar::v1::Pulse::NewStub(grpc::CreateChannel(server->pulse_endpoint(), client.identity->channel_credentials()));
        // 签名伪造和已被替换的真实凭证均拒绝, 不能只验证 TLS 能建立连接.
        const auto reject_credential = [&](const std::string& admission, const std::string& signature) {
            // context 独占当前 RPC 的截止, 凭证元数据和取消状态, 寿命覆盖流.
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 2s);
            context.AddMetadata("astra-admission-bin", admission);
            context.AddMetadata("astra-signature-bin", signature);
            // stream 拥有本负例对时调用, 同步读写结束后取得最终状态.
            auto stream = pulse_stub->Bounce(&context);
            // pong 为独占响应缓冲, 失败场景不得收到有效响应.
            proto::pulsar::v1::Pong pong;
            static_cast<void>(stream->WritesDone());
            CHECK(!stream->Read(&pong));
            CHECK(stream->Finish().error_code() == grpc::StatusCode::PERMISSION_DENIED);
        };
        reject_credential(original, original_signature);
        reject_credential(hello->admission(), std::string(64, '\0'));
        {
            // context 独占当前 RPC 的截止, 凭证元数据和取消状态, 寿命覆盖流.
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 2s);
            // stream 拥有本负例对时调用, 同步读写结束后取得最终状态.
            auto stream = pulse_stub->Bounce(&context);
            // pong 为独占响应缓冲, 失败场景不得收到有效响应.
            proto::pulsar::v1::Pong pong;
            static_cast<void>(stream->WritesDone());
            CHECK(!stream->Read(&pong));
            CHECK(stream->Finish().error_code() == grpc::StatusCode::UNAUTHENTICATED);
        }

        // 系统时间未就绪不阻塞以上登记, 但带有效凭证的采样仍必须明确失败.
        {
            // context 独占当前 RPC 的截止, 凭证元数据和取消状态, 寿命覆盖流.
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 2s);
            context.AddMetadata("astra-admission-bin", hello->admission());
            context.AddMetadata("astra-signature-bin", hello->admission_signature());
            // stream 拥有本负例对时调用, 同步读写结束后取得最终状态.
            auto stream = pulse_stub->Bounce(&context);
            // ping 保存本次本地发送计数, 与响应中的 t0 核对.
            proto::pulsar::v1::Ping ping;
            ping.set_t0(static_cast<std::uint64_t>(elapsed_ns(Clock::Elapsed::now())));
            // pong 为独占响应缓冲, 失败场景不得收到有效响应.
            proto::pulsar::v1::Pong pong;
            CHECK(stream->Write(ping));
            CHECK(!stream->Read(&pong));
            static_cast<void>(stream->WritesDone());
            CHECK(stream->Finish().error_code() == grpc::StatusCode::UNAVAILABLE);
        }
        source_available.store(true);
        // reference_deadline 最多允许 10 s 等待参考源进入就绪状态.
        const auto reference_deadline = Steady::now() + 10s;
        while (Steady::now() < reference_deadline) {
            // time 是单次服务端读数, 不把不同调用的存在与就绪结果拼接.
            if (const auto time = server->time(); time && time->synchronized) {
                break;
            }
            std::this_thread::sleep_for(10ms);
        }
        CHECK(server->time() && server->time()->synchronized);
        {
            // 无符号协议字段不能把有符号本地计数的越界值带入偏移运算.
            Idle invalid(server->pulse_endpoint(), *client.identity, *hello);
            // ping 保存本次本地发送计数, 与响应中的 t0 核对.
            proto::pulsar::v1::Ping ping;
            // pong 为独占响应缓冲, 失败场景不得收到有效响应.
            proto::pulsar::v1::Pong pong;
            ping.set_t0(UINT64_MAX);
            CHECK(invalid.stream->Write(ping));
            CHECK(!invalid.stream->Read(&pong));
            // status 保存最终结果, 随后标记 finished 防止析构重复结束.
            const auto status = invalid.stream->Finish();
            invalid.finished = true;
            CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
        }
        {
            // 一条完整采样流恰好八个应答, 服务主动正常结束, 不等待第九个请求或客户端截止.
            Idle complete(server->pulse_endpoint(), *client.identity, *hello);
            // sample 是八次有效探测的零基序号, 每次都等待对应 Pong.
            for (unsigned sample = 0; sample < 8; ++sample) {
                complete.ping();
            }

            // extra 尝试读取不存在的第九个响应, 服务端应已结束流.
            proto::pulsar::v1::Pong extra;
            CHECK(!complete.stream->Read(&extra));
            // status 保存最终结果, 随后标记 finished 防止析构重复结束.
            const auto status = complete.stream->Finish();
            complete.finished = true;
            CHECK(status.ok());
        }

        // 没有客户端截止和超长截止均不能无限占用流, 由服务端主动回收.
        for (const auto long_deadline : {false, true}) {
            // idle 取得一个有效响应后保持静默, 等待服务端自行取消.
            Idle idle(server->pulse_endpoint(), *client.identity, *hello, long_deadline);
            idle.ping();
            // start 是当前流进入静默前的单调起点, 用于限制测试等待.
            const auto start = Steady::now();
            CHECK(idle.wait_expiry().error_code() == grpc::StatusCode::CANCELLED);
            CHECK(Steady::now() - start < 10s);
        }
        {
            // 多个静默流存在时, 新 Pulse 和独立 Register 仍可完成. 不将此用例当作吞吐或尾延迟基准.
            std::vector<std::unique_ptr<Idle>> idle;
            // i 是 24 条静默流的创建序号, 不作为协议标识.
            for (unsigned i = 0; i < 24; ++i) {
                // stream 先成功取得一个响应再移入容器, 确认确实占用服务端流.
                auto stream = std::make_unique<Idle>(server->pulse_endpoint(), *client.identity, *hello);
                stream->ping();
                idle.push_back(std::move(stream));
            }
            CHECK(client.call(replacement, response).ok());
            // active 验证静默流存在时仍可为新流服务.
            Idle active(server->pulse_endpoint(), *client.identity, *hello);
            active.ping();
            // 析构同时取消多个流, 覆盖取消与 Alarm/OnDone 交接, 后续正常采样证明配额可复用.
        }

        // clock 是 Star 端业务时钟, Pulsar 故障及重启期间保持同一实例.
        Clock clock;
        {
            // synchronizer 独占采样线程, 只向 clock 发布完整有效批次.
            Sampler synchronizer(server->pulse_endpoint(), client.identity, hello, clock);
            // before 是对时成功后的读数, 用于生成一次不可续满的绝对期限.
            const auto before = wait_clock(clock);
            CHECK(before.rtt_ns <= 200'000'000 && before.time >= Clock::Time(1'800'000'000s));
            // 真实网络对时产生一次期限, 后续 Pulsar 停机不应冻结清理或为数据重新续满 TTL.
            Store store;
            store.tick(before.time);
            // deadline 为已受理的两秒期限, 失联和换参考源后不重新生成.
            const auto deadline = before.deadline_after(2s);
            CHECK(deadline);
            store.put("expires-during-outage", {1}, *deadline);
            // snapshot 保留过期前的不可变值, 检查后续清理不修改旧快照.
            const auto snapshot = store.snapshot();
            // previous_sample 是停机前采样下界, 恢复必须使用更新的观测.
            const auto previous_sample = before.sampled;
            source_available.store(false);
            // 先仅撤销 Pulsar 的物理参考. Pulse 不能把仍在走时的旧读数当作可信样本, 掩盖 Star 的失联状态.
            const auto outage_deadline = Steady::now() + 10s;
            while (Steady::now() < outage_deadline && clock.now()->synchronized) {
                std::this_thread::sleep_for(10ms);
            }
            CHECK(server->time() && server->time()->ready && !server->time()->synchronized);
            // offline 已超过采样新鲜度, 仍能为本地新写入生成有限期限.
            const auto offline = clock.now();
            CHECK(offline && offline->ready && !offline->synchronized && offline->time > before.time);
            store.tick(offline->time);
            CHECK(store.snapshot()->data.empty() && store.version() == 2);
            CHECK(snapshot->data.at("expires-during-outage").deadline == *deadline);
            // created 固定断开参考期间的新期限, 恢复不能按回执时间重新续满.
            const auto created = offline->deadline_after(30s);
            CHECK(created);
            store.put("accepted-during-outage", {2}, *created);
            server->stop();
            server.reset();
            // before_restart 保存重启参考服务前的业务时间, 恢复不可回退.
            const auto before_restart = clock.now()->time;
            source_available.store(true);
            server = std::make_unique<Server>(config, provider);
            server->start();
            // recovered 必须来自新样本且就绪, 继续使用原 clock 状态.
            const auto recovered = wait_clock(clock, previous_sample);
            CHECK(recovered.time >= before_restart);
            store.tick(recovered.time);
            CHECK(store.snapshot()->data.size() == 1 && store.version() == 3);
            CHECK(store.snapshot()->data.at("accepted-during-outage").deadline == *created);
            CHECK(!store.snapshot()->data.contains("expires-during-outage"));
            // restored 在重启后的同一端点重试登记, 验证持久身份和成员表恢复.
            Client restored(server->admission_endpoint());
            CHECK(restored.call(replacement, response).ok());
            CHECK(response.admission() == committed && response.members_size() == 2);
            CHECK(restored.call(first, response).error_code() == grpc::StatusCode::ABORTED);
            synchronizer.stop();
        }
        CHECK(clock.now() && clock.now()->ready && !clock.now()->synchronized);
        CHECK(clock.now()->deadline_after(1s));
        server->stop();
        server->stop();
        std::cout << "PASS Pulsar TLS/password admission, current topology, signed Pulse, restart recovery and Star clock reconnect\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
