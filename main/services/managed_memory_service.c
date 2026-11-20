#include "managed_memory_service.h"
#include "app_config.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_HISTORY_DIR APP_RECOVERY_ROOT "/memory_versions"

struct managed_memory_service {
    cc_runtime_builder_t *builder;       /* 借用，提供 generation 租约及恢复维护屏障。 */
    recovery_service_t *recovery;        /* 借用，核对当前挂载。 */
    cc_memory_store_t *store;            /* 从 builder 借用的 JSON store。 */
    cc_filesystem_t filesystem;          /* 本服务取得并释放的文件系统句柄。 */
    cc_memory_snapshot_store_t *snapshots; /* 本服务拥有，先于 filesystem 销毁。 */
    cc_managed_memory_editor_t *editor;  /* 本服务拥有，借用 store/snapshots。 */
    cc_managed_memory_worker_t *worker;  /* 本服务拥有，包含 RAM 队列及提取缓存。 */
    cc_memory_extraction_options_t extraction; /* 创建时复制，cancel_token 按次传入。 */
    char *model;                        /* extraction.model 使用的堆副本。 */
    cc_memory_before_ack_fn extra_guard; /* 调用方提供的附加确认回调。 */
    void *extra_guard_data;             /* 借用至服务销毁的回调上下文。 */
};

typedef struct observed_memory_provider {
    cc_llm_provider_t *inner;       /* 本次 generation 租约内借用的 provider。 */
    cc_agent_runtime_t *runtime;   /* 同一租约内借用的用量归因目标。 */
} observed_memory_provider_t;

/**
 * @brief 在 provider 边界记录真实模型调用。
 *
 * worker 的保留方案重试复用提取结果，实际已发生但随后被拒绝或写入确认失败的调用仍计入观测。
 * @param[in] self 栈上观测包装上下文，同步调用期间借用 provider/runtime。
 * @param[in] request 原样转交底层的请求，读取 model/session/origin turn 作为用量归因。
 * @param[out] response 按 provider 契约填写的响应，调用者清理其中拥有字段。
 * @return provider/chat/response 不可用时返回 CC_ERR_UNSUPPORTED，其余原样返回 chat 结果，上报成功调用或失败响应中的有效用量。
 */
static cc_result_t observed_memory_chat(void *self, const cc_llm_chat_request_t *request,
                                        cc_llm_response_t *response)
{
    observed_memory_provider_t *observed=self;
    if (!observed || !observed->inner || !observed->inner->vtable ||
        !observed->inner->vtable->chat || !response)
        return cc_result_error(CC_ERR_UNSUPPORTED,"Memory model chat is unavailable");
    cc_result_t rc=observed->inner->vtable->chat(observed->inner->self,request,response);
    if (rc.code==CC_OK || response->usage.valid)
        cc_agent_runtime_report_auxiliary_usage_for_turn(observed->runtime,CC_AUXILIARY_USAGE_MEMORY_EXTRACTION,
            request ? request->model : NULL,request ? request->session_id : NULL,
            cc_llm_chat_request_origin_turn_id(request),
            response->usage.valid ? &response->usage : NULL);
    return rc;
}

static const cc_llm_provider_vtable_t observed_memory_vtable={.chat=observed_memory_chat};

/**
 * @brief 产品淘汰规则按重要度、更新时间、ID 字节序逐级选最小项。
 *
 * records 调用期借用，成功只返回索引，规划器负责实际删除与资源归还。
 * @param[in] records 调用期间借用的候选记忆数组，每条须含有效 ID。
 * @param[in] count records 中有效候选记录数量，须大于零。
 * @param[in] user_data 本策略忽略的兼容上下文，允许 NULL。
 * @param[out] out_index 必填，全部 ID 校验通过后写入零基索引，失败保持原值。
 * @return CC_OK 表示输出淘汰索引，参数或候选 ID 无效时返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t managed_memory_select_eviction(
    const cc_memory_plan_record_t *records, size_t count, void *user_data, size_t *out_index)
{
    (void)user_data;
    if (!records || !count || !out_index)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory eviction request");
    size_t selected=0;
    for (size_t i=0;i<count;i++) {
        if (!records[i].id || !records[i].id[0])
            return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Memory eviction record has no identity");
        if (i && (records[i].importance<records[selected].importance ||
            (records[i].importance==records[selected].importance &&
             (records[i].updated_at<records[selected].updated_at ||
              (records[i].updated_at==records[selected].updated_at &&
               strcmp(records[i].id,records[selected].id)<0)))))
            selected=i;
    }
    *out_index=selected;
    return cc_result_ok();
}

/**
 * @brief 正文/类别重写或移除同时达到 5 条与原条目的 30% 时要求 REWRITE 快照。
 *
 * 使用分解算式求上取整以控制 size_t 溢出，空基线或小变更返回零。
 * @param[in] plan 借用的方案，读取 rewritten_existing，允许 NULL。
 * @param[in] original_count 生成方案前已有受管理记忆的条数。
 * @param[in] user_data 本策略忽略的兼容上下文，允许 NULL。
 * @return 达到阈值返回 CC_MEMORY_SNAPSHOT_REWRITE，空方案、空基线或未达阈值返回零。
 */
