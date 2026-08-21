#pragma once

#include "app_event_bus.h"
#include "cc/app/cc_agent_runtime.h"

typedef enum {
    COMMAND_SERVICE_NOT_HANDLED = 0,
    COMMAND_SERVICE_HANDLED,
    COMMAND_SERVICE_CLEAR,
} command_service_result_t;

typedef struct {
    cc_agent_runtime_t *runtime;
    app_event_source_t source;
} command_service_context_t;

/*
 * 解析并执行命令行前缀（/help、/heap、/clear、/lua、/thinking、/profile 等），
 * 执行对应的操作并发布状态事件。
 * 参数: ctx  - 上下文（运行时指针、事件来源）
 * 参数: text - 命令行文本（以 '/' 开头才会处理）
 * 返回: COMMAND_SERVICE_HANDLED 已处理，COMMAND_SERVICE_CLEAR 需要清屏，
 *       COMMAND_SERVICE_NOT_HANDLED 非命令文本
 */
command_service_result_t command_service_handle(const command_service_context_t *ctx, char *text);
