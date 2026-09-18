#include "pulse_client.hpp"
#include "clock_filter.hpp"
#include <algorithm>
#include <condition_variable>
#include <grpc/support/time.h>
#include <grpcpp/create_channel.h>

namespace astra {
Sampler::Sampler(std::string endpoint, std::shared_ptr<Identity> identity, std::shared_ptr<const proto::astra::v1::Hello> hello, Clock& output) : identity_(std::move(identity)), hello_(std::move(hello)), output_(output) {

    // arguments 配置专用对时通道, 收发各限 128 字节, 禁用 gRPC 隐式重试.
    grpc::ChannelArguments arguments;
    arguments.SetMaxReceiveMessageSize(128);
    arguments.SetMaxSendMessageSize(128);
    arguments.SetInt("grpc.enable_retries", 0);
    channel_ = grpc::CreateCustomChannel(endpoint, identity_->channel_credentials(), arguments);
    stub_ = proto::pulsar::v1::Pulse::NewStub(channel_);
    jitter_ = std::hash<std::string>{}(hello_->admission_signature());
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

Sampler::~Sampler() {

    stop();
    if (worker_.joinable()) {
        worker_.join();
    }
    output_.revoke();
}

void Sampler::stop() noexcept {
    worker_.request_stop();
}

grpc::Status Sampler::sample(std::stop_token stop) {

    // context 覆盖整批八次探测, 使用单调两秒截止, 在流排空后才析构.
    grpc::ClientContext context;
    context.set_deadline(gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC), gpr_time_from_seconds(2, GPR_TIMESPAN)));
    context.AddMetadata("astra-admission-bin", hello_->admission());
    context.AddMetadata("astra-signature-bin", hello_->admission_signature());
    // stop_callback 的寿命短于 context; 析构会等待正在执行的取消回调结束.
    std::stop_callback cancel(stop, [&context] { context.TryCancel(); });
    // stream 独占本批双向 RPC, 只在本工作线程执行阻塞读写.
    auto stream = stub_->Bounce(&context);
    if (!stream) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Pulse stream unavailable");
    }

    // filter 独立筛选本批四时间戳样本, 不混用不同 RPC 的观测.
    Filter filter(precision_ns_);
    // ping 复用发送缓冲, 每次 Write 结束后才能清空和修改.
    proto::pulsar::v1::Ping ping;
    // pong 复用接收缓冲, Read 成功后消费, 不保留跨次消息引用.
    proto::pulsar::v1::Pong pong;
    // valid 初始为 true, 回显不匹配或时间设施异常时撤销本批资格.
    bool valid = true;
    // received 从零统计完整响应, 只有收到八个才提交本批滤波结果.
    unsigned received = 0;
    // 采样期间读钟/分配异常也必须取消并排空 RPC, 不能让 context 先于活动流销毁.
    try {
        // 单个在途请求, T0/T3 在实际写/读调用紧邻处采样, 不使用 Runtime 的 10 ms 轮询时间.
        for (unsigned index = 0; index < 8 && !stop.stop_requested(); ++index) {
            // 同步 Write 已结束, 在 T0 采样前清空上一包, 保持未来可选请求字段的独立性.
            ping.Clear();
            // t0 紧邻写调用采样, 使用本机 BOOTTIME 纳秒, 负数拒绝发送.
            const auto t0 = elapsed_ns(Clock::Elapsed::now());
            if (t0 < 0) {
                valid = false;
                break;
            }
            ping.set_t0(static_cast<std::uint64_t>(t0));
            if (!stream->Write(ping) || !stream->Read(&pong)) {
                break;
            }

            // t3 紧邻读完成采样, 与 t0 同源, 不使用控制循环的轮询时刻.
            const auto t3 = Clock::Elapsed::now();
            ++received;
            if (pong.t0() != ping.t0() || !pong.synchronized()) {
                valid = false;
                break;
            }
            static_cast<void>(filter.observe(pong.t0(), pong.t1(), pong.t2(), t3, pong.precision_ns(), pong.uncertainty_ns(), pong.synchronized()));
            // 批内间隔五毫秒, 避免连续包全部落入同一个短暂排队窗口.
            if (index != 7 && !wait(stop, Milliseconds(5))) {
                break;
            }
        }
        if (!valid || stop.stop_requested() || received != 8) {
            context.TryCancel();
        }
        static_cast<void>(stream->WritesDone());
    } catch (...) {
        context.TryCancel();
        static_cast<void>(stream->Finish());
        throw;
    }

    // status 是排空后的最终 RPC 结果, 只有成功结束才能发布时钟估计.
    const auto status = stream->Finish();
    if (!valid) {
        return grpc::Status(grpc::StatusCode::DATA_LOSS, "Invalid Pulse response");
    }
    if (!status.ok()) {
        return status;
    }

    // estimate 至少包含三个有效观测才能非空, 其误差预算仍由输出时钟检查.
    const auto estimate = filter.result();
    if (stop.stop_requested() || received != 8 || !estimate) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Insufficient Pulse samples");
    }
    return output_.publish(*estimate) ? grpc::Status::OK : grpc::Status(grpc::StatusCode::DATA_LOSS, "Invalid Pulse estimate");
}

void Sampler::run(std::stop_token stop) noexcept {

    try {
        static_cast<void>(wait(stop, Milliseconds(jitter_ % 1000)));
        // failures 统计连续失败批次, 饱和于七, 成功后归零以恢复一秒采样周期.
        unsigned failures = 0;
        while (!stop.stop_requested()) {
            // 标定属于专用工作线程. 失败撤销可用性并退避重试, 不通过构造异常终止 Star 控制循环.
            grpc::Status status;
            try {
                if (precision_ns_ == 0) {
                    precision_ns_ = elapsed_precision_ns(stop);
                }
                if (stop.stop_requested()) {
                    break;
                }
                status = sample(stop);
            } catch (...) {
                precision_ns_ = 0;
                output_.revoke();
                status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "Pulse clock sampling unavailable");
            }
            if (status.ok()) {
                failures = 0;
            } else {
                failures = std::min(failures + 1, 7U);
                if (status.error_code() == grpc::StatusCode::PERMISSION_DENIED || status.error_code() == grpc::StatusCode::UNAUTHENTICATED || status.error_code() == grpc::StatusCode::DATA_LOSS) {
                    output_.revoke();
                }
            }

            // 失联只使观测过期, Clock 继续走时; 读取者自行判断质量, 不依赖采样线程准时醒来.
            const auto delay = failures == 0 ? 1000U : std::min(100U << (failures - 1), 5000U);
            static_cast<void>(wait(stop, Milliseconds(delay + jitter_ % 201)));
        }
    } catch (...) {
        // 后台异常不能 terminate 进程或留下一份永久有效的旧时钟.
        output_.revoke();
    }
    output_.revoke();
}

bool Sampler::wait(std::stop_token stop, Milliseconds delay) {

    // lock 配合 stop_token 等待, 取消可唤醒而不必等完整间隔.
    std::unique_lock lock(wait_mutex_);
    condition_.wait_for(lock, stop, delay, [] { return false; });
    return !stop.stop_requested();
}
} // namespace astra
