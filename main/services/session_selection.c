#include "session_selection.h"
#include "cc/core/cc_id.h"
#include "cc/internal/cc_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct session_selection {
    cc_runtime_controller_t *controller;
    cc_runtime_builder_t *builder;
    recovery_service_t *recovery;
    session_selection_phase_t phase;
    char session_id[96];
};

/** @brief 核对业务阶段及当前恢复盘。
 * @param[in] selection 可为 NULL 的 owner 借用对象，NULL 返回 INVALID_STATE。
 * @param[in] phase 本次要求的业务阶段。
 * @return CC_OK、INVALID_STATE 或 STORAGE，诊断归调用者释放。
 * @pre 本文件全部对象操作由同一 owner 串行，controller/builder/recovery 覆盖对象及调用寿命。
 */
static cc_result_t require_phase(session_selection_t *selection, session_selection_phase_t phase)
{
    if (!selection || selection->phase != phase)
        return cc_result_error(CC_ERR_INVALID_STATE, "Session selection is in a different phase");
    if (!recovery_service_is_current(selection->recovery))
        return cc_result_error(CC_ERR_STORAGE, "Recovery storage is not current");
    return cc_result_ok();
}

/** @brief 借用同属 builder 的 controller/recovery，要求 controller 空闲且会话未落盘。
 * 成功创建 CHOOSING 对象，失败保持 *out_selection 为 NULL。
 * 这些依赖由外层持有，所有 selection API 由同一个交互 owner 串行调用。
 * @param[in] controller 必填运行中、空闲且会话未落盘的控制器借用。
 * @param[in] builder 必填控制器所属 builder 借用。
 * @param[in] recovery 必填同属 builder 的恢复服务借用。
 * @param[out] out_selection 必填新槽，先置 NULL，成功对象由调用者 destroy。
 * @return CC_OK 或参数、状态、分配错误，结果诊断由调用者释放。
 */
cc_result_t session_selection_create(cc_runtime_controller_t *controller,
                                     cc_runtime_builder_t *builder,
                                     recovery_service_t *recovery,
                                     session_selection_t **out_selection)
{
    if (!out_selection) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid selection output");
    *out_selection=NULL;
    if (!controller || !builder || !recovery || !recovery_service_uses_builder(recovery,builder) ||
        !cc_runtime_controller_uses_builder(controller,builder))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid selection dependencies");
    cc_runtime_controller_status_t status={0};
    cc_runtime_controller_get_status(controller,&status);
    if (status.lifecycle != CC_RUNTIME_CONTROLLER_RUNNING || status.active_run_id || status.session_is_persisted)
        return cc_result_error(CC_ERR_INVALID_STATE, "Selection requires an idle, unsaved session");
    session_selection_t *selection=calloc(1,sizeof(*selection));
    if (!selection) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate session selection");
    selection->controller=controller;
    selection->builder=builder;
    selection->recovery=recovery;
    selection->phase=SESSION_SELECTION_CHOOSING;
    *out_selection=selection;
    return cc_result_ok();
}

/** @brief 释放选择对象，借用依赖继续由外层管理。
 * @param[in,out] selection 可为 NULL 的拥有对象，调用者变量自行失效。
 * @pre 外层已关调用入口并等待全部调用退出。
 */
void session_selection_destroy(session_selection_t *selection) { free(selection); }
/** @brief 读取选择阶段。
 * @param[in] selection 可为 NULL 的 owner 借用对象。
 * @return 当前阶段，NULL 返回零，无所有权交接。
 */
session_selection_phase_t session_selection_phase(const session_selection_t *selection)
{ return selection ? selection->phase : 0; }
/** @brief 取得所选会话名借用。
 * @param[in] selection 可为 NULL 的 owner 对象。
 * @return 内部 NUL 字符串，NULL 对象返回 NULL，选择前可为空串。
 * @pre 借用只使用至下一次选择、暂留或销毁，禁止并发修改。
 */
const char *session_selection_session_id(const session_selection_t *selection)
{ return selection ? selection->session_id : NULL; }

/** @brief 在 CHOOSING 列出当前盘历史，损坏历史模式拒绝读取。
 * 成功数组归调用者，以 cc_session_free_array 释放；读取后挂载变化则先清理
 * 已取得数组再返回错误，避免把旧盘快照交给后续选择。
 * @param[in] selection 可为 NULL 的串行对象，须处于 CHOOSING。
 * @param[out] out_sessions 必填空槽，成功数组由 cc_session_free_array 消费。
 * @param[out] out_count 必填条数槽，基本参数有效后与数组一同初始化为空。
 * @return CC_OK 或参数、阶段、存储及底层错误，结果诊断归调用者。
 */
cc_result_t session_selection_list_sessions(session_selection_t *selection,
                                            cc_session_t **out_sessions, size_t *out_count)
{
    if (!out_sessions || !out_count) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid session output");
    *out_sessions=NULL; *out_count=0;
    cc_result_t rc=require_phase(selection,SESSION_SELECTION_CHOOSING);
    if (rc.code==CC_OK && recovery_service_history_damaged(selection->recovery))
        return cc_result_error(CC_ERR_INVALID_STATE,"Historical sessions require repair before viewing");
    if (rc.code == CC_OK) rc=cc_runtime_controller_list_sessions(selection->controller,out_sessions,out_count);
    if (rc.code == CC_OK && !recovery_service_is_current(selection->recovery)) {
        cc_session_free_array(*out_sessions,*out_count); *out_sessions=NULL; *out_count=0;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE, "USB storage changed while listing sessions");
    }
    return rc;
}

/** @brief 用临时 generation 租约读取分段历史并再次核对挂载。
 * @param[in] selection 必填 owner 对象，依赖已核对且覆盖调用。
 * @param[in] session_id 必填目标会话 NUL 借用。
 * @param[in] before_id 可为 NULL，NULL 取最新页，否则取该 ID 之前消息。
 * @param[in] limit 页条数限制，具体有效范围交分段存储检查。
 * @param[out] out_messages 必填空槽，成功逐项 cleanup 后 free 数组。
 * @param[out] out_count 必填消息条数槽。
 * @param[out] out_has_more 必填标志槽，非零表示还有更早消息。
 * @return 底层结果，读后换盘清理整页并交空输出及 STORAGE。
 */
