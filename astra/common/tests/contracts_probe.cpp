#include <string_view>
#include <vector>

// 负例只在独立 Linux 子进程中运行, 驱动器禁止 core dump 并核对终止信号和诊断.
int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    const std::string_view mode(argv[1]);
    const auto value = mode == "valid" ? 7 : 0;
    if (mode == "valid" || mode == "contract") {
        contract_assert(value == 7);
        return 0;
    }
    if (mode == "stl") {
        const std::vector<int> values{7};
        return values[static_cast<std::size_t>(argc)];
    }
    return 2;
}
