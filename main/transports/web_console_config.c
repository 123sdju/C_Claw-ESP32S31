#include "web_console_internal.h"

#include <stdlib.h>
#include <string.h>

#include "cc/util/cc_json.h"
#include "esp_heap_caps.h"

/*
 * 处理 GET /api/config 请求。通过全局上下文的 config_json 回调获取当前配置 JSON，
 * 以 application/json 类型返回。若回调未设置则返回空 JSON 对象 "{}"。
 * 参数: req - HTTP 请求句柄
 * 返回: ESP_OK 表示成功返回配置
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

/*
 * 处理 PUT /api/config 请求。接收请求体中的 JSON 配置（最大 64KB），
 * 先用 cc_json_parse 校验 JSON 格式合法性，再通过 config_applied 回调执行事务。
 * 参数: req - HTTP 请求句柄（content_len 指定 JSON 长度）
 * 返回: JSON 中包含每个子系统的应用结果和 restart_required；格式错误返回 400。
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
    while (remaining > 0) {
        int n = httpd_req_recv(req, write, remaining);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            heap_caps_free(buf);
            return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        write += n;
        remaining -= n;
    }
    buf[req->content_len] = '\0';

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(buf, &root);
    if (rc.code != CC_OK || !root || !cc_json_is_object(root)) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s", rc.message ? rc.message : cc_error_string(rc.code));
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
