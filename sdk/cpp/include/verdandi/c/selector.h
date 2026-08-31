#ifndef VERDANDI_C_SELECTOR_H
#define VERDANDI_C_SELECTOR_H

#include "verdandi/c/registration.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 一个常驻监听任务和至多一个临时同步任务组成的 C ABI Selector。
typedef struct verdandi_selector verdandi_selector;

/// 仅在策略回调期间有效的借用候选集合。
typedef struct verdandi_candidates verdandi_candidates;

/// 仅在策略回调期间有效的事务选择结果构造器。
typedef struct verdandi_selection verdandi_selection;

/// 一组由 Verdandi 拥有、从 Selector 事务脱离的候选。
typedef struct verdandi_candidate_list verdandi_candidate_list;

/// 一份由 Verdandi 拥有的完整活动及 retained Selector 视图。
typedef struct verdandi_selector_snapshot verdandi_selector_snapshot;

/// 同步策略回调；返回非零提交 selection，返回零并填写 error 回滚事务。
typedef int(VERDANDI_C_CALL* verdandi_selector_policy)(void* context, verdandi_candidates* candidates, verdandi_selection* selection, verdandi_error* error);

/// 为 Registry Type 创建并完成初始同步。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_create(verdandi_registration_client* client, verdandi_string_view type, verdandi_selector** output,
                                                            verdandi_error* error);

/// 返回策略回调当前可见的活动候选数量。
VERDANDI_C_API size_t VERDANDI_C_CALL verdandi_candidates_size(const verdandi_candidates* value);

/// 读取一个借用候选的元数据；UUID 视图仅在策略回调内有效。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_candidates_metadata(const verdandi_candidates* value, size_t index, verdandi_registration_metadata* output);

/// 在策略回调内遍历候选 Attr。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_candidates_visit_attr(const verdandi_candidates* value, size_t index, verdandi_field_visitor visitor, void* context,
                                                                  verdandi_error* error);

/// 在策略回调内遍历当前预测优先的候选 Data。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_candidates_visit_data(const verdandi_candidates* value, size_t index, verdandi_field_visitor visitor, void* context,
                                                                  verdandi_error* error);

/// 在当前事务中用完整 Data 暂存本地预测；失败时不提交。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_candidates_mutate(verdandi_candidates* value, size_t index, verdandi_fields_view data, verdandi_error* error);

/// 把一个候选加入当前 One/Any 结果；One 最多允许一个，Any 拒绝重复项。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selection_add(verdandi_selection* value, size_t index, verdandi_error* error);

/// 同步执行策略并选择零或一个候选；无选择时 output 保持为空。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_one(verdandi_selector* value, verdandi_selector_policy policy, void* context,
                                                         verdandi_candidate_list** output, verdandi_error* error);

/// 同步执行策略并选择任意数量候选；空选择时 output 保持为空。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_any(verdandi_selector* value, verdandi_selector_policy policy, void* context,
                                                         verdandi_candidate_list** output, verdandi_error* error);

/// 返回脱离候选数量；空句柄返回零。
VERDANDI_C_API size_t VERDANDI_C_CALL verdandi_candidate_list_size(const verdandi_candidate_list* value);

/// 读取一个脱离候选的元数据；UUID 视图在候选列表释放前有效。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_candidate_list_metadata(const verdandi_candidate_list* value, size_t index, verdandi_registration_metadata* output);

/// 遍历一个脱离候选的 Attr。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_candidate_list_visit_attr(const verdandi_candidate_list* value, size_t index, verdandi_field_visitor visitor,
                                                                      void* context, verdandi_error* error);

/// 遍历一个脱离候选的 Data。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_candidate_list_visit_data(const verdandi_candidate_list* value, size_t index, verdandi_field_visitor visitor,
                                                                      void* context, verdandi_error* error);

/// 释放脱离候选列表；允许传入空指针。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_candidate_list_release(verdandi_candidate_list* value);

/// 创建完整脱离视图；这是显式重型操作，不执行 Redis I/O。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_snapshot_create(verdandi_selector* value, verdandi_selector_snapshot** output, verdandi_error* error);

/// 返回视图 generation。
VERDANDI_C_API uint64_t VERDANDI_C_CALL verdandi_selector_snapshot_generation(const verdandi_selector_snapshot* value);

/// 返回视图是否完成同步。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_snapshot_is_synchronized(const verdandi_selector_snapshot* value);

/// 返回活动或 retained 候选数量；retained 非零时查询 retained 视图。
VERDANDI_C_API size_t VERDANDI_C_CALL verdandi_selector_snapshot_size(const verdandi_selector_snapshot* value, int retained);

/// 读取活动或 retained 候选元数据。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_snapshot_metadata(const verdandi_selector_snapshot* value, int retained, size_t index,
                                                                       verdandi_registration_metadata* output);

/// 返回 retained 候选的截止 Redis 毫秒；活动候选或越界返回零。
VERDANDI_C_API uint64_t VERDANDI_C_CALL verdandi_selector_snapshot_retained_until(const verdandi_selector_snapshot* value, size_t index);

/// 遍历活动或 retained 候选 Attr。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_snapshot_visit_attr(const verdandi_selector_snapshot* value, int retained, size_t index,
                                                                         verdandi_field_visitor visitor, void* context, verdandi_error* error);

/// 遍历活动或 retained 候选 Data。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_snapshot_visit_data(const verdandi_selector_snapshot* value, int retained, size_t index,
                                                                         verdandi_field_visitor visitor, void* context, verdandi_error* error);

/// 释放完整 Selector 视图。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_selector_snapshot_release(verdandi_selector_snapshot* value);

/// 非阻塞取得一条同步、恢复或协议诊断。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_try_error(verdandi_selector* value, int* available, verdandi_error* error);

/// 关闭常驻监听和当前临时同步任务；幂等。
VERDANDI_C_API int VERDANDI_C_CALL verdandi_selector_close(verdandi_selector* value, verdandi_error* error);

/// 尽力关闭并释放 Selector。
VERDANDI_C_API void VERDANDI_C_CALL verdandi_selector_release(verdandi_selector* value);

#ifdef __cplusplus
}
#endif

#endif
