#include "app_context.h"

#include <string.h>

/**
 * @brief 将应用组合根的配置、状态和资源引用初始化为空值。
 * @param[out] ctx 调用者提供的上下文存储，NULL 时直接返回。
 * @pre 非空上下文尚未持有资源，或其上次生命周期已经完成清理。
 * @post 非空上下文全部清零，由 app_lifecycle_run 后续装配配置与服务。
 * @note 此函数直接覆盖结构体。调用者负责与读写该上下文的任务互斥。
 */
void app_context_init(app_context_t *ctx)
{
    if (ctx) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

/**
 * @brief 释放应用上下文拥有的配置，并清空状态及借用的资源引用。
 * @param[in,out] ctx 已初始化的上下文，NULL 时直接返回。
 * @pre 配置的所有借用者已经退出，调用者独占上下文。runtime_service 等 owner
 *      已完成 builder 和 USB 队列等外部资源的停止、释放或必要交接。
 * @post 配置动态成员经 app_config_destroy 释放并清理密钥，非空上下文全部清零。
 * @note builder 与 usb_events 的销毁责任分别由它们的 owner 承担。
 */
void app_context_destroy(app_context_t *ctx)
{
    if (!ctx) {
        return;
    }
    app_config_destroy(&ctx->config);
    memset(ctx, 0, sizeof(*ctx));
}