static cc_result_t read_history(session_selection_t *selection, const char *session_id,
                                const char *before_id, size_t limit,
                                cc_message_t **out_messages, size_t *out_count, int *out_has_more)
{
    cc_result_t rc=cc_result_ok();
    cc_runtime_generation_t *generation=NULL;
    if (rc.code == CC_OK) rc=cc_runtime_builder_acquire_generation(selection->builder,&generation);
    if (rc.code == CC_OK) {
        cc_session_store_t *store=cc_agent_runtime_session_store(cc_runtime_generation_runtime(generation));
        rc=cc_json_segmented_store_load_page(store,session_id,before_id,limit,out_messages,out_count,out_has_more);
    }
    if (generation) cc_runtime_generation_release(generation);
    if (rc.code == CC_OK && !recovery_service_is_current(selection->recovery)) {
        for (size_t i=0;i<*out_count;i++) cc_message_cleanup(&(*out_messages)[i]);
        free(*out_messages); *out_messages=NULL; *out_count=0; *out_has_more=0;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE, "USB storage changed while reading history");
    }
    return rc;
}

/** @brief 在 CHOOSING 预览历史，保持选择及控制器会话。
 * @param[in] selection 可为 NULL 的串行对象，要求 CHOOSING 及当前盘。
 * @param[in] session_id 必填非空目标会话名借用。
 * @param[in] before_id 可为 NULL，NULL 从最新页开始。
 * @param[in] limit 条数限制，沿分段存储规则校验。
 * @param[out] out_messages 必填空槽，成功逐消息 cleanup 后 free 数组。
 * @param[out] out_count 必填条数槽，基本参数有效后置零。
 * @param[out] out_has_more 必填更早消息标志，基本参数有效后置零。
 * @return CC_OK 或参数、阶段、损坏历史及存储错误，换盘清理输出。
 */
cc_result_t session_selection_preview(session_selection_t *selection, const char *session_id,
                                      const char *before_id, size_t limit,
                                      cc_message_t **out_messages, size_t *out_count, int *out_has_more)
{
    if (!session_id || !session_id[0] || !out_messages || !out_count || !out_has_more)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid history preview");
    *out_messages=NULL; *out_count=0; *out_has_more=0;
    cc_result_t rc=require_phase(selection,SESSION_SELECTION_CHOOSING);
    if (rc.code==CC_OK && recovery_service_history_damaged(selection->recovery))
        return cc_result_error(CC_ERR_INVALID_STATE,"Historical sessions require repair before viewing");
    return rc.code==CC_OK ? read_history(selection,session_id,before_id,limit,out_messages,out_count,out_has_more) : rc;
}

/** @brief 在 CHATTING 读取所选会话最新页以刷新显示。
 * @param[in] selection 可为 NULL 的串行对象，要求 CHATTING 及当前盘。
 * @param[in] limit 页条数限制，交分段存储校验。
 * @param[out] out_messages 必填空槽，成功逐消息 cleanup 后 free 数组。
 * @param[out] out_count 必填条数槽。
 * @param[out] out_has_more 必填更早消息标志。
 * @return CC_OK 或参数、阶段、代际和存储错误，基本参数有效后先清空输出。
 */
cc_result_t session_selection_current_history(session_selection_t *selection, size_t limit,
                                              cc_message_t **out_messages, size_t *out_count,
                                              int *out_has_more)
{
    if (!out_messages || !out_count || !out_has_more)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid current history output");
    *out_messages=NULL; *out_count=0; *out_has_more=0;
    cc_result_t rc=require_phase(selection,SESSION_SELECTION_CHATTING);
    return rc.code==CC_OK ? read_history(selection,selection->session_id,NULL,limit,
                                         out_messages,out_count,out_has_more) : rc;
}

/** @brief 统计指定会话的中断任务并可定位目标 run。
 * @param[in] selection 必填串行对象，recovery 借用覆盖调用。
 * @param[in] session_id 必填会话名借用。
 * @param[in] run_id 可为 NULL，非空时核对是否属于目标会话。
 * @param[out] out_matches 可为 NULL 的匹配条数槽。
 * @param[out] out_found 可为 NULL 的目标存在槽，失败时仍写 false。
 * @return 列表原结果，临时列表自行清理，输出仅成功时作为业务依据。
 */
static cc_result_t matching_runs(session_selection_t *selection, const char *session_id,
                                 const char *run_id, size_t *out_matches, bool *out_found)
{
    recovery_run_info_t *runs=NULL;
    size_t count=0, matches=0;
    bool found=false;
    cc_result_t rc=recovery_service_list_runs(selection->recovery,&runs,&count);
    if (rc.code == CC_OK) {
        for (size_t i=0;i<count;i++) {
            if (strcmp(runs[i].session_id,session_id)!=0) continue;
            matches++;
            if (run_id && strcmp(runs[i].run_id,run_id)==0) found=true;
        }
    }
    recovery_run_info_array_free(runs,count);
    if (out_matches) *out_matches=matches;
    if (out_found) *out_found=found;
    return rc;
}

/** @brief 在 CHOOSING 选择未落盘新会话。
 * @param[in,out] selection 可为 NULL 的 owner 对象，要求当前盘及 CHOOSING。
 * @return 控制器结果，成功复制新会话名并进入 CHATTING，切换后换盘进入 UNAVAILABLE。
 * @note 空新会话由控制器延迟持久化，首条消息前只持有身份。
 */
cc_result_t session_selection_choose_new(session_selection_t *selection)
{
    cc_result_t rc=require_phase(selection,SESSION_SELECTION_CHOOSING);
    if (rc.code == CC_OK) rc=cc_runtime_controller_start_new_session(selection->controller);
    if (rc.code == CC_OK && !recovery_service_is_current(selection->recovery)) {
        selection->phase=SESSION_SELECTION_UNAVAILABLE;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE, "USB storage changed during session selection");
    }
    if (rc.code == CC_OK) {
        cc_runtime_controller_status_t status={0};
        cc_runtime_controller_get_status(selection->controller,&status);
        snprintf(selection->session_id,sizeof(selection->session_id),"%s",status.session_id);
        selection->phase=SESSION_SELECTION_CHATTING;
    }
    return rc;
}

