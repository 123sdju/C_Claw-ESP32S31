#include "runtime_service.h"

#include <stdio.h>
#include <stdlib.h>

#include "app_profile.h"
#include "app_event_bus.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_version.h"
#include "cclaw_runtime_features.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "runtime_service";

static cc_runtime_builder_t *s_runtime_builder;

/**
 * @brief 在启动任务中创建并销毁本地文本消息，检查版本和消息摘要链路。
 * @param[in] config 调用期间借用的配置，允许 NULL，缺失 provider/model 使用 none。
 * @post 成功时记录版本和最多 159 字节的演示正文摘要，临时消息、摘要和结果消息均释放。
 *       创建或摘要失败时记录错误并提前返回。
 * @note 自检只使用本地消息 API，远端连接能力由后续运行路径验证。
 */
void runtime_service_run_self_test(const cc_config_t *config)
{
    cc_message_t *message = NULL;
    char *summary = NULL;
    char text[160];

    snprintf(text,
             sizeof(text),
             "ESP32S31 loaded C-Claw config for provider=%s model=%s.",
             config && config->provider ? config->provider : "none",
             config && config->model ? config->model : "none");

    ESP_LOGI(TAG, "C-Claw SDK runtime version: %s", cc_claw_version_string());

    cc_result_t result = cc_message_create_text(
        "boot-msg-1",
        "esp32s31-boot",
        CC_ROLE_USER,
        text,
        NULL,
        &message);

    if (result.code != CC_OK) {
        ESP_LOGE(TAG, "cc_message_create_text failed: %s", cc_error_string(result.code));
        cc_result_free(&result);
        return;
    }
    cc_result_free(&result);

    result = cc_message_get_text_summary(message, &summary);
    if (result.code != CC_OK) {
        ESP_LOGE(TAG, "cc_message_get_text_summary failed: %s", cc_error_string(result.code));
        cc_result_free(&result);
        cc_message_destroy(message);
        return;
    }

    ESP_LOGI(TAG, "SDK message summary: %s", summary);

    free(summary);
    cc_result_free(&result);
    cc_message_destroy(message);
}

/**
 * @brief 创建应用持有的 builder，或复用已有实例，再应用 thinking 设置。
 * @param[in] config 必填调用期配置，创建过程由各工厂复制所需长期内容。
 * @param[in] require_recovery 为 true 时要求全新 builder，并在暴露给其他入口前关闭工具派发。
 * @return 创建或复用成功返回 true，参数、装配或恢复门禁设置失败返回 false 并记录错误。
 * @pre 生命周期 owner 串行协调创建、激活和停止，覆盖全局 builder 引用的交接。
 * @note 已有 builder 的普通 start 只更新 thinking。恢复模式失败后若清理也失败，
 *       全局引用保留给后续 stop 重试，调用者应按返回值继续失败收尾。
 *       新实例诊断发到应用事件总线，普通模式订阅 profile，恢复模式由激活入口订阅。
 */
static bool runtime_service_start_internal(const cc_config_t *config, bool require_recovery)
{
    if (!config) {
        ESP_LOGW(TAG, "Runtime needs config first");
        return false;
    }

    if (require_recovery && s_runtime_builder) {
        ESP_LOGE(TAG, "Recovery preparation requires a new runtime builder");
        return false;
    }
    if (!s_runtime_builder) {
        cc_result_t rc = cc_runtime_builder_create(
            config,
            cclaw_runtime_features(),
            &s_runtime_builder);
        if (rc.code != CC_OK) {
            ESP_LOGE(TAG,
                     "runtime builder failed: %s",
                     rc.message ? rc.message : cc_error_string(rc.code));
            cc_result_free(&rc);
            return false;
        }
        cc_result_free(&rc);

        if (require_recovery) {
            rc = cc_runtime_builder_require_recovery(s_runtime_builder);
            if (rc.code != CC_OK) {
                ESP_LOGE(TAG, "Cannot require recovery: %s",
                         rc.message ? rc.message : cc_error_string(rc.code));
                cc_result_free(&rc);
                rc = cc_runtime_builder_destroy(s_runtime_builder, 5000U);
                if (rc.code == CC_OK) s_runtime_builder = NULL;
                cc_result_free(&rc);
                return false;
            }
            cc_result_free(&rc);
        }

        const cc_runtime_diagnostics_t *diagnostics = cc_runtime_builder_diagnostics(s_runtime_builder);
        for (size_t i = 0; diagnostics && i < diagnostics->count; i++) {
            const cc_runtime_diagnostic_t *item = &diagnostics->items[i];
            ESP_LOGW(TAG, "%s unavailable (%s): %s", item->kind, item->id, item->message);
            app_event_publishf(APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_RUNTIME, CC_LOG_WARN,
                               "%s unavailable (%s): %s", item->kind, item->id, item->message);
        }

        (void)app_profile_set_cache_provider(config->provider ? config->provider : "");
        if (!require_recovery) app_profile_subscribe_runtime(s_runtime_builder);
    }

    runtime_service_set_thinking(config->thinking_mode != 0);
    return true;
}

/**
 * @brief 按普通模式创建或复用 builder，并应用 thinking 配置。
 * @param[in] config 必填调用期配置，新实例按该配置装配。
 * @return 创建或复用成功为 true，失败为 false，详细原因记录到日志。
 * @pre 生命周期 owner 排除并发创建、停止和全局引用替换。
 */
bool runtime_service_start(const cc_config_t *config)
{
    return runtime_service_start_internal(config, false);
}

