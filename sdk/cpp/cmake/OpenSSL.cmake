# OpenSSL 只消费外部准备好的开发包。本文件不获取源码，也不调用包管理器。
# 显式候选根用于 vcpkg 已安装目录和 build/deps 缓存，避免残缺包混用系统库。
if(VERDANDI_OPENSSL_ROOT)
    if(NOT EXISTS "${VERDANDI_OPENSSL_ROOT}/include/openssl/ssl.h")
        message(FATAL_ERROR "The OpenSSL development package is incomplete: ${VERDANDI_OPENSSL_ROOT}/include/openssl/ssl.h is missing.")
    endif()
    set(OPENSSL_ROOT_DIR "${VERDANDI_OPENSSL_ROOT}")
endif()

find_package(OpenSSL 3.0 QUIET COMPONENTS Crypto SSL)
if(NOT OPENSSL_FOUND)
    message(FATAL_ERROR
        "A compatible prebuilt OpenSSL 3.0+ development package is required (headers, Crypto/SSL libraries, and runtime files for shared libraries). "
        "Provide OPENSSL_ROOT_DIR or an existing package through the native build script. "
        "If no suitable binary package is available, build OpenSSL externally. Verdandi will not download or build it.")
endif()

if(VERDANDI_OPENSSL_ROOT)
    file(REAL_PATH "${VERDANDI_OPENSSL_ROOT}" _verdandi_openssl_root)
    foreach(_verdandi_openssl_path IN LISTS OPENSSL_INCLUDE_DIR OPENSSL_CRYPTO_LIBRARY OPENSSL_SSL_LIBRARY)
        # FindOpenSSL 的多配置库列表含有 optimized/debug 关键字，不是文件路径。
        if(_verdandi_openssl_path MATCHES "^(optimized|debug|general)$")
            continue()
        endif()
        file(REAL_PATH "${_verdandi_openssl_path}" _verdandi_openssl_file)
        cmake_path(IS_PREFIX _verdandi_openssl_root "${_verdandi_openssl_file}" NORMALIZE _verdandi_openssl_inside)
        if(NOT _verdandi_openssl_inside)
            message(FATAL_ERROR
                "OpenSSL resolved outside the selected package: ${_verdandi_openssl_path}. "
                "Expected files below ${VERDANDI_OPENSSL_ROOT}. Use a fresh CMake build tree and a complete development package.")
        endif()
    endforeach()
endif()
