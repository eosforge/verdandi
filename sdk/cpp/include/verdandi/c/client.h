#ifndef VERDANDI_C_CLIENT_H
#define VERDANDI_C_CLIENT_H

#include "verdandi/c/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 一个拥有配置副本和共享 Redis 传输的 C ABI 根 Client。
typedef struct verdandi_client verdandi_client;

/// 从严格 v1 JSON 构造根 Client；成功写入 output，失败写入 error。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_client_open_json(verdandi_bytes_view json, verdandi_client** output, verdandi_error* error);

/// 返回 Client 是否仍接纳新工作；空句柄返回零。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_client_is_open(const verdandi_client* value);

/// 使用配置的命令上限执行一次 Redis PING。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_client_ping(verdandi_client* value, verdandi_error* error);

/// 终止共享传输；幂等且不会删除 Redis 数据。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_client_close(verdandi_client* value, verdandi_error* error);

/// 尽力关闭并释放根 Client；调用方必须先释放所有子域句柄。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_client_release(verdandi_client* value);

/// 读取一个二进制 Key；不存在时 found 为零且 output 保持为空。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_key_load(verdandi_client* client, verdandi_string_view key, int* found, verdandi_blob** output,
                                                     verdandi_error* error);

/// 无 TTL 覆盖写入一个二进制 Key。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_key_store(verdandi_client* client, verdandi_string_view key, verdandi_bytes_view value, verdandi_error* error);

/// 以精确正整数毫秒 TTL 覆盖写入一个二进制 Key。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_key_store_ttl(verdandi_client* client, verdandi_string_view key, verdandi_bytes_view value, uint64_t ttl_ms,
                                                          verdandi_error* error);

/// 删除整个 Key，并把删除前是否存在写入 removed。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_key_erase(verdandi_client* client, verdandi_string_view key, int* removed, verdandi_error* error);

/// 判断 Key 是否存在。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_key_contains(verdandi_client* client, verdandi_string_view key, int* present, verdandi_error* error);

/// 为现存 Key 设置精确正整数毫秒 TTL，并报告是否实际设置。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_key_expire(verdandi_client* client, verdandi_string_view key, uint64_t ttl_ms, int* changed, verdandi_error* error);

/// 读取一个完整 Redis Hash；不存在时返回空字段集合。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_hash_load(verdandi_client* client, verdandi_string_view key, verdandi_field_set** output, verdandi_error* error);

/// 用一次 HSET 原子写入完整字段集合；空集合被拒绝。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_hash_store(verdandi_client* client, verdandi_string_view key, verdandi_fields_view value, verdandi_error* error);

/// 删除 names 指定的 Hash 字段，并返回实际删除数量。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_hash_erase(verdandi_client* client, verdandi_string_view key, const verdandi_string_view* names, size_t count,
                                                       size_t* removed, verdandi_error* error);

/// 判断 Hash 中是否存在指定字段。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_hash_contains(verdandi_client* client, verdandi_string_view key, verdandi_string_view name, int* present,
                                                          verdandi_error* error);

/// 返回 Hash 当前字段数量。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_hash_size(verdandi_client* client, verdandi_string_view key, size_t* size, verdandi_error* error);

#ifdef __cplusplus
}
#endif

#endif
