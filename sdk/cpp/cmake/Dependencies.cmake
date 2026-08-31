include(FetchContent)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

find_package(SQLite3 3.37 QUIET)
if(NOT TARGET SQLite::SQLite3)
    if(NOT VERDANDI_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "SQLite3 is required for Catalog checkpoints")
    endif()
    FetchContent_Declare(sqlite_amalgamation
        URL "https://sqlite.org/2026/sqlite-amalgamation-3530400.zip"
        URL_HASH "SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e"
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

find_package(Boost 1.92 CONFIG QUIET COMPONENTS redis)
if(NOT TARGET Boost::redis)
    if(NOT VERDANDI_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "Boost.Redis 1.92 or newer is required")
    endif()
    set(BUILD_TESTING OFF CACHE BOOL "Disable third-party tests" FORCE)
    FetchContent_Declare(boost_headers
        URL "https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.bz2"
        URL_HASH "SHA256=5c1d40cb8e19adbf740a4ec2da35b3e58f3f5804b1dce44deb53df72193cbc6c"
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(boost_headers)
    add_library(verdandi_boost_redis INTERFACE)
    add_library(Boost::redis ALIAS verdandi_boost_redis)
    target_include_directories(verdandi_boost_redis SYSTEM INTERFACE "${boost_headers_SOURCE_DIR}")
    target_link_libraries(verdandi_boost_redis INTERFACE Threads::Threads OpenSSL::Crypto OpenSSL::SSL)
endif()

find_package(yyjson 0.12 CONFIG QUIET)
if(NOT TARGET yyjson)
    if(NOT VERDANDI_FETCH_DEPENDENCIES)
        message(FATAL_ERROR "yyjson 0.12 or newer is required")
    endif()
    set(YYJSON_BUILD_TESTS OFF CACHE BOOL "Disable yyjson tests" FORCE)
    set(YYJSON_BUILD_MISC OFF CACHE BOOL "Disable yyjson tools" FORCE)
    set(YYJSON_BUILD_DOC OFF CACHE BOOL "Disable yyjson documentation" FORCE)
    FetchContent_Declare(yyjson
        GIT_REPOSITORY "https://github.com/ibireme/yyjson.git"
        GIT_TAG "8b4a38dc994a110abaec8a400615567bd996105f"
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(yyjson)
endif()
