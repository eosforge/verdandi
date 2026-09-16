// 功能: 声明 Star 与 Planet 共用的进程入口, 统一启动, 退出码和资源收尾.
// 将系统的启动、运行循环及信号捕获封装为一个可复用的统一入口，简化主函数编写。
#pragma once
#include <astra/policy.hpp>

namespace astra {
// 系统核心运行入口函数。
// 两个二进制程序(如 astra-star 和 astra-planet)均共用该进程主入口, factory 参数提供钩子去创建当前所需的实际角色业务策略对象。
// 返回值规范：成功平滑退出返回 0, 运行时发生致命异常退出返回 1, 用户命令行参数输入错误则返回 2。
// 该函数内部将独自拥有和接管全部的系统级信号适配捕获逻辑以及网络底层资源的生命周期, 
// 执行期间杜绝使用 stdin 处理退出交互, 并且内部做过专门异常安全保证，绝不让底层登录认证等敏感材料发生任何从异常传播边界意外泄漏出去的问题。
// 参数 argc/argv: 直接来自标准 main 程序的 C 风格参数约定, 内存仅在执行调用期间被借用访问;
// 参数 role: 显式指示当前启动需要采取的角色标识，它必须必须与 factory 策略工厂准备创建出的具体策略匹配一致,
// 参数 factory: 创建具体 Policy 对象实例的回调工厂指针，在任何情况下传入的指针均不得为空。
int run_node(int argc, char** argv, Role role, PolicyFactory factory);
} // namespace astra
