// 功能: 声明 Star 与 Planet 共用的进程入口, 统一启动, 退出码和资源收尾.
#pragma once
#include <verdandi/cluster/policy.hpp>

namespace verdandi::cluster {
// 两个二进制共用进程入口, factory 创建实际角色策略. 成功退出 0, 运行失败 1, 参数错误 2.
// 函数拥有全部信号适配和网络资源, 不使用 stdin 退出, 不从异常边界泄漏登录材料.
// argc/argv 使用标准 main 参数约定, 仅调用期间借用; role 必须与 factory 创建的策略一致, factory 不得为空.
int run_node(int argc, char** argv, Role role, PolicyFactory factory);
} // namespace verdandi::cluster
