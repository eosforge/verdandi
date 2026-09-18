#include "process.hpp"
#include "server.hpp"
#include <iostream>

// 进程入口, 解析配置后托管运行及清理, 参数错误和运行故障返回不同退出码.
int main(int argc, char** argv) {

    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            std::cout << astra::Server::Config::help();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "pulsar 0.1.0 (C++26, gRPC v1)\n";
            return 0;
        }

        // arguments 只借用 argv, 不复制参数字节, 在本次启动解析结束前保持有效.
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }

        // config 为完整解析结果, 失败输出固定配置诊断并退出, 不启动服务.
        auto config = astra::Server::Config::parse(arguments);
        if (!config) {
            std::cerr << config.error().message << '\n';
            return 2;
        }
        // signals 托管本进程退出信号, 寿命覆盖服务与主循环.
        astra::Signals signals;
        // logger 写出有限诊断, 不缓存无法及时写出的日志.
        astra::Logger logger("pulsar");
        // server 接管配置并独占所有服务资源, 异常展开时负责停止.
        astra::Server server(std::move(*config));
        server.start();
        logger.write("started", "{\"admission\":" + astra::json_string(server.admission_endpoint()) + ",\"pulse\":" + astra::json_string(server.pulse_endpoint()) + "}");
        // wake 用于主循环有界等待, 不承载业务时间或 RPC 生命周期.
        astra::Wakeup wake;
        // reported_ready 初始为 false, 记录上次已打印的资格, 避免反复输出相同状态.
        bool reported_ready = false;
        // next_report 是下一次状态日志的本地单调截止, 与公开的 Unix 时间分离.
        auto next_report = astra::Steady::now();
        while (!signals.requested()) {
            // local 为本控制轮的本地单调时间, 只用于日志节流和退出轮询.
            const auto local = astra::Steady::now();
            if (local >= next_report) {
                // 每秒公开时间质量, 首次未校准也可诊断; 登记服务不因外部对时失效而退出.
                const auto time = server.time();
                // ready 要求读数存在且质量达标, 单纯已有锚点不意味着可以签发新有限租约.
                const bool ready = time && time->ready;
                if (ready != reported_ready) {
                    reported_ready = ready;
                    logger.write(ready ? "clock_synchronized" : "clock_unavailable");
                }
                logger.write("clock_status", time ? std::string("{\"ready\":") + (ready ? "true" : "false") + ",\"nanoseconds\":" + std::to_string(time->time.time_since_epoch().count()) + ",\"uncertainty_ns\":" + std::to_string(time->uncertainty_ns) + "}" : "{\"ready\":false}");
                next_report = local + std::chrono::seconds(1);
            }
            wake.wait(wake.observe());
        }
        server.stop();
        logger.write("stopped");
        return 0;
    } catch (...) {
        std::cerr << "Pulsar failed to start or operate; check configuration, identity, ports and journal\n";
        return 1;
    }
}
