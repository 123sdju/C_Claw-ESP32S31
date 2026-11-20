#include "startup_managed_memory.h"
#include "cc/adapters/cc_json_memory_store.h"

#include <stdlib.h>

struct startup_managed_memory {
    managed_memory_service_t *memory;
    session_memory_turns_t *turns;
    int summary_prune_deferred;
};

/**
 * @brief 在交互启动前创建自动记忆服务、任务槽并安装摘要缓存限制。
 *
 * 先根据配置组装 worker 的容量、淘汰和快照策略，提取请求最多输出 2048 token。
 * 摘要清理失败只在两类状态下延后：原本已阻断且仍保持阻断的存储错误，
 * 或清理前后均可写且挂载仍有效的存储/I/O 错误。此次新出现的未知安装继续报错。
 * 成功后对象拥有 memory 与 turns，失败释放本次对象。已经设置的 store 召回过滤
 * 和摘要限制可能继续生效，后续装配由启动 owner 处理。
 * @param[in] config 本次借用的配置，输入/输出字节上限均须在提取器支持范围内。
 * @param[in,out] builder 借用的运行时 builder，其记忆存储会设置召回和摘要策略。
 * @param[in] recovery 已附加到同一 builder 的恢复装配，寿命覆盖输出对象。
 * @param[out] out_owner 成功取得拥有句柄，失败为 NULL，使用 startup_managed_memory_destroy 释放。
 * @return CC_OK 表示组合对象可用，参数、容量、分配或依赖初始化错误向上传递。
 * @pre recovery 已附加，交互入口尚未开放，外层排除卸载、重挂载及存储并发使用。
 * @note 答复容量加 128 字节仅作早期预算检查，实际输入和标识仍在 reserve 时计费。
 */
cc_result_t startup_managed_memory_create(
    const app_config_t *config, cc_runtime_builder_t *builder, recovery_service_t *recovery,
    startup_managed_memory_t **out_owner)
{
    if (!out_owner) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory owner output");
    *out_owner=NULL;
    if (!config || !builder || !recovery || !config->runtime.memory_path ||
        !config->managed_memory_capacity || !config->managed_memory_pending_items ||
        !config->managed_memory_pending_bytes || !config->summary_cache_per_session ||
        !config->summary_cache_max_entries || !config->summary_cache_max_bytes ||
        config->runtime.max_input_bytes<=0 ||
        config->runtime.max_output_bytes<=0 ||
        (size_t)config->runtime.max_input_bytes>CC_MEMORY_EXTRACTION_MAX_TURN_BYTES ||
        (size_t)config->runtime.max_output_bytes>CC_MEMORY_EXTRACTION_MAX_TURN_BYTES)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid automatic memory configuration");
    size_t answer_bytes=(size_t)config->runtime.max_output_bytes;
    if (config->managed_memory_pending_bytes<=answer_bytes+128U)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Memory queue cannot reserve one answer");

    startup_managed_memory_t *owner=calloc(1,sizeof(*owner));
    if (!owner) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate memory owner");
    cc_managed_memory_worker_options_t worker={0};
    worker.size=sizeof(worker);
    worker.capacity=config->managed_memory_capacity;
    worker.max_pending_items=config->managed_memory_pending_items;
    worker.max_pending_bytes=config->managed_memory_pending_bytes;
    worker.select_eviction=managed_memory_select_eviction;
    worker.decide_snapshot=managed_memory_decide_snapshot;
    cc_memory_extraction_options_t extraction={0};
    extraction.size=sizeof(extraction);
    extraction.model=config->runtime.model;
    extraction.timeout_ms=config->runtime.provider_timeout_ms;
    extraction.max_output_tokens=2048;
    cc_result_t rc=managed_memory_service_create(builder,recovery,config->runtime.memory_path,
                                                  &worker,&extraction,&owner->memory);
    if (rc.code==CC_OK) {
        cc_result_free(&rc);
        cc_memory_store_t *store=cc_runtime_builder_borrow_memory_store(builder);
        int ready=0;
        rc=cc_json_memory_write_ready(store,&ready);
        if (rc.code==CC_OK) {
            cc_result_free(&rc);
            rc=cc_json_memory_set_summary_cache_limits(store,config->summary_cache_per_session,
                config->summary_cache_max_entries,config->summary_cache_max_bytes);
            if (rc.code==CC_ERR_STORAGE || rc.code==CC_ERR_IO) {
                int still_ready=0;
                cc_result_t status=cc_json_memory_write_ready(store,&still_ready);
                int safely_deferred=status.code==CC_OK && recovery_service_is_current(recovery) &&
                    ((!ready && !still_ready && rc.code==CC_ERR_STORAGE) || (ready && still_ready));
                cc_result_free(&status);
                if (safely_deferred) {
                    cc_result_free(&rc);
                    owner->summary_prune_deferred=1;
                    rc=cc_result_ok();
                }
            }
        }
    }
    if (rc.code==CC_OK)
        rc=session_memory_turns_create(owner->memory,config->managed_memory_pending_items,
                                       answer_bytes,&owner->turns);
    if (rc.code!=CC_OK) {
        managed_memory_service_destroy(owner->memory);
        free(owner);
        return rc;
    }
    *out_owner=owner;
    return rc;
}

/**
 * @brief 取得组合对象持有的任务槽 owner，供交互任务串行使用。
 * @param[in] owner 借用的组合对象，可为 NULL。
 * @return turns 的借用指针，owner 为 NULL 时返回 NULL，借用期至组合对象销毁。
 */
session_memory_turns_t *startup_managed_memory_turns(startup_managed_memory_t *owner)
{
    return owner ? owner->turns : NULL;
}

/**
 * @brief 查询创建阶段是否曾安全延后摘要清理。
 * @param[in] owner 借用的组合对象，可为 NULL。
 * @return 创建阶段曾延后时返回一，其余返回零，后续处理保持这个启动记录。
 * @note 实时可写性由 JSON store 查询，当前字段用于启动提示。
 */
int startup_managed_memory_summary_prune_deferred(const startup_managed_memory_t *owner)
{
    return owner && owner->summary_prune_deferred;
}

/**
 * @brief 按 turns、memory、组合对象的顺序结束自动记忆的当前生命周期。
 * @param[in,out] inout_owner 拥有句柄的地址，成功后置空，空句柄可重复调用。
 * @return CC_OK 表示清理完成，地址为空返回 CC_ERR_INVALID_ARGUMENT，
 *         turns 取消预留失败时保管 owner 和剩余槽供重试。
 * @pre interaction 和全部借用者已经退出，builder/recovery 仍存活。
 * @post 成功后旧对象及其借用结果结束寿命，后续调用使用新的有效对象。
 */
cc_result_t startup_managed_memory_destroy(startup_managed_memory_t **inout_owner)
{
    if (!inout_owner) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory owner");
    startup_managed_memory_t *owner=*inout_owner;
    if (!owner) return cc_result_ok();
    cc_result_t rc=session_memory_turns_destroy_for_shutdown(owner->turns);
    if (rc.code!=CC_OK) return rc;
    owner->turns=NULL;
    managed_memory_service_destroy(owner->memory);
    free(owner);
    *inout_owner=NULL;
    return rc;
}
