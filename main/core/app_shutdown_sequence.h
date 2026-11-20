#pragma once

#include "esp_err.h"
#include <stdint.h>

/** 关闭执行顺序。USB_OWNER 至 PROFILE_DESTROY 连续编号，与操作表调用顺序对应。 */
typedef enum app_shutdown_stage {
    APP_SHUTDOWN_NONE = 0,
    APP_SHUTDOWN_USB_OWNER,
    APP_SHUTDOWN_SERIAL,
    APP_SHUTDOWN_CONFIG_DRAIN,
    APP_SHUTDOWN_UI_CALLBACKS,
    APP_SHUTDOWN_WEB,
    APP_SHUTDOWN_CAMERA,
    APP_SHUTDOWN_INTERACTION,
    APP_SHUTDOWN_PROFILE_STOP,
    APP_SHUTDOWN_NETWORK,
    APP_SHUTDOWN_RUNTIME,
    APP_SHUTDOWN_UI,
    APP_SHUTDOWN_EVENTS,
    APP_SHUTDOWN_PROFILE_DESTROY
} app_shutdown_stage_t;

/**
 * @brief 完成一个关闭或排空阶段，并保管该阶段的实际完成状态。
 * @param[in,out] context 原样传入的生命周期上下文，由具体回调解释。
 * @param[in] timeout_ms 本步骤获得的毫秒参数，具体等待语义由实现决定。
 * @return ESP_OK 允许后续步骤推进，其他结果使本次序列停止并交给拥有者处理。
 * @note 步骤可能已经完成部分清理，后续重试由其实现识别当前状态。
 */
typedef esp_err_t (*app_shutdown_step_fn)(void *context, uint32_t timeout_ms);
/** 十三个必需步骤，按枚举顺序执行，先排空上游入口再释放其依赖。 */
typedef struct app_shutdown_ops {
    app_shutdown_step_fn usb_owner; /**< 关闭 USB 生命周期 owner 的介质操作接纳并排空当前操作。 */
    app_shutdown_step_fn serial; /**< 停止串口入口。 */
    app_shutdown_step_fn config_drain; /**< 等待已接纳配置请求退出。 */
    app_shutdown_step_fn ui_callbacks; /**< 关闭并排空 UI 回调。 */
    app_shutdown_step_fn web; /**< 停止 Web handler 与其借用。 */
    app_shutdown_step_fn camera; /**< 停止相机业务。 */
    app_shutdown_step_fn interaction; /**< 停止交互 owner。 */
    app_shutdown_step_fn profile_stop; /**< 停止采样任务。 */
    app_shutdown_step_fn network; /**< 停止网络服务。 */
    app_shutdown_step_fn runtime; /**< 释放记忆及运行时装配。 */
    app_shutdown_step_fn ui; /**< 结束 UI 对象和显示借用。 */
    app_shutdown_step_fn events; /**< 排空并释放事件路径。 */
    app_shutdown_step_fn profile_destroy; /**< 释放采样对象。 */
} app_shutdown_ops_t;

/**
 * @brief 执行固定关闭序列，失败时保管后续仍可能被引用的依赖。
 * @param[in] ops 必填操作表，所有十三个回调都须有效且在调用期间稳定。
 * @param[in,out] context 各步骤共用的生命周期上下文，可空与否由各回调决定。
 * @param[in] timeout_ms 传给每个步骤的同一毫秒值，总耗时由各步骤与调度共同决定。
 * @param[out] out_failed_stage 可选失败阶段，先置 NONE，只有步骤实际失败才改为其编号。
 * @return 全部成功为 ESP_OK，操作表不完整为 ESP_ERR_INVALID_ARG，其余为首个步骤错误。
 * @note 外层根据已记录状态决定重试或重启。失败前的步骤可能已经关闭入口，
 *       后续对象保持存活，接纳能力由其各自状态决定。
 */
esp_err_t app_shutdown_sequence_run(const app_shutdown_ops_t *ops, void *context,
    uint32_t timeout_ms, app_shutdown_stage_t *out_failed_stage);
