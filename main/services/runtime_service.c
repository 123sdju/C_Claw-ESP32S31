#include "runtime_service.h"

#include <stdio.h>
#include <stdlib.h>

#include "app_profile.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/core/cc_message.h"
#include "cc/core/cc_version.h"
#include "cclaw_runtime_features.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "runtime_service";

static cc_runtime_builder_t *s_runtime_builder;

/*
 * 执行运行时自检：创建一条测试消息、获取消息摘要文本、打印 SDK 版本。
 * 用于验证 cc_message API 和内存链路在启动后是否正常。
 * 参数: config - 运行时配置（用于日志输出 provider/model 信息）
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

/*
 * 根据 cc_config_t 创建并启动 Agent 运行时。
 * 过程：创建 cc_runtime_builder → 订阅 profile；运行期访问通过 generation 租约完成。
 * 如果 builder 已存在（上次启动未释放），则复用组合根并取得当前 generation。
 * 参数: config - 运行时配置（provider、model、thinking_mode 等）
 * 返回: true 成功，false 失败（记录错误日志）
 */
bool runtime_service_start(const cc_config_t *config)
{
    if (!config) {
        ESP_LOGW(TAG, "Runtime needs config first");
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

        app_profile_subscribe_runtime(s_runtime_builder);
    }

    runtime_service_set_thinking(config->thinking_mode != 0);
    return true;
}

cc_result_t runtime_service_stop(uint32_t timeout_ms)
{
    cc_runtime_builder_t *builder = s_runtime_builder;
    if (!builder) return cc_result_ok();
    uint64_t started_ms = (uint64_t)esp_timer_get_time() / 1000U;
    /* Stop the sampler first so it cannot acquire a fresh generation while
     * the runtime is entering DRAINING. */
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

/*
 * 返回全局 runtime builder 组合根。
 * 返回: builder 指针，未启动时返回 NULL；调用方不得销毁或长期缓存其内部裸 runtime。
 */
cc_runtime_builder_t *runtime_service_builder(void)
{
    return s_runtime_builder;
}

cc_result_t runtime_service_reset_http_connections(void)
{
    cc_runtime_builder_t *builder = runtime_service_builder();
    if (!builder) return cc_result_error(CC_ERR_INVALID_STATE, "Runtime service is not started");
    return cc_runtime_builder_reset_http_connections(builder);
}

/*
 * 动态设置 Agent 运行时的 thinking_mode 开关。
 * 仅在 builder 已启动且能取得当前 generation 时生效。
 * 参数: thinking_mode - true 开启思考模式，false 关闭
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
