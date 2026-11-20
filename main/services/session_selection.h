#pragma once

#include "cc/adapters/cc_json_segmented_history.h"
#include "cc/app/cc_runtime_controller.h"
#include "managed_memory_service.h"
#include "recovery_service.h"

typedef struct session_selection session_selection_t;
/* 由交互 owner 串行推进的业务阶段，与 controller/挂载状态分别核对。 */
typedef enum {
    SESSION_SELECTION_CHOOSING = 1, /* 等待选择新会话或历史 */
    SESSION_SELECTION_INTERRUPTED,  /* 已选会话仍有中断任务 */
    SESSION_SELECTION_CHATTING,     /* 已选会话可以提交普通输入 */
    SESSION_SELECTION_UNAVAILABLE   /* 当前装配需由外层处理 */
} session_selection_phase_t;

/** @brief 创建等待选择新会话或历史的对象。
 * @param[in] controller 必填、运行且空闲、会话尚未落盘的控制器借用。
 * @param[in] builder 必填、与控制器同属的 builder 借用。
 * @param[in] recovery 必填、与 builder 同属的恢复服务借用。
 * @param[out] out_selection 必填新槽，先置 NULL，成功对象由调用者 destroy。
 * @return CC_OK 或参数、状态、分配错误，诊断由调用者释放。
 * @pre 全部 API 由同一 owner 串行调用，三项依赖覆盖对象寿命。
 */
cc_result_t session_selection_create(cc_runtime_controller_t *controller,
                                     cc_runtime_builder_t *builder,
                                     recovery_service_t *recovery,
                                     session_selection_t **out_selection);
/** @brief 消费选择对象。
 * @param[in,out] selection 可为 NULL 的拥有对象，依赖由外层保管。
 * @pre 入口关闭且全部调用退出，调用者变量自行失效。
 */
void session_selection_destroy(session_selection_t *selection);
/** @brief 读取业务阶段。
 * @param[in] selection 可为 NULL 的串行对象借用。
 * @return 当前阶段，NULL 返回零，无所有权转移。
 */
session_selection_phase_t session_selection_phase(const session_selection_t *selection);
/** @brief 借用所选会话名。
 * @param[in] selection 可为 NULL 的串行对象借用。
 * @return 内部字符串，选择前可为空串，NULL 对象返回 NULL，借用至下次选择、暂留或销毁。
 */
const char *session_selection_session_id(const session_selection_t *selection);
/** @brief 在 CHOOSING 列出当前盘历史。
 * @param[in] selection 串行对象借用，须处于 CHOOSING。
 * @param[out] out_sessions 必填新槽，成功由 cc_session_free_array 消费。
 * @param[out] out_count 必填条数槽，基本参数有效后与数组一同清空。
 * @return CC_OK 或参数、阶段、损坏历史及存储错误，换盘清理输出。
 */
cc_result_t session_selection_list_sessions(session_selection_t *selection,
                                            cc_session_t **out_sessions, size_t *out_count);
/** @brief 在 CHOOSING 预览历史页。
 * @param[in] selection 串行对象借用，要求 CHOOSING 和当前盘。
 * @param[in] session_id 必填非空会话名借用。
 * @param[in] before_id 可为 NULL，NULL 从最新页开始，否则取该 ID 之前消息。
 * @param[in] limit 消息条数预算，交存储校验。
 * @param[out] out_messages 必填新槽，逐项 cleanup 后 free 数组。
 * @param[out] out_count 必填条数槽，基本参数有效后置零。
 * @param[out] out_has_more 必填更早消息标志槽，基本参数有效后置零。
 * @return CC_OK 或参数、阶段、损坏历史及读取错误，换盘清理输出。
 */
cc_result_t session_selection_preview(session_selection_t *selection, const char *session_id,
                                      const char *before_id, size_t limit,
                                      cc_message_t **out_messages, size_t *out_count, int *out_has_more);
/** @brief 在 CHATTING 读取所选会话最新页。
 * @param[in] selection 串行对象借用，要求 CHATTING 和当前盘。
 * @param[in] limit 消息条数预算，交存储校验。
 * @param[out] out_messages 必填新槽，逐项 cleanup 后 free 数组。
 * @param[out] out_count 必填条数槽。
 * @param[out] out_has_more 必填更早消息标志槽。
 * @return CC_OK 或参数、阶段、代际及存储错误，基本参数有效后先清空输出。
 */
cc_result_t session_selection_current_history(session_selection_t *selection, size_t limit,
                                              cc_message_t **out_messages, size_t *out_count,
                                              int *out_has_more);
/** @brief 在 CHOOSING 开始未落盘新会话。
 * @param[in,out] selection 串行对象借用，要求当前盘和 CHOOSING。
 * @return 控制器结果，成功进入 CHATTING，切换后换盘进入 UNAVAILABLE。
 */
