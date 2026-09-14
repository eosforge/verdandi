// 功能: 提供 Star 可执行程序入口, 将角色和策略工厂交给公共运行时.
#include "topology.hpp"
#include <verdandi/cluster/runtime.hpp>

// Star 入口只能选择 Star 策略, 账号授权仍由 Supervisor 独立检查.
// 转交 argc/argv 和固定角色工厂给 run_node, 返回 0 表示正常退出, 1 表示运行失败, 2 表示参数错误.
int main(int argc, char** argv) {
    return verdandi::cluster::run_node(argc, argv, verdandi::cluster::Role::star, verdandi::cluster::make_star);
}
