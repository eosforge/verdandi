#ifndef VERDANDI_C_CONFIGURATION_H
#define VERDANDI_C_CONFIGURATION_H

#include "verdandi/c/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 离线校验一份不超过 1 MiB 的严格 v1 JSON；不读取 TLS 文件、不建立连接，也不创建句柄。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_configuration_validate_json(verdandi_bytes_view json, verdandi_error* error);

#ifdef __cplusplus
}
#endif

#endif
