// 许可证: MIT, 详见仓库根目录 LICENSE.
#pragma once
#include <verdandi/peer/policy.hpp>

namespace verdandi::peer {
// 两个二进制共用进程入口, factory 创建实际角色策略. 成功退出 0, 运行失败 1, 参数错误 2.
// 函数拥有全部信号适配和网络资源, 不使用 stdin 退出, 不从异常边界泄漏登录材料.
int run_node(int argc, char** argv, Role role, PolicyFactory factory);
} // namespace verdandi::peer
