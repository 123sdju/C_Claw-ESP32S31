#pragma once

#include "session_selection.h"

/**
 * @brief 由交互 owner 串行使用的对话记忆交接对象。
 *
 * 本对象没有内部锁，所有调用与借用结果的读取均由外层协调。
 * 流式回调不得重入修改或销毁本对象，memory 必须存活至本对象销毁。
 */
typedef struct session_memory_turns session_memory_turns_t;

typedef struct session_memory_turn_result {
    const char *run_id; /**< 运行 ID，可为 NULL，借用至本对象下一次修改调用。 */
    const char *response; /**< 答复或失败诊断，可为 NULL，借用期同 run_id。 */
    bool completed; /**< 本次调用已确认运行完成。 */
    bool abandoned; /**< 本次调用已确认运行放弃。 */
    bool awaiting_resolution; /**< 本对象仍持有预留，需要核对或重试交接。 */
} session_memory_turn_result_t;

typedef struct session_memory_pending_turn {
    const char *run_id; /**< 借用的运行 ID，有效至本对象下一次修改调用。 */
    bool ready_to_commit; /**< 已确认结果，可重试提交 FIFO。 */
    bool has_answer; /**< 槽中有答复字符串，空字符串也计入。 */
    bool local_submission; /**< 预留来自本对象当前进程的新对话提交。 */
    size_t retained_answer_bytes; /**< 槽中独立保管的答复字节数，包含 NUL。 */
} session_memory_pending_turn_t;

typedef struct session_memory_pump_result {
    size_t committed; /**< 本次成功交接到 FIFO 的槽数。 */
    size_t remaining_ready; /**< 最后一次成功查询得到的 FIFO 项数。 */
    size_t remaining_reserved; /**< 最后一次成功查询得到的预留数。 */
    bool processed; /**< 本次 process_one 返回成功。 */
    cc_managed_memory_worker_result_t processing; /**< 队首处理详情，错误时也可能有部分进展。 */
} session_memory_pump_result_t;

/**
 * @brief owner 创建固定数量的未交接任务槽，借用 memory 至本对象销毁。
 *
 * max_answer_bytes 是每次初始答复预留，实际预留还受 memory 队列预算控制。
 * 成功输出拥有句柄，失败清理本次分配并保持输出为空。
 *
 * 超出预留的已完成答复由槽单独保管，交接时按实际长度申请扩容。
 * @param[in] memory 非空产品记忆服务，借用至 turns 销毁。
 * @param[in] max_outstanding 同时持有的任务槽上限，须大于零且槽数组大小可由 size_t 表示。
 * @param[in] max_answer_bytes 初始答复正文预留字节数，须大于零且小于 SIZE_MAX，底层另计 NUL。
 * @param[out] out_turns 必填，先置 NULL，成功取得拥有对象，使用正常或关闭销毁入口释放。
 * @return CC_OK 表示创建成功，参数无效或分配失败分别返回对应错误。
 */
cc_result_t session_memory_turns_create(managed_memory_service_t *memory,
    size_t max_outstanding, size_t max_answer_bytes, session_memory_turns_t **out_turns);
/**
 * @brief 外层已排空调用时正常销毁，存在任何尚存预留则返回错误并保留整个对象。
 *
 * @pre 外层已关闭入口并排空正在访问 turns 的调用。
 *
 * @param[in,out] turns 待销毁对象，可为 NULL，成功后所有借用失效。
 * @return CC_OK 表示已销毁或参数为 NULL，仍有预留时返回 CC_ERR_INVALID_STATE 并保管对象。
 */
cc_result_t session_memory_turns_destroy(session_memory_turns_t *turns);
/**
 * @brief 已停止 interaction 后按关机规则逐槽取消 RAM 预留并释放答复，再销毁 owner。
 *
 * 中途失败保留对象及剩余槽供重试，已经清理的槽保持空状态。
 * @pre interaction owner 已完全停止，直接借用者已排空。
 *
 * 持久检查点与工具操作继续按各自账本管理。
 * @param[in,out] turns 待清理对象，可为 NULL。
 * @return CC_OK 表示已释放或输入为 NULL。取消失败返回底层错误，保管对象及剩余槽供重试。
 */
cc_result_t session_memory_turns_destroy_for_shutdown(session_memory_turns_t *turns);
/**
 * @brief 只核对 memory service 的 builder/recovery 身份，离线时也可做拥有关系检查。
 *
 * @param[in] turns 待核对对象，可为 NULL。
 * @param[in] builder 调用期间借用运行时 builder，拥有者保证有效。
 * @param[in] recovery 调用期间借用当前恢复装配，拥有者保证有效。
 * @return 依赖属于同一装配时为真，缺失或身份不匹配时为假。
 */
