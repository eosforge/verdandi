#ifndef VERDANDI_C_CATALOG_H
#define VERDANDI_C_CATALOG_H

#include "verdandi/c/client.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 一个共享根传输、独立拥有 Catalog Zone 生命周期的 C ABI Client。
typedef struct verdandi_catalog_client verdandi_catalog_client;

/// 一个无任务 Catalog Publisher。
typedef struct verdandi_catalog_publisher verdandi_catalog_publisher;

/// 一个常驻监听和至多一个临时同步任务组成的 Catalog Subscriber。
typedef struct verdandi_catalog_subscriber verdandi_catalog_subscriber;

/// 一个在 Subscriber 生命周期内保持 Path 身份稳定的 Entry。
typedef struct verdandi_catalog_entry verdandi_catalog_entry;

/// 一个不拥有的 Catalog Path。
typedef struct verdandi_catalog_path_view {
    verdandi_string_view part;
    verdandi_string_view id;
} verdandi_catalog_path_view;

/// Catalog 覆盖范围；zone 非零选择全 Zone，其余数组在调用期间借用。
typedef struct verdandi_catalog_subscription {
    int zone;
    const verdandi_string_view* parts;
    size_t part_count;
    const verdandi_catalog_path_view* paths;
    size_t path_count;
} verdandi_catalog_subscription;

/// 使用根 JSON 中的 catalog 配置打开子域；未配置时返回 missing。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_client_open(verdandi_client* root, verdandi_catalog_client** output, verdandi_error* error);

/// 返回 Catalog Client 是否仍接纳新对象。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_client_is_open(const verdandi_catalog_client* value);

/// 关闭 Subscriber 和检查点资源；不关闭根传输。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_client_close(verdandi_catalog_client* value, verdandi_error* error);

/// 尽力关闭并释放 Catalog Client。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_catalog_client_release(verdandi_catalog_client* value);

/// 创建一个无任务轻量 Publisher。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_publisher_create(verdandi_catalog_client* client, verdandi_catalog_publisher** output,
                                                                     verdandi_error* error);

/// 释放 Publisher；不关闭 Catalog Client。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_catalog_publisher_release(verdandi_catalog_publisher* value);

/// 原子发布完整 Value、Array 或 Map；kind 接受稳定字符串 value/array/map。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_replace(verdandi_catalog_publisher* publisher, verdandi_catalog_path_view path, verdandi_string_view kind,
                                                            verdandi_fields_view fields, uint64_t* revision, verdandi_error* error);

/// 在 base_revision 精确匹配时原子覆盖 Array/Map 字段。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_patch(verdandi_catalog_publisher* publisher, verdandi_catalog_path_view path, uint64_t base_revision,
                                                          verdandi_fields_view fields, uint64_t* revision, verdandi_error* error);

/// 原子删除完整 Path 并产生 tombstone revision。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_erase(verdandi_catalog_publisher* publisher, verdandi_catalog_path_view path, uint64_t* revision,
                                                          verdandi_error* error);

/// 完成订阅确认、权威同步和同连接栅栏后创建 Subscriber。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_subscriber_create(verdandi_catalog_client* client, const verdandi_catalog_subscription* subscription,
                                                                      verdandi_catalog_subscriber** output, verdandi_error* error);

/// 查找覆盖范围内的稳定 Entry；未覆盖时 found 为零且 output 保持为空。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_subscriber_find(verdandi_catalog_subscriber* subscriber, verdandi_catalog_path_view path, int* found,
                                                                    verdandi_catalog_entry** output, verdandi_error* error);

/// 非阻塞取得一条同步、恢复、检查点或协议诊断。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_subscriber_try_error(verdandi_catalog_subscriber* value, int* available, verdandi_error* error);

/// 关闭常驻监听和当前临时同步任务。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_subscriber_close(verdandi_catalog_subscriber* value, verdandi_error* error);

/// 尽力关闭并释放 Subscriber。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_catalog_subscriber_release(verdandi_catalog_subscriber* value);

/// 借用 Entry 的 Path Part；视图在 Entry 释放前有效。
VERDANDI_C_API verdandi_string_view VERDANDI_C_CALL verdandi_catalog_entry_part(const verdandi_catalog_entry* value);

/// 借用 Entry 的 Path ID；视图在 Entry 释放前有效。
VERDANDI_C_API verdandi_string_view VERDANDI_C_CALL verdandi_catalog_entry_id(const verdandi_catalog_entry* value);

/// 返回稳定小写状态 synchronizing/present/absent/deleted/unavailable/closed。
VERDANDI_C_API const char* VERDANDI_C_CALL verdandi_catalog_entry_status(const verdandi_catalog_entry* value);

/// 返回 Entry 最后已知完整 revision。
VERDANDI_C_API uint64_t VERDANDI_C_CALL verdandi_catalog_entry_revision(const verdandi_catalog_entry* value);

/// 返回 Entry 当前状态是否为同步完成。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_entry_is_synchronized(const verdandi_catalog_entry* value);

/// 从同一个不可变状态读取完整 Fields；没有 Value 时 present 为零且 output 为空。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_catalog_entry_load(const verdandi_catalog_entry* value, uint64_t* revision, const char** status, int* synchronized,
                                                               int* present, verdandi_field_set** output, verdandi_error* error);

/// 释放稳定 Entry 句柄；不影响 Subscriber 内部身份。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_catalog_entry_release(verdandi_catalog_entry* value);

#ifdef __cplusplus
}
#endif

#endif