cc_memory_snapshot_reason_t managed_memory_decide_snapshot(
    const cc_memory_plan_t *plan, size_t original_count, void *user_data)
{
    (void)user_data;
    if (!plan || !original_count || plan->rewritten_existing<5U) return 0;
    /* 通过分解算式向上取整 original_count 的 30%，控制 size_t 乘法溢出。 */
    size_t threshold=(original_count/10U)*3U+((original_count%10U)*3U+9U)/10U;
    return plan->rewritten_existing>=threshold ? CC_MEMORY_SNAPSHOT_REWRITE : 0;
}

/**
 * @brief worker 确认出队前核对原挂载，调用外层附加 guard 后再次检查。
 *
 * worker 在处理锁内调用，附加 guard 禁止重入同一 worker 的处理、重规划、丢弃和销毁入口。
 * @param[in] user_data 创建时绑定的 service，其依赖和附加 guard data 均保持存活。
 * @return 挂载有效且附加 guard 通过时返回 CC_OK，否则返回存储或 guard 错误。
 */
static cc_result_t check_before_ack(void *user_data)
{
    managed_memory_service_t *service=user_data;
    if (!recovery_service_is_current(service->recovery))
        return cc_result_error(CC_ERR_STORAGE,"USB changed before acknowledging the memory update");
    if (service->extra_guard) {
        cc_result_t rc=service->extra_guard(service->extra_guard_data);
        if (rc.code!=CC_OK) return rc;
        cc_result_free(&rc);
    }
    return recovery_service_is_current(service->recovery) ? cc_result_ok() :
        cc_result_error(CC_ERR_STORAGE,"USB changed before acknowledging the memory update");
}

/**
 * @brief 在当前恢复装配上组合受管理记忆。
 *
 * 核对依赖身份和 JSON 路径后设置受管理召回限制，再分配服务及其子对象。
 * 后续失败清理已拥有的资源，先前设置的召回限制仍然生效。
 * model 复制到堆中，提取选项的 cancel_token 清空，附加 guard 由服务的挂载检查包装。
 * @param[in,out] builder 借用至服务销毁的运行时 builder，提供当前 JSON store。
 * @param[in,out] recovery 借用至服务销毁的恢复装配，须属于 builder。
 * @param[in] memory_path 当前恢复装配使用的 JSON 记忆路径，调用期间借用。
 * @param[in] worker_options 容量、淘汰、快照及确认策略，标量被复制，策略上下文继续借用。
 * @param[in] extraction_options 模型提取选项，model 在服务创建时复制，取消 token 按次设置。
 * @param[out] out_service 必填，先置 NULL，成功接收拥有的服务。
 * @return CC_OK 表示创建成功，参数、装配、挂载、文件系统、分配或子对象创建错误向上传递。
 */
