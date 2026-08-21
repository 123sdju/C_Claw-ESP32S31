#include "app_context.h"

#include <string.h>

/*
 * 初始化应用全局上下文结构体。
 *
 * 参数:
 *   ctx - 指向 app_context_t 的指针，将被零填充。
 *
 * 零初始化后，config、status、runtime_builder 和 usb_events 均为空/零值，
 * 后续由 app_lifecycle_run() 逐步填充。
 */
void app_context_init(app_context_t *ctx)
{
    if (ctx) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

/*
 * 销毁应用全局上下文，释放关联资源。
 *
 * 参数:
 *   ctx - 指向 app_context_t 的指针。
 *
 * 调用 app_config_destroy() 释放配置中的动态内存
 * （runtime 配置、IO 配置、profile 配置等），然后清零整个结构。
 * 注意：此函数不释放 ctx->runtime_builder，
 * Builder 及其 generations 由 runtime_service 独立管理生命周期。
 */
void app_context_destroy(app_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    app_config_destroy(&ctx->config);
    memset(ctx, 0, sizeof(*ctx));
}
