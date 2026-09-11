cmake_minimum_required(VERSION 3.28)

project(verdandi_catalog_value_tests LANGUAGES CXX)

# 仅编译标准库和 Verdandi 纯函数；不加载父工程，不声明外部依赖或包管理器。
# 正常 SDK 构建也运行同一个测试文件，并链接完整运行库。
get_filename_component(VERDANDI_CPP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

enable_testing()
add_executable(verdandi_cpp_catalog_value_tests "${VERDANDI_CPP_ROOT}/tests/catalog_value_test.cpp")
add_executable(verdandi_cpp_catalog_event_fields_tests "${VERDANDI_CPP_ROOT}/tests/catalog_event_fields_test.cpp")
add_executable(verdandi_cpp_runtime_boundaries_tests "${VERDANDI_CPP_ROOT}/tests/runtime_boundaries_test.cpp")
add_executable(verdandi_cpp_selector_transaction_tests "${VERDANDI_CPP_ROOT}/tests/selector_transaction_test.cpp")
add_library(verdandi_cpp_core_compile_checks OBJECT "${VERDANDI_CPP_ROOT}/src/catalog.cpp" "${VERDANDI_CPP_ROOT}/src/selector.cpp")
target_sources(verdandi_cpp_catalog_event_fields_tests PRIVATE "${VERDANDI_CPP_ROOT}/src/catalog_event_fields.cpp")
set(VERDANDI_OFFLINE_TARGETS verdandi_cpp_catalog_value_tests verdandi_cpp_catalog_event_fields_tests
    verdandi_cpp_runtime_boundaries_tests verdandi_cpp_selector_transaction_tests verdandi_cpp_core_compile_checks)

if(WIN32)
    add_executable(verdandi_cpp_checkpoint_failure_tests "${VERDANDI_CPP_ROOT}/tests/checkpoint_failure_test.cpp"
        "${VERDANDI_CPP_ROOT}/src/catalog_path.cpp")
    target_include_directories(verdandi_cpp_checkpoint_failure_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/windows")
    target_link_libraries(verdandi_cpp_checkpoint_failure_tests PRIVATE winsqlite3)
    list(APPEND VERDANDI_OFFLINE_TARGETS verdandi_cpp_checkpoint_failure_tests)
    add_test(NAME verdandi_cpp_checkpoint_failure_tests COMMAND verdandi_cpp_checkpoint_failure_tests)
endif()

option(VERDANDI_BUILD_BENCHMARKS "Build the standalone Catalog validation microbenchmark" OFF)
if(VERDANDI_BUILD_BENCHMARKS)
    add_executable(verdandi_cpp_catalog_value_benchmark "${VERDANDI_CPP_ROOT}/tests/catalog_value_benchmark.cpp")
    list(APPEND VERDANDI_OFFLINE_TARGETS verdandi_cpp_catalog_value_benchmark)
endif()

foreach(VERDANDI_TARGET IN LISTS VERDANDI_OFFLINE_TARGETS)
    target_sources(${VERDANDI_TARGET} PRIVATE "${VERDANDI_CPP_ROOT}/src/catalog_value.cpp" "${VERDANDI_CPP_ROOT}/src/error.cpp")
    target_compile_features(${VERDANDI_TARGET} PRIVATE cxx_std_23)
    set_target_properties(${VERDANDI_TARGET} PROPERTIES CXX_EXTENSIONS OFF)
    target_include_directories(${VERDANDI_TARGET} PRIVATE "${VERDANDI_CPP_ROOT}/include" "${VERDANDI_CPP_ROOT}/src")
    if(MSVC)
        target_compile_options(${VERDANDI_TARGET} PRIVATE /utf-8 /W4 /WX /permissive- /Zc:__cplusplus)
    else()
        target_compile_options(${VERDANDI_TARGET} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()
add_executable(verdandi_cpp_legacy_transaction_tests "${VERDANDI_CPP_ROOT}/tests/legacy_transaction_test.cpp")
target_include_directories(verdandi_cpp_legacy_transaction_tests PRIVATE "${VERDANDI_CPP_ROOT}/include")
set_target_properties(verdandi_cpp_legacy_transaction_tests PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(verdandi_cpp_legacy_transaction_tests PRIVATE /utf-8 /W4 /WX /permissive-)
else()
    target_compile_options(verdandi_cpp_legacy_transaction_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME verdandi_cpp_legacy_transaction_tests COMMAND verdandi_cpp_legacy_transaction_tests)
add_test(NAME verdandi_cpp_catalog_value_tests COMMAND verdandi_cpp_catalog_value_tests)
add_test(NAME verdandi_cpp_catalog_event_fields_tests COMMAND verdandi_cpp_catalog_event_fields_tests)
add_test(NAME verdandi_cpp_runtime_boundaries_tests COMMAND verdandi_cpp_runtime_boundaries_tests)
add_test(NAME verdandi_cpp_selector_transaction_tests COMMAND verdandi_cpp_selector_transaction_tests)
set_tests_properties(verdandi_cpp_runtime_boundaries_tests PROPERTIES TIMEOUT 15)
add_test(NAME verdandi_cpp_cmake_regression_tests COMMAND "${CMAKE_COMMAND}"
    "-DCPP_ROOT=${VERDANDI_CPP_ROOT}" "-DWORK_ROOT=${CMAKE_CURRENT_BINARY_DIR}/fixtures"
    "-DGENERATOR=${CMAKE_GENERATOR}" -P "${VERDANDI_CPP_ROOT}/tests/cmake_regression_test.cmake")
