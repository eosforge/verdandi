// 功能: 提供 Planet 可执行程序入口，将角色和策略工厂交给公共运行时环境。
// 该文件定义了程序的 main 函数，作为整个 Planet 节点的启动入口。
#include "upstream.hpp"
#include <astra/runtime.hpp>

// Planet 入口不能通过命令行提升为 Star 节点。它复用公共的网络核心组件，并且组合了特定的单上游连接策略。
// main 函数的主要逻辑是将传入的命令行参数 argc 和 argv，以及固定的 Planet 角色枚举和工厂函数 make_planet，
// 转交给 astra::run_node 执行。
// 返回值: 
// - 0 表示节点正常退出。
// - 1 表示运行时出现故障或异常。
// - 2 表示传入的命令行参数解析错误。
//
// 参数说明:
// - argc: 命令行参数的个数。
// - argv: 命令行参数的字符串数组。
int main(int argc, char** argv) {
    // 调用 astra::run_node 函数启动节点。
    // astra::Role::planet 指定当前节点角色为 Planet。
    // astra::make_planet 是负责创建 PlanetUpstream 策略实例的工厂函数。
    return astra::run_node(argc, argv, astra::Role::planet, astra::make_planet);
}
