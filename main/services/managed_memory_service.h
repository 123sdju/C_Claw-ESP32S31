#pragma once

#include "cc/adapters/cc_managed_memory_editor.h"
#include "cc/adapters/cc_managed_memory_worker.h"
#include "cc/app/cc_runtime_builder.h"
#include "recovery_service.h"

/**
 * @brief 当前恢复装配的受管理记忆服务。
 *
 * 服务借用 builder、recovery、JSON store 和策略/确认回调上下文，拥有文件系统句柄、
 * 快照存储、编辑器、worker 和 model 副本。借用依赖须覆盖服务寿命。
 * 生命周期 owner 协调输入、处理、恢复和卸载，并排空直接 store/editor 使用者。
 * generation 租约保证运行时依赖存活，普通请求与提取请求的 provider 并发由外层协调。
 * 策略及确认回调在 worker 处理锁内执行，禁止重入同一 worker 的处理、重规划、丢弃和销毁入口。
 */
typedef struct managed_memory_service managed_memory_service_t;

/**
 * @brief 产品淘汰规则按重要度、更新时间、ID 字节序逐级选最小项。
 *
 * records 调用期借用，成功只返回索引，规划器负责实际删除与资源归还。
 *
 * @param[in] records 调用期间借用的候选记忆数组，每条须含有效 ID。
 * @param[in] count records 中有效候选记录数量，须大于零。
 * @param[in] user_data 此策略忽略的兼容上下文，允许 NULL。
 * @param[out] out_index 必填，成功取得应淘汰记录的零基索引，失败保持原值。
 * @return CC_OK 表示输出淘汰索引，参数或候选 ID 无效时返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t managed_memory_select_eviction(
    const cc_memory_plan_record_t *records, size_t count, void *user_data, size_t *out_index);
/**
 * @brief 正文/类别重写或移除同时达到 5 条与原条目的 30% 时要求 REWRITE 快照。
 *
 * 使用分解算式求上取整以控制 size_t 溢出，空基线或小变更返回零。
 * @param[in] plan 调用期间借用的方案，读取 rewritten_existing，允许 NULL。
 * @param[in] original_count 生成方案前已有受管理记忆的条数。
 * @param[in] user_data 此策略忽略的兼容上下文，允许 NULL。
 * @return 达到阈值返回 CC_MEMORY_SNAPSHOT_REWRITE，方案为空、基线为空或未达阈值返回零。
 */
cc_memory_snapshot_reason_t managed_memory_decide_snapshot(
    const cc_memory_plan_t *plan, size_t original_count, void *user_data);

/**
 * @brief 在当前恢复装配上组合受管理记忆。
 *
 * 创建前核对装配与 JSON 文件路径，再把普通召回限制到受管理条目。
 * 后续失败会清理本次资源，已设置的召回限制仍然生效。
 * model 被复制，提取选项的 cancel_token 被清空，每次处理单独传入取消信号。
 * @pre owner 协调依赖使用并保持当前挂载装配有效。
 * @param[in,out] builder 借用至服务销毁的运行时 builder，提供当前 JSON store。
 * @param[in,out] recovery 借用至服务销毁的恢复装配，须属于 builder。
 * @param[in] memory_path 当前恢复装配使用的 JSON 记忆路径，调用期间借用。
 * @param[in] worker_options 容量、淘汰、快照及确认策略，标量被复制，策略上下文继续借用。
 * @param[in] extraction_options 模型提取选项，model 在服务创建时复制，取消 token 按次设置。
 * @param[out] out_service 必填，先置 NULL，成功取得调用者拥有的服务，先排空调用再销毁。
 * @return CC_OK 表示创建成功，参数、装配、挂载、文件系统、分配或子对象创建错误向上传递。
 */
cc_result_t managed_memory_service_create(cc_runtime_builder_t *builder, recovery_service_t *recovery,
    const char *memory_path, const cc_managed_memory_worker_options_t *worker_options,
    const cc_memory_extraction_options_t *extraction_options, managed_memory_service_t **out_service);
/**
 * @brief 依次释放 worker、editor、快照存储、文件系统句柄及 model 副本。
 * @pre 入口已关闭，处理、回调、预留使用者和直接 store/editor 调用已排空，借用依赖仍存活。
 * @param[in,out] service 待销毁服务，允许 NULL。
 * @post 非空服务寿命结束，借出的编辑器和全部预留指针失效。
 */
