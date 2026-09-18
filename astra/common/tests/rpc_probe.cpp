#include "admission.hpp"
#include "astra.grpc.pb.h"
#include "check.hpp"
#include <openssl/rand.h>

#include <algorithm>
#include <grpcpp/create_channel.h>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace astra;

namespace {

// 探针独占一条同步 RPC, 退出时取消并取最终状态, 不允许调用方遗留阻塞流.
struct Stream {
    // context 保存本次调用的截止及取消状态, 寿命覆盖流.
    grpc::ClientContext context;
    // io 独占会话流, 所有读写由当前测试线程顺序执行.
    std::unique_ptr<grpc::ClientReaderWriter<proto::astra::v1::SessionPacket, proto::astra::v1::SessionPacket>> io;
    // finished 初始 false, Finish 后置 true, 避免析构重复结束.
    bool finished{};

    // 借用 stub 创建一条最多 15 s 的流, 不接管存根所有权.
    explicit Stream(proto::astra::v1::StarTransport::Stub& stub) {
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(15));
        io = stub.OpenSession(&context);
    }

    // 异常和正常离开作用域都回收未结束流, 不向外抛异常.
    ~Stream() {

        if (!finished) {
            context.TryCancel();
            static_cast<void>(io->Finish());
        }
    }

    // 显式取消并等待流结束, 之后仅允许销毁对象.
    void finish() {

        context.TryCancel();
        static_cast<void>(io->Finish());
        finished = true;
    }

    // 发送 hello 并用 identity 校验远端身份; 消息缓冲仅在调用内存活.
    bool hello(const proto::astra::v1::Hello& hello, const Identity& identity) {

        // message 独占本次读写缓冲, 同步操作返回后才复用.
        proto::astra::v1::SessionPacket message;
        message.mutable_hello()->CopyFrom(hello);
        if (!io->Write(message) || !io->Read(&message)) {
            return false;
        }

        // admission 借用响应的准入字节, 不超过所属消息寿命.
        auto admission = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(message.hello().admission().data()), message.hello().admission().size());
        // sig 借用响应签名, 与 admission 一起完成验签后即丢弃.
        auto sig = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(message.hello().admission_signature().data()), message.hello().admission_signature().size());
        // 接收侧独立检查公开 v1 契约, 避免探针与服务同时改错常量后仍被判为互通.
        return message.has_hello() && message.hello().protocol_major() == 1 && identity.verify(admission, sig).has_value();
    }

    // 发送非零 id, 同时回复反向 Ping; 最多读取 20 包, 未收到对应 Pong 返回 false.
    bool ping(std::uint64_t id) {

        // message 独占本次读写缓冲, 同步操作返回后才复用.
        proto::astra::v1::SessionPacket message;
        message.mutable_ping()->set_request_id(id);
        if (!io->Write(message)) {
            return false;
        }

        // received 是本次探针已读消息数, 上限 20 防止无关 Ping 无界延长循环.
        for (unsigned received = 0; received < 20 && io->Read(&message); ++received) {
            if (message.has_pong() && message.pong().request_id() == id) {
                return true;
            }
            if (message.has_ping()) {
                // reply 仅承载当前反向 Ping 的对应响应, 不修改正在读取的 message.
                proto::astra::v1::SessionPacket reply;
                reply.mutable_pong()->set_request_id(message.ping().request_id());
                if (!io->Write(reply)) {
                    return false;
                }
            }
        }
        return false;
    }
};

// 使用指定 supervisor 和 advertise 登记测试节点, 验证签名后返回拥有凭证的 Hello.
proto::astra::v1::Hello admit(const Identity& identity, const std::string& supervisor, const std::string& advertise) {

    // channel 共享持有使用公开测试证书的 TLS 通道, 由存根覆盖调用寿命.
    auto channel = grpc::CreateChannel(supervisor, identity.channel_credentials());
    // stub 独占当前生成服务存根, 不跨进程共享.
    auto stub = proto::orbit::v1::Admission::NewStub(channel);
    // request 拥有测试登记参数, 与每次启动生成的随机请求 ID 绑定.
    proto::orbit::v1::RegistrationRequest request;
    request.set_username(identity.username());
    request.set_password(identity.password());
    request.set_galaxy("alpha");
    request.set_advertise(advertise);
    // request_id 是 32 字节随机启动请求标识, 初始零缓冲随后由 BoringSSL 填充.
    std::string request_id(32, '\0');
    CHECK(RAND_bytes(reinterpret_cast<unsigned char*>(request_id.data()), request_id.size()) == 1);
    request.set_request_id(std::move(request_id));
    request.set_role(proto::orbit::v1::ROLE_STAR);
    request.set_group("default");
    // context 保存本次调用的截止及取消状态, 寿命覆盖流.
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    // response 独占同步登记结果, 验签完成前不得复用.
    proto::orbit::v1::RegistrationResponse response;
    CHECK(stub->Register(&context, request, &response).ok());
    // admission 借用响应的准入字节, 不超过所属消息寿命.
    auto admission = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.admission().data()), response.admission().size());
    // sig 借用响应签名, 与 admission 一起完成验签后即丢弃.
    auto sig = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(response.signature().data()), response.signature().size());
    CHECK(identity.verify(admission, sig).has_value());
    // hello 复制准入正文和签名, 返回后不借用 response.
    proto::astra::v1::Hello hello;
    hello.set_protocol_major(protocol_major);
    hello.set_max_frame_bytes(4096);
    hello.set_admission(response.admission());
    hello.set_admission_signature(response.signature());
    return hello;
}

} // namespace