/** @brief 在空闲 INTERRUPTED 暂留任务并返回选择阶段。
 * @param[in,out] selection 可为 NULL 的 owner 对象。
 * @return CC_OK 或阶段、运行状态、存储错误，成功清选中名并转 CHOOSING。
 * @post 原检查点与未知工具账本继续保留供后续选择。
 */
cc_result_t session_selection_defer_interrupted(session_selection_t *selection)
{
    cc_result_t rc=require_phase(selection,SESSION_SELECTION_INTERRUPTED);
    if (rc.code!=CC_OK) return rc;
    cc_runtime_controller_status_t status={0};
    cc_runtime_controller_get_status(selection->controller,&status);
    if (status.lifecycle!=CC_RUNTIME_CONTROLLER_RUNNING || status.active_run_id)
        return cc_result_error(CC_ERR_INVALID_STATE,"An active run cannot be deferred");
    if (!recovery_service_is_current(selection->recovery)) {
        selection->phase=SESSION_SELECTION_UNAVAILABLE;
        return cc_result_error(CC_ERR_STORAGE,"Recovery storage changed while deferring a run");
    }
    selection->phase=SESSION_SELECTION_CHOOSING;
    selection->session_id[0]='\0';
    return cc_result_ok();
}

/** @brief 在 CHOOSING 选择既有会话并核对中断任务。
 * @param[in,out] selection 可为 NULL 的 owner 对象，须处于 CHOOSING。
 * @param[in] session_id 必填非空且小于 96 B 的会话名调用期借用。
 * @return CC_OK 或参数、阶段、损坏历史及存储错误。
 * @post 成功复制会话名，有中断任务转 INTERRUPTED，否则 CHATTING，切换后换盘转 UNAVAILABLE。
 */
cc_result_t session_selection_choose_existing(session_selection_t *selection, const char *session_id)
{
    if (!session_id || !session_id[0] || strlen(session_id)>=sizeof(selection->session_id))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid selected session");
    cc_result_t rc=require_phase(selection,SESSION_SELECTION_CHOOSING);
    if (rc.code==CC_OK && recovery_service_history_damaged(selection->recovery))
        return cc_result_error(CC_ERR_INVALID_STATE,"Historical sessions require repair before resuming");
    size_t matches=0;
    if (rc.code == CC_OK) rc=matching_runs(selection,session_id,NULL,&matches,NULL);
    if (rc.code == CC_OK) rc=cc_runtime_controller_resume_session(selection->controller,session_id);
    if (rc.code == CC_OK && !recovery_service_is_current(selection->recovery)) {
        selection->phase=SESSION_SELECTION_UNAVAILABLE;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE, "USB storage changed during session selection");
    }
    if (rc.code == CC_OK) {
        snprintf(selection->session_id,sizeof(selection->session_id),"%s",session_id);
        selection->phase=matches ? SESSION_SELECTION_INTERRUPTED : SESSION_SELECTION_CHATTING;
    }
    return rc;
}

/** @brief 核对 run 属于当前所选中断会话。
 * @param[in] selection 可为 NULL 的串行对象，要求 INTERRUPTED 及当前盘。
 * @param[in] run_id 必填非空 ID 调用期借用。
 * @return CC_OK、参数/阶段/存储错误或 NOT_FOUND，临时列表自行归还。
 */
static cc_result_t require_selected_run(session_selection_t *selection, const char *run_id)
{
    if (!run_id || !run_id[0]) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid run ID");
    cc_result_t rc=require_phase(selection,SESSION_SELECTION_INTERRUPTED);
    bool found=false;
    if (rc.code == CC_OK) rc=matching_runs(selection,selection->session_id,run_id,NULL,&found);
    if (rc.code == CC_OK && !found)
        rc=cc_result_error(CC_ERR_NOT_FOUND, "Interrupted run does not belong to the selected session");
    return rc;
}

/** @brief 重读中断任务并收敛业务阶段。
 * @param[in,out] selection 必填 owner 对象，已选会话名有效。
 * @return 列表原结果，成功按条数转 INTERRUPTED/CHATTING，失败保留原阶段。
 */
static cc_result_t refresh_phase(session_selection_t *selection)
{
    size_t matches=0;
    cc_result_t rc=matching_runs(selection,selection->session_id,NULL,&matches,NULL);
    if (rc.code == CC_OK) selection->phase=matches ? SESSION_SELECTION_INTERRUPTED : SESSION_SELECTION_CHATTING;
    return rc;
}

static cc_result_t load_selected_checkpoint(session_selection_t *selection, const char *run_id,
                                             int allow_backup, cc_run_checkpoint_t *checkpoint);
static cc_result_t persist_abandoned_input(session_selection_t *selection,
                                            const cc_run_checkpoint_t *checkpoint);

/** @brief 在 INTERRUPTED 重试列表读取。
 * @param[in,out] selection 可为 NULL 的 owner 对象，须为当前盘。
 * @return 阶段检查或刷新结果，无剩余任务时成功推进 CHATTING。
 */
cc_result_t session_selection_refresh_runs(session_selection_t *selection)
{
    cc_result_t rc=require_phase(selection,SESSION_SELECTION_INTERRUPTED);
    return rc.code == CC_OK ? refresh_phase(selection) : rc;
}

/** @brief 保留原输入、放弃检查点并刷新阶段。
 * @param[in,out] selection 可为 NULL 的 owner 对象，须选中该中断会话。
 * @param[in] run_id 必填非空所属 run ID 调用期借用。
 * @return 首个加载、保存、放弃或刷新错误，换盘后转 UNAVAILABLE。
 * @note 需要自动记忆时使用组合入口，原 UNKNOWN 工具继续由当前盘账本核对。
 */
cc_result_t session_selection_abandon_run(session_selection_t *selection, const char *run_id)
{
    cc_run_checkpoint_t checkpoint={0};
    cc_result_t rc=load_selected_checkpoint(selection,run_id,1,&checkpoint);
    if (rc.code==CC_OK) rc=persist_abandoned_input(selection,&checkpoint);
    cc_run_checkpoint_cleanup(&checkpoint);
    if (rc.code == CC_OK) rc=recovery_service_abandon_run(selection->recovery,run_id);
    if (rc.code == CC_OK) rc=refresh_phase(selection);
    if (rc.code != CC_OK && selection && !recovery_service_is_current(selection->recovery))
        selection->phase=SESSION_SELECTION_UNAVAILABLE;
    return rc;
}

