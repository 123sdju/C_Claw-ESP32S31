#include "startup_recovery.h"
#include "storage_nvs_service.h"

/* 生命周期 owner 持有当前已呈现 service 的借用。入口任务读取原子 gate，
 * service 销毁或呈现下一挂载前由 owner 调用 reset 结束此处借用。 */
static recovery_service_t *s_presented_service;

/**
 * @brief 发布入口阶段并把同一提示送入启动显示事件路径。
 * @param[in] phase 要发布的阶段，FAILED 和 NVS_REPAIR 使用错误事件。
 * @post 原子 gate 已更新，启动覆盖层收到与清屏和事件环排序的提示请求。
 */
static void publish_phase(startup_gate_phase_t phase)
{
    startup_gate_set_phase(phase);
    /* 普通 I/O 路由装配前，经启动事件路径同步呈现当前阶段。 */
    bool error = phase==STARTUP_GATE_FAILED || phase==STARTUP_GATE_NVS_REPAIR ?
        true : false;
    app_event_publish_boot_status(error ? APP_EVENT_KIND_ERROR : APP_EVENT_KIND_STATUS,
        APP_EVENT_SOURCE_SYSTEM,error ? CC_LOG_ERROR : CC_LOG_INFO,startup_gate_message(phase));
}

/**
 * @brief 在启动 gate 尚未占用时检查设备 NVS，失败发布修复阶段。
 * @return gate 已占用为 CC_ERR_INVALID_STATE，其余返回 NVS 检查结果。
 * @pre 生命周期 owner 串行调用，使用内部 RAM 栈，并保持串口和启动事件依赖可用。
 * @note 成功后 gate 保持 OFFLINE，后续装配继续由启动 owner 推进。
 */
cc_result_t startup_recovery_check_nvs(void)
{
    if (s_presented_service || startup_gate_phase()!=STARTUP_GATE_OFFLINE)
        return cc_result_error(CC_ERR_INVALID_STATE,"Startup gate is already in use");
    cc_result_t rc=storage_nvs_service_check();
    if (rc.code!=CC_OK) publish_phase(STARTUP_GATE_NVS_REPAIR);
    return rc;
}

/**
 * @brief 把恢复服务装配状态映射为尚未开放交互的启动阶段。
 * @param[in] state 当前恢复服务状态。
 * @return READY_TO_ATTACH、READY、HISTORY_DAMAGED 映射为 LOADING，其余映射为 FAILED。
 */
static startup_gate_phase_t phase_for(recovery_service_state_t state)
{
    switch (state) {
    case RECOVERY_SERVICE_READY_TO_ATTACH:
    case RECOVERY_SERVICE_READY:
    case RECOVERY_SERVICE_HISTORY_DAMAGED: return STARTUP_GATE_LOADING;
    default: return STARTUP_GATE_FAILED;
    }
}

/**
 * @brief 记录当前 service 的借用并发布其启动状态。
 * @param[in] service 待呈现的恢复服务，借用到 reset，拥有者负责最终销毁。
 * @return 成功登记为 CC_OK，空 service 为 CC_ERR_INVALID_ARGUMENT，gate 已占用为 CC_ERR_INVALID_STATE。
 * @note CC_OK 表示呈现完成，具体服务可用性仍由 service 状态和后续装配检查决定。
 */