cc_result_t session_selection_choose_new(session_selection_t *selection);
/** @brief 在 CHOOSING 选择已有会话。
 * @param[in,out] selection 串行对象借用，要求 CHOOSING。
 * @param[in] session_id 必填非空、小于 96 B 的会话名借用，成功保存副本。
 * @return 参数、阶段、存储或控制器结果，成功按中断任务转 INTERRUPTED/CHATTING，换盘转 UNAVAILABLE。
 */
cc_result_t session_selection_choose_existing(session_selection_t *selection, const char *session_id);
/** @brief 暂留中断任务并返回选择列表。
 * @param[in,out] selection 串行对象借用，要求当前盘、INTERRUPTED 和空闲控制器。
 * @return CC_OK 或阶段、运行状态、存储错误，成功清空选择并转 CHOOSING，检查点及账本保留。
 */
cc_result_t session_selection_defer_interrupted(session_selection_t *selection);
/** @brief 重读中断任务并刷新阶段。
 * @param[in,out] selection 串行对象借用，要求当前盘及 INTERRUPTED。
 * @return 阶段检查或列表结果，无剩余任务时推进 CHATTING，失败保留阶段。
 */
cc_result_t session_selection_refresh_runs(session_selection_t *selection);
/** @brief 幂等保留原输入后放弃所选会话的检查点。
 * @param[in,out] selection 串行对象借用，要求所属 INTERRUPTED 会话。
 * @param[in] run_id 必填非空 run ID 调用期借用。
 * @return 加载、保存、放弃或刷新结果，失败可能已保留输入或放弃任务，换盘转 UNAVAILABLE。
 */
cc_result_t session_selection_abandon_run(session_selection_t *selection, const char *run_id);
/** @brief 同步继续所选会话的中断运行。
 * @param[in,out] selection 串行对象借用，要求所属 INTERRUPTED 会话。
 * @param[in] run_id 必填非空 run ID 借用。
 * @param[in] on_stream 可为 NULL，流数据仅在同步回调内借用。
 * @param[in,out] user_data 可为 NULL 的同步回调上下文借用。
 * @param[out] out_response 必填新槽，先置 NULL，错误时仍可能交付调用者 free 的答复。
 * @param[out] out_completed 必填槽，先置 false，运行完成后置 true，后续刷新错误仍保留。
 * @return 运行或刷新结果，false 标志仍须结合持久证据核对。
 */
cc_result_t session_selection_resume_run_ex(session_selection_t *selection, const char *run_id,
                                            cc_runtime_controller_stream_fn on_stream, void *user_data,
                                            char **out_response, bool *out_completed);
/** @brief 同步继续运行并省略完成标志。
 * @param[in,out] selection 串行对象借用。
 * @param[in] run_id 必填所属中断 run ID 借用。
 * @param[in] on_stream 可为 NULL 的同步回调，流数据仅回调期有效。
 * @param[in,out] user_data 可为 NULL 的回调期借用。
 * @param[out] out_response 必填新槽，错误时仍可能拥有答复，调用者 free。
 * @return 原样透传扩展入口结果。
 */
cc_result_t session_selection_resume_run(session_selection_t *selection, const char *run_id,
                                         cc_runtime_controller_stream_fn on_stream, void *user_data,
                                         char **out_response);

typedef struct session_resume_memory_result {
    bool completed;
    char *response;
    cc_memory_pending_reservation_t *reservation;
} session_resume_memory_result_t;

/** @brief 预留检查点原输入及答案空间后继续运行并提交记忆。
 * @param[in,out] selection 串行对象借用，要求所属中断会话。
 * @param[in] run_id 必填非空 run ID 借用。
 * @param[in,out] memory 必填同属装配的服务，覆盖未决预留寿命。
 * @param[in] max_answer_bytes 正的预期答案字节预算，超预算需扩容重试提交。
 * @param[in] on_stream 可为 NULL 的同步流回调。
 * @param[in,out] user_data 可为 NULL 的回调期上下文借用。
 * @param[out] out_result 必填新槽，先清零，错误时也可能拥有 response/reservation。
 * @return 检查点、预留、运行或提交结果，调用者 free 答复并保管未决预留。
 */
cc_result_t session_selection_resume_run_with_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    size_t max_answer_bytes, cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_resume_memory_result_t *out_result);

/** @brief 验证并转移已有预留后继续运行。
 * @param[in,out] selection 串行对象借用。
 * @param[in] run_id 必填所属 run ID 借用。
 * @param[in,out] memory 必填同属装配服务，覆盖未决预留寿命。
 * @param[in,out] inout_reservation 必填活预留，核对 run/会话/原文后移入结果并清空，禁止结果成员别名。
 * @param[in] on_stream 可为 NULL 的同步流回调。
 * @param[in,out] user_data 可为 NULL 的回调期借用。
 * @param[out] out_result 必填新槽，别名检查后清零，答复由调用者 free，未决预留由其保管。
 * @return 核对前失败保留输入句柄，转移后失败检查结果中的拥有对象。
 */
