// 功能: Pulsar 进程入口, 加载明确配置并托管签发/对时服务, 由 SIGINT/SIGTERM 停止.
#include "process.hpp"
#include "server.hpp"
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h")) {
            std::cout << astra::PulsarConfig::help();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "pulsar 0.1.0 (C++26, gRPC v1)\n";
            return 0;
        }
        std::vector<std::string_view> arguments;
        for (int i = 1; i < argc; ++i) {
            arguments.emplace_back(argv[i]);
        }
        auto config = astra::PulsarConfig::parse(arguments);
        if (!config) {
            std::cerr << config.error().message << '\n';
            return 2;
        }
        astra::Signals signals;
        astra::Logger logger("pulsar");
        astra::PulsarServer server(std::move(*config));
        server.start();
        logger.write("started",
                     "{\"admission\":" + astra::json_string(server.admission_endpoint()) + ",\"pulse\":" + astra::json_string(server.pulse_endpoint()) + "}");
        astra::Wakeup wake;
        bool reported_ready = false;
        auto next_report = astra::Clock::now();
        while (!signals.requested()) {
            const auto local = astra::Clock::now();
            if (local >= next_report) {
                // 每秒公开时间质量, 首次未校准也可诊断; 登记服务不因外部对时失效而退出.
                const auto time = server.time();
                const bool ready = time && time->ready;
                if (ready != reported_ready) {
                    reported_ready = ready;
                    logger.write(ready ? "clock_synchronized" : "clock_unavailable");
                }
                logger.write("clock_status", time ? std::string("{\"ready\":") + (ready ? "true" : "false") +
                                                        ",\"nanoseconds\":" + std::to_string(time->time.time_since_epoch().count()) +
                                                        ",\"uncertainty_ns\":" + std::to_string(time->uncertainty_ns) + "}"
                                                  : "{\"ready\":false}");
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
