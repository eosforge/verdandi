#include <string_view>
#include <vector>

// 负例只在独立 Linux 子进程中运行, 驱动器禁止 core dump 并核对终止信号和诊断.
int main(int argc, char** argv) {

    if (argc != 2) {
        return 2;
    }

    // mode 借用唯一命令行参数, 选择正常值, 契约失败或 STL 越界子进程.
    const std::string_view mode(argv[1]);
    // value 在 valid 模式取 7, 其他模式取 0, 用于可重复触发契约失败.
    const auto value = mode == "valid" ? 7 : 0;
    if (mode == "valid" || mode == "contract") {
        contract_assert(value == 7);
        return 0;
    }
    if (mode == "stl") {
        // values 只有一个元素, argc 为 2 时故意越界以核对 STL 硬化.
        const std::vector<int> values{7};
        return values[static_cast<std::size_t>(argc)];
    }
    return 2;
}
