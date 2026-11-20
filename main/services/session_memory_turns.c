#include "session_memory_turns.h"
#include "cc/internal/cc_alloc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct pending_turn {
    char *run_id;
    cc_memory_pending_reservation_t *reservation;
    char *answer;
    bool ready;
    bool submitted_here;
    bool input_only;
    bool abandon_requested;
    bool input_preserved;
} pending_turn_t;

struct session_memory_turns {
    managed_memory_service_t *memory;
    pending_turn_t *slots;
    size_t slot_count;
    size_t max_answer_bytes;
    char *last_run_id;
    char *last_answer;
};

/**
 * @brief 清零字符串已使用的正文后释放，用于本 owner 持有的答复。
 * @param[in,out] value 调用者交给本函数清零并释放的拥有字符串，可为 NULL。
 * @post 输入正文已清零并释放，来参值本身由调用方管理。
 */
static void wipe_free(char *value)
{
    if (!value) return;
    size_t length=strlen(value);
    volatile char *bytes=(volatile char *)value;
    while (length) bytes[--length]=0;
    free(value);
}

/**
 * @brief 使上次完成结果的借用失效并释放 run/answer，调用者先处理所有旧借用。
 * @param[in,out] turns 非空对象，清理其上次结果，调用前须结束相关借用。
 * @post 旧 run/answer 已释放并置空，此前返回的相关借用结束寿命。
 */
static void clear_last(session_memory_turns_t *turns)
{
    free(turns->last_run_id); turns->last_run_id=NULL;
    wipe_free(turns->last_answer); turns->last_answer=NULL;
}

/**
 * @brief owner 串行查找未完成交接的任务槽，返回内部借用指针。
 * @param[in] turns 待查找对象，可为 NULL。
 * @param[in] run_id 目标运行的稳定标识，调用期间借用。
 * @return 匹配的内部任务槽借用指针，参数无效或未找到时为 NULL。
 */
static pending_turn_t *find_turn(session_memory_turns_t *turns, const char *run_id)
{
    if (!turns || !run_id || !run_id[0]) return NULL;
    for (size_t i=0;i<turns->slot_count;i++)
        if (turns->slots[i].run_id && strcmp(turns->slots[i].run_id,run_id)==0)
            return &turns->slots[i];
    return NULL;
}

/**
 * @brief 只选择 run ID 和预留句柄均为空的槽，防止覆盖尚需核对的任务。
 * @param[in] turns 非空对象，调用期间由 owner 独占槽数组。
 * @return run ID 与预留均为空的内部槽，容量耗尽时为 NULL。
 */
static pending_turn_t *free_turn(session_memory_turns_t *turns)
{
    for (size_t i=0;i<turns->slot_count;i++)
        if (!turns->slots[i].run_id && !turns->slots[i].reservation) return &turns->slots[i];
    return NULL;
}

/**
 * @brief 预留已提交或取消后释放槽内拥有的 run/answer 并清零。
 *
 * reservation 的生命周期须由调用者先完成，本 helper 只清理槽的独有字段。
 * @param[in,out] turn 非空内部槽，预留必须已提交或取消。
 * @post 槽内 run/answer 已释放且槽已清零，预留生命周期由调用者先完成。
 */
static void release_turn(pending_turn_t *turn)
{
    free(turn->run_id); wipe_free(turn->answer); memset(turn,0,sizeof(*turn));
}

/**
 * @brief 将结果按借用形式交给调用者。
 *
 * 有预留时 run/answer 留在原槽，等待核对。
 * 无预留时移到 last 字段并回收槽，借用有效期到下一次修改 owner 的调用。
 * 调用者已清理上次 last，完成标志与预留是否尚存分别输出。
 * @param[in,out] turns 非空对象，last 字段须已清理。
 * @param[in,out] turn 待发布内部槽，无预留时将 run_id 和 answer 的所有权转给 last 并清零槽。
 * @param[in] completed 本次运行是否已经完成。
 * @param[in] abandoned 本次运行是否已经按用户请求放弃。
 * @param[out] out 完成标志及 run/answer 借用，下一次修改 owner 前使用。
 * @post out 获得完成标志和借用结果，拥有资源留在槽或转入 last 字段。
 */