void managed_memory_service_destroy(managed_memory_service_t *service);
/**
 * @brief 借出 service 拥有的 editor，至 service 销毁前按其接口契约使用。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 所属 service 拥有的 editor 借用指针，参数为空时返回 NULL。
 */
cc_managed_memory_editor_t *managed_memory_service_editor(managed_memory_service_t *service);
/**
 * @brief 只比较装配身份，支持介质离线时核对 RAM 预留属于哪个 service。
 * @param[in] service 借用的服务，允许 NULL。
 * @param[in] builder 要比较的 builder 指针。
 * @param[in] recovery 要比较的恢复装配指针。
 * @return 服务存在且 builder/recovery 指针均匹配时为真，其余为假。
 */
bool managed_memory_service_uses(const managed_memory_service_t *service,
    const cc_runtime_builder_t *builder, const recovery_service_t *recovery);
/**
 * @brief 当前盘有效时复制一轮对话到有界 FIFO，重复 turn ID 按队列契约核对内容。
 * @param[in,out] service 借用的服务及其队列。
 * @param[in] turn_id 非空的稳定轮次 ID，调用期间借用。
 * @param[in] session_id 非空的原会话 ID，调用期间借用。
 * @param[in] user_input 非空原输入，同步借用并复制，重复 ID 时逐字比较。
 * @param[in] assistant_text 完整答复，同步借用并复制，重复 ID 时逐字比较。
 * @return CC_OK 表示 FIFO 已接纳或当前队列中已有相同内容，参数、容量、身份、分配或挂载错误向上传递。
 */
cc_result_t managed_memory_service_enqueue(managed_memory_service_t *service,
    const char *turn_id, const char *session_id, const char *user_input, const char *assistant_text);
/**
 * @brief 模型运行前分配有界 RAM 预留，创建前后检查 recovery 的当前挂载。
 *
 * 若创建期间换盘，尝试取消预留并返回错误。
 * 取消也失败时输出句柄仍需 owner 保管。
 * @param[in,out] service 借用的服务及其队列。
 * @param[in] turn_id 非空的稳定轮次 ID，同步借用并复制到预留。
 * @param[in] session_id 非空的原会话 ID，同步借用并复制到预留。
 * @param[in] user_input 非空原输入，同步借用并复制到预留。
 * @param[in] max_answer_bytes 最大答复正文字节数，额外分配一个 NUL 字节，须小于 SIZE_MAX。
 * @param[out] out_reservation 必填，参数有效后先置 NULL，取得句柄后由调用者串行提交、扩容或取消。
 * @return CC_OK 表示预留成功，参数、重复身份、容量、分配或挂载错误向上传递，取消失败时须保管残留输出句柄。
 */
cc_result_t managed_memory_service_reserve(managed_memory_service_t *service,
    const char *turn_id, const char *session_id, const char *user_input,
    size_t max_answer_bytes, cc_memory_pending_reservation_t **out_reservation);
/**
 * @brief 将已预留输入/答案提交到 RAM FIFO，模型期间介质离线仍可保留结果。
 *
 * 成功消费预留句柄，失败保持调用者拥有关系，后续由 turns 核对并重试。
 * @param[in,out] service 借用的服务及其队列，允许介质离线。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @param[in] assistant_text 完整答复，允许空字符串，同步复制到预留缓冲。
 * @return CC_OK 表示句柄已消费并转入 FIFO，答复超长或句柄无效时返回错误并保管原预留。
 */
cc_result_t managed_memory_service_commit(managed_memory_service_t *service,
    cc_memory_pending_reservation_t **reservation, const char *assistant_text);
/**
 * @brief 在同一 RAM 队列预算中扩大答复预留，失败保留原容量与计费。
 *
 * @param[in,out] service 借用的服务及其队列，允许介质离线。
 * @param[in,out] reservation 仍属于该队列的预留句柄。
 * @param[in] max_answer_bytes 新的最大答复正文字节数，额外保留一个 NUL 字节，须小于 SIZE_MAX。
 * @return CC_OK 表示容量已足够或扩容成功，错误时保管既有预留、容量和计费。
 */
cc_result_t managed_memory_service_expand_reservation(managed_memory_service_t *service,
    cc_memory_pending_reservation_t *reservation, size_t max_answer_bytes);
