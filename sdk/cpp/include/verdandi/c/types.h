#ifndef VERDANDI_C_TYPES_H
#define VERDANDI_C_TYPES_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define VERDANDI_C_CALL __cdecl
#if defined(VERDANDI_C_SHARED)
#if defined(VERDANDI_C_BUILD)
#define VERDANDI_C_API __declspec(dllexport)
#else
#define VERDANDI_C_API __declspec(dllimport)
#endif
#else
#define VERDANDI_C_API
#endif
#else
#define VERDANDI_C_CALL
#define VERDANDI_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// 首个稳定 C ABI 版本；它独立于 SDK 和 Redis 协议版本。
#define VERDANDI_C_ABI_VERSION 1U

/// C ABI 错误码、字段和诊断的固定拥有型容量。
#define VERDANDI_C_ERROR_CODE_BYTES 16U
#define VERDANDI_C_ERROR_FIELD_BYTES 256U
#define VERDANDI_C_ERROR_DETAIL_BYTES 513U

/// 一段不拥有、无需零结尾的文本；仅在所属调用约定的生命周期内有效。
typedef struct verdandi_string_view {
    const char* data;
    size_t size;
} verdandi_string_view;

/// 一段不拥有的二进制字节；仅在所属调用约定的生命周期内有效。
typedef struct verdandi_bytes_view {
    const uint8_t* data;
    size_t size;
} verdandi_bytes_view;

/// 一个不拥有名称和值的顶层字段。
typedef struct verdandi_field_view {
    verdandi_string_view name;
    verdandi_bytes_view value;
} verdandi_field_view;

/// 一组调用期间借用的完整顶层字段；字段名必须唯一。
typedef struct verdandi_fields_view {
    const verdandi_field_view* data;
    size_t size;
} verdandi_fields_view;

/// 一个拥有 C ABI 边界错误文本的稳定结果；机器逻辑读取 code 字符串。
typedef struct verdandi_error {
    uint64_t revision;
    uint8_t has_revision;
    char code[VERDANDI_C_ERROR_CODE_BYTES];
    char field[VERDANDI_C_ERROR_FIELD_BYTES];
    char detail[VERDANDI_C_ERROR_DETAIL_BYTES];
} verdandi_error;

/// 一个由 Verdandi 分配并拥有的连续二进制结果。
typedef struct verdandi_blob verdandi_blob;

/// 一个由 Verdandi 分配并拥有、按字段名排序的完整字段结果。
typedef struct verdandi_field_set verdandi_field_set;

/// 逐字段访问借用值；返回非零继续，返回零中止并产生 callback 错误。
typedef int(VERDANDI_C_CALL* verdandi_field_visitor)(void* context, verdandi_string_view name, verdandi_bytes_view value);

/// 返回当前库实现的稳定 C ABI 版本。
VERDANDI_C_API uint32_t VERDANDI_C_CALL verdandi_c_abi_version(void);

/// 查询当前运行库是否实现一项稳定字符串能力；未知、空值或无效视图返回零。
/// 成功只说明代码能力存在，不代表当前 Redis、证书、ACL 或网络部署已经可用。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_c_has_capability(verdandi_string_view capability);

/// 把错误结构重置为没有错误；允许传入空指针。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_error_reset(verdandi_error* value);

/// 返回 Blob 的借用字节；空句柄返回空视图。
VERDANDI_C_API verdandi_bytes_view VERDANDI_C_CALL verdandi_blob_view(const verdandi_blob* value);

/// 释放 Blob；允许传入空指针。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_blob_release(verdandi_blob* value);

/// 返回拥有型字段结果的数量；空句柄返回零。
VERDANDI_C_API size_t VERDANDI_C_CALL verdandi_field_set_size(const verdandi_field_set* value);

/// 按规范字段名顺序借用一个字段；越界或参数无效返回零。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_field_set_at(const verdandi_field_set* value, size_t index, verdandi_field_view* output);

/// 一次遍历拥有型字段结果；回调收到的视图只在该次调用内有效。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_field_set_visit(const verdandi_field_set* value, verdandi_field_visitor visitor, void* context,
                                                            verdandi_error* error);

/// 释放拥有型字段结果；允许传入空指针。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_field_set_release(verdandi_field_set* value);

#ifdef __cplusplus
}
#endif

#endif
