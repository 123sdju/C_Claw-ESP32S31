#pragma once

#include "app_config.h"
#include "app_status.h"
#include "cc/app/cc_runtime_builder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* 应用全局上下文，持有整个应用生命周期内的核心资源引用。 */
typedef struct {
    app_config_t config;              /* 从 config.json 加载的应用配置 */
    app_status_t status;              /* 周期性采集的运行时状态快照 */
    cc_runtime_builder_t *runtime_builder;
    QueueHandle_t usb_events;         /* USB 热插拔事件队列 */
} app_context_t;

/*
 * 初始化应用上下文，将所有字段清零。
 *
 * 参数:
 *   ctx - 指向待初始化的 app_context_t。
 */
void app_context_init(app_context_t *ctx);
/*
 * 销毁应用上下文，释放 config 中的动态内存并清零整个结构。
 *
 * 参数:
 *   ctx - 指向待销毁的 app_context_t。
 *
 * 注意：不释放 ctx->runtime_builder，其生命周期由 runtime_service 独立管理。
 */
void app_context_destroy(app_context_t *ctx);