static void publish_result(session_memory_turns_t *turns, pending_turn_t *turn,
                           bool completed, bool abandoned, session_memory_turn_result_t *out)
{
    out->completed=completed; out->abandoned=abandoned;
    out->awaiting_resolution=turn->reservation!=NULL;
    if (turn->reservation) {
        out->run_id=turn->run_id; out->response=turn->answer;
    } else {
        turns->last_run_id=turn->run_id; turns->last_answer=turn->answer;
        turn->run_id=NULL; turn->answer=NULL; memset(turn,0,sizeof(*turn));
        out->run_id=turns->last_run_id; out->response=turns->last_answer;
    }
}

/**
 * @brief owner 创建固定数量的未交接任务槽，借用 memory 至本对象销毁。
 *
 * max_answer_bytes 是每次初始答复预留，实际预留还受 memory 队列预算控制。
 * 成功输出拥有句柄，失败清理本次分配并保持输出为空。
 * @param[in] memory 非空记忆服务，借用至本对象销毁。
 * @param[in] max_outstanding 同时持有的任务槽上限，须大于零且槽数组大小可由 size_t 表示。
 * @param[in] max_answer_bytes 初始答复正文预留容量，须大于零且小于 SIZE_MAX，底层另计 NUL。
 * @param[out] out_turns 必填，先置 NULL，成功取得拥有对象，使用正常或关闭销毁入口释放。
 * @return CC_OK 表示分配成功，参数无效或分配失败分别返回对应错误。
 */
