cmake_minimum_required(VERSION 3.28)

project(verdandi_build_probe LANGUAGES CXX)

# 构建工具先用不带 OpenSSL 的模式确认编译器确实具备 Verdandi 所需的
# C++23 标准库能力，再用启用此选项的模式验证 OpenSSL 头文件和链接库。
option(VERDANDI_PROBE_OPENSSL "Probe OpenSSL headers and link libraries" OFF)

add_executable(verdandi_build_probe main.cpp)
target_compile_features(verdandi_build_probe PRIVATE cxx_std_23)

# 探针源码和当前生产源码一样含有审阅期中文注释；MSVC 必须显式按 UTF-8
# 读取，否则本地代码页的多字节尾字节可能被误认为反斜杠并改变预处理结果。
if(MSVC)
    target_compile_options(verdandi_build_probe PRIVATE /utf-8)
endif()

if(VERDANDI_PROBE_OPENSSL)
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto SSL)
    target_compile_definitions(verdandi_build_probe PRIVATE VERDANDI_PROBE_OPENSSL=1)
    target_link_libraries(verdandi_build_probe PRIVATE OpenSSL::Crypto OpenSSL::SSL)
endif()