/** @brief 同步继续当前会话所属 run，stream 数据在相应回调期间借用。
 * runtime 完成后先置 completed，再检查挂载和刷新阶段，因此返回错误时仍可能
 * completed=true 且 response 归调用者 free。false 也需结合持久记录核对结果。
 * @param[in,out] selection 可为 NULL 的 owner 对象，要求所属 INTERRUPTED 会话。
 * @param[in] run_id 必填非空 run ID 借用。
 * @param[in] on_stream 可为 NULL 的同步流回调。
 * @param[in,out] user_data 可为 NULL 的回调上下文借用，覆盖全部同步回调。
 * @param[out] out_response 必填新槽，先置 NULL，错误时也可能交付调用者 free 的答复。
 * @param[out] out_completed 必填槽，先置 false，运行完成后置 true，后续刷新错误仍保留。
 * @return 控制器或刷新结果，换盘转 UNAVAILABLE。
 */
cc_result_t session_selection_resume_run_ex(session_selection_t *selection, const char *run_id,
                                            cc_runtime_controller_stream_fn on_stream, void *user_data,
                                            char **out_response, bool *out_completed)
{
    if (!out_response || !out_completed)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid resume output");
    *out_response=NULL; *out_completed=false;
    cc_result_t rc=require_selected_run(selection,run_id);
    if (rc.code == CC_OK)
        rc=cc_runtime_controller_resume_run(selection->controller,run_id,on_stream,user_data,out_response);
    if (rc.code == CC_OK) *out_completed=true;
    if (rc.code == CC_OK && !recovery_service_is_current(selection->recovery)) {
        selection->phase=SESSION_SELECTION_UNAVAILABLE;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE, "USB storage changed during run recovery");
    }
    if (rc.code == CC_OK) {
        rc=refresh_phase(selection);
        if (rc.code != CC_OK && !recovery_service_is_current(selection->recovery))
            selection->phase=SESSION_SELECTION_UNAVAILABLE;
    } else if (selection && !recovery_service_is_current(selection->recovery)) {
        selection->phase=SESSION_SELECTION_UNAVAILABLE;
    }
    return rc;
}

/** @brief 委托扩展继续入口并省略 completed 标志。
 * @param[in,out] selection 可为 NULL 的串行对象。
 * @param[in] run_id 必填所属中断 run ID 借用。
 * @param[in] on_stream 可为 NULL 的同步回调。
 * @param[in,out] user_data 可为 NULL 的调用期回调借用。
 * @param[out] out_response 必填空槽，错误时仍可能拥有答复，调用者 free。
 * @return 原样透传扩展入口结果。
 */
cc_result_t session_selection_resume_run(session_selection_t *selection, const char *run_id,
                                         cc_runtime_controller_stream_fn on_stream, void *user_data,
                                         char **out_response)
{
    bool completed=false;
    return session_selection_resume_run_ex(selection,run_id,on_stream,user_data,out_response,&completed);
}

/** @brief 租用 generation 加载检查点并核对归属与来源。
 * @param[in] selection 可为 NULL 的串行对象，要求已选该中断 run。
 * @param[in] run_id 必填非空 run ID 借用。
 * @param[in] allow_backup 非零允许人工放弃读取备份/临时来源，普通继续为零。
 * @param[out] checkpoint 必填初始空结构，部分输出也由调用者 checkpoint_cleanup。
 * @return 加载、身份、来源或挂载错误，临时代际租约自行归还。
 */
static cc_result_t load_selected_checkpoint(session_selection_t *selection, const char *run_id,
                                             int allow_backup, cc_run_checkpoint_t *checkpoint)
{
    cc_result_t rc=require_selected_run(selection,run_id);
    cc_runtime_generation_t *generation=NULL;
    if (rc.code==CC_OK) rc=cc_runtime_builder_acquire_generation(selection->builder,&generation);
    if (rc.code==CC_OK)
        rc=cc_agent_runtime_load_interrupted_run(cc_runtime_generation_runtime(generation),run_id,checkpoint);
    if (generation) cc_runtime_generation_release(generation);
    if (rc.code==CC_OK && ((!allow_backup && (checkpoint->recovered_from_backup || checkpoint->recovered_from_temporary)) ||
        strcmp(checkpoint->session_id,selection->session_id)!=0))
        rc=cc_result_error(CC_ERR_INVALID_STATE, "Interrupted run does not match the selection or requires manual recovery");
    if (rc.code==CC_OK && !recovery_service_is_current(selection->recovery))
        rc=cc_result_error(CC_ERR_STORAGE, "USB storage changed while loading the interrupted run");
    return rc;
}

/** @brief 使用已有预留继续，运行完成后提交记忆队列。
 * @param[in,out] selection 必填 owner 对象，当前会话已选中。
 * @param[in] run_id 必填所属中断 run ID 借用。
 * @param[in,out] memory 必填同属装配的记忆服务，寿命覆盖剩余预留。
 * @param[in] on_stream 可为 NULL 的同步流回调。
 * @param[in,out] user_data 可为 NULL 的回调期借用。
 * @param[in,out] out_result 必填已有预留结果，答复归调用者 free，提交失败保留预留供核对/扩容/重试。
 * @return 运行/刷新错误，提交失败优先返回 commit 错误并释放先前诊断。
 */
static cc_result_t resume_reserved_memory(session_selection_t *selection, const char *run_id,
    managed_memory_service_t *memory, cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_resume_memory_result_t *out_result)
{
    cc_result_t rc=session_selection_resume_run_ex(selection,run_id,on_stream,user_data,
                                                   &out_result->response,&out_result->completed);
    if (out_result->completed) {
        cc_result_t commit=managed_memory_service_commit(memory,&out_result->reservation,out_result->response);
        if (commit.code!=CC_OK) { cc_result_free(&rc); return commit; }
        cc_result_free(&commit);
    }
    return rc;
}

/** @brief 从检查点原输入预留记忆，再继续运行。
 * @param[in,out] selection 可为 NULL 的 owner 对象，要求所属中断会话。
 * @param[in] run_id 必填非空 run ID 借用。
 * @param[in,out] memory 必填同属 builder/recovery 的服务，覆盖预留寿命。
 * @param[in] max_answer_bytes 正的预期答案字节预算。
 * @param[in] on_stream 可为 NULL 的同步回调。
 * @param[in,out] user_data 可为 NULL 的回调上下文借用。
 * @param[out] out_result 必填空槽，先清零，错误时仍可能拥有 response/reservation。
 * @return 参数、检查点、预留、运行或提交错误，调用者 free 答复并保管未决预留。
 */
