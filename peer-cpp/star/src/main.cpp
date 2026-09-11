// 许可证: MIT, 详见仓库根目录 LICENSE.
#include "topology.hpp"
#include <verdandi/peer/runtime.hpp>

// Star 入口只能选择 Star 策略, 账号授权仍由 Supervisor 独立检查.
int main(int argc, char** argv) {
    return verdandi::peer::run_node(argc, argv, verdandi::peer::Role::star, verdandi::peer::make_star);
}
