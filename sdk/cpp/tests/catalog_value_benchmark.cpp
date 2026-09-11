#include "internal/catalog_value.hpp"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <string>

namespace {
bool tracking{};
std::size_t allocations{};
std::size_t allocation_bytes{};
} // namespace

void* operator new(const std::size_t size) {
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) {
        if (tracking) {
            ++allocations;
            allocation_bytes += size;
        }
        return pointer;
    }
    throw std::bad_alloc();
}

void operator delete(void* pointer) noexcept {
    std::free(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

void* operator new[](const std::size_t size) {
    return ::operator new(size);
}

void operator delete[](void* pointer) noexcept {
    std::free(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

int main() {
    constexpr std::size_t limit = 4 * 1024 * 1024;
    bool first = true;
    std::cout << std::fixed << std::setprecision(2) << '[';
    for (const std::size_t count : {32U, 512U, 4096U, 65536U}) {
        verdandi::fields value;
        for (std::size_t index = 0; index < count; ++index) {
            value.emplace("field" + std::to_string(index), verdandi::bytes(32, std::byte{'x'}));
        }
        const std::size_t iterations = count <= 512 ? 2000U : count <= 4096 ? 300U : 20U;
        for (const bool patch : {false, true}) {
            allocations = 0;
            allocation_bytes = 0;
            std::size_t checksum{};
            const auto start = std::chrono::steady_clock::now();
            tracking = true;
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                if (patch) {
                    auto result = verdandi::catalog::detail::validate_catalog_patch(value, limit);
                    if (!result) {
                        return 1;
                    }
                    ++checksum;
                } else {
                    auto result = verdandi::catalog::detail::validate_catalog_value(verdandi::catalog::kind::map, value, limit);
                    if (!result) {
                        return 1;
                    }
                    checksum += *result;
                }
            }
            tracking = false;
            const auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            if (!first) {
                std::cout << ',';
            }
            first = false;
            std::cout << "{\"operation\":\"" << (patch ? "patch" : "map") << "\",\"fields\":" << count
                      << ",\"ns_per_op\":" << elapsed / static_cast<double>(iterations)
                      << ",\"allocations_per_op\":" << static_cast<double>(allocations) / static_cast<double>(iterations)
                      << ",\"allocated_bytes_per_op\":" << static_cast<double>(allocation_bytes) / static_cast<double>(iterations)
                      << ",\"checksum\":" << checksum << '}';
        }
    }
    std::cout << "]\n";
}
