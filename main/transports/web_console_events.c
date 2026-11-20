#include "web_console_internal.h"

#include <stdlib.h>
#include <string.h>

#include "app_event_bus.h"
#include "app_profile.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define WEB_PROFILE_SLOW_US 200000ULL

/**
 * @brief 为启用的请求 span 记录失败或至少二百毫秒的慢调用。
 * @param[in,out] span 借用 span，NULL 或未启用时跳过。
 * @param[in] status 借用状态，非 NULL 且非 ok 时原样结束，否则只记录慢调用。
 * @post 满足条件时调用 profile 结束入口，普通快速成功保持无记录，无资源移交。
 */
static void web_profile_span_end_if_slow(app_profile_span_t *span, const char *status)
{
    if (!span || !span->enabled) {
        return;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (status && strcmp(status, "ok") != 0) {
        app_profile_span_end(span, status);
        return;
    }
    if (now_us >= span->start_us && now_us - span->start_us >= WEB_PROFILE_SLOW_US) {
        app_profile_span_end(span, "slow");
    }
}

/**
 * @brief 请求业务状态快照，发送后释放回调交付的拥有正文。
 * @param[in,out] req HTTPD 借用请求，寿命覆盖同步回调和响应发送。
 * @return 响应发送结果，缺回调或 NULL 正文形成 HTTP 500，HTTP 错误响应也可能发送成功。
 * @note 回调和 user_data 由 Web 生命周期保活，正文按 free 清理，慢成功或发送失败记录 profile。
 */
esp_err_t web_console_status_handler(httpd_req_t *req)
{
    app_profile_span_t span = app_profile_span_begin("web", "api_status", NULL, NULL);
    if (!g_web_console_context.status_json) {
        app_profile_span_end(&span, "unavailable");
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status unavailable");
    }

    char *json = g_web_console_context.status_json(g_web_console_context.user_data);
    if (!json) {
        app_profile_span_end(&span, "oom");
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    esp_err_t err = web_console_send_text(req, json, "application/json");
    free(json);
    web_profile_span_end_if_slow(&span, err == ESP_OK ? "ok" : "send_error");
    return err;
}

/**
 * @brief 解析事件查询参数，缺省条数从当前原子热限制读取，再请求业务事件快照。
 * @param[in,out] req HTTPD 借用请求，查询支持 since、limit 和 kind。
 * @return 响应发送结果，回调缺失或 NULL 正文形成 HTTP 500。
 * @note 查询值副本以 heap_caps_free 清理，回调正文以 free 清理。显式数值沿现有 strtoul/atoi 宽松解析，范围由事件 owner 处理。
 */
esp_err_t web_console_events_handler(httpd_req_t *req)
{
    app_profile_span_t span = app_profile_span_begin("web", "api_events", NULL, NULL);
    if (!g_web_console_context.events_json) {
        app_profile_span_end(&span, "unavailable");
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "events unavailable");
    }

    char *since_text = web_console_query_value(req, "since");
    char *limit_text = web_console_query_value(req, "limit");
    char *kind_text = web_console_query_value(req, "kind");
    uint32_t since = since_text ? (uint32_t)strtoul(since_text, NULL, 10) : 0;
    int limit = limit_text ? atoi(limit_text) :
        web_console_event_limit();
    uint32_t kind_mask = kind_text ? (uint32_t)strtoul(kind_text, NULL, 0) : APP_EVENT_KIND_MASK_ALL;
    heap_caps_free(since_text);
    heap_caps_free(limit_text);
    heap_caps_free(kind_text);

    char *json = g_web_console_context.events_json(since,
                                                   limit,
                                                   kind_mask,
                                                   g_web_console_context.user_data);
    if (!json) {
        app_profile_span_end(&span, "oom");
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    esp_err_t err = web_console_send_text(req, json, "application/json");
    free(json);
    web_profile_span_end_if_slow(&span, err == ESP_OK ? "ok" : "send_error");
    return err;
}

/**
 * @brief 用本请求开始时的原子字节上限检查消息，接收完整正文后同步提交。
 * @param[in,out] req 必需 HTTPD 借用请求，content_len 为正且不超过当前限制才接收。
 * @return 响应发送结果，提交成功发送 queued 正文，超限或业务拒绝为 HTTP 400，接收失败/OOM 为 500，总期限为 408。
 * @note PSRAM 正文追加 NUL 后仅在回调期间借用，所有路径以 heap_caps_free 清理；超时 recv 可重试至六十秒总期限。
 */
esp_err_t web_console_input_handler(httpd_req_t *req)
{
    app_profile_span_t span = app_profile_span_begin("web", "api_messages", NULL, NULL);
    if (!g_web_console_context.message_submitted) {
        app_profile_span_end(&span, "unavailable");
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "message unavailable");
    }

    int max_input_bytes = web_console_max_input_bytes();
    if (req->content_len <= 0 || req->content_len > max_input_bytes) {
        app_profile_span_end(&span, "bad_size");
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "bad message size");
    }

    char *buf = web_console_psram_alloc((size_t)req->content_len + 1);
    if (!buf) {
        app_profile_span_end(&span, "oom");
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }

    int remaining = req->content_len;
    char *write = buf;
    int64_t deadline = esp_timer_get_time() + WEB_REQUEST_TIMEOUT_US;
    while (remaining > 0) {
        if (esp_timer_get_time() >= deadline) {
            heap_caps_free(buf);
            app_profile_span_end(&span, "recv_timeout");
            return web_console_send_error(req, HTTPD_408_REQ_TIMEOUT, "message upload deadline exceeded");
        }
        int n = httpd_req_recv(req, write, remaining);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            heap_caps_free(buf);
            app_profile_span_end(&span, "recv_failed");
            return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        write += n;
        remaining -= n;
    }
    buf[req->content_len] = '\0';

    esp_err_t err = g_web_console_context.message_submitted(buf, g_web_console_context.user_data);
    heap_caps_free(buf);
    app_profile_span_end(&span, err == ESP_OK ? "ok" : "submit_error");
    return err == ESP_OK ?
        web_console_send_text(req, "{\"ok\":true,\"queued\":true}", "application/json") :
        web_console_send_error(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
}
