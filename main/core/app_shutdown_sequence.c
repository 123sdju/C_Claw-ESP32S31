#include "app_shutdown_sequence.h"

/**
 * @brief 顺序调用十三个关闭步骤，在首个失败处停止推进。
 * @param[in] ops 调用期间稳定的关闭操作表，十三个回调均须非空。
 * @param[in,out] context 原样传给每个回调的上下文，其有效性与状态修改由回调契约决定。
 * @param[in] timeout_ms 逐步原样传递的超时参数，单位毫秒，每步均取得相同值。
 * @param[out] out_failed_stage 可选输出，先置 NONE，步骤失败时写入对应阶段。
 * @return 全部步骤成功为 ESP_OK，操作表不完整为 ESP_ERR_INVALID_ARG，其余返回首个失败步骤的错误。
 * @note 已完成步骤由各回调保管其状态，后续依赖继续存活。再次调用从第一步开始，
 *       各回调需要按实际关闭状态处理重试，本函数本身只记录当次失败阶段。
 */
esp_err_t app_shutdown_sequence_run(const app_shutdown_ops_t *ops, void *context,
    uint32_t timeout_ms, app_shutdown_stage_t *out_failed_stage)
{
    if (out_failed_stage) *out_failed_stage=APP_SHUTDOWN_NONE;
    if (!ops || !ops->usb_owner || !ops->serial || !ops->config_drain || !ops->ui_callbacks || !ops->web || !ops->camera || !ops->interaction || !ops->profile_stop ||
        !ops->runtime || !ops->network || !ops->ui || !ops->events || !ops->profile_destroy)
        return ESP_ERR_INVALID_ARG;
    const app_shutdown_step_fn steps[]={ops->usb_owner,ops->serial,ops->config_drain,ops->ui_callbacks,ops->web,ops->camera,ops->interaction,ops->profile_stop,
        ops->network,ops->runtime,ops->ui,ops->events,ops->profile_destroy};
    for (unsigned i=0;i<sizeof(steps)/sizeof(steps[0]);i++) {
        esp_err_t err=steps[i](context,timeout_ms);
        if (err!=ESP_OK) {
            if (out_failed_stage) *out_failed_stage=(app_shutdown_stage_t)(APP_SHUTDOWN_USB_OWNER+i);
            return err;
        }
    }
    return ESP_OK;
}
