// 许可证: MIT, 详见仓库根目录 LICENSE.
#include "upstream.hpp"
#include <verdandi/peer/runtime.hpp>

// Planet 入口不能通过命令行提升为 Star, 复用网络核心并组合单上游策略.
int main(int argc, char** argv) {
    return verdandi::peer::run_node(argc, argv, verdandi::peer::Role::planet, verdandi::peer::make_planet);
}
