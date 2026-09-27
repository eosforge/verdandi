# gRPC 1.84 的 MSVC 构建兼容入口, 通过 CMAKE_PROJECT_grpc_INCLUDE 注入, 不修改依赖源码.
# 只对融合实现禁用上游可选优化; 普通认证、压缩和消息大小过滤器仍由原注册路径构建.
if(MSVC AND PROJECT_NAME STREQUAL "grpc")
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/src/core/filter/fused_filters.cc")
        message(FATAL_ERROR "Comet MSVC compatibility requires the pinned gRPC filter layout")
    endif()
    # grpc 与 grpc_unsecure 共用此源文件. 定义只作用于该文件, 不传播到 SDK 或其他依赖.
    set_property(SOURCE "${PROJECT_SOURCE_DIR}/src/core/filter/fused_filters.cc" APPEND PROPERTY COMPILE_DEFINITIONS GRPC_NO_FILTER_FUSION=1)
endif()