/**
 * @brief RAM 内核对预留的 turn/session/原输入，句柄及字符串在调用中借用。
 *
 * @param[in] service 借用的服务及其队列，允许介质离线。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 同步借用的待比较原输入。
 * @return CC_OK 表示预留身份和原输入匹配，句柄无效或内容冲突返回相应错误。
 */
cc_result_t managed_memory_service_verify_reservation(managed_memory_service_t *service,
    cc_memory_pending_reservation_t *reservation, const char *turn_id,
    const char *session_id, const char *user_input);
/**
 * @brief 返回预留内容的拥有副本，用 cc_memory_pending_item_cleanup 清理。
 *
 * @param[in] service 借用的服务及其队列，允许介质离线。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[out] out_item 必填且无待释放内容，成功取得原输入及空答复的独立副本，用 cc_memory_pending_item_cleanup 清理。
 * @return CC_OK 表示输出拥有副本，参数、句柄或分配错误向上传递。
 */
cc_result_t managed_memory_service_inspect_reservation(managed_memory_service_t *service,
    cc_memory_pending_reservation_t *reservation, cc_memory_pending_item_t *out_item);
/**
 * @brief 取消 RAM 预留并归还预算，成功清空句柄，错误由持有预留的 owner 处理。
 *
 * @param[in,out] service 借用的服务及其队列，允许介质离线。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @return CC_OK 表示预留已释放且句柄置空，句柄无效时返回错误。
 */
cc_result_t managed_memory_service_cancel_reservation(managed_memory_service_t *service,
    cc_memory_pending_reservation_t **reservation);
/**
 * @brief 取得临时 generation 租约，为 worker 借用当前 provider 并附加用量观测包装。
 *
 * 每次仅处理队首，独立模型请求与普通请求由 owner 串行协调，token 仅本次借用。
 * 释放租约后再次核对介质。末次检查报错时队首可能已经确认出队，
 * 其他处理错误保留的队列与提取缓存由 worker 管理，调用者按原 turn 核对。
 * @pre owner 已协调普通请求与独立提取请求的 provider 使用。
 * @param[in,out] service 借用的服务、存储及 worker。
 * @param[in] updated_at 应用提供的非负更新时间，worker 首次成功提取后固定该值。
 * @param[in] cancel_token 可选取消信号，调用期间借用。
 * @param[out] out_result 必填，worker 填写方案计数与本次模型用量，后续失败仍可能保留结果，进入 worker 前失败保持原值。
 * @return 返回队首处理结果，空队列为 CC_ERR_NOT_FOUND，末次挂载检查失败也会报错，此时须核对是否已出队。
 */
cc_result_t managed_memory_service_process_one(managed_memory_service_t *service,
    int64_t updated_at, cc_cancel_token_t *cancel_token, cc_managed_memory_worker_result_t *out_result);
/**
 * @brief 读取 RAM 队列总项数与计费字节，总项包括尚未提交的预留，离线仍可查询。
 * @param[in] service 借用的服务及其 RAM 队列。
 * @param[out] out_pending 可选输出，FIFO 项与预留合计数量。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @return CC_OK 表示状态已填写，对象无效或所有输出均为空时返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t managed_memory_service_status(managed_memory_service_t *service, size_t *out_pending, size_t *out_bytes);
/**
 * @brief 分别读取 FIFO 就绪项、预留数量和总计费字节，至少提供一个输出。
 * @param[in] service 借用的服务及其 RAM 队列。
 * @param[out] out_ready 可选输出，FIFO 中已提交项数。
 * @param[out] out_reserved 可选输出，尚未提交的预留数。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @return CC_OK 表示状态已填写，对象无效或所有输出均为空时返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t managed_memory_service_status_detail(managed_memory_service_t *service,
    size_t *out_ready, size_t *out_reserved, size_t *out_bytes);
/**
 * @brief 由空闲 owner 在 generation 排空屏障下恢复 store，队列与提取缓存保留供核对。
 *
 * 操作前后检查介质，直接借用者由外层先停止，失败返回当前错误供后续处理。
 * @pre 直接 store/editor 借用者已排空，owner 串行执行本次维护。
 * @param[in,out] service 借用的服务及待恢复存储。
 * @return CC_OK 表示屏障内重载及挂载复查完成，屏障、存储或挂载错误向上传递，末次复查失败时存储可能已恢复。
 */
cc_result_t managed_memory_service_recover_store(managed_memory_service_t *service);
