#include "web_console_internal.h"

#include <stdlib.h>
#include <string.h>

#include "cc/util/cc_json.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

/**
 * @brief 同步请求 owner 的脱敏配置快照，发送后归还拥有正文。
 * @param[in,out] req 必填 HTTPD 请求借用，覆盖配置回调和响应发送期。
 * @return 正文或错误响应的原生发送结果，快照不可用形成 HTTP 500，缺回调时发送空对象。
 * @note 上下文由 Web 生命周期保活，回调自行同步配置快照，返回字符串须兼容 free。
 */
esp_err_t web_console_config_get_handler(httpd_req_t *req)
{
    char *json = g_web_console_context.config_json ?
        g_web_console_context.config_json(g_web_console_context.user_data) : strdup("{}");
    if (!json) {
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "config snapshot unavailable");
    }
    esp_err_t err = web_console_send_text(req, json, "application/json");
    free(json);
    return err;
}

/**
 * @brief 在六十秒接收期限内取得完整配置对象，再同步委托应用 owner 执行事务。
 * @param[in,out] req 必填 HTTPD 请求借用，content_len 须为一至 65536 字节，正文仅在事务回调期间借用。
 * @return 原生响应发送结果，大小/JSON/对象错误为 HTTP 400，超时为 408，接收或缓冲失败为 500。
 * 事务失败报告 HTTP 400 及阶段字段，成功发送阶段报告不额外承诺各动作的持久化状态。
 * @note 原始及解码 NUL、非法 Unicode 和尾随内容在回调前拒绝。解析树、PSRAM 正文及诊断在本次调用内释放。
 * 保存锁定、配置歧义、已安装但确认未知和旧对话保留均由 config_applied 的事务 owner 判断。
 */
esp_err_t web_console_config_put_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 64 * 1024) {
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "bad config size");
    }

    char *buf = web_console_psram_alloc((size_t)req->content_len + 1);
    if (!buf) {
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    int remaining = req->content_len;
    char *write = buf;
    int64_t deadline = esp_timer_get_time() + WEB_REQUEST_TIMEOUT_US;
    while (remaining > 0) {
        if (esp_timer_get_time() >= deadline) {
            heap_caps_free(buf);
            return web_console_send_error(req, HTTPD_408_REQ_TIMEOUT, "config upload deadline exceeded");
        }
        int n = httpd_req_recv(req, write, remaining);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0 || n > remaining) {
            heap_caps_free(buf);
            return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        write += n;
        remaining -= n;
    }
    buf[req->content_len] = '\0';

    if (memchr(buf, '\0', (size_t)req->content_len) || cc_storage_json_has_nul_escape(buf)) {
        heap_caps_free(buf);
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "config contains NUL");
    }

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse_strict(buf, &root);
    if (rc.code != CC_OK || !root || !cc_json_is_object(root)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s", rc.code == CC_OK ? "config must be a JSON object" :
            (rc.message ? rc.message : cc_error_string(rc.code)));
        cc_result_free(&rc);
        cc_json_destroy(root);
        heap_caps_free(buf);
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, msg);
    }
    cc_result_free(&rc);
    cc_json_destroy(root);

    app_config_apply_result_t result;
    memset(&result, 0, sizeof(result));
    esp_err_t apply_err = g_web_console_context.config_applied ?
        g_web_console_context.config_applied(buf, &result, g_web_console_context.user_data) :
        ESP_ERR_INVALID_STATE;
    heap_caps_free(buf);

    char response[768];
    int length = snprintf(response, sizeof(response),
        "{\"ok\":%s,\"parsed\":%s,\"event_bus\":%s,\"interaction\":%s,"
        "\"network\":%s,\"profile\":%s,\"font\":%s,\"runtime_controls\":%s,"
        "\"durable_commit\":%s,\"rollback_attempted\":%s,\"rollback_succeeded\":%s,"
        "\"restart_required\":%s,\"error\":%d,\"failed_subsystem\":\"%s\"}",
        apply_err == ESP_OK ? "true" : "false",
        result.parsed ? "true" : "false",
        result.event_bus_applied ? "true" : "false",
        result.interaction_applied ? "true" : "false",
        result.network_applied ? "true" : "false",
        result.profile_applied ? "true" : "false",
        result.font_applied ? "true" : "false",
        result.runtime_controls_applied ? "true" : "false",
        result.durable_committed ? "true" : "false",
        result.rollback_attempted ? "true" : "false",
        result.rollback_succeeded ? "true" : "false",
        result.restart_required ? "true" : "false",
        (int)(result.error != ESP_OK ? result.error : apply_err),
        result.failed_subsystem);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "config result serialization failed");
    }
    if (apply_err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
    }
    return web_console_send_text(req, response, "application/json");
}
