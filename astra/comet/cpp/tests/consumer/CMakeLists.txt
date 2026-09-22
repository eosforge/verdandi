cmake_minimum_required(VERSION 3.28)
project(CometConsumer LANGUAGES CXX)

# 同一真实消费源码分别检查源码树和安装包, 不向应用泄露任何内部头目录或 Star 链接目标.
if(COMET_SOURCE)
    add_subdirectory("${COMET_SOURCE}" comet)
else()
    find_package(Comet CONFIG REQUIRED)
endif()
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE comet::comet)
