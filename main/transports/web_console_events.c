#include "web_console_internal.h"

#include <stdlib.h>
#include <string.h>

#include "app_event_bus.h"
#include "app_profile.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define WEB_PROFILE_SLOW_US 200000ULL

/*
 * 性能剖析辅助函数：若 span 启用且耗时超过 200ms（WEB_PROFILE_SLOW_US）
 * 或状态不是 "ok"，则结束 span 并标记相应状态。用于检测慢速 API 调用。
 * 参数: span   - 性能剖析 span 对象
 *       status - 操作状态（"ok" 或错误原因）
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

/*
 * 处理 GET /api/status 请求。调用上下文 status_json 回调获取设备运行时状态 JSON，
 * 以 application/json 类型返回。回调全部通过 free() 释放。
 * 参数: req - HTTP 请求句柄
 * 返回: ESP_OK 表示成功；若回调未设置或返回 NULL 则返回 500
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

/*
 * 处理 GET /api/events 请求。支持查询参数 since（起始序列号）、limit（最大条数，
 * 默认来自 io 配置或 120）和 kind（事件种类掩码，默认 APP_EVENT_KIND_MASK_ALL），
 * 调用 events_json 回调获取事件 JSON 并返回。
 * 参数: req - HTTP 请求句柄（含可选查询参数）
 * 返回: ESP_OK 表示成功；若回调未设置或返回 NULL 则返回 500
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
        (g_web_console_context.io ? g_web_console_context.io->web.event_limit : 120);
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

/*
 * 处理 POST /api/messages 请求。接收请求体文本，通过 message_submitted 回调
 * 提交到交互服务处理。请求体大小受 io->max_input_bytes 限制（默认 1024）。
 * 参数: req - HTTP 请求句柄（content_len 为消息文本长度）
 * 返回: ESP_OK 且提交成功返回 {"ok":true,"queued":true}；超限或失败返回错误
 */
esp_err_t web_console_input_handler(httpd_req_t *req)
{
    app_profile_span_t span = app_profile_span_begin("web", "api_messages", NULL, NULL);
    if (!g_web_console_context.message_submitted) {
        app_profile_span_end(&span, "unavailable");
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "message unavailable");
    }

    int max_input_bytes = g_web_console_context.io ?
        g_web_console_context.io->max_input_bytes : 1024;
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
    while (remaining > 0) {
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
