#include "command_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_profile.h"
#include "app_event_bus.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

/**
 * @brief 跳过前导 ASCII 空白并原地终止尾部空白。
 * @param[in,out] text 可为 NULL 的可写 NUL 文本，识别空格、制表、回车和换行。
 * @return 原缓冲内首个非空白位置，NULL 输入返回 NULL，调用者继续持有原分配地址。
 */
static char *trim_ascii(char *text)
{
    if (!text) return NULL;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return text;
}

/**
 * @brief 同步向产品事件入口发布 INFO 状态文本。
 * @param[in] source 事件来源枚举。
 * @param[in] text 本次调用期间借用的 NUL 文本。
 * @post 事件入口按自身接纳规则复制内容，调用者继续持有原文。
 */
static void publish_status(app_event_source_t source, const char *text)
{
    app_event_publish(APP_EVENT_KIND_STATUS, source, CC_LOG_INFO, text);
}

/**
 * @brief 发布通用堆最低余量、内部堆和 PSRAM 可用量及最大连续块。
 * @param[in] source 接收本次 INFO 状态事件的来源。
 * @post 各统计为原生读取瞬间的字节数，读取之间其他任务仍可分配或释放。
 */
static void handle_heap_command(app_event_source_t source)
{
    app_event_publishf(APP_EVENT_KIND_STATUS,
                       source,
                       CC_LOG_INFO,
                       "free_heap=%" PRIu32 " min_free_heap=%" PRIu32 " internal_free=%" PRIu32
                       " psram_free=%" PRIu32 " psram_largest=%" PRIu32,
                       (uint32_t)esp_get_free_heap_size(),
                       (uint32_t)esp_get_minimum_free_heap_size(),
                       (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                       (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/**
 * @brief 发布采样器当前启用状态和级别。
 * @param[in] source 本次 INFO 状态事件来源。
 * @post 两个查询各自取得瞬时值，函数不启动或停止采样。
 */
static void handle_profile_status(app_event_source_t source)
{
    app_event_publishf(APP_EVENT_KIND_STATUS,
                       source,
                       CC_LOG_INFO,
                       "profile enabled=%s level=%s",
                       app_profile_enabled() ? "on" : "off",
                       app_profile_level_string(app_profile_level()));
}

/**
 * @brief 执行本地状态、采样、调试和思考命令，并将清屏交给 owner。
 * @param[in] ctx 可为 NULL 的调用期借用上下文，缺省来源为 SYSTEM，runtime 可为 NULL。
 * @param[in,out] text 可为 NULL 的可写 NUL 文本，interval 参数会原地去除尾部空白。
 * @return 已发布结果为 HANDLED，/clear 为 CLEAR，未知斜杠命令及普通文本为 NOT_HANDLED。
 * @pre owner 保证 runtime 在本次调用结束前存活，并协调其代际租约。
 * @note 手动 Lua 由交互层携操作账本执行，直接交给本解析器时返回已处理错误提示。
 */
command_service_result_t command_service_handle(const command_service_context_t *ctx, char *text)
{
    app_event_source_t source = ctx ? ctx->source : APP_EVENT_SOURCE_SYSTEM;
    cc_agent_runtime_t *runtime = ctx ? ctx->runtime : NULL;
    if (!text || text[0] != '/') return COMMAND_SERVICE_NOT_HANDLED;

    if (strcmp(text, "/help") == 0) {
        publish_status(source,
                       "Commands: /help /heap /clear /lua <code> /luafile <path> "
                       "/thinking on|off /debug on|off /stream on|off "
                       "/profile on|off|sample|level basic|trace|interval <ms>");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/heap") == 0) {
        handle_heap_command(source);
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/clear") == 0) {
        return COMMAND_SERVICE_CLEAR;
    }
    if (strcmp(text, "/debug on") == 0) {
        app_event_bus_set_serial_debug(true);
        publish_status(source, "serial debug on");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/debug off") == 0) {
        app_event_bus_set_serial_debug(false);
        publish_status(source, "serial debug off");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/debug") == 0) {
        app_event_publishf(APP_EVENT_KIND_STATUS,
                           source,
                           CC_LOG_INFO,
                           "serial_debug=%s",
                           app_event_bus_serial_debug_enabled() ? "on" : "off");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/profile") == 0) {
        handle_profile_status(source);
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/profile on") == 0) {
        app_profile_set_enabled(true);
        publish_status(source, "profile on");
        app_profile_sample("manual", "profile_on", true);
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/profile off") == 0) {
        app_profile_sample("manual", "profile_off", true);
        app_profile_set_enabled(false);
        publish_status(source, "profile off");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/profile sample") == 0) {
        app_profile_sample("manual", "sample", true);
        publish_status(source, "profile sample emitted");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/profile level basic") == 0) {
        app_profile_set_level(APP_PROFILE_LEVEL_BASIC);
        publish_status(source, "profile level basic");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/profile level trace") == 0) {
        app_profile_set_level(APP_PROFILE_LEVEL_TRACE);
        publish_status(source, "profile level trace");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strncmp(text, "/profile interval ", 18) == 0) {
        char *value_text = trim_ascii(text + 18);
        char *end = NULL;
        long interval_ms = strtol(value_text ? value_text : "", &end, 10);
        end = trim_ascii(end);
        if (!value_text || value_text[0] == '\0' || (end && end[0] != '\0') || interval_ms < 0) {
            publish_status(source, "usage: /profile interval <ms>");
            return COMMAND_SERVICE_HANDLED;
        }
        if (interval_ms > INT32_MAX) {
            interval_ms = INT32_MAX;
        }
        app_profile_set_interval_ms((int)interval_ms);
        app_event_publishf(APP_EVENT_KIND_STATUS,
                           source,
                           CC_LOG_INFO,
                           "profile interval %ld ms",
                           interval_ms);
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/thinking on") == 0) {
        if (!runtime) {
            app_event_publish(APP_EVENT_KIND_ERROR, source, CC_LOG_WARN, "runtime unavailable");
            return COMMAND_SERVICE_HANDLED;
        }
        cc_agent_runtime_set_thinking_mode(runtime, 1);
        publish_status(source, "thinking on");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/thinking off") == 0) {
        if (!runtime) {
            app_event_publish(APP_EVENT_KIND_ERROR, source, CC_LOG_WARN, "runtime unavailable");
            return COMMAND_SERVICE_HANDLED;
        }
        cc_agent_runtime_set_thinking_mode(runtime, 0);
        publish_status(source, "thinking off");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/lua") == 0 || strcmp(text, "/luafile") == 0 ||
        strncmp(text, "/lua ", 5) == 0 || strncmp(text, "/luafile ", 9) == 0) {
        app_event_publish(APP_EVENT_KIND_ERROR, source, CC_LOG_WARN,
                          "手动 Lua 需要通过交互服务核对当前 U 盘操作账本。");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/stream on") == 0 || strcmp(text, "/stream off") == 0) {
        publish_status(source, "stream follows route config");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/exit") == 0) {
        publish_status(source, "console remains active");
        return COMMAND_SERVICE_HANDLED;
    }
    return COMMAND_SERVICE_NOT_HANDLED;
}