cc_result_t startup_recovery_present(recovery_service_t *service)
{
    if (!service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid startup recovery service");
    if (s_presented_service || startup_gate_phase()!=STARTUP_GATE_OFFLINE)
        return cc_result_error(CC_ERR_INVALID_STATE,"Startup gate is already in use");
    s_presented_service=service;
    publish_phase(phase_for(recovery_service_state(service)));
    return cc_result_ok();
}

/**
 * @brief 根据已捕获挂载代数的配置暂存准备恢复服务并呈现启动状态。
 * @param[in] stage 仍有效的配置暂存，成功后由 service 借用，直到 release_stage 或 service 销毁。
 * @param[in] mount_generation 打开 stage 前后核实的非零挂载代数。
 * @param[out] out_failure 可选失败类别，进入函数即置为 OTHER，准备过程可填写具体类别。
 * @param[out] out_service stage 和输出地址合法后先置空，成功返回拥有 service，调用者负责销毁。
 * @return CC_OK 表示准备及呈现完成，gate 占用或依赖准备错误向上传递。
 * @pre 生命周期 owner 排除卸载、重挂载和配置写入，NVS 访问所在任务使用内部 RAM 栈。
 * @note 呈现失败会销毁本次 service，已呈现的 service 在销毁前先 reset。
 */
cc_result_t startup_recovery_prepare_staged(app_config_stage_t *stage, uint64_t mount_generation,
    recovery_preflight_failure_t *out_failure, recovery_service_t **out_service)
{
    if (out_failure) *out_failure = RECOVERY_PREFLIGHT_OTHER;
    if (!stage || !out_service)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid staged startup request");
    *out_service = NULL;
    if (s_presented_service || startup_gate_phase() != STARTUP_GATE_OFFLINE)
        return cc_result_error(CC_ERR_INVALID_STATE, "Startup gate is already in use");
    cc_result_t rc = recovery_service_prepare_staged(stage, mount_generation, out_failure, out_service);
    if (rc.code != CC_OK) return rc;
    rc = startup_recovery_present(*out_service);
    if (rc.code != CC_OK) {
        if (out_failure) *out_failure = RECOVERY_PREFLIGHT_OTHER;
        recovery_service_destroy(*out_service);
        *out_service = NULL;
        return rc;
    }
    return cc_result_ok();
}

/**
 * @brief 复查当前呈现 service 的配置暂存和挂载代数。
 * @param[in,out] service 当前借用同一 stage 的服务，校验失败可能转为 FAILED。
 * @return 身份或暂存关系无效为 CC_ERR_INVALID_STATE，其余返回恢复服务校验结果。
 * @post 校验失败时根据 service 的当前状态更新启动 gate。
 */
cc_result_t startup_recovery_validate_staged(recovery_service_t *service)
{
    if (!service || service != s_presented_service || !recovery_service_has_staged_config(service))
        return cc_result_error(CC_ERR_INVALID_STATE, "No staged config belongs to this startup service");
    cc_result_t rc = recovery_service_validate_staged(service);
    if (rc.code != CC_OK) publish_phase(phase_for(recovery_service_state(service)));
    return rc;
}

/**
 * @brief 在转移前后校验暂存，把解析配置移入调用者的独立配置对象。
 * @param[in,out] service 已呈现且配置修复完成的 READY_TO_ATTACH 服务。
 * @param[in,out] stage service 借用的同一暂存，take 后解析配置被消费，原始校验快照仍保管。
 * @param[in,out] out_config 已初始化且独立拥有的配置，成功时释放旧内容并接管转移结果。
 * @return 参数或别名无效为 CC_ERR_INVALID_ARGUMENT，阶段未就绪为 CC_ERR_INVALID_STATE，
 *         暂存转移或校验失败返回存储/校验错误，成功为 CC_OK。
 * @note 转移后校验失败会销毁本次转移结果，out_config 保管旧内容，stage 的解析配置可能已消费。
 * @pre 已停止借用 stage 解析配置地址的使用者，或为启动 Web 使用独立稳定的 I/O 快照。
 */
cc_result_t startup_recovery_take_staged_config(recovery_service_t *service,
    app_config_stage_t *stage, app_config_t *out_config)
{
    if (!service || !stage || !out_config || service!=s_presented_service ||
        !recovery_service_uses_staged_config(service,stage) ||
        (const app_config_t *)out_config==app_config_stage_config(stage))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid staged config transfer");
    if (startup_gate_phase()!=STARTUP_GATE_LOADING || !recovery_service_staged_config_ready(service) ||
        recovery_service_state(service)!=RECOVERY_SERVICE_READY_TO_ATTACH)
        return cc_result_error(CC_ERR_INVALID_STATE,"Staged configuration is not ready for transfer");
    cc_result_t rc=startup_recovery_validate_staged(service);
    if (rc.code!=CC_OK) return rc;
    app_config_t transferred={0};
    if (app_config_stage_take(stage,&transferred)!=ESP_OK) {
        recovery_service_invalidate_staged(service);
        publish_phase(STARTUP_GATE_FAILED);
        return cc_result_error(CC_ERR_STORAGE,"Staged config changed during transfer");
    }
    rc=startup_recovery_validate_staged(service);
    if (rc.code!=CC_OK) { app_config_destroy(&transferred); return rc; }
    app_config_destroy(out_config);
    *out_config=transferred;
    return cc_result_ok();
}

/**
 * @brief 在配置转移前修复当前已选配置候选并核对暂存。
 * @param[in,out] service 当前呈现的 READY_TO_ATTACH 服务，成功设置配置修复完成标志。
 * @return 阶段或身份无效为 CC_ERR_INVALID_STATE，其余返回修复结果。
 * @post 修复失败且 service 已进入 FAILED 时，启动 gate 同步转为 FAILED。
 */
cc_result_t startup_recovery_repair_config(recovery_service_t *service)
{
    if (!service || service!=s_presented_service || startup_gate_phase()!=STARTUP_GATE_LOADING ||
        recovery_service_state(service)!=RECOVERY_SERVICE_READY_TO_ATTACH)
        return cc_result_error(CC_ERR_INVALID_STATE,"Startup configuration is not ready to repair");
    cc_result_t rc=recovery_service_repair_staged_config(service);
    if (rc.code!=CC_OK && recovery_service_state(service)==RECOVERY_SERVICE_FAILED)
        publish_phase(STARTUP_GATE_FAILED);
    return rc;
}

/**
 * @brief 恢复附加和交互装配后，最后校验并解除 service 对暂存的借用。
 * @param[in,out] service 当前呈现且仍借用 stage 的服务，gate 必须为 LOADING。
 * @return 前置状态无效为 CC_ERR_INVALID_STATE，其余返回 service 释放暂存结果。
 * @post 成功后 stage 可由拥有者销毁，失败保管借用并发布当前 service 对应阶段。
 */
cc_result_t startup_recovery_release_stage(recovery_service_t *service)
{
    if (!service || service != s_presented_service || !recovery_service_has_staged_config(service) ||
        startup_gate_phase() != STARTUP_GATE_LOADING)
        return cc_result_error(CC_ERR_INVALID_STATE, "Staged config is still required by startup");
    cc_result_t rc = recovery_service_release_staged(service);
    if (rc.code != CC_OK) publish_phase(phase_for(recovery_service_state(service)));
    return rc;
}

/**
 * @brief 按暂存配置建立工作区，在 builder 创建前完成恢复目录准备。
 * @param[in,out] service 当前呈现的 READY_TO_ATTACH 服务，gate 必须为 LOADING。
 * @return 前置状态无效为 CC_ERR_INVALID_STATE，其余返回工作区准备结果。
 * @post 准备失败且 service 进入 FAILED 时，启动 gate 同步转为 FAILED。
 */
cc_result_t startup_recovery_prepare_workspace(recovery_service_t *service)
{
    if (!service || service!=s_presented_service || startup_gate_phase()!=STARTUP_GATE_LOADING ||
        recovery_service_state(service)!=RECOVERY_SERVICE_READY_TO_ATTACH)
        return cc_result_error(CC_ERR_INVALID_STATE,"Startup workspace is not ready");
    cc_result_t rc=recovery_service_prepare_staged_workspace(service);
    if (rc.code!=CC_OK && recovery_service_state(service)==RECOVERY_SERVICE_FAILED)
        publish_phase(STARTUP_GATE_FAILED);
    return rc;
}

/**
 * @brief 在没有可呈现 service 的情况下显示一般预检失败。
 * @param[in] failure 仅接收 RECOVERY_PREFLIGHT_OTHER，NVS 错误由专门检查路径呈现。
 * @return gate 空闲且失败类别匹配为 CC_OK，其余为 CC_ERR_INVALID_STATE。
 * @post 成功时 gate 进入 FAILED，并发布启动错误提示。
 */
cc_result_t startup_recovery_present_failure(recovery_preflight_failure_t failure)
{
    if (s_presented_service || startup_gate_phase()!=STARTUP_GATE_OFFLINE || failure!=RECOVERY_PREFLIGHT_OTHER)
        return cc_result_error(CC_ERR_INVALID_STATE,"Invalid startup preflight failure");
    publish_phase(STARTUP_GATE_FAILED);
    return cc_result_ok();
}

/**
 * @brief 将启动入口设为 OFFLINE 并结束当前呈现 service 的借用。
 * @post gate 已关闭，内部 service 指针置空，service 本体由外层继续管理。
 * @pre 生命周期 owner 串行调用，服务销毁与在途请求按各服务屏障协调。
 */
void startup_recovery_reset(void)
{
    startup_gate_set_phase(STARTUP_GATE_OFFLINE);
    s_presented_service=NULL;
}

/**
 * @brief 核对装配阶段并把恢复账本和检查点接入 builder。
 * @param[in,out] service 当前呈现的 READY_TO_ATTACH 服务，调用可能修改其状态和依赖关系。
 * @param[in,out] builder 尚未接纳交互的运行时组合根，附加后借用 service 的恢复资源。
 * @return 参数无效为 CC_ERR_INVALID_ARGUMENT，阶段或身份无效为 CC_ERR_INVALID_STATE，其余返回附加结果。
 * @post 调用附加后按 service 状态发布 LOADING 或 FAILED，入口由 mark_running 最后开放。
 * @note 已附加的 builder 结束借用并销毁后，外层再销毁 service。
 */
cc_result_t startup_recovery_attach(recovery_service_t *service, cc_runtime_builder_t *builder)
{
    if (!service || !builder) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid startup recovery attachment");
    if (service!=s_presented_service || startup_gate_phase()!=STARTUP_GATE_LOADING ||
        recovery_service_state(service)!=RECOVERY_SERVICE_READY_TO_ATTACH)
        return cc_result_error(CC_ERR_INVALID_STATE,"Startup recovery is not ready to attach");
    cc_result_t rc=recovery_service_attach(service,builder);
    publish_phase(phase_for(recovery_service_state(service)));
    return rc;
}

/**
 * @brief 在交互和恢复装配完成后，核对当前存储并发布 RUNNING。
 * @param[in] service 已呈现且已解除 stage 借用的恢复服务。
 * @return 成功为 CC_OK，参数无效为 CC_ERR_INVALID_ARGUMENT，身份或 stage 关系无效为 CC_ERR_INVALID_STATE，
 *         阶段或当前介质核对失败为 CC_ERR_STORAGE 并发布 FAILED。
 * @pre 交互服务已经就绪，启动存储使用者受控，暂存已成功释放。
 */
cc_result_t startup_recovery_mark_running(recovery_service_t *service)
{
    if (!service) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid startup recovery service");
    if (service!=s_presented_service)
        return cc_result_error(CC_ERR_INVALID_STATE,"Startup service is not presented");
    if (recovery_service_has_staged_config(service))
        return cc_result_error(CC_ERR_INVALID_STATE,"Release the staged config before opening input");
    if (startup_gate_phase()!=STARTUP_GATE_LOADING || !recovery_service_is_current(service)) {
        publish_phase(STARTUP_GATE_FAILED);
        return cc_result_error(CC_ERR_STORAGE,"Recovery storage is not current at startup");
    }
    publish_phase(STARTUP_GATE_RUNNING);
    return cc_result_ok();
}