/**
 * @brief 创建工具派发关闭的新 builder，供启动阶段附加恢复组件。
 * @param[in] config 必填调用期配置。
 * @return 准备成功为 true，已有实例或创建/设置失败为 false。
 * @pre 生命周期 owner 独占装配交接，随后附加 recovery bundle 再激活观测与输入。
 * @note 失败清理未完成时保留全局引用，由调用者继续 stop。
 */
bool runtime_service_prepare_recovery(const cc_config_t *config)
{
    return runtime_service_start_internal(config, true);
}

/**
 * @brief 将已装配 builder 的运行事件订阅交给应用 profile。
 * @pre 生命周期 owner 已完成恢复组件附加，并排除并发停止。
 * @post builder 存在时调用 profile 订阅入口，未创建时直接返回。
 * @note profile 负责自己的订阅状态与资源寿命，本函数没有独立错误输出。
 */
void runtime_service_activate_observability(void)
{
    if (s_runtime_builder) app_profile_subscribe_runtime(s_runtime_builder);
}

/**
 * @brief 停止 profile 使用者、排空运行事件订阅，再销毁应用拥有的 builder。
 * @param[in] timeout_ms 从本次调用起算的毫秒预算，按已用单调时间向后续阶段传递剩余值。
 * @return 无实例或销毁完成返回 CC_OK，采样、退订或 builder 排空失败返回对应拥有结果。
 * @pre owner 已关闭其他入口并协调其退出，串行调用 stop 并保管 builder 到本次返回。
 * @note 首个失败停止推进，全局引用保留供重试，成功后清空。锁等待与底层 join 的
 *       实际退出语义继续由相应端口和 builder 契约约束。
 */
cc_result_t runtime_service_stop(uint32_t timeout_ms)
{
    cc_runtime_builder_t *builder = s_runtime_builder;
    if (!builder) return cc_result_ok();
    uint64_t started_ms = (uint64_t)esp_timer_get_time() / 1000U;
    /* 先停止采样任务，使其在运行时进入排空后结束新 generation 的获取。 */
    esp_err_t profile_stop_err = app_profile_stop(timeout_ms);
    if (profile_stop_err != ESP_OK) {
        return cc_result_error(profile_stop_err == ESP_ERR_TIMEOUT ?
                               CC_ERR_TIMEOUT : CC_ERR_UNKNOWN,
                               "Profile sampler did not stop");
    }
    uint64_t elapsed_ms = (uint64_t)esp_timer_get_time() / 1000U - started_ms;
    uint32_t remaining_ms = elapsed_ms >= timeout_ms ? 0U :
        timeout_ms - (uint32_t)elapsed_ms;
    esp_err_t unsubscribe_err = app_profile_unsubscribe_runtime(remaining_ms);
    if (unsubscribe_err != ESP_OK) {
        return cc_result_error(unsubscribe_err == ESP_ERR_TIMEOUT ?
                               CC_ERR_TIMEOUT : CC_ERR_UNKNOWN,
                               "Profile runtime subscription did not drain");
    }
    elapsed_ms = (uint64_t)esp_timer_get_time() / 1000U - started_ms;
    remaining_ms = elapsed_ms >= timeout_ms ? 0U : timeout_ms - (uint32_t)elapsed_ms;
    cc_result_t rc = cc_runtime_builder_destroy(builder, remaining_ms);
    if (rc.code == CC_OK) s_runtime_builder = NULL;
    return rc;
}

/**
 * @brief 借出当前由应用持有的 builder 组合根。
 * @return 返回借用 builder，未创建或已经停止为 NULL。
 * @pre 调用方通过生命周期交接保证全局引用读取及后续使用期间对象有效。
 * @note runtime/provider 等代内资源通过 generation 租约取得，builder 由本服务 stop 清理。
 */
cc_runtime_builder_t *runtime_service_builder(void)
{
    return s_runtime_builder;
}

/**
 * @brief 将网络 owner 的连接池重置请求交给当前 builder 的 HTTP 端口。
 * @return 未创建为 CC_ERR_INVALID_STATE，其他原样返回端口结果，消息由调用者释放。
 * @pre 网络 owner 协调在途 HTTP 操作，生命周期保证 builder 覆盖本次调用。
 */
cc_result_t runtime_service_reset_http_connections(void)
{
    cc_runtime_builder_t *builder = runtime_service_builder();
    if (!builder) return cc_result_error(CC_ERR_INVALID_STATE, "Runtime service is not started");
    return cc_runtime_builder_reset_http_connections(builder);
}

/**
 * @brief 在当前 generation 的租约内设置运行时 thinking 标志。
 * @param[in] thinking_mode true 开启，false 关闭。
 * @pre 生命周期保证全局 builder 引用和调用期对象有效，排除同时停止或替换该引用。
 * @post 当前代可获取时设置标志并归还租约，无实例或代际准入拒绝时直接结束。
 * @note generation 获取错误在此消费，调用者通过既有运行状态判断是否可操作。
 */
void runtime_service_set_thinking(bool thinking_mode)
{
    if (!s_runtime_builder) return;
    cc_runtime_generation_t *generation = NULL;
    cc_result_t rc = cc_runtime_builder_acquire_generation(s_runtime_builder, &generation);
    if (rc.code == CC_OK) {
        cc_agent_runtime_set_thinking_mode(
            cc_runtime_generation_runtime(generation), thinking_mode ? 1 : 0);
        cc_runtime_generation_release(generation);
    }
    cc_result_free(&rc);
}
