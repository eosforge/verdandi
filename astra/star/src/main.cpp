// 该文件定义了 Star 程序的 main 函数。
#include "topology.hpp"
#include <astra/runtime.hpp>

// Star 入口只能被指定并选择运行 Star 策略，账号认证以及授权信息仍交由 Supervisor 组件独立校验。
// main 函数的主要职责就是将接收到的命令行参数 argc 和 argv，
// 连同固定写死的 astra::Member::Role::star 角色和 astra::make_star 工厂方法，
// 一并透传给 astra::run_node 去执行节点核心循环。
//
// 返回值:
// - 0 表示节点逻辑正常结束并退出。
// - 1 表示运行期间发生了不可恢复的故障或内部错误。
// - 2 表示命令行启动参数存在解析错误。
//
// 参数说明:
// - argc: 传入命令行参数的个数。
// - argv: 包含具体命令行参数的字符串数组。
int main(int argc, char** argv) {
    // astra::run_node 接管控制权，启动 Star 节点的运行时上下文。
    return astra::run_node(argc, argv, astra::Member::Role::star, astra::make_star);
}
