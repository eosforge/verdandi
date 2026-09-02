include(FetchContent)

find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto SSL)
find_package(Threads REQUIRED)

# 为一个 fallback 依赖选择远程 URL 或经过摘要复核的本地缓存包。离线模式必须先
# 验证文件存在且内容完全匹配锁定摘要，再把绝对本地路径交给 FetchContent 解压；
# 因此全新构建树可以离线创建，但缺包/坏包绝不会退回网络。
function(verdandi_dependency_source OUTPUT RELATIVE_PATH REMOTE_URL HASH_ALGORITHM EXPECTED_HASH)
    set(ARCHIVE "${VERDANDI_DOWNLOAD_CACHE}/${RELATIVE_PATH}")
    string(TOLOWER "${EXPECTED_HASH}" NORMALIZED_EXPECTED_HASH)

    # 已缓存且正确的归档在在线/离线模式都使用同一个本地 URL，避免切换模式导致
    # FetchContent 误判下载元数据改变并重新解压大型依赖。在线坏包回到远程 URL，
    # 让 FetchContent 删除并重新下载；离线坏包则必须直接失败。
    if(EXISTS "${ARCHIVE}")
        file(${HASH_ALGORITHM} "${ARCHIVE}" ACTUAL_HASH)
        string(TOLOWER "${ACTUAL_HASH}" ACTUAL_HASH)
        if(ACTUAL_HASH STREQUAL NORMALIZED_EXPECTED_HASH)
            set(${OUTPUT} "${ARCHIVE}" PARENT_SCOPE)
            return()
        endif()
        if(VERDANDI_OFFLINE_DEPENDENCIES)
            message(FATAL_ERROR
                "Offline dependency archive checksum mismatch: ${ARCHIVE}. "
                "Expected ${HASH_ALGORITHM}=${NORMALIZED_EXPECTED_HASH}, got ${ACTUAL_HASH}."
            )
        endif()
    endif()

    if(VERDANDI_OFFLINE_DEPENDENCIES)
        message(FATAL_ERROR
            "Offline dependency archive is missing: ${ARCHIVE}. "
            "Run an online configure once to populate the verified shared cache."
        )
    endif()
    set(${OUTPUT} "${REMOTE_URL}" PARENT_SCOPE)
endfunction()

if(NOT VERDANDI_USE_MANAGED_DEPENDENCIES)
    find_package(SQLite3 3.37 QUIET)
endif()
if(NOT TARGET SQLite::SQLite3)
    if(NOT VERDANDI_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "SQLite3 is required for Catalog checkpoints")
    endif()
    verdandi_dependency_source(
        VERDANDI_SQLITE_SOURCE
        "sqlite/sqlite-amalgamation-3530400.zip"
        "https://sqlite.org/2026/sqlite-amalgamation-3530400.zip"
        SHA3_256
        "628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e"
    )
    FetchContent_Declare(sqlite_amalgamation
        URL "${VERDANDI_SQLITE_SOURCE}"
        URL_HASH "SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e"
        DOWNLOAD_DIR "${VERDANDI_DOWNLOAD_CACHE}/sqlite"
        TIMEOUT 600
        INACTIVITY_TIMEOUT 120
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(sqlite_amalgamation)
    add_library(verdandi_sqlite STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
    add_library(SQLite::SQLite3 ALIAS verdandi_sqlite)
    set_target_properties(verdandi_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(verdandi_sqlite SYSTEM PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
    target_compile_definitions(verdandi_sqlite PRIVATE
        SQLITE_DQS=0
        SQLITE_OMIT_DEPRECATED
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_THREADSAFE=1
    )
endif()

if(NOT VERDANDI_USE_MANAGED_DEPENDENCIES)
    find_package(Boost 1.92 CONFIG QUIET COMPONENTS redis)
endif()
if(NOT TARGET Boost::redis)
    if(NOT VERDANDI_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "Boost.Redis 1.92 or newer is required")
    endif()
    set(BUILD_TESTING OFF CACHE BOOL "Disable third-party tests" FORCE)
    verdandi_dependency_source(
        VERDANDI_BOOST_SOURCE
        "boost/boost_1_92_0.tar.bz2"
        "https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2"
        SHA256
        "5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c"
    )
    FetchContent_Declare(boost_headers
        URL "${VERDANDI_BOOST_SOURCE}"
        URL_HASH "SHA256=5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c"
        DOWNLOAD_DIR "${VERDANDI_DOWNLOAD_CACHE}/boost"
        # Boost 完整源码包接近 200 MiB。总超时允许较慢链路完成，无活动超时则
        # 防止代理或连接失效后 CMake 永久阻塞。
        TIMEOUT 3600
        INACTIVITY_TIMEOUT 120
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(boost_headers)
    add_library(verdandi_boost_redis INTERFACE)
    add_library(Boost::redis ALIAS verdandi_boost_redis)
    target_include_directories(verdandi_boost_redis SYSTEM INTERFACE "${boost_headers_SOURCE_DIR}")
    target_link_libraries(verdandi_boost_redis INTERFACE Threads::Threads OpenSSL::Crypto OpenSSL::SSL)
endif()

if(NOT VERDANDI_USE_MANAGED_DEPENDENCIES)
    find_package(yyjson 0.12 CONFIG QUIET)
endif()
if(NOT TARGET yyjson)
    if(NOT VERDANDI_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "yyjson 0.12 or newer is required")
    endif()
    set(YYJSON_BUILD_TESTS OFF CACHE BOOL "Disable yyjson tests" FORCE)
    set(YYJSON_BUILD_MISC OFF CACHE BOOL "Disable yyjson tools" FORCE)
    set(YYJSON_BUILD_DOC OFF CACHE BOOL "Disable yyjson documentation" FORCE)
    verdandi_dependency_source(
        VERDANDI_YYJSON_SOURCE
        "yyjson/8b4a38dc994a110abaec8a400615567bd996105f.tar.gz"
        "https://github.com/ibireme/yyjson/archive/8b4a38dc994a110abaec8a400615567bd996105f.tar.gz"
        SHA256
        "94e9c90f8f12d8329f2c0756782d21e5c278729023cd2c5fe47e323a35947ed6"
    )
    FetchContent_Declare(yyjson
        # 固定提交归档比 git clone 少一个工具依赖，并可与其他不可变下载统一按
        # SHA-256 校验及跨构建树缓存。
        URL "${VERDANDI_YYJSON_SOURCE}"
        URL_HASH "SHA256=94e9c90f8f12d8329f2c0756782d21e5c278729023cd2c5fe47e323a35947ed6"
        DOWNLOAD_DIR "${VERDANDI_DOWNLOAD_CACHE}/yyjson"
        TIMEOUT 600
        INACTIVITY_TIMEOUT 120
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(yyjson)
endif()