bool session_memory_turns_uses(const session_memory_turns_t *turns,
    const cc_runtime_builder_t *builder, const recovery_service_t *recovery);
/**
 * @brief 统计本 owner 中仍持有预留的槽，FIFO 已提交任务由底层队列另行计数。
 *
 * @param[in] turns 待统计对象，可为 NULL。
 * @return 仍有 reservation 的槽数，turns 为 NULL 时为零。
 */
size_t session_memory_turns_count(const session_memory_turns_t *turns);
/**
 * @brief 按未交接槽的序号读取借用 ID 和状态，输出答案保留字节包括 NUL。
 *
 * 输入 index 只遍历有预留的槽，owner 后续修改前调用者消费借用数据。
 *
 * @param[in] turns 非空待查询对象，调用期间须保持槽状态稳定。
 * @param[in] index 按槽数组顺序枚举仍有预留任务的零基序号，须小于 count 结果。
 * @param[out] out_pending 必填状态快照，参数有效时先清零，run_id 借用至 owner 下一次修改调用。
 * @return CC_OK 表示读取成功，越界返回 CC_ERR_NOT_FOUND，参数无效返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t session_memory_turns_pending_at(const session_memory_turns_t *turns,
    size_t index, session_memory_pending_turn_t *out_pending);
/**
 * @brief 查询底层已提交/预留数量及计费字节，独立保留的答案通过 pending_at 另报。
 *
 * @param[in] turns 非空待查询对象，输出须至少提供一项。
 * @param[out] out_ready 可选输出，FIFO 中已提交项数。
 * @param[out] out_reserved 可选输出，尚未提交的预留数。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @return CC_OK 表示状态已填写，对象无效或所有输出均为空时返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t session_memory_turns_queue_status(session_memory_turns_t *turns,
    size_t *out_ready, size_t *out_reserved, size_t *out_bytes);
/**
 * @brief 借出 memory service 持有的 editor，借用受 service 寿命约束。
 *
 * @param[in] turns 待借出编辑器的对象，可为 NULL。
 * @return 所属 service 拥有的 editor 借用指针，参数为空时返回 NULL。
 */
cc_managed_memory_editor_t *session_memory_turns_editor(session_memory_turns_t *turns);

/**
 * @brief 取得空槽后清理旧借用结果，经 selection 预留并同步执行新对话。
 *
 * 接管其 run/预留和真正完成的答案，失败诊断仅保留给显示。
 * 错误时仍检查 awaiting_resolution，结果字符串借用至本对象下一次修改调用。
 *
 * @param[in,out] turns 非空对象，须有空槽，取得空槽后清理上次借用结果。
 * @param[in,out] selection 非空会话选择状态，调用期间借用并由执行路径更新。
 * @param[in] input 新对话原输入，须与上次返回的借用答复分离。
 * @param[in] on_stream 可选同步流式回调，须遵守 controller 回调契约和本对象禁止重入的约束。
 * @param[in] user_data 传给 on_stream 的上下文，调用期间借用，可为 NULL。
 * @param[out] out_result 必填独立输出，先清零，再发布完成及待核对状态。
 * 不得与 input 或对象内部存储重叠，字符串按 turns 的借用寿命读取。
 * @return 返回本次执行或交接结果，错误时也检查 out_result 的完成标志、借用答复及 awaiting_resolution。
 */
cc_result_t session_memory_turns_submit(session_memory_turns_t *turns,
    session_selection_t *selection, const char *input,
    cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_memory_turn_result_t *out_result);
/**
 * @brief 历史任务先创建原输入预留，既有任务先核对保留证据再复用预留。
 *
 * 完成答案与尚存预留由槽接管，诊断单独保存在 last 供调用者显示。
 * 返回错误也发布部分完成状态，调用者按借用寿命使用结果。
 *
 * @param[in,out] turns 非空对象，历史任务需有空槽，继续前清理上次借用结果。
 * @param[in,out] selection 非空会话选择状态，调用期间借用并由执行路径更新。
 * @param[in] run_id 目标运行的非空 ID，允许借自上次结果，新建槽时先复制再清理旧结果。
 * @param[in] on_stream 可选同步流式回调，须遵守 controller 回调契约和本对象禁止重入的约束。
 * @param[in] user_data 传给 on_stream 的上下文，调用期间借用，可为 NULL。
 * @param[out] out_result 必填独立输出，先清零，完成标志及字符串按 turns 的借用寿命读取。
 * @return 返回本次执行或交接结果，错误时也检查 out_result 的完成标志、借用答复及 awaiting_resolution。
 */
cc_result_t session_memory_turns_resume(session_memory_turns_t *turns,
    session_selection_t *selection, const char *run_id,
    cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_memory_turn_result_t *out_result);
