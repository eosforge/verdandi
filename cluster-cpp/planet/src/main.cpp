// 功能: 提供 Planet 可执行程序入口, 将角色和策略工厂交给公共运行时.
#include "upstream.hpp"
#include <verdandi/cluster/runtime.hpp>

// Planet 入口不能通过命令行提升为 Star, 复用网络核心并组合单上游策略.
// 转交 argc/argv 和固定角色工厂给 run_node, 返回 0 表示正常退出, 1 表示运行失败, 2 表示参数错误.
int main(int argc, char** argv) {
    return verdandi::cluster::run_node(argc, argv, verdandi::cluster::Role::planet, verdandi::cluster::make_planet);
}