cc_result_t session_selection_resume_run_with_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    size_t max_answer_bytes, cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_resume_memory_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid resume output");
    memset(out_result,0,sizeof(*out_result));
    if (!selection || !memory || !max_answer_bytes ||
        !managed_memory_service_uses(memory,selection->builder,selection->recovery))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Memory service does not belong to the selected runtime");

    cc_run_checkpoint_t checkpoint={0};
    cc_result_t rc=load_selected_checkpoint(selection,run_id,0,&checkpoint);
    if (rc.code==CC_OK)
        rc=managed_memory_service_reserve(memory,checkpoint.run_id,checkpoint.session_id,
                                         checkpoint.user_input,max_answer_bytes,&out_result->reservation);
    cc_run_checkpoint_cleanup(&checkpoint);
    if (rc.code!=CC_OK) return rc;
    return resume_reserved_memory(selection,run_id,memory,on_stream,user_data,out_result);
}

/** @brief 核对既有预留归属后转移并继续。
 * @param[in,out] selection 可为 NULL 的 owner 对象。
 * @param[in] run_id 必填所属 run ID 借用。
 * @param[in,out] memory 必填同属装配的服务，覆盖未决预留寿命。
 * @param[in,out] inout_reservation 必填且持活预留，核对成功后移入结果并清空，禁止指向结果成员。
 * @param[in] on_stream 可为 NULL 的同步回调。
 * @param[in,out] user_data 可为 NULL 的同步回调期借用。
 * @param[out] out_result 必填空槽，别名检查后清零，答复和未决预留归调用者。
 * @return 核对前失败保留输入句柄，转移后失败检查结果中的拥有对象。
 */
cc_result_t session_selection_resume_run_with_reserved_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    cc_memory_pending_reservation_t **inout_reservation,
    cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_resume_memory_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid resume output");
    if (inout_reservation==&out_result->reservation)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Resume input and output must not alias");
    memset(out_result,0,sizeof(*out_result));
    if (!selection || !memory || !inout_reservation || !*inout_reservation ||
        !managed_memory_service_uses(memory,selection->builder,selection->recovery))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid reserved memory resume");
    cc_run_checkpoint_t checkpoint={0};
    cc_result_t rc=load_selected_checkpoint(selection,run_id,0,&checkpoint);
    if (rc.code==CC_OK)
        rc=managed_memory_service_verify_reservation(memory,*inout_reservation,checkpoint.run_id,
                                                      checkpoint.session_id,checkpoint.user_input);
    cc_run_checkpoint_cleanup(&checkpoint);
    if (rc.code!=CC_OK) return rc;
    out_result->reservation=*inout_reservation; *inout_reservation=NULL;
    return resume_reserved_memory(selection,run_id,memory,on_stream,user_data,out_result);
}

/** @brief 按 run-final 标识读取已有 assistant 答复。
 * @param[in] selection 必填串行对象，已选会话和当前盘有效。
 * @param[in] run_id 必填 ID 借用，组合 final ID 须容纳于 64 B。
 * @param[out] out_answer 必填空槽，成功交拥有答复或空串，调用者 free，错误清空。
 * @param[out] out_found 可为 NULL，先置零，找到有效 assistant 后置一。
 * @return ID 容量、角色、代际、历史或分配错误，临时消息和租约自行归还。
 */
static cc_result_t load_final_for_abandoned_run(session_selection_t *selection,
                                                 const char *run_id, char **out_answer, int *out_found)
{
    *out_answer=NULL;
    if (out_found) *out_found=0;
    char final_id[64];
    int written=snprintf(final_id,sizeof(final_id),"%s-final",run_id);
    if (written<0 || (size_t)written>=sizeof(final_id))
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Run final message id is too long");
    cc_runtime_generation_t *generation=NULL;
    cc_result_t rc=cc_runtime_builder_acquire_generation(selection->builder,&generation);
    cc_message_t final={0}; int found=0;
    if (rc.code==CC_OK) {
        cc_session_store_t *store=cc_agent_runtime_session_store(cc_runtime_generation_runtime(generation));
        rc=cc_json_segmented_store_find_message(store,selection->session_id,CC_RUN_HISTORY_MESSAGE_ID,
                                               final_id,&final,&found);
    }
    if (rc.code==CC_OK && found) {
        if (final.role!=CC_ROLE_ASSISTANT)
            rc=cc_result_error(CC_ERR_STORAGE,"Run final message has an invalid role");
        else rc=cc_message_get_text_summary(&final,out_answer);
    } else if (rc.code==CC_OK) {
        *out_answer=cc_copy_string("");
        if (!*out_answer) rc=cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate empty assistant response");
    }
    cc_message_cleanup(&final);
    if (generation) cc_runtime_generation_release(generation);
    if (rc.code==CC_OK && !recovery_service_is_current(selection->recovery)) {
        cc_result_free(&rc); rc=cc_result_error(CC_ERR_STORAGE,"USB storage changed while reading the final answer");
    }
    if (rc.code==CC_OK && out_found) *out_found=found;
    if (rc.code!=CC_OK) { free(*out_answer); *out_answer=NULL; }
    return rc;
}

/** @brief 无活动运行时只读核对预留、检查点与最终答复。
 * 预留仍归调用者，输出 assistant_text 由调用者 free。两份持久证据都缺失时
 * 报 NO_DURABLE_EVIDENCE，外部工具是否执行仍由账本及人工核对判定。
 * @param[in] selection 可为 NULL 的 owner 对象，controller 会话须与选择一致且无活动 run。
 * @param[in] run_id 必填非空原 run ID 借用。
 * @param[in] memory 必填同属装配的服务借用。
 * @param[in] reservation 必填活预留借用，所有权继续归调用者。
 * @param[out] out_inspection 必填新槽，先清零，成功 assistant_text 由调用者 free。
 * @return 参数、身份、存储或读取错误，函数保持账本及预留状态。
 */
