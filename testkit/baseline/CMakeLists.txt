cmake_minimum_required(VERSION 3.28)
project(astra_redis_baseline LANGUAGES C CXX)

# 独立构建旧版适配器, 防止两套 TLS 库链接入同一进程. 缓存缺失时明确失败.
set(VERDANDI_BUILD_TESTS OFF CACHE BOOL "Baseline only" FORCE)
set(VERDANDI_OFFLINE_DEPENDENCIES ON CACHE BOOL "No network access" FORCE)
set(VCPKG_MANIFEST_INSTALL OFF CACHE BOOL "No implicit installation" FORCE)
add_subdirectory(../../sdk/cpp legacy)
add_executable(baseline_redis redis.cpp)
target_link_libraries(baseline_redis PRIVATE verdandi::verdandi)
target_compile_features(baseline_redis PRIVATE cxx_std_23)
verdandi_enable_warnings(baseline_redis)
