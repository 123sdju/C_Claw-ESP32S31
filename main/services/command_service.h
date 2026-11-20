#pragma once

#include "app_event_bus.h"
#include "cc/app/cc_agent_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    COMMAND_SERVICE_NOT_HANDLED = 0,
    COMMAND_SERVICE_HANDLED,
    COMMAND_SERVICE_CLEAR,
} command_service_result_t;

typedef struct {
    cc_agent_runtime_t *runtime;
    app_event_source_t source;
} command_service_context_t;

/**
 * @brief 处理本地查询、调试、采样及思考开关，并把清屏交回交互 owner。
 * @param[in] ctx 可为 NULL 的调用期借用上下文，NULL 使用 SYSTEM 来源及空 runtime。
 * @param[in,out] text 可为 NULL 的可写 NUL 文本，interval 参数可能被原地去除空白。
 * @return HANDLED 表示已发布结果或错误，CLEAR 表示调用者执行清屏，其他文本为 NOT_HANDLED。
 * @pre 非 NULL runtime 由调用者持有代际租约或其他存活保证至函数返回。
 * @note 手动 Lua 需要交互服务提供操作账本，直接传入时发布错误提示并返回 HANDLED。
 */
command_service_result_t command_service_handle(const command_service_context_t *ctx, char *text);

#ifdef __cplusplus
}
#endif