cc_result_t session_selection_inspect_reserved_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    cc_memory_pending_reservation_t *reservation, session_memory_turn_inspection_t *out_inspection)
{
    if (!out_inspection) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory inspection output");
    memset(out_inspection,0,sizeof(*out_inspection));
    if (!selection || !run_id || !run_id[0] || !memory || !reservation ||
        !managed_memory_service_uses(memory,selection->builder,selection->recovery))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid reserved memory inspection");
    if (!recovery_service_is_current(selection->recovery))
        return cc_result_error(CC_ERR_STORAGE,"Recovery storage is not current");
    cc_runtime_controller_status_t status={0};
    cc_runtime_controller_get_status(selection->controller,&status);
    if (status.active_run_id || strcmp(status.session_id,selection->session_id)!=0)
        return cc_result_error(CC_ERR_INVALID_STATE,"Selected session is active or changed");

    cc_memory_pending_item_t item={0};
    cc_result_t rc=managed_memory_service_inspect_reservation(memory,reservation,&item);
    if (rc.code==CC_OK && (strcmp(item.turn_id,run_id)!=0 || strcmp(item.session_id,selection->session_id)!=0))
        rc=cc_result_error(CC_ERR_INVALID_STATE,"Reservation belongs to another run or session");
    cc_runtime_generation_t *generation=NULL;
    cc_run_checkpoint_t checkpoint={0}; int has_checkpoint=0;
    if (rc.code==CC_OK) rc=cc_runtime_builder_acquire_generation(selection->builder,&generation);
    if (rc.code==CC_OK) {
        rc=cc_agent_runtime_load_interrupted_run(cc_runtime_generation_runtime(generation),run_id,&checkpoint);
        if (rc.code==CC_ERR_NOT_FOUND) { cc_result_free(&rc); rc=cc_result_ok(); }
        else if (rc.code==CC_OK) has_checkpoint=1;
    }
    if (generation) cc_runtime_generation_release(generation);
    if (rc.code==CC_OK && has_checkpoint &&
        (strcmp(checkpoint.session_id,item.session_id)!=0 || strcmp(checkpoint.user_input,item.user_input)!=0))
        rc=cc_result_error(CC_ERR_STORAGE,"Checkpoint does not match the reserved memory turn");
    int has_final=0; char *answer=NULL;
    if (rc.code==CC_OK) rc=load_final_for_abandoned_run(selection,run_id,&answer,&has_final);
    if (rc.code==CC_OK) {
        out_inspection->checkpoint_from_backup=has_checkpoint && checkpoint.recovered_from_backup;
        out_inspection->state=has_checkpoint ?
            (has_final ? SESSION_MEMORY_CHECKPOINT_AND_FINAL : SESSION_MEMORY_CHECKPOINT_ONLY) :
            (has_final ? SESSION_MEMORY_FINAL_ONLY : SESSION_MEMORY_NO_DURABLE_EVIDENCE);
        if (has_final) { out_inspection->assistant_text=answer; answer=NULL; }
    }
    free(answer); cc_run_checkpoint_cleanup(&checkpoint); cc_memory_pending_item_cleanup(&item);
    return rc;
}

/** @brief 按检查点原消息 ID 幂等保留用户输入。
 * 使用临时 generation 租约，并在追加前后核对挂载。追加结果不确定时保留
 * 错误供上层重读核对，后续重试继续使用原 ID。
 * @param[in] selection 必填 owner 对象，借用依赖覆盖调用。
 * @param[in] checkpoint 必填原会话/消息 ID/输入借用，调用期间稳定。
 * @return CC_OK 或角色/正文冲突、代际、存储、追加错误，追加失败可能已有持久前缀。
 */
static cc_result_t persist_abandoned_input(session_selection_t *selection,
                                            const cc_run_checkpoint_t *checkpoint)
{
    cc_runtime_generation_t *generation=NULL;
    cc_result_t rc=cc_runtime_builder_acquire_generation(selection->builder,&generation);
    cc_session_store_t *store=rc.code==CC_OK ?
        cc_agent_runtime_session_store(cc_runtime_generation_runtime(generation)) : NULL;
    cc_message_t existing={0}; int found=0;
    if (rc.code==CC_OK && (!store || !store->vtable || !store->vtable->append_records))
        rc=cc_result_error(CC_ERR_STORAGE,"Session store cannot preserve the interrupted input");
    if (rc.code==CC_OK)
        rc=cc_json_segmented_store_find_message(store,checkpoint->session_id,CC_RUN_HISTORY_MESSAGE_ID,
                                               checkpoint->user_message_id,&existing,&found);
    if (rc.code==CC_OK && found) {
        char *text=NULL;
        if (existing.role!=CC_ROLE_USER)
            rc=cc_result_error(CC_ERR_STORAGE,"Interrupted input has an invalid role");
        else {
            rc=cc_message_get_text_summary(&existing,&text);
            if (rc.code==CC_OK && (!text || strcmp(text,checkpoint->user_input)!=0))
                rc=cc_result_error(CC_ERR_STORAGE,"Interrupted input does not match the checkpoint");
        }
        free(text);
    }
    cc_message_cleanup(&existing);
    if (rc.code==CC_OK && !recovery_service_is_current(selection->recovery))
        rc=cc_result_error(CC_ERR_STORAGE,"USB storage changed before preserving the input");
    if (rc.code==CC_OK && !found) {
        cc_message_t *message=NULL;
        rc=cc_message_create_text(checkpoint->user_message_id,checkpoint->session_id,CC_ROLE_USER,
                                  checkpoint->user_input,NULL,&message);
        if (rc.code==CC_OK) {
            cc_session_record_t record={.type=CC_SESSION_RECORD_MESSAGE,
                .session_id=checkpoint->session_id,.data.message=message};
            rc=store->vtable->append_records(store->self,&record,1);
        }
        cc_message_destroy(message);
    }
    if (generation) cc_runtime_generation_release(generation);
    if (rc.code==CC_OK && !recovery_service_is_current(selection->recovery)) {
        cc_result_free(&rc); rc=cc_result_error(CC_ERR_STORAGE,"USB storage changed while preserving the input");
    }
    return rc;
}

