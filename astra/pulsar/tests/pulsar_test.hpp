#pragma once
#include "check.hpp"
#include "fixture.hpp"
#include <cstdlib>
#include <unistd.h>

namespace astra::test {
struct Directory {
    std::filesystem::path path;
    Directory() {
        auto name = (std::filesystem::current_path() / "pulsar-test-XXXXXX").string();
        const auto result = ::mkdtemp(name.data());
        if (!result) {
            throw std::runtime_error("Cannot create owned test directory");
        }
        path = result;
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
};

inline Member member(unsigned index, Member::Role role = Member::Role::star) {
    Principal principal;
    principal.bytes[0] = static_cast<std::uint8_t>(index);
    return Member{"alpha", "node-" + std::to_string(index), principal, *Endpoint::parse("127.0.0.1:" + std::to_string(7400 + index)), {}, role, "default"};
}
} // namespace astra::test
