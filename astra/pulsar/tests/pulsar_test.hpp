#pragma once
#include "check.hpp"
#include "fixture.hpp"
#include <cstdlib>
#include <unistd.h>

namespace astra::test {
// 测试专有临时目录, 以 mkdtemp 保证不覆盖现有路径, 析构清理本对象创建的资源.
struct Directory {
    // path 独占本次测试目录的绝对路径, 构造成功后非空.
    std::filesystem::path path;

    // 在当前构建目录创建独立目录, 系统创建失败抛异常.
    Directory() {

        // name 是 mkdtemp 所需的可写模板, 后缀由内核替换.
        auto name = (std::filesystem::current_path() / "pulsar-test-XXXXXX").string();
        // result 借用 name 的成功路径, 空表示创建失败.
        const auto result = ::mkdtemp(name.data());
        if (!result) {
            throw std::runtime_error("Cannot create owned test directory");
        }
        path = result;
    }

    // 仅清理本对象拥有的临时目录, 使用 error_code 避免析构抛异常.
    ~Directory() {
        // error 接收清理失败信息, 不覆盖正在传播的测试异常.
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    // 禁止复制目录清理责任, 避免另一对象提前删除资源.
    Directory(const Directory&) = delete;
    // 禁止赋值覆盖已有目录及其清理责任.
    Directory& operator=(const Directory&) = delete;
};

// 构造独立测试成员, index 用于端点和主体标识, 当前用例在 1..255 范围; role 默认 Star.
inline Member member(unsigned index, Member::Role role = Member::Role::star) {

    // principal 初始全零, 仅首字节使用当前测试序号区分部署.
    Principal principal;
    principal.bytes[0] = static_cast<std::uint8_t>(index);
    return Member{"alpha", "node-" + std::to_string(index), principal, *Endpoint::parse("127.0.0.1:" + std::to_string(7400 + index)), {}, role, "default"};
}
} // namespace astra::test