/** @brief 保留输入与已有答复，放弃检查点后提交记忆。
 * input_preserved/abandoned 分别记录已完成阶段，整体错误时也要核对这两个
 * 标志以及拥有的 assistant_text/reservation，避免重复放弃或丢失待评估内容。
 * @param[in,out] selection 可为 NULL 的 owner 对象，要求所属中断会话。
 * @param[in] run_id 必填所属 run ID 借用。
 * @param[in,out] memory 必填同属装配的服务，覆盖未决预留寿命。
 * @param[in,out] inout_reservation 可为 NULL 或空句柄，非空时核对并扩容后移入结果，禁止结果成员别名。
 * @param[out] out_result 必填新槽，别名检查后清零，错误时也要检查阶段及拥有对象。
 * @return 首个保存/放弃/提交/刷新错误，答复由调用者 free，未决预留继续保管。
 */
cc_result_t session_selection_abandon_run_with_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    cc_memory_pending_reservation_t **inout_reservation,
    session_abandon_memory_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid abandonment output");
    if (inout_reservation==&out_result->reservation)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Abandonment input and output must not alias");
    memset(out_result,0,sizeof(*out_result));
    if (!selection || !memory || !managed_memory_service_uses(memory,selection->builder,selection->recovery))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory abandonment");
    cc_run_checkpoint_t checkpoint={0};
    cc_result_t rc=load_selected_checkpoint(selection,run_id,1,&checkpoint);
    if (rc.code==CC_OK) rc=load_final_for_abandoned_run(selection,run_id,&out_result->assistant_text,NULL);
    if (rc.code==CC_OK && inout_reservation && *inout_reservation) {
        rc=managed_memory_service_verify_reservation(memory,*inout_reservation,checkpoint.run_id,
                                                      checkpoint.session_id,checkpoint.user_input);
        if (rc.code==CC_OK)
            rc=managed_memory_service_expand_reservation(memory,*inout_reservation,strlen(out_result->assistant_text));
        if (rc.code==CC_OK) { out_result->reservation=*inout_reservation; *inout_reservation=NULL; }
    } else if (rc.code==CC_OK) {
        rc=managed_memory_service_reserve(memory,checkpoint.run_id,checkpoint.session_id,checkpoint.user_input,
                                          strlen(out_result->assistant_text),&out_result->reservation);
    }
    if (rc.code==CC_OK) {
        rc=persist_abandoned_input(selection,&checkpoint);
        if (rc.code==CC_OK) out_result->input_preserved=true;
    }
    cc_run_checkpoint_cleanup(&checkpoint);
    if (rc.code!=CC_OK) {
        if (!out_result->reservation) { free(out_result->assistant_text); out_result->assistant_text=NULL; }
        if (!recovery_service_is_current(selection->recovery)) selection->phase=SESSION_SELECTION_UNAVAILABLE;
        return rc;
    }
    rc=recovery_service_abandon_run(selection->recovery,run_id);
    if (rc.code==CC_OK) {
        out_result->abandoned=true;
        cc_result_t commit=managed_memory_service_commit(memory,&out_result->reservation,out_result->assistant_text);
        if (commit.code!=CC_OK) { cc_result_free(&rc); rc=commit; }
        else { cc_result_free(&commit); free(out_result->assistant_text); out_result->assistant_text=NULL; }
        cc_result_t refresh=refresh_phase(selection);
        if (refresh.code!=CC_OK) {
            if (rc.code==CC_OK) { cc_result_free(&rc); rc=refresh; }
            else cc_result_free(&refresh);
            if (!recovery_service_is_current(selection->recovery)) selection->phase=SESSION_SELECTION_UNAVAILABLE;
        } else cc_result_free(&refresh);
    } else if (!recovery_service_is_current(selection->recovery)) {
        selection->phase=SESSION_SELECTION_UNAVAILABLE;
    }
    return rc;
}

/** @brief 用户明确放弃本次启动且持久证据缺失的新任务。
 * 调用者保证预留来自确定性输入 ID 的新任务路径。保留输入前后重新核对
 * 检查点和最终答复，再提交空答案用于记忆评估。失败时保管输出中的预留。
 * 工具结果由独立核对流程处理。
 * @param[in,out] selection 可为 NULL 的 owner 对象，当前会话必须匹配预留。
 * @param[in] run_id 必填非空本次新任务 ID 借用。
 * @param[in,out] memory 必填同属装配的服务，覆盖预留寿命。
 * @param[in,out] inout_reservation 必填活的新任务预留，转移后清空，禁止结果成员别名。
 * @param[out] out_result 必填新槽，别名检查后清零，错误时保管剩余预留与答复。
 * @return 参数、身份、持久证据变化、保存、提交或刷新错误，阶段标志保留部分完成。
 */