// 进程入口, 捕获可报告异常并返回非零失败状态; 测试断言不受 NDEBUG 影响.
int main(int argc, char** argv) {

    try {
        CHECK(argc == 5);
        // endpoint 从第四个参数解析本地公告地址, 失败由断言结束探针.
        auto endpoint = Endpoint::parse(argv[4]);
        CHECK(endpoint);
        // identity 加载第三个参数指定的测试身份目录, 验证公告端点授权.
        auto identity = Identity::load(argv[3], *endpoint);
        CHECK(identity);
        // hello 是本探针重复建流时共享使用的同一份合法凭证.
        const auto hello = admit(**identity, argv[1], argv[4]);
        // channel 共享持有使用公开测试证书的 TLS 通道, 由存根覆盖调用寿命.
        auto channel = grpc::CreateChannel(argv[2], (*identity)->channel_credentials());
        // stub 独占当前生成服务存根, 不跨进程共享.
        auto stub = proto::astra::v1::StarTransport::NewStub(channel);
        {
            // 仅建立 TLS/RPC 而不提交 Hello 的调用方不能收到 bearer, 并且会在应用握手预算后被关闭.
            Stream silent(*stub);
            // message 独占本次读写缓冲, 同步操作返回后才复用.
            proto::astra::v1::SessionPacket message;
            // started 记录本场景本地单调起点, 只衡量时长, 不作为业务期限.
            const auto started = Steady::now();
            CHECK(!silent.io->Read(&message));
            CHECK(Steady::now() - started < std::chrono::seconds(8));
        }

        // first 为保留的合法活动流, 重复凭证流不能将它挤掉.
        Stream first(*stub);
        CHECK(first.hello(hello, **identity));
        CHECK(first.ping(41));
        {
            // duplicate 尝试以相同凭证并发建流, 必须被拒绝.
            Stream duplicate(*stub);
            CHECK(!duplicate.hello(hello, **identity));
            CHECK(!duplicate.ping(42));
        }
        CHECK(first.ping(43));
        {
            // 强制独立 subchannel, 验证同一凭证跨实际连接也不能绕过活动逻辑会话索引.
            grpc::ChannelArguments arguments;
            arguments.SetInt("grpc.use_local_subchannel_pool", 1);
            // isolated 强制独立子通道, 排除同一底层连接复用掩盖凭证问题.
            auto isolated = grpc::CreateCustomChannel(argv[2], (*identity)->channel_credentials(), arguments);
            // isolated_stub 借用独立通道创建新的会话服务存根.
            auto isolated_stub = proto::astra::v1::StarTransport::NewStub(isolated);
            // duplicate 尝试以相同凭证并发建流, 必须被拒绝.
            Stream duplicate(*isolated_stub);
            CHECK(!duplicate.hello(hello, **identity));
        }
        CHECK(first.ping(45));
        {
            // 一次只保留一个业务探针, 正常处理期间仍回复服务端 Ping. 测量控制 RTT, 不模拟未实现的数据业务.
            std::vector<double> samples;
            samples.reserve(500);
            // i 是 520 次探针的序号, 前 20 次预热, 同时生成不同请求 ID.
            for (std::uint64_t i = 0; i < 520; ++i) {
                // start 为本次单个 Ping 的单调起点.
                const auto start = Steady::now();
                CHECK(first.ping(1000 + i));
                if (i >= 20) {
                    samples.push_back(std::chrono::duration<double, std::micro>(Steady::now() - start).count());
                }
            }
            std::ranges::sort(samples);
            std::cout << "{\"control_rtt_us\":{\"samples\":" << samples.size() << ",\"p50\":" << samples[249] << ",\"p95\":" << samples[474] << ",\"p99\":" << samples[494] << ",\"max\":" << samples.back() << "}}\n";
        }
        first.finish();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        {
            // next 在前一流排空后重用合法凭证, 必须能够重新安装.
            Stream next(*stub);
            CHECK(next.hello(hello, **identity));
            CHECK(next.ping(44));
            next.finish();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // mutation 依次选择协议版本, 签名, 帧容量和超大凭证四种非法输入.
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
            // invalid 复制合法 Hello 后仅修改当前场景字段, 保持其他条件一致.
            auto invalid = hello;
            if (mutation == 0) {
                invalid.set_protocol_major(2);
            } else if (mutation == 1) {
                (*invalid.mutable_admission_signature())[0] ^= 1;
            } else if (mutation == 2) {
                invalid.set_max_frame_bytes(1);
            } else {
                invalid.set_admission(std::string(5000, 'x'));
            }

            // bad 独占当前非法握手场景, 结束时清理 RPC.
            Stream bad(*stub);
            CHECK(!bad.hello(invalid, **identity));
            CHECK(!bad.ping(50 + mutation));
        }
        {
            // 对端持续写 Ping 却不读取响应, 不应令服务端无界积累消息或阻止关闭.
            Stream slow(*stub);
            CHECK(slow.hello(hello, **identity));
            // message 独占本次读写缓冲, 同步操作返回后才复用.
            proto::astra::v1::SessionPacket message;
            // started 记录本场景本地单调起点, 只衡量时长, 不作为业务期限.
            const auto started = Steady::now();
            // sent 从零统计成功尝试的写入序号, 100000 是探针自身的安全上限.
            std::uint64_t sent = 0;
            while (sent < 100000) {
                message.mutable_ping()->set_request_id(++sent);
                if (!slow.io->Write(message)) {
                    break;
                }
            }
            CHECK(sent < 100000 && Steady::now() - started < std::chrono::seconds(10));
            std::cout << "{\"slow_reader\":{\"attempted_pings\":" << sent << ",\"close_ms\":" << std::chrono::duration_cast<Milliseconds>(Steady::now() - started).count() << "}}\n";
        }
        std::cout << "PASS silent Hello deadline, logical session reuse, duplicate fencing, Ping/Pong, rejection without credential disclosure\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