cc_result_t session_memory_turns_create(managed_memory_service_t *memory,
    size_t max_outstanding, size_t max_answer_bytes, session_memory_turns_t **out_turns)
{
    if (!out_turns) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory turn owner output");
    *out_turns=NULL;
    if (!memory || !max_outstanding || !max_answer_bytes || max_answer_bytes==SIZE_MAX ||
        max_outstanding>SIZE_MAX/sizeof(pending_turn_t))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory turn limits");
    session_memory_turns_t *turns=calloc(1,sizeof(*turns));
    if (!turns) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate memory turn owner");
    turns->slots=calloc(max_outstanding,sizeof(*turns->slots));
    if (!turns->slots) { free(turns); return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate memory turn slots"); }
    turns->memory=memory; turns->slot_count=max_outstanding; turns->max_answer_bytes=max_answer_bytes;
    *out_turns=turns; return cc_result_ok();
}

/**
 * @brief 外层已排空调用时正常销毁，存在任何尚存预留则返回错误并保留整个对象。
 * @pre 外层已关闭入口并排空正在访问 turns 的调用。
 * @param[in,out] turns 待销毁对象，可为 NULL，成功释放后全部借用失效。
 * @return CC_OK 表示已销毁或参数为 NULL，仍有预留时返回 CC_ERR_INVALID_STATE 并保管对象。
 */
cc_result_t session_memory_turns_destroy(session_memory_turns_t *turns)
{
    if (!turns) return cc_result_ok();
    if (session_memory_turns_count(turns))
        return cc_result_error(CC_ERR_INVALID_STATE,"Memory turns still require resolution");
    clear_last(turns); free(turns->slots); free(turns); return cc_result_ok();
}

/**
 * @brief 已停止 interaction 后按关机规则逐槽取消 RAM 预留并释放答复，再销毁 owner。
 *
 * 中途失败保留对象及剩余槽供重试，已经清理的槽保持空状态。
 * @pre interaction owner 已完全停止，直接借用者已排空。
 * @param[in,out] turns 待关闭对象，可为 NULL。按槽数组顺序取消预留并释放槽。
 * @return CC_OK 表示已销毁或输入为 NULL。取消失败时立即返回底层错误，保留对象及剩余槽供重试。
 */
cc_result_t session_memory_turns_destroy_for_shutdown(session_memory_turns_t *turns)
{
    if (!turns) return cc_result_ok();
    for (size_t i=0;i<turns->slot_count;i++) {
        pending_turn_t *turn=&turns->slots[i];
        if (turn->reservation) {
            cc_result_t rc=managed_memory_service_cancel_reservation(turns->memory,&turn->reservation);
            if (rc.code!=CC_OK) return rc;
            cc_result_free(&rc);
        }
        release_turn(turn);
    }
    return session_memory_turns_destroy(turns);
}

/**
 * @brief 只核对 memory service 的 builder/recovery 身份，离线时也可做拥有关系检查。
 * @param[in] turns 待核对对象，可为 NULL。
 * @param[in] builder 调用期间借用运行时 builder，拥有者保证有效。
 * @param[in] recovery 调用期间借用当前恢复装配，拥有者保证有效。
 * @return 依赖属于同一装配时为真，缺失或身份不匹配时为假。
 */
bool session_memory_turns_uses(const session_memory_turns_t *turns,
    const cc_runtime_builder_t *builder, const recovery_service_t *recovery)
{ return turns && managed_memory_service_uses(turns->memory,builder,recovery); }

/**
 * @brief 借出 memory service 持有的 editor，借用受 service 寿命约束。
 * @param[in] turns 待借出编辑器的对象，可为 NULL。
 * @return 所属 service 拥有的 editor 借用指针，参数为空时返回 NULL。
 */
cc_managed_memory_editor_t *session_memory_turns_editor(session_memory_turns_t *turns)
{ return turns ? managed_memory_service_editor(turns->memory) : NULL; }

/**
 * @brief 查询底层已提交/预留数量及计费字节，独立保留的答案通过 pending_at 另报。
 * @param[in] turns 非空待查询对象，输出须至少提供一项。
 * @param[out] out_ready 可选输出，FIFO 中已提交项数。
 * @param[out] out_reserved 可选输出，尚未提交的预留数。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @return CC_OK 表示状态已填写，对象无效或所有输出均为空时返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t session_memory_turns_queue_status(session_memory_turns_t *turns,
    size_t *out_ready, size_t *out_reserved, size_t *out_bytes)
{
    if (!turns) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory turn owner");
    return managed_memory_service_status_detail(turns->memory,out_ready,out_reserved,out_bytes);
}

/**
 * @brief 统计本 owner 中仍持有预留的槽，FIFO 已提交任务由底层队列另行计数。
 * @param[in] turns 待统计对象，可为 NULL。
 * @return 仍有 reservation 的槽数，turns 为 NULL 时为零。
 */
size_t session_memory_turns_count(const session_memory_turns_t *turns)
{
    size_t count=0;
    if (turns) for (size_t i=0;i<turns->slot_count;i++) if (turns->slots[i].reservation) count++;
    return count;
}

/**
 * @brief 按未交接槽的序号读取借用 ID 和状态，输出答案保留字节包括 NUL。
 *
 * 输入 index 只遍历有预留的槽，owner 后续修改前调用者消费借用数据。
 * @param[in] turns 非空待查询对象，调用期间须保持槽状态稳定。
 * @param[in] index 按槽数组顺序枚举有预留槽的零基序号，须小于 count 结果。
 * @param[out] out_pending 必填状态快照，参数有效时先清零，run_id 借用至 owner 下一次修改调用。
 * @return CC_OK 表示读取成功，越界返回 CC_ERR_NOT_FOUND，参数无效返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t session_memory_turns_pending_at(const session_memory_turns_t *turns,
    size_t index, session_memory_pending_turn_t *out_pending)
{
    if (!turns || !out_pending) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid pending turn output");
    memset(out_pending,0,sizeof(*out_pending));
    for (size_t i=0;i<turns->slot_count;i++) {
        const pending_turn_t *turn=&turns->slots[i];
        if (!turn->reservation) continue;
        if (index--) continue;
        out_pending->run_id=turn->run_id; out_pending->ready_to_commit=turn->ready;
        out_pending->has_answer=turn->answer!=NULL;
        out_pending->local_submission=turn->submitted_here;
        out_pending->retained_answer_bytes=turn->answer ? strlen(turn->answer)+1U : 0U;
        return cc_result_ok();
    }
    return cc_result_error(CC_ERR_NOT_FOUND,"Pending memory turn not found");
}

/**
 * @brief 取得空槽后清理旧借用结果，经 selection 预留并同步执行新对话。
 *
 * 接管其 run/预留和真正完成的答案，失败诊断仅保留给显示。
 * 取得空槽后清理 last，因此 input 不得引用此前借出的 response。
 * @param[in,out] turns 非空对象，须有空槽，取得空槽后清理 last。
 * @param[in,out] selection 非空会话选择状态，调用期间借用并由执行路径更新。
 * @param[in] input 新对话原输入，须与上次返回的借用答复分离。
 * @param[in] on_stream 可选同步流式回调，不得重入修改或销毁 turns。
 * @param[in] user_data 传给 on_stream 的上下文，调用期间借用，可为 NULL。
 * @param[out] out_result 必填独立输出，先清零，不得与 input 或对象内部存储重叠。
 * @return 返回本次执行或交接结果，错误时也检查 out_result 的完成标志、借用答复及 awaiting_resolution。
 */
cc_result_t session_memory_turns_submit(session_memory_turns_t *turns,
    session_selection_t *selection, const char *input,
    cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_memory_turn_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory turn output");
    memset(out_result,0,sizeof(*out_result));
    if (!turns || !selection || !input || !input[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory turn submission");
    pending_turn_t *turn=free_turn(turns);
    if (!turn) return cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Memory turn slots are full");
    clear_last(turns);
    session_new_turn_memory_result_t result={0};
    cc_result_t rc=session_selection_submit_turn_with_memory(selection,turns->memory,input,
        turns->max_answer_bytes,on_stream,user_data,&result);
    /* 运行失败返回的诊断文字供显示使用，只有真实完成的答复才进入记忆评估。 */
    char *diagnostic=result.completed ? NULL : result.response;
    turn->run_id=result.run_id; turn->reservation=result.reservation;
    turn->answer=result.completed ? result.response : NULL;
    turn->submitted_here=result.reservation!=NULL;
    turn->input_only=result.input_only_ready;
    turn->ready=(result.completed || result.input_only_ready) && result.reservation;
    publish_result(turns,turn,result.completed,false,out_result);
    if (diagnostic) { turns->last_answer=diagnostic; out_result->response=diagnostic; }
    return rc;
}

/**
 * @brief 重试前核对保留答复与持久证据，匹配后释放旧答复。
 *
 * 放弃失败后的重试先核实已有观察，再推进状态。
 * @param[in] turns 提供记忆服务的非空对象，本函数不修改其字段。
 * @param[in] selection 调用期间借用会话选择状态，拥有者保证有效。
 * @param[in,out] turn 非空槽。匹配后清理 answer，冲突或读取失败保留 answer。
 * @return CC_OK 表示可继续执行，槽已就绪或证据冲突返回 CC_ERR_INVALID_STATE，读取错误向上传递。
 */
static cc_result_t prepare_retry(session_memory_turns_t *turns, session_selection_t *selection,
                                 pending_turn_t *turn)
{
    if (turn->ready) return cc_result_error(CC_ERR_INVALID_STATE,"Completed memory turn needs commit");
    if (!turn->answer) return cc_result_ok();
    session_memory_turn_inspection_t inspection={0};
    cc_result_t rc=session_selection_inspect_reserved_memory(selection,turn->run_id,turns->memory,
                                                              turn->reservation,&inspection);
    if (rc.code==CC_OK) {
        bool matches=(inspection.state==SESSION_MEMORY_CHECKPOINT_ONLY && !turn->answer[0]) ||
            (inspection.state==SESSION_MEMORY_CHECKPOINT_AND_FINAL && inspection.assistant_text &&
             strcmp(inspection.assistant_text,turn->answer)==0);
        if (!matches) rc=cc_result_error(CC_ERR_INVALID_STATE,"Memory turn evidence changed; reconcile before retry");
        else { wipe_free(turn->answer); turn->answer=NULL; }
    }
    free(inspection.assistant_text); return rc;
}

/**
 * @brief 找到或创建 run 对应槽，复制新 ID 后再清 last，允许 run_id 借自上次结果。
 *
 * out_turn 借用内部槽，out_new 区分历史任务首次接管与既有预留重试。
 * @param[in,out] turns 待查找对象，成功时清理 last，新任务占用一个空槽。
 * @param[in] run_id 非空运行 ID，可引用 last_run_id 或既有槽 ID。
 * @param[out] out_turn 必填，成功时借出内部槽，失败时不提供可用结果。
 * @param[out] out_new 必填，成功时标明是否新建槽，调用者仅在成功后读取。
 * @return CC_OK 表示取得既有或新槽，容量、参数及 run ID 分配错误向上传递。
 */
static cc_result_t begin_existing(session_memory_turns_t *turns, const char *run_id,
                                  pending_turn_t **out_turn, bool *out_new)
{
    if (!turns || !run_id || !run_id[0]) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory run id");
    pending_turn_t *turn=find_turn(turns,run_id);
    *out_new=false;
    if (!turn) {
        turn=free_turn(turns);
        if (!turn) return cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Memory turn slots are full");
        turn->run_id=cc_copy_string(run_id);
        if (!turn->run_id) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot retain memory run id");
        *out_new=true;
    }
    /* 先复制新 run ID，再清理旧借用结果，因为来参可能指向 last_run_id。 */
    clear_last(turns); *out_turn=turn; return cc_result_ok();
}

/**
 * @brief 历史任务先创建原输入预留，既有任务先核对保留证据再复用预留。
 *
 * 完成答案与尚存预留由槽接管，诊断单独保存在 last 供调用者显示。
 * 返回错误也发布部分完成状态，调用者按借用寿命使用结果。
 * @param[in,out] turns 非空对象，历史任务占用空槽，已有任务复用同一预留。
 * @param[in,out] selection 非空会话选择状态，由继续运行路径更新。
 * @param[in] run_id 非空运行 ID，允许借自上次结果，新建槽先复制再清理 last。
 * @param[in] on_stream 可选同步流式回调，不得重入修改或销毁 turns。
 * @param[in] user_data 传给 on_stream 的上下文，调用期间借用，可为 NULL。
 * @param[out] out_result 必填独立输出，先清零，再输出完成状态与借用字符串。
 * @return 返回本次执行或交接结果，错误时也检查 out_result 的完成标志、借用答复及 awaiting_resolution。
 */
cc_result_t session_memory_turns_resume(session_memory_turns_t *turns,
    session_selection_t *selection, const char *run_id,
    cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_memory_turn_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory resume output");
    memset(out_result,0,sizeof(*out_result));
    if (!selection) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory resume selection");
    pending_turn_t *turn=NULL; bool is_new=false;
    cc_result_t rc=begin_existing(turns,run_id,&turn,&is_new);
    if (rc.code!=CC_OK) return rc;
    if (!is_new) rc=prepare_retry(turns,selection,turn);
    session_resume_memory_result_t result={0};
    if (rc.code==CC_OK) {
        if (is_new) rc=session_selection_resume_run_with_memory(selection,turn->run_id,turns->memory,
            turns->max_answer_bytes,on_stream,user_data,&result);
        else rc=session_selection_resume_run_with_reserved_memory(selection,turn->run_id,turns->memory,
            &turn->reservation,on_stream,user_data,&result);
    }
    char *diagnostic=result.completed ? NULL : result.response;
    if (result.reservation) turn->reservation=result.reservation;
    if (result.completed && result.response) turn->answer=result.response;
    if (result.completed && turn->reservation) turn->ready=true;
    publish_result(turns,turn,result.completed,false,out_result);
    if (diagnostic) { turns->last_answer=diagnostic; out_result->response=diagnostic; }
    return rc;
}

/**
 * @brief 在槽中记录用户放弃意图，通过 selection 保留输入、放弃检查点并排队记忆。
 *
 * input_preserved 与 ready 按各步骤结果累积，失败时保管答案/预留供核对。
 * @param[in,out] turns 非空对象，历史任务占用空槽，失败时累计保存放弃意图和输入保存状态。
 * @param[in,out] selection 非空会话选择状态，由放弃与历史保存路径更新。
 * @param[in] run_id 非空运行 ID，允许借自上次结果。
 * @param[out] out_result 必填独立输出，先清零，再输出放弃确认与借用字符串。
 * @return 返回本次执行或交接结果，错误时也检查 out_result 的完成标志、借用答复及 awaiting_resolution。
 */
cc_result_t session_memory_turns_abandon(session_memory_turns_t *turns,
    session_selection_t *selection, const char *run_id, session_memory_turn_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory abandonment output");
    memset(out_result,0,sizeof(*out_result));
    if (!selection) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory abandonment selection");
    pending_turn_t *turn=NULL; bool is_new=false;
    cc_result_t rc=begin_existing(turns,run_id,&turn,&is_new);
    if (rc.code!=CC_OK) return rc;
    if (!is_new) rc=prepare_retry(turns,selection,turn);
    session_abandon_memory_result_t result={0};
    if (rc.code==CC_OK) {
        turn->abandon_requested=true;
        rc=session_selection_abandon_run_with_memory(selection,turn->run_id,turns->memory,
            is_new ? NULL : &turn->reservation,&result);
    }
    if (result.reservation) turn->reservation=result.reservation;
    if (result.assistant_text) turn->answer=result.assistant_text;
    if (result.input_preserved) turn->input_preserved=true;
    if (result.abandoned && turn->reservation) turn->ready=true;
    publish_result(turns,turn,false,result.abandoned,out_result);
    return rc;
}

/**
 * @brief 用户明确放弃本机尚未完成且缺少持久证据的任务，要求保有原始预留。
 *
 * selection 验证缺失证据并保存输入，槽接管返回的预留、答案及完成标志。
 * @param[in,out] turns 须持有未就绪的本地新对话预留，槽中没有非空答案。
 * @param[in,out] selection 非空会话选择状态，验证证据缺失并保存原输入。
 * @param[in] run_id 目标运行的稳定标识，调用期间借用。
 * @param[out] out_result 必填独立输出，先清零，再输出放弃确认与借用字符串。
 * @return 返回本次执行或交接结果，错误时也检查 out_result 的完成标志、借用答复及 awaiting_resolution。
 */
cc_result_t session_memory_turns_abandon_missing(session_memory_turns_t *turns,
    session_selection_t *selection, const char *run_id, session_memory_turn_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory abandonment output");
    memset(out_result,0,sizeof(*out_result));
    pending_turn_t *turn=find_turn(turns,run_id);
    if (!turn || !turn->reservation) return cc_result_error(CC_ERR_NOT_FOUND,"Pending memory turn not found");
    if (!selection || !turn->submitted_here || turn->ready || (turn->answer && turn->answer[0]))
        return cc_result_error(CC_ERR_INVALID_STATE,"Missing-run abandonment requires a local unresolved input");
    clear_last(turns);
    wipe_free(turn->answer); turn->answer=NULL; turn->abandon_requested=true;
    session_abandon_memory_result_t result={0};
    cc_result_t rc=session_selection_abandon_missing_with_memory(selection,turn->run_id,turns->memory,
                                                                 &turn->reservation,&result);
    if (result.reservation) turn->reservation=result.reservation;
    if (result.assistant_text) turn->answer=result.assistant_text;
    if (result.input_preserved) turn->input_preserved=true;
    if (result.abandoned && turn->reservation) turn->ready=true;
    publish_result(turns,turn,false,result.abandoned,out_result);
    return rc;
}

/**
 * @brief 只重试已确定可交接的答案，按实际长度扩容后提交 RAM 队列。
 *
 * 成功释放本 owner 槽，失败保留答案和预留，后续重试复用已有结果。
 * @param[in,out] turns 持有已就绪任务的对象，找到任务后清理 last，交接成功释放目标槽。
 * @param[in] run_id 目标任务 ID，允许引用该槽借出的 ID。
 * @return CC_OK 表示已消费预留并释放槽，未就绪、扩容或交接错误保留待处理内容。
 */
cc_result_t session_memory_turns_retry_commit(session_memory_turns_t *turns, const char *run_id)
{
    pending_turn_t *turn=find_turn(turns,run_id);
    if (!turn || !turn->reservation) return cc_result_error(CC_ERR_NOT_FOUND,"Pending memory turn not found");
    clear_last(turns);
    if (!turn->ready || (!turn->answer && !turn->input_only))
        return cc_result_error(CC_ERR_INVALID_STATE,"Memory turn outcome requires inspection");
    const char *answer=turn->answer ? turn->answer : "";
    cc_result_t rc=managed_memory_service_expand_reservation(turns->memory,turn->reservation,strlen(answer));
    if (rc.code==CC_OK) rc=managed_memory_service_commit(turns->memory,&turn->reservation,answer);
    if (rc.code==CC_OK) release_turn(turn);
    return rc;
}

/**
 * @brief 核对历史证据，结果明确时立即调用 retry_commit 交接预留。
 *
 * FINAL_ONLY 可接管原答案，明确请求放弃且已保留输入的空答案也可推进。
 * 其他证据组合留给用户选择。
 * out_inspection 的独立答案副本由调用者 free，转移给槽时该字段清空。
 * @param[in,out] turns 持有未就绪预留的对象，先清理 last，证据充分时交接并释放目标槽。
 * @param[in] selection 调用期间借用会话选择状态，拥有者保证有效。
 * @param[in] run_id 目标运行的稳定标识，调用期间借用。
 * @pre 核对期间没有运行修改目标会话。
 * @param[out] out_inspection 必填独立输出，先清零，assistant_text 非空时由调用者 free，错误时也须清理。
 * @return 返回检查或交接结果。CC_OK 也可能仍需用户核对，须结合 inspection 和槽状态判断。
 */
cc_result_t session_memory_turns_reconcile(session_memory_turns_t *turns,
    session_selection_t *selection, const char *run_id, session_memory_turn_inspection_t *out_inspection)
{
    if (!out_inspection) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory inspection output");
    memset(out_inspection,0,sizeof(*out_inspection));
    pending_turn_t *turn=find_turn(turns,run_id);
    if (!turn || !turn->reservation) return cc_result_error(CC_ERR_NOT_FOUND,"Pending memory turn not found");
    clear_last(turns);
    if (turn->ready) return cc_result_error(CC_ERR_INVALID_STATE,"Completed memory turn needs commit");
    cc_result_t rc=session_selection_inspect_reserved_memory(selection,turn->run_id,turns->memory,
                                                              turn->reservation,out_inspection);
    if (rc.code!=CC_OK) return rc;
    if (out_inspection->state==SESSION_MEMORY_FINAL_ONLY) {
        if (!out_inspection->assistant_text)
            return cc_result_error(CC_ERR_STORAGE,"Stored final answer is unavailable");
        if (turn->answer && strcmp(turn->answer,out_inspection->assistant_text)!=0)
            return cc_result_error(CC_ERR_INVALID_STATE,"Stored final answer differs from retained answer");
        if (!turn->answer) {
            turn->answer=out_inspection->assistant_text;
            out_inspection->assistant_text=NULL;
        }
        turn->ready=true;
    } else if (out_inspection->state==SESSION_MEMORY_NO_DURABLE_EVIDENCE &&
               turn->abandon_requested && turn->input_preserved && turn->answer && !turn->answer[0]) {
        turn->ready=true;
    }
    if (!turn->ready) return rc;
    cc_result_free(&rc);
    return session_memory_turns_retry_commit(turns,turn->run_id);
}

/**
 * @brief owner 遍历可交接槽，成功删除后继续处理相同逻辑序号，失败保留槽并前进。
 *
 * 累积成功数并返回第一份错误，其余错误释放，保证每次扫描有限推进。
 * @param[in,out] turns 非空对象，遍历并交接其中全部已就绪槽。
 * @param[in,out] out_committed 必填累计计数器，每个成功交接槽加一，调用者负责初值。
 * @return 返回首个交接错误或 CC_OK，成功数量由 out_committed 单独报告。
 */
static cc_result_t commit_ready_turns(session_memory_turns_t *turns, size_t *out_committed)
{
    cc_result_t first_error=cc_result_ok();
    size_t index=0;
    while (index<session_memory_turns_count(turns)) {
        session_memory_pending_turn_t pending={0};
        cc_result_t rc=session_memory_turns_pending_at(turns,index,&pending);
        if (rc.code!=CC_OK) { cc_result_free(&first_error); return rc; }
        cc_result_free(&rc);
        if (!pending.ready_to_commit) { index++; continue; }
        rc=session_memory_turns_retry_commit(turns,pending.run_id);
        if (rc.code==CC_OK) {
            (*out_committed)++; cc_result_free(&rc);
            /* 当前槽已释放，下一项现在使用相同的遍历序号。 */
        } else {
            if (first_error.code==CC_OK) { cc_result_free(&first_error); first_error=rc; }
            else cc_result_free(&rc);
            index++;
        }
    }
    return first_error;
}

/**
 * @brief 空闲 owner 先重试就绪槽，再最多处理一个 FIFO 队首，释放空间后再次交接。
 *
 * 存储错误时尝试在 builder 维护屏障下重读，原错误仍向上报告，后续 pump 核对保留方案。
 * 直接借用者由外层排空，输出状态与返回错误共同描述部分进展。
 * @param[in,out] turns 非空待推进对象，调用可能释放槽及 last 结果。
 * @param[in] updated_at 应用提供的非负更新时间，worker 首次成功提取后固定该值。
 * @param[in] cancel_token 可选取消信号，调用期间借用。
 * @param[out] out_result 必填独立输出，先清零，错误时仍按其中的已交接、已处理及剩余数量判断进展。
 * @pre 外层已排空直接存储借用者，允许恢复路径进入 builder 维护屏障。
 * @return 返回提交、查询或处理结果。恢复尝试的结果被释放，仍返回原处理错误。
 * 即使返回错误也可能成功交接其他槽，已完成步骤和剩余项目以 out_result 为准。
 */
cc_result_t session_memory_turns_pump(session_memory_turns_t *turns, int64_t updated_at,
    cc_cancel_token_t *cancel_token, session_memory_pump_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory pump output");
    memset(out_result,0,sizeof(*out_result));
    if (!turns || updated_at<0) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory pump request");
    cc_result_t pending_error=commit_ready_turns(turns,&out_result->committed);
    cc_result_t rc=managed_memory_service_status_detail(turns->memory,&out_result->remaining_ready,
                                                        &out_result->remaining_reserved,NULL);
    if (rc.code!=CC_OK) { cc_result_free(&pending_error); return rc; }
    cc_result_free(&rc);
    if (out_result->remaining_ready) {
        rc=managed_memory_service_process_one(turns->memory,updated_at,cancel_token,&out_result->processing);
        if (rc.code==CC_OK) {
            out_result->processed=true;
            cc_result_free(&pending_error);
            pending_error=commit_ready_turns(turns,&out_result->committed);
        } else if (rc.code==CC_ERR_STORAGE || rc.code==CC_ERR_IO) {
            /* owner 串行协调直接记忆使用者。失败写入的现场具备重读条件时，在 builder generation 屏障内重载主文件，下次 pump 核对保留方案。 */
            cc_result_t recovered=managed_memory_service_recover_store(turns->memory);
            cc_result_free(&recovered);
        }
    } else rc=cc_result_ok();
    cc_result_t status=managed_memory_service_status_detail(turns->memory,&out_result->remaining_ready,
                                                            &out_result->remaining_reserved,NULL);
    if (status.code!=CC_OK && rc.code==CC_OK) { cc_result_free(&rc); rc=status; }
    else cc_result_free(&status);
    if (rc.code!=CC_OK) { cc_result_free(&pending_error); return rc; }
    cc_result_free(&rc); return pending_error;
}