cc_result_t session_selection_resume_run_with_reserved_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    cc_memory_pending_reservation_t **inout_reservation,
    cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_resume_memory_result_t *out_result);

typedef struct session_abandon_memory_result {
    bool abandoned;
    /* 本次决定结束前已在历史中核对并保留原输入。 */
    bool input_preserved;
    char *assistant_text;
    cc_memory_pending_reservation_t *reservation;
} session_abandon_memory_result_t;

typedef enum session_memory_turn_state {
    SESSION_MEMORY_NO_DURABLE_EVIDENCE = 1,
    SESSION_MEMORY_CHECKPOINT_ONLY,
    SESSION_MEMORY_CHECKPOINT_AND_FINAL,
    SESSION_MEMORY_FINAL_ONLY
} session_memory_turn_state_t;
typedef struct session_memory_turn_inspection {
    session_memory_turn_state_t state;
    bool checkpoint_from_backup;
    char *assistant_text;
} session_memory_turn_inspection_t;

/** @brief 只读核对预留、检查点与最终答复。
 * @param[in] selection 串行对象借用，控制器会话须匹配且无活动 run。
 * @param[in] run_id 必填非空原 run ID 借用。
 * @param[in] memory 必填同属装配的服务借用。
 * @param[in] reservation 必填活预留借用，所有权保留。
 * @param[out] out_inspection 必填新槽，先清零，assistant_text 由调用者 free。
 * @return 参数、身份、存储或读取结果，外部工具结果交账本及人工核对。
 */
cc_result_t session_selection_inspect_reserved_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    cc_memory_pending_reservation_t *reservation, session_memory_turn_inspection_t *out_inspection);

/** @brief 保留原输入及已有答复，放弃检查点后提交记忆。
 * @param[in,out] selection 串行对象借用，要求所属中断会话。
 * @param[in] run_id 必填所属 run ID 借用。
 * @param[in,out] memory 必填同属装配服务，覆盖未决预留寿命。
 * @param[in,out] inout_reservation 可为 NULL 或空句柄，非空时核对并扩容后转移，禁止结果成员别名。
 * @param[out] out_result 必填新槽，别名检查后清零，答复由调用者 free，未决预留由其保管。
 * @return 保存、放弃、提交或刷新结果，错误时仍检查 input_preserved/abandoned 及拥有对象。
 */
cc_result_t session_selection_abandon_run_with_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    cc_memory_pending_reservation_t **inout_reservation,
    session_abandon_memory_result_t *out_result);

/** @brief 明确放弃本次启动且持久证据缺失的新任务并保留输入。
 * @param[in,out] selection 串行对象借用，当前会话须匹配预留。
 * @param[in] run_id 必填非空本次新任务 ID 借用。
 * @param[in,out] memory 必填同属装配服务，覆盖预留寿命。
 * @param[in,out] inout_reservation 必填、本进程新任务路径的活预留，转移后清空，禁止结果成员别名。
 * @param[out] out_result 必填新槽，别名检查后清零，错误时保管答复和未决预留。
 * @return 身份、证据变化、保存、提交或刷新结果，阶段标志反映部分完成。
 * @pre 调用者已明确决定放弃，外部工具结果仍交独立账本核对。
 */
cc_result_t session_selection_abandon_missing_with_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    cc_memory_pending_reservation_t **inout_reservation, session_abandon_memory_result_t *out_result);

typedef struct session_new_turn_memory_result {
    char *run_id;
    bool completed;
    char *response;
    cc_memory_pending_reservation_t *reservation;
    /* 历史损坏的纯对话模式中，模型失败后保留输入，可配空答案排入记忆队列。 */
    bool input_only_ready;
} session_new_turn_memory_result_t;

/** @brief 在 CHATTING 预留原输入和答案预算后同步提交新轮次。
 * @param[in,out] selection 串行对象借用，要求当前盘及 CHATTING。
 * @param[in,out] memory 必填同属装配服务，覆盖未决预留寿命。
 * @param[in] user_input 必填非空 NUL 正文调用期借用，预留复制原文。
 * @param[in] max_answer_bytes 正的预期答案字节预算。
 * @param[in] on_stream 可为 NULL 的同步流回调。
 * @param[in,out] user_data 可为 NULL 的回调期借用。
 * @param[out] out_result 必填新槽，先清零，run_id/response 由调用者 free，未决预留由其保管。
 * @return 预留、运行、提交或挂载错误，错误时仍检查 completed/input_only_ready。
 */
cc_result_t session_selection_submit_turn_with_memory(
    session_selection_t *selection, managed_memory_service_t *memory,
    const char *user_input, size_t max_answer_bytes,
    cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_new_turn_memory_result_t *out_result);