/**
 * @brief 在槽中记录用户放弃意图，通过 selection 保留输入、放弃检查点并排队记忆。
 *
 * input_preserved 与 ready 按各步骤结果累积，失败时保管答案/预留供核对。
 *
 * @param[in,out] turns 非空对象，历史任务需有空槽，失败时保管已取得答案、预留和放弃意图。
 * @param[in,out] selection 非空会话选择状态，由放弃与历史保存路径更新。
 * @param[in] run_id 目标运行的非空 ID，允许借自上次结果。
 * @param[out] out_result 必填独立输出，先清零，再输出放弃确认和借用结果。
 * @return 返回本次执行或交接结果，错误时也检查 out_result 的完成标志、借用答复及 awaiting_resolution。
 */
cc_result_t session_memory_turns_abandon(session_memory_turns_t *turns,
    session_selection_t *selection, const char *run_id,
    session_memory_turn_result_t *out_result);
/**
 * @brief 用户明确放弃本机尚未完成且缺少持久证据的任务，要求保有原始预留。
 *
 * selection 验证缺失证据并保存输入，槽接管返回的预留、答案及完成标志。
 *
 * 待核对工具操作继续由账本管理。
 * @param[in,out] turns 须仍持有本地新对话预留，任务未就绪且没有非空答案。
 * @param[in,out] selection 非空会话选择状态，核实证据缺失并将原输入写入历史。
 * @param[in] run_id 目标运行的稳定标识，调用期间借用。
 * @param[out] out_result 必填独立输出，先清零，再输出放弃确认和借用结果。
 * @return 返回本次执行或交接结果，错误时也检查 out_result 的完成标志、借用答复及 awaiting_resolution。
 */
cc_result_t session_memory_turns_abandon_missing(session_memory_turns_t *turns,
    session_selection_t *selection, const char *run_id, session_memory_turn_result_t *out_result);

/**
 * @brief 只重试已确定可交接的答案，按实际长度扩容后提交 RAM 队列。
 *
 * 扩容遵守队列总字节限制。成功释放槽，失败保留答案和预留供重试。
 * @param[in,out] turns 持有已就绪任务的对象，找到任务后清理上次借用结果。
 * @param[in] run_id 目标任务 ID，允许引用该槽借出的 ID。
 * @return CC_OK 表示已消费预留并释放槽，未就绪、扩容或交接错误保留待处理内容。
 */
cc_result_t session_memory_turns_retry_commit(session_memory_turns_t *turns, const char *run_id);
/**
 * @brief 核对持久证据，结果明确时立即尝试交接既有预留。
 *
 * FINAL_ONLY 可接管原答案，明确请求放弃且已保留输入的空答案也可推进。
 * 其他证据组合留给用户选择。
 * out_inspection 的独立答案副本由调用者 free，转移给槽时该字段清空。
 *
 * @pre 核对期间没有运行修改目标会话。
 * @param[in,out] turns 持有未就绪预留的对象，清理上次结果，并可能在交接成功后释放目标槽。
 * @param[in] selection 调用期间借用会话选择状态，拥有者保证有效。
 * @param[in] run_id 目标运行的稳定标识，调用期间借用。
 * @param[out] out_inspection 必填独立输出，先清零。assistant_text 非空时由调用者 free，错误时也须清理。
 * @return 返回证据检查或后续交接结果。CC_OK 也可能仍需用户核对，须结合 inspection 与槽状态判断。
 */
cc_result_t session_memory_turns_reconcile(session_memory_turns_t *turns,
    session_selection_t *selection, const char *run_id,
    session_memory_turn_inspection_t *out_inspection);
/**
 * @brief 空闲 owner 先重试就绪槽，再最多处理一个 FIFO 队首，释放空间后再次交接。
 *
 * 存储错误时尝试在 builder 维护屏障下重读，原错误仍向上报告，后续 pump 核对保留方案。
 * 直接借用者由外层排空，输出状态与返回错误共同描述部分进展。
 *
 * @pre 外层已排空直接存储借用者，允许恢复路径进入 builder 维护屏障。
 * @param[in,out] turns 非空待推进对象，调用可能释放槽及上次借用结果。
 * @param[in] updated_at 应用提供的非负更新时间，worker 首次成功提取后固定该值。
 * @param[in] cancel_token 可选取消信号，调用期间借用。
 * @param[out] out_result 必填独立输出，先清零，错误时仍按其中的已交接、已处理及剩余数量判断进展。
 * @return 返回提交、查询或处理结果。存储恢复尝试的结果被释放，仍返回原处理错误。
 * 错误发生前后也可能成功交接其他槽，已完成步骤和剩余项目以 out_result 为准。
 */
cc_result_t session_memory_turns_pump(session_memory_turns_t *turns, int64_t updated_at,
    cc_cancel_token_t *cancel_token, session_memory_pump_result_t *out_result);
