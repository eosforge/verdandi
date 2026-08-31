#ifndef VERDANDI_C_REGISTRATION_H
#define VERDANDI_C_REGISTRATION_H

#include "verdandi/c/client.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 一个共享根传输、独立拥有 Registration Zone 生命周期的 C ABI Client。
typedef struct verdandi_registration_client verdandi_registration_client;

/// 一条延迟发布、拥有唯一同步任务的 C ABI Registration。
typedef struct verdandi_registration verdandi_registration;

/// 构造 Registration 所需的固定选项。
typedef struct verdandi_registration_options {
    verdandi_string_view type;
    uint64_t ttl_ms;
    uint64_t renew_interval_ms;
    uint64_t version;
    uint8_t has_renew_interval;
} verdandi_registration_options;

/// Selector 和脱离候选可见的稳定 Registration 元数据。
typedef struct verdandi_registration_metadata {
    verdandi_string_view uuid;
    uint64_t revision;
    uint64_t timestamp;
    uint64_t ttl_ms;
    uint64_t version;
} verdandi_registration_metadata;

/// 使用根 JSON 中的 registration 配置打开子域；未配置该域时返回 missing。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_client_open(verdandi_client* root, verdandi_registration_client** output, verdandi_error* error);

/// 返回 Registration Client 是否仍接纳对象。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_client_is_open(const verdandi_registration_client* value);

/// 关闭并汇合该域全部 Registration 和 Selector；不关闭根传输。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_client_close(verdandi_registration_client* value, verdandi_error* error);

/// 尽力关闭并释放 Registration Client。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_registration_client_release(verdandi_registration_client* value);

/// 本地构造一条未发布 Registration；不执行 Redis I/O。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_create(verdandi_registration_client* client, const verdandi_registration_options* options,
                                                                verdandi_registration** output, verdandi_error* error);

/// 借用构造时生成的 32 位 UUID；视图在 Registration 释放前有效。
VERDANDI_C_API verdandi_string_view VERDANDI_C_CALL verdandi_registration_uuid(const verdandi_registration* value);

/// 返回 Registration 是否已完成首次发布。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_is_published(const verdandi_registration* value);

/// 返回当前期望内容 revision。
VERDANDI_C_API uint64_t VERDANDI_C_CALL verdandi_registration_revision(const verdandi_registration* value);

/// 返回最近一次 Redis 确认的毫秒时间戳。
VERDANDI_C_API uint64_t VERDANDI_C_CALL verdandi_registration_timestamp(const verdandi_registration* value);

/// 发布完整不可变 Attr 和完整 Data，并启动唯一同步任务。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_publish(verdandi_registration* value, verdandi_fields_view attr, verdandi_fields_view data,
                                                                 verdandi_error* error);

/// 提交完整期望 Data；固定字段集合不得改变。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_update(verdandi_registration* value, verdandi_fields_view data, verdandi_error* error);

/// 只修改应用 Version。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_set_version(verdandi_registration* value, uint64_t version, verdandi_error* error);

/// 用一个内容 revision 同时修改 Version 和完整期望 Data。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_update_content(verdandi_registration* value, uint64_t version, verdandi_fields_view data,
                                                                        verdandi_error* error);

/// 显式续期，只刷新 timestamp 和 TTL。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_renew(verdandi_registration* value, verdandi_error* error);

/// 终止、排空并尽力删除 Redis Registration；幂等。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_close(verdandi_registration* value, verdandi_error* error);

/// 非阻塞取得一条异步诊断；成功但没有诊断时 available 为零。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_registration_try_error(verdandi_registration* value, int* available, verdandi_error* error);

/// 尽力关闭并释放 Registration。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_registration_release(verdandi_registration* value);

#ifdef __cplusplus
}
#endif

#endif
