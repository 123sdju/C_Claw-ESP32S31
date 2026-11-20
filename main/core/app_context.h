#pragma once

#include "app_config.h"
#include "app_status.h"
#include "cc/app/cc_runtime_builder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* 应用组合根拥有配置和状态值，借用 builder 与 USB 事件队列句柄。 */
typedef struct {
    app_config_t config;              /* 从 config.json 加载的应用配置 */
    app_status_t status;              /* 周期性采集的运行时状态快照 */
    cc_runtime_builder_t *runtime_builder;
    QueueHandle_t usb_events;         /* USB 热插拔事件队列 */
} app_context_t;

/**
 * @brief 清零应用组合根，供启动编排逐步装配。
 * @param[out] ctx 尚未持有资源或已完成清理的上下文，允许 NULL。
 * @pre 调用者独占上下文并已清理上次生命周期的资源。
 * @post 非空上下文的配置、状态和资源引用全部归零。
 */
void app_context_init(app_context_t *ctx);
/**
 * @brief 释放拥有的配置，再清空应用上下文。
 * @param[in,out] ctx 已初始化且由调用者独占的上下文，允许 NULL。
 * @pre 配置借用者已经退出，外部 owner 已完成 builder、USB 队列的资源交接或释放。
 * @post 非空上下文清零，配置动态成员和密钥经 app_config_destroy 清理。
 * @note runtime_service 等 owner 分别负责借用句柄所指资源的生命周期。
 */
void app_context_destroy(app_context_t *ctx);