cc_result_t session_selection_abandon_missing_with_memory(
    session_selection_t *selection, const char *run_id, managed_memory_service_t *memory,
    cc_memory_pending_reservation_t **inout_reservation, session_abandon_memory_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid abandonment output");
    if (inout_reservation==&out_result->reservation)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Abandonment input and output must not alias");
    memset(out_result,0,sizeof(*out_result));
    if (!selection || !run_id || !run_id[0] || !memory || !inout_reservation || !*inout_reservation ||
        !managed_memory_service_uses(memory,selection->builder,selection->recovery))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid missing-run abandonment");
    if (!recovery_service_is_current(selection->recovery))
        return cc_result_error(CC_ERR_STORAGE,"Recovery storage is not current");
    cc_memory_pending_item_t item={0}; char *message_id=NULL;
    cc_result_t rc=managed_memory_service_inspect_reservation(memory,*inout_reservation,&item);
    if (rc.code==CC_OK && (strcmp(item.turn_id,run_id)!=0 ||
                            strcmp(item.session_id,selection->session_id)!=0))
        rc=cc_result_error(CC_ERR_INVALID_STATE,"Reservation belongs to another run or session");
    if (rc.code==CC_OK) rc=cc_run_checkpoint_derive_user_message_id(run_id,&message_id);
    if (rc.code==CC_OK) {
        out_result->assistant_text=cc_copy_string("");
        if (!out_result->assistant_text) rc=cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot prepare empty answer");
    }
    if (rc.code==CC_OK) rc=cc_runtime_controller_ensure_session(selection->controller,selection->session_id);
    if (rc.code==CC_OK && !recovery_service_is_current(selection->recovery))
        rc=cc_result_error(CC_ERR_STORAGE,"USB changed while preparing the session");
    session_memory_turn_inspection_t inspection={0};
    if (rc.code==CC_OK)
        rc=session_selection_inspect_reserved_memory(selection,run_id,memory,*inout_reservation,&inspection);
    if (rc.code==CC_OK && inspection.state!=SESSION_MEMORY_NO_DURABLE_EVIDENCE)
        rc=cc_result_error(CC_ERR_INVALID_STATE,"Run has a checkpoint or final answer; inspect it first");
    free(inspection.assistant_text);
    if (rc.code==CC_OK) {
        out_result->reservation=*inout_reservation; *inout_reservation=NULL;
        cc_run_checkpoint_t input={.session_id=item.session_id,.user_message_id=message_id,.user_input=item.user_input};
        rc=persist_abandoned_input(selection,&input);
        if (rc.code==CC_OK) out_result->input_preserved=true;
    }
    if (rc.code==CC_OK) {
        memset(&inspection,0,sizeof(inspection));
        rc=session_selection_inspect_reserved_memory(selection,run_id,memory,out_result->reservation,&inspection);
        if (rc.code==CC_OK && inspection.state!=SESSION_MEMORY_NO_DURABLE_EVIDENCE) {
            free(out_result->assistant_text); out_result->assistant_text=NULL;
            rc=cc_result_error(CC_ERR_INVALID_STATE,"Run evidence changed while preserving the input");
        }
        free(inspection.assistant_text);
    }
    if (rc.code==CC_OK) {
        out_result->abandoned=true;
        rc=managed_memory_service_commit(memory,&out_result->reservation,out_result->assistant_text);
        if (rc.code==CC_OK) { free(out_result->assistant_text); out_result->assistant_text=NULL; }
    }
    if (out_result->abandoned && selection->phase==SESSION_SELECTION_INTERRUPTED) {
        cc_result_t refresh=refresh_phase(selection);
        if (refresh.code!=CC_OK) {
            if (rc.code==CC_OK) { cc_result_free(&rc); rc=refresh; }
            else cc_result_free(&refresh);
        } else cc_result_free(&refresh);
    }
    if (!out_result->reservation) { free(out_result->assistant_text); out_result->assistant_text=NULL; }
    free(message_id); cc_memory_pending_item_cleanup(&item);
    if (rc.code!=CC_OK && !recovery_service_is_current(selection->recovery))
        selection->phase=SESSION_SELECTION_UNAVAILABLE;
    return rc;
}

/** @brief 在 CHATTING 预留原输入和答案空间，再同步提交新轮次。
 * 普通模式使用指定 run_id 检查点，损坏历史模式使用受限制的纯对话路径。
 * 成功后提交答案，失败保留运行标识和预留供 owner 核对；纯对话失败时还可
 * 通过 input_only_ready 提交原输入。调用者拥有输出字符串及尚存的预留。
 * completed 与内存入队、阶段刷新分别判定，返回错误时也检查全部输出。
 * @param[in,out] selection 可为 NULL 的 owner 对象，要求当前盘及 CHATTING。
 * @param[in,out] memory 必填同属装配服务，覆盖预留寿命。
 * @param[in] user_input 必填非空 NUL 正文调用期借用，预留复制原文。
 * @param[in] max_answer_bytes 正的预期答案字节预算。
 * @param[in] on_stream 可为 NULL 的同步流回调。
 * @param[in,out] user_data 可为 NULL 的回调期借用。
 * @param[out] out_result 必填新槽，先清零，run_id/response 由调用者 free，未决预留由其保管。
 * @return 参数、预留、运行、提交或挂载错误，错误时仍检查 completed/input_only_ready。
 */
cc_result_t session_selection_submit_turn_with_memory(
    session_selection_t *selection, managed_memory_service_t *memory,
    const char *user_input, size_t max_answer_bytes,
    cc_runtime_controller_stream_fn on_stream, void *user_data,
    session_new_turn_memory_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid turn output");
    memset(out_result,0,sizeof(*out_result));
    if (!selection || !memory || !user_input || !user_input[0] || !max_answer_bytes ||
        !managed_memory_service_uses(memory,selection->builder,selection->recovery))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory turn submission");
    cc_result_t rc=require_phase(selection,SESSION_SELECTION_CHATTING);
    if (rc.code==CC_OK) rc=cc_id_generate_uuid_v4("run-",&out_result->run_id);
    if (rc.code==CC_OK)
        rc=managed_memory_service_reserve(memory,out_result->run_id,selection->session_id,
                                         user_input,max_answer_bytes,&out_result->reservation);
    if (rc.code!=CC_OK) {
        if (!out_result->reservation) { free(out_result->run_id); out_result->run_id=NULL; }
        return rc;
    }
    bool text_only=recovery_service_history_damaged(selection->recovery);
    rc=text_only ? cc_runtime_controller_submit_text(selection->controller,user_input,
                                                      on_stream,user_data,&out_result->response) :
        cc_runtime_controller_submit_text_with_run_id(selection->controller,selection->session_id,
            out_result->run_id,user_input,on_stream,user_data,&out_result->response);
    if (rc.code==CC_OK) {
        out_result->completed=true;
        cc_result_t commit=managed_memory_service_commit(memory,&out_result->reservation,out_result->response);
        if (commit.code!=CC_OK) { cc_result_free(&rc); rc=commit; }
        else cc_result_free(&commit);
    } else if (text_only && recovery_service_is_current(selection->recovery)) {
        /* 此模式没有可继续的检查点，provider 失败后仍保留原输入用于记忆评估。 */
        out_result->input_only_ready=true;
        cc_result_t commit=managed_memory_service_commit(memory,&out_result->reservation,"");
        cc_result_free(&commit);
    }
    if (!recovery_service_is_current(selection->recovery)) {
        selection->phase=SESSION_SELECTION_UNAVAILABLE;
        if (rc.code==CC_OK) { cc_result_free(&rc); rc=cc_result_error(CC_ERR_STORAGE,"USB storage changed during the turn"); }
    } else if (!out_result->completed && !text_only) {
        /* 中断检查点使当前会话进入等待处理阶段，原错误及预留继续交 owner。 */
        cc_result_t refresh=refresh_phase(selection);
        if (refresh.code!=CC_OK) selection->phase=SESSION_SELECTION_UNAVAILABLE;
        cc_result_free(&refresh);
    }
    return rc;
}