cc_result_t managed_memory_service_create(cc_runtime_builder_t *builder, recovery_service_t *recovery,
    const char *memory_path, const cc_managed_memory_worker_options_t *worker_options,
    const cc_memory_extraction_options_t *extraction_options, managed_memory_service_t **out_service)
{
    if (!out_service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service output");
    *out_service=NULL;
    if (!builder || !recovery || !memory_path || !memory_path[0] || !worker_options ||
        worker_options->size<offsetof(cc_managed_memory_worker_options_t,before_ack) ||
        !worker_options->capacity || !worker_options->max_pending_items || !worker_options->max_pending_bytes ||
        !worker_options->select_eviction || !worker_options->decide_snapshot || !extraction_options ||
        extraction_options->size<sizeof(*extraction_options))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Memory limits, policies and extraction options are required");
    if (!recovery_service_uses_builder(recovery,builder))
        return cc_result_error(CC_ERR_STORAGE,"Runtime is not attached to current recovery storage");
    cc_memory_store_t *store=cc_runtime_builder_borrow_memory_store(builder);
    if (!store || !cc_json_memory_store_matches_path(store,memory_path))
        return cc_result_error(CC_ERR_INVALID_STATE,"Runtime memory store does not match the configured JSON file");
    /* 普通召回限制到受管理事实，容量规则统一作用于用户记忆。派生上下文摘要继续供内部读取。 */
    cc_result_t restricted=cc_json_memory_restrict_recall_to_managed(store);
    if (restricted.code!=CC_OK) return restricted;
    cc_result_free(&restricted);
    managed_memory_service_t *service=calloc(1,sizeof(*service));
    if (!service) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate memory service");
    service->builder=builder; service->recovery=recovery; service->store=store;
    service->extraction=*extraction_options; service->extraction.cancel_token=NULL;
    if (extraction_options->model) {
        size_t length=strlen(extraction_options->model)+1U;
        service->model=malloc(length);
        if (service->model) memcpy(service->model,extraction_options->model,length);
        if (!service->model) { managed_memory_service_destroy(service); return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot copy memory model"); }
        service->extraction.model=service->model;
    }
    if (worker_options->size>=offsetof(cc_managed_memory_worker_options_t,before_ack)+sizeof(worker_options->before_ack))
        service->extra_guard=worker_options->before_ack;
    if (worker_options->size>=offsetof(cc_managed_memory_worker_options_t,before_ack_user_data)+sizeof(worker_options->before_ack_user_data))
        service->extra_guard_data=worker_options->before_ack_user_data;
    cc_result_t rc=cc_filesystem_get_default(&service->filesystem);
    if (rc.code==CC_OK && (!service->filesystem.vtable || !service->filesystem.vtable->make_dir))
        rc=cc_result_error(CC_ERR_UNSUPPORTED,"Memory snapshots require a writable filesystem");
    if (rc.code==CC_OK) rc=service->filesystem.vtable->make_dir(service->filesystem.self,MEMORY_HISTORY_DIR);
    if (rc.code==CC_OK && !recovery_service_is_current(recovery))
        rc=cc_result_error(CC_ERR_STORAGE,"USB changed while preparing memory snapshots");
    if (rc.code==CC_OK)
        rc=cc_memory_snapshot_store_open(&service->filesystem,MEMORY_HISTORY_DIR,memory_path,&service->snapshots);
    if (rc.code==CC_OK) rc=cc_managed_memory_editor_create(store,service->snapshots,&service->editor);
    if (rc.code==CC_OK) {
        cc_managed_memory_worker_options_t options={0};
        memcpy(&options,worker_options,offsetof(cc_managed_memory_worker_options_t,before_ack));
        options.size=sizeof(options); options.before_ack=check_before_ack; options.before_ack_user_data=service;
        rc=cc_managed_memory_worker_create(store,service->snapshots,&options,&service->worker);
    }
    if (rc.code==CC_OK && !recovery_service_is_current(recovery))
        rc=cc_result_error(CC_ERR_STORAGE,"USB changed while opening memory service");
    if (rc.code!=CC_OK) { managed_memory_service_destroy(service); return rc; }
    *out_service=service; return cc_result_ok();
}

/**
 * @brief 按依赖顺序清理 worker、editor、快照存储、文件系统句柄和 model 副本。
 * @pre 入口已关闭，处理、回调、预留及直接 store/editor 使用者已排空，借用依赖仍存活。
 * @param[in,out] service 待释放服务，允许 NULL，支持创建失败后的部分装配。
 * @post 非空服务寿命结束，借出的编辑器及全部预留指针失效。
 */
void managed_memory_service_destroy(managed_memory_service_t *service)
{
    if (!service) return;
    cc_managed_memory_worker_destroy(service->worker);
    cc_managed_memory_editor_destroy(service->editor);
    cc_memory_snapshot_store_destroy(service->snapshots);
    if (service->filesystem.vtable && service->filesystem.vtable->destroy)
        service->filesystem.vtable->destroy(service->filesystem.self);
    free(service->model); free(service);
}

/**
 * @brief 借出 service 拥有的 editor，至 service 销毁前按其接口契约使用。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 所属 service 拥有的 editor 借用指针，参数为空时返回 NULL。
 */
cc_managed_memory_editor_t *managed_memory_service_editor(managed_memory_service_t *service)
{ return service ? service->editor : NULL; }

/**
 * @brief 只比较装配身份，支持介质离线时核对 RAM 预留属于哪个 service。
 * @param[in] service 借用的服务，允许 NULL。
 * @param[in] builder 要比较的 builder 指针。
 * @param[in] recovery 要比较的恢复装配指针。
 * @return 服务存在且两个依赖指针均匹配时为真，其余为假。
 */
bool managed_memory_service_uses(const managed_memory_service_t *service,
    const cc_runtime_builder_t *builder, const recovery_service_t *recovery)
{ return service && service->builder==builder && service->recovery==recovery; }

/**
 * @brief 先检查挂载，再委托 worker 复制对话到 RAM FIFO。
 * @param[in,out] service 借用的服务及其队列。
 * @param[in] turn_id 非空的稳定轮次 ID，调用期间借用。
 * @param[in] session_id 非空的原会话 ID，调用期间借用。
 * @param[in] user_input 非空原输入，同步借用并复制，重复 ID 时逐字比较。
 * @param[in] assistant_text 完整答复，同步借用并复制，重复 ID 时逐字比较。
 * @return CC_OK 表示 FIFO 已接纳或当前队列中已有相同内容，参数、容量、身份、分配或挂载错误向上传递。
 */
cc_result_t managed_memory_service_enqueue(managed_memory_service_t *service,
    const char *turn_id, const char *session_id, const char *user_input, const char *assistant_text)
{
    if (!service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service");
    if (!recovery_service_is_current(service->recovery))
        return cc_result_error(CC_ERR_STORAGE,"Recovery storage is not current");
    return cc_managed_memory_worker_enqueue(service->worker,turn_id,session_id,user_input,assistant_text);
}

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
 * @param[out] out_reservation 必填，服务和输出有效后先置 NULL，成功输出须串行提交、扩容或取消。
 * @return CC_OK 表示预留成功，参数、重复身份、容量、分配或挂载错误向上传递，取消失败时须保管残留句柄。
 */
cc_result_t managed_memory_service_reserve(managed_memory_service_t *service,
    const char *turn_id, const char *session_id, const char *user_input,
    size_t max_answer_bytes, cc_memory_pending_reservation_t **out_reservation)
{
    if (!service || !out_reservation) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory reservation request");
    *out_reservation=NULL;
    if (!recovery_service_is_current(service->recovery))
        return cc_result_error(CC_ERR_STORAGE,"Recovery storage is not current");
    cc_result_t rc=cc_managed_memory_worker_reserve(service->worker,turn_id,session_id,user_input,
                                                     max_answer_bytes,out_reservation);
    if (rc.code==CC_OK && !recovery_service_is_current(service->recovery)) {
        cc_result_t cancel=cc_managed_memory_worker_cancel_reservation(service->worker,out_reservation);
        if (cancel.code!=CC_OK) { cc_result_free(&rc); return cancel; }
        cc_result_free(&cancel); cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE,"USB changed before the memory turn started");
    }
    return rc;
}

/**
 * @brief 将已预留输入/答案提交到 RAM FIFO，模型期间介质离线仍可保留结果。
 *
 * 成功消费预留句柄，失败保持调用者拥有关系，后续由 turns 核对并重试。
 * @param[in,out] service 借用的服务及其队列，本路径不检查挂载。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @param[in] assistant_text 完整答复，允许空字符串，同步复制到预留缓冲。
 * @return CC_OK 表示句柄已消费并转入 FIFO，答复超长或句柄无效时返回错误并保管原预留。
 */
cc_result_t managed_memory_service_commit(managed_memory_service_t *service,
    cc_memory_pending_reservation_t **reservation, const char *assistant_text)
{
    if (!service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service");
    return cc_managed_memory_worker_commit(service->worker,reservation,assistant_text);
}

/**
 * @brief 在同一 RAM 队列预算中扩大答复预留，失败保留原容量与计费。
 * @param[in,out] service 借用的服务及其队列，本路径不检查挂载。
 * @param[in,out] reservation 仍属于该队列的预留句柄。
 * @param[in] max_answer_bytes 新的最大答复正文字节数，额外保留一个 NUL 字节，须小于 SIZE_MAX。
 * @return CC_OK 表示容量已足够或扩容成功，错误时保管既有预留、容量和计费。
 */
cc_result_t managed_memory_service_expand_reservation(managed_memory_service_t *service,
    cc_memory_pending_reservation_t *reservation, size_t max_answer_bytes)
{
    if (!service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service");
    return cc_managed_memory_worker_expand_reservation(service->worker,reservation,max_answer_bytes);
}

/**
 * @brief RAM 内核对预留的 turn/session/原输入，句柄及字符串在调用中借用。
 * @param[in] service 借用的服务及其队列，本路径不检查挂载。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[in] turn_id 待处理对话的稳定标识，调用期间借用。
 * @param[in] session_id 原输入所属会话标识，调用期间借用。
 * @param[in] user_input 同步借用的待比较原输入。
 * @return CC_OK 表示预留身份和原输入匹配，句柄无效或内容冲突返回相应错误。
 */
cc_result_t managed_memory_service_verify_reservation(managed_memory_service_t *service,
    cc_memory_pending_reservation_t *reservation, const char *turn_id,
    const char *session_id, const char *user_input)
{
    if (!service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service");
    return cc_managed_memory_worker_verify_reservation(service->worker,reservation,turn_id,session_id,user_input);
}

/**
 * @brief 返回预留内容的拥有副本，用 cc_memory_pending_item_cleanup 清理。
 * @param[in] service 借用的服务及其队列，本路径不检查挂载。
 * @param[in] reservation 仍属于该队列的预留句柄。
 * @param[out] out_item 必填且无待释放内容，成功取得原输入及空答复的独立副本，用 cc_memory_pending_item_cleanup 清理。
 * @return CC_OK 表示输出拥有副本，参数、句柄或分配错误向上传递。
 */
cc_result_t managed_memory_service_inspect_reservation(managed_memory_service_t *service,
    cc_memory_pending_reservation_t *reservation, cc_memory_pending_item_t *out_item)
{
    if (!service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service");
    return cc_managed_memory_worker_inspect_reservation(service->worker,reservation,out_item);
}

/**
 * @brief 取消 RAM 预留并归还预算，成功清空句柄，错误由持有预留的 owner 处理。
 * @param[in,out] service 借用的服务及其队列，本路径不检查挂载。
 * @param[in,out] reservation 仍属于该队列的预留句柄，成功消费后置空。
 * @return CC_OK 表示预留已释放且句柄置空，句柄无效时返回错误。
 */
cc_result_t managed_memory_service_cancel_reservation(managed_memory_service_t *service,
    cc_memory_pending_reservation_t **reservation)
{
    if (!service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service");
    return cc_managed_memory_worker_cancel_reservation(service->worker,reservation);
}

/**
 * @brief 取得临时 generation 租约，为 worker 借用当前 provider 并附加用量观测包装。
 *
 * 每次仅处理队首，独立模型请求与普通请求由 owner 串行协调，token 仅本次借用。
 * 释放租约后再次核对挂载。末次检查报错时 worker 可能已确认出队，须按原 turn 核对。
 * worker 的其他失败保留队首及已有提取缓存供重试。
 * @pre owner 已协调普通请求与独立提取请求的 provider 使用。
 * @param[in,out] service 借用的服务、存储及 worker。
 * @param[in] updated_at 应用提供的非负更新时间，worker 首次成功提取后固定该值。
 * @param[in] cancel_token 可选取消信号，调用期间借用。
 * @param[out] out_result 必填，worker 填写方案计数与本次模型用量，后续失败仍可能保留结果，进入 worker 前失败保持原值。
 * @return 返回队首处理结果，末次挂载检查失败也会报错，此时需另查队列是否已出队。
 */
cc_result_t managed_memory_service_process_one(managed_memory_service_t *service,
    int64_t updated_at, cc_cancel_token_t *cancel_token, cc_managed_memory_worker_result_t *out_result)
{
    if (!service || !out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory processing request");
    if (!recovery_service_is_current(service->recovery))
        return cc_result_error(CC_ERR_STORAGE,"Recovery storage is not current");
    cc_runtime_generation_t *generation=NULL;
    cc_result_t rc=cc_runtime_builder_acquire_generation(service->builder,&generation);
    if (rc.code==CC_OK) {
        cc_llm_provider_t *provider=cc_runtime_generation_borrow_provider(generation);
        if (!provider) rc=cc_result_error(CC_ERR_INVALID_STATE,"Memory model provider is unavailable");
        else {
            cc_memory_extraction_options_t options=service->extraction; options.cancel_token=cancel_token;
            observed_memory_provider_t observed={provider,cc_runtime_generation_runtime(generation)};
            cc_llm_provider_t wrapped={&observed,&observed_memory_vtable};
            rc=cc_managed_memory_worker_process_one(service->worker,&wrapped,&options,updated_at,out_result);
        }
    }
    if (generation) cc_runtime_generation_release(generation);
    if (rc.code==CC_OK && !recovery_service_is_current(service->recovery)) {
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE,"USB changed after the memory update; verify its outcome");
    }
    return rc;
}

/**
 * @brief 读取 RAM 队列总项数与计费字节，总项包括尚未提交的预留，离线仍可查询。
 * @param[in] service 借用的服务及其 RAM 队列。
 * @param[out] out_pending 可选输出，FIFO 项与预留合计数量。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @return CC_OK 表示状态已填写，对象无效或所有输出均为空时返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t managed_memory_service_status(managed_memory_service_t *service, size_t *out_pending, size_t *out_bytes)
{
    if (!service || (!out_pending && !out_bytes))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service status request");
    cc_managed_memory_worker_status(service->worker,out_pending,out_bytes);
    return cc_result_ok();
}

/**
 * @brief 分别读取 FIFO 就绪项、预留数量和总计费字节，至少提供一个输出。
 * @param[in] service 借用的服务及其 RAM 队列。
 * @param[out] out_ready 可选输出，FIFO 中已提交项数。
 * @param[out] out_reserved 可选输出，尚未提交的预留数。
 * @param[out] out_bytes 可选输出，包含完整答复预留的计费字节数。
 * @return CC_OK 表示状态已填写，对象无效或所有输出均为空时返回 CC_ERR_INVALID_ARGUMENT。
 */
cc_result_t managed_memory_service_status_detail(managed_memory_service_t *service,
    size_t *out_ready, size_t *out_reserved, size_t *out_bytes)
{
    if (!service || (!out_ready && !out_reserved && !out_bytes))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service status request");
    cc_managed_memory_worker_status_detail(service->worker,out_ready,out_reserved,out_bytes);
    return cc_result_ok();
}

/**
 * @brief builder 维护屏障中的回调，先核对 store 身份与当前盘，再原位重读 JSON 主文件。
 *
 * 外层已排空直接借用者，仍由 JSON 存储检查恢复现场是否具备重读条件。
 * @param[in,out] store builder 在维护屏障内提供的当前存储，须与 service 借用对象相同。
 * @param[in] user_data 本次维护借用的 service，依赖在回调期间保持存活。
 * @return 装配身份与挂载有效时返回原位重载结果，否则返回 CC_ERR_STORAGE。
 */
static cc_result_t recover_memory_quiescent(cc_memory_store_t *store, void *user_data)
{
    managed_memory_service_t *service=user_data;
    if (!store || store!=service->store || !recovery_service_is_current(service->recovery))
        return cc_result_error(CC_ERR_STORAGE,"Recovery storage is not current");
    return cc_json_memory_recover_in_place(store);
}

/**
 * @brief 由空闲 owner 在 generation 排空屏障下恢复 store，队列与提取缓存保留供核对。
 *
 * 操作前后检查介质，直接借用者由外层先停止，失败返回当前错误供后续处理。
 * @pre 直接 store/editor 借用者已排空，owner 串行执行本次维护。
 * @param[in,out] service 借用的服务及待恢复存储。
 * @return CC_OK 表示屏障内恢复及挂载复查完成，屏障、存储或挂载错误向上传递，末次复查失败时存储可能已恢复。
 */
cc_result_t managed_memory_service_recover_store(managed_memory_service_t *service)
{
    if (!service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory service");
    if (!recovery_service_is_current(service->recovery))
        return cc_result_error(CC_ERR_STORAGE,"Recovery storage is not current");
    cc_result_t rc=cc_runtime_builder_maintain_memory_quiescent(service->builder,
                                                                recover_memory_quiescent,service);
    if (rc.code==CC_OK && !recovery_service_is_current(service->recovery)) {
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE,"USB changed while recovering memory storage");
    }
    return rc;
}
