cmake_minimum_required(VERSION 3.28)

project(verdandi_cpp VERSION 0.1.0 LANGUAGES C CXX)

option(VERDANDI_BUILD_TESTS "Build the Verdandi C++ tests" ON)
option(VERDANDI_FETCH_DEPENDENCIES "Fetch locked third-party dependencies when they are not installed" ON)
option(VERDANDI_USE_MANAGED_DEPENDENCIES "Prefer Verdandi's locked dependency sources over compatible system packages" OFF)
option(VERDANDI_OFFLINE_DEPENDENCIES "Resolve fallback dependencies only from Verdandi's verified local archive cache" OFF)
option(VERDANDI_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(verdandi_enable_warnings TARGET)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE
            /utf-8
            /W4
            /WX
            "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
            "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>"
        )
    else()
        target_compile_options(${TARGET} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endfunction()

# 测试目标保持与生产代码相同的严格警告；仅在 MSVC 下关闭 CRT 对标准
# getenv 的平台弃用提示，避免为了测试环境变量引入非便携的所有权分支。
function(verdandi_enable_test_warnings TARGET)
    verdandi_enable_warnings(${TARGET})
    if(MSVC)
        target_compile_definitions(${TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

# 每个构建树独立保存第三方构建产物，避免 Debug/Release、静态/共享或
# Sanitizer 配置相互重写；调用方仍可显式提供自己的隔离缓存目录。
set(FETCHCONTENT_BASE_DIR "${CMAKE_BINARY_DIR}/_deps" CACHE PATH "Per-build FetchContent dependency directory")

# 压缩包只含不可变、带摘要校验的原始下载，可以安全地跨构建树复用；解压后的源码
# 与编译目录仍由 FETCHCONTENT_BASE_DIR 隔离。统一脚本会把该目录指向根 build/deps，
# 直接调用 CMake 时则默认保存在当前构建树内。
set(VERDANDI_DOWNLOAD_CACHE "${CMAKE_BINARY_DIR}/downloads" CACHE PATH "Shared immutable dependency download cache")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)
include(EmbedProtocol)

verdandi_embed_protocol(VERDANDI_PROTOCOL_SOURCE)

add_library(verdandi_cpp
    src/c/base.cpp
    src/c/catalog.cpp
    src/c/client.cpp
    src/c/registration.cpp
    src/c/selector.cpp
    src/catalog.cpp
    src/catalog_checkpoint.cpp
    src/catalog_subscriber.cpp
    src/client.cpp
    src/configuration.cpp
    src/configuration_json.cpp
    src/driver.cpp
    src/error.cpp
    src/registration.cpp
    src/selector.cpp
    src/script.cpp
    "${VERDANDI_PROTOCOL_SOURCE}"
)
add_library(verdandi::verdandi ALIAS verdandi_cpp)

# C ABI 与原生 C++23 API 位于同一个运行时目标。LINK_ONLY 阻止
# verdandi_cpp 的 C++23 编译特性传播给只包含 C 头文件的调用方。
add_library(verdandi_c INTERFACE)
add_library(verdandi::c ALIAS verdandi_c)

# C++11 Legacy 目标只提供可内联的 RAII/Codec 包装，并继续链接同一个 C ABI
# 运行时；它不拥有 Redis、同步、恢复或生命周期状态机。
add_library(verdandi_legacy INTERFACE)
add_library(verdandi::legacy ALIAS verdandi_legacy)

target_include_directories(verdandi_cpp
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_compile_features(verdandi_cpp PUBLIC cxx_std_23)
target_link_libraries(verdandi_cpp PRIVATE Boost::redis OpenSSL::Crypto OpenSSL::SSL SQLite::SQLite3 yyjson)

if(WIN32)
    # Windows 10 是原生运行时的最低契约，避免 Boost.Asio 按旧版 Windows
    # 自动选择较弱的 API 边界；WINVER 与 _WIN32_WINNT 必须始终一致。
    target_compile_definitions(verdandi_cpp PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
endif()

target_include_directories(verdandi_c INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(verdandi_c INTERFACE "$<LINK_ONLY:verdandi_cpp>")

target_include_directories(verdandi_legacy INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_compile_features(verdandi_legacy INTERFACE cxx_std_11)
target_link_libraries(verdandi_legacy INTERFACE verdandi::c)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(verdandi_cpp PRIVATE VERDANDI_C_BUILD PUBLIC VERDANDI_C_SHARED)
    target_compile_definitions(verdandi_c INTERFACE VERDANDI_C_SHARED)
    if(WIN32)
        set_property(TARGET verdandi_cpp PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS ON)

        # 把 CMake 已知的运行时 DLL 放到 Verdandi DLL 同目录，使 CTest、C# 和
        # 下游源码构建无需依赖生成器或包管理器偶然注入的 PATH。
        add_custom_command(TARGET verdandi_cpp POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:verdandi_cpp>
                $<TARGET_FILE_DIR:verdandi_cpp>
            COMMAND_EXPAND_LISTS
            VERBATIM
        )
    endif()
endif()

verdandi_enable_warnings(verdandi_cpp)

if(VERDANDI_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(verdandi_cpp PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(verdandi_cpp PUBLIC -fsanitize=address,undefined)
endif()

if(VERDANDI_BUILD_TESTS)
    enable_testing()
    add_executable(verdandi_cpp_tests
        tests/schema_test.cpp
    )
    target_link_libraries(verdandi_cpp_tests PRIVATE verdandi::verdandi yyjson)
    verdandi_enable_test_warnings(verdandi_cpp_tests)
    target_compile_definitions(verdandi_cpp_tests PRIVATE
        VERDANDI_CONFIGURATION_CONFORMANCE="${CMAKE_CURRENT_SOURCE_DIR}/../../testkit/conformance/v1/configuration.json"
        VERDANDI_CONFIGURATION_EXAMPLE="${CMAKE_CURRENT_SOURCE_DIR}/../../configuration.example.json"
    )
    add_test(NAME verdandi_cpp_tests COMMAND verdandi_cpp_tests)

    add_executable(verdandi_cpp_redis_tests
        tests/redis_integration_test.cpp
    )
    target_link_libraries(verdandi_cpp_redis_tests PRIVATE verdandi::verdandi)
    verdandi_enable_test_warnings(verdandi_cpp_redis_tests)
    add_test(NAME verdandi_cpp_redis_tests COMMAND verdandi_cpp_redis_tests)
    set_tests_properties(verdandi_cpp_redis_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(verdandi_c_abi_tests tests/c_abi_test.c)
    set_target_properties(verdandi_c_abi_tests PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    target_link_libraries(verdandi_c_abi_tests PRIVATE verdandi::c)
    verdandi_enable_test_warnings(verdandi_c_abi_tests)
    add_test(NAME verdandi_c_abi_tests COMMAND verdandi_c_abi_tests)

    add_executable(verdandi_c_abi_redis_tests tests/c_abi_redis_test.c)
    set_target_properties(verdandi_c_abi_redis_tests PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
    )
    target_link_libraries(verdandi_c_abi_redis_tests PRIVATE verdandi::c)
    verdandi_enable_test_warnings(verdandi_c_abi_redis_tests)
    add_test(NAME verdandi_c_abi_redis_tests COMMAND verdandi_c_abi_redis_tests)
    set_tests_properties(verdandi_c_abi_redis_tests PROPERTIES SKIP_RETURN_CODE 77)

    foreach(VERDANDI_LEGACY_STANDARD IN ITEMS 11 14 17)
        set(VERDANDI_LEGACY_TARGET "verdandi_cpp${VERDANDI_LEGACY_STANDARD}_legacy_tests")
        add_executable(${VERDANDI_LEGACY_TARGET} tests/legacy_consumer_test.cpp)
        set_target_properties(${VERDANDI_LEGACY_TARGET} PROPERTIES
            CXX_STANDARD ${VERDANDI_LEGACY_STANDARD}
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        target_link_libraries(${VERDANDI_LEGACY_TARGET} PRIVATE verdandi::legacy)
        verdandi_enable_test_warnings(${VERDANDI_LEGACY_TARGET})
        add_test(NAME ${VERDANDI_LEGACY_TARGET} COMMAND ${VERDANDI_LEGACY_TARGET})
    endforeach()

    add_executable(verdandi_cpp11_legacy_redis_tests tests/legacy_redis_test.cpp)
    set_target_properties(verdandi_cpp11_legacy_redis_tests PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_link_libraries(verdandi_cpp11_legacy_redis_tests PRIVATE verdandi::legacy)
    verdandi_enable_test_warnings(verdandi_cpp11_legacy_redis_tests)
    add_test(NAME verdandi_cpp11_legacy_redis_tests COMMAND verdandi_cpp11_legacy_redis_tests)
    set_tests_properties(verdandi_cpp11_legacy_redis_tests PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(verdandi_cpp11_legacy_header_tests
        tests/legacy_headers/catalog.cpp
        tests/legacy_headers/client.cpp
        tests/legacy_headers/error.cpp
        tests/legacy_headers/fields.cpp
        tests/legacy_headers/registration.cpp
        tests/legacy_headers/runtime.cpp
        tests/legacy_headers/selector.cpp
        tests/legacy_headers/verdandi.cpp
    )
    set_target_properties(verdandi_cpp11_legacy_header_tests PROPERTIES
        CXX_STANDARD 11
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_link_libraries(verdandi_cpp11_legacy_header_tests PRIVATE verdandi::legacy)
    verdandi_enable_test_warnings(verdandi_cpp11_legacy_header_tests)
    add_test(NAME verdandi_cpp11_legacy_header_tests COMMAND verdandi_cpp11_legacy_header_tests)
endif()

find_program(VERDANDI_CLANG_FORMAT NAMES clang-format clang-format-18)
if(VERDANDI_CLANG_FORMAT)
    file(GLOB_RECURSE VERDANDI_FORMAT_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    )
    add_custom_target(verdandi_cpp_format_check
        COMMAND "${VERDANDI_CLANG_FORMAT}" --dry-run --Werror ${VERDANDI_FORMAT_SOURCES}
        VERBATIM
    )
endif()

find_program(VERDANDI_RUN_CLANG_TIDY NAMES run-clang-tidy run-clang-tidy-18)
if(VERDANDI_RUN_CLANG_TIDY)
    add_custom_target(verdandi_cpp_clang_tidy
        # Clang 18 报告的 concepts 特性宏低于 GCC 13 的 std::expected 门槛；
        # 这两个参数只修正静态分析器的标准库解析环境，不会进入正式编译参数。
        COMMAND "${VERDANDI_RUN_CLANG_TIDY}"
            -p "${CMAKE_BINARY_DIR}"
            -quiet
            -extra-arg=-U__cpp_concepts
            -extra-arg=-D__cpp_concepts=202002L
            "^${CMAKE_CURRENT_SOURCE_DIR}/(src/.*|tests/(?!legacy_).*)\\.cpp$"
        # Legacy 编译单元保持各自的 C++11/14/17 语法环境；不得注入用于
        # GCC 13 C++23 标准库解析的 concepts 特性宏。
        COMMAND "${VERDANDI_RUN_CLANG_TIDY}"
            -p "${CMAKE_BINARY_DIR}"
            -quiet
            "^${CMAKE_CURRENT_SOURCE_DIR}/tests/legacy_.*\\.cpp$"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS verdandi_cpp
        VERBATIM
    )
endif()
