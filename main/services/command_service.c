#include "command_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_profile.h"
#include "app_event_bus.h"
#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lua_runtime.h"

/*
 * 将 ASCII 字符串的前导和尾部空白字符（空格、制表符、回车、换行）去除。
 * 原地修改字符串，返回调整后的起始指针。
 * 参数: text - 输入字符串
 * 返回: 去除空白后的起始位置，NULL 输入返回 NULL
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

/*
 * 发布一条状态事件到事件总线（APP_EVENT_KIND_STATUS，日志级别 INFO）。
 * 参数: source - 事件来源
 * 参数: text   - 状态文本
 */
static void publish_status(app_event_source_t source, const char *text)
{
    app_event_publish(APP_EVENT_KIND_STATUS, source, CC_LOG_INFO, text);
}

/*
 * 处理 /heap 命令：通过 app_event_publishf 发布当前堆内存统计信息
 * （free_heap、min_free_heap、internal_free、psram_free、psram_largest）。
 * 参数: source - 事件来源
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

/*
 * 处理 /profile 命令（无子命令时）：发布当前 profile 的启用状态和采样级别。
 * 参数: source - 事件来源
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

/*
 * 处理 /lua <code> 命令：在沙箱中执行 Lua 代码字符串，发布执行结果或错误。
 * 参数: source - 事件来源
 * 参数: code   - Lua 源码字符串
 */
static void handle_lua_code(app_event_source_t source, const char *code)
{
    lua_runtime_limits_t limits = lua_runtime_default_limits();
    char *lua_output = NULL;
    cc_result_t rc = lua_runtime_run_code(code, &limits, &lua_output);
    if (rc.code == CC_OK) {
        app_event_publish(APP_EVENT_KIND_TOOL, source, CC_LOG_INFO, lua_output ? lua_output : "lua ok");
    } else {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          source,
                          CC_LOG_ERROR,
                          rc.message ? rc.message : cc_error_string(rc.code));
    }
    heap_caps_free(lua_output);
    cc_result_free(&rc);
}

/*
 * 处理 /luafile <path> 命令：从文件系统加载并执行 Lua 脚本。
 * 参数: source - 事件来源
 * 参数: path   - Lua 文件路径
 */
static void handle_lua_file(app_event_source_t source, const char *path)
{
    lua_runtime_limits_t limits = lua_runtime_default_limits();
    char *lua_output = NULL;
    cc_filesystem_t filesystem = {0};
    cc_result_t rc = cc_filesystem_get_default(&filesystem);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = lua_runtime_run_file(&filesystem, NULL, path, &limits, &lua_output);
    }
    if (rc.code == CC_OK) {
        app_event_publish(APP_EVENT_KIND_TOOL, source, CC_LOG_INFO, lua_output ? lua_output : "lua file ok");
    } else {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          source,
                          CC_LOG_ERROR,
                          rc.message ? rc.message : cc_error_string(rc.code));
    }
    heap_caps_free(lua_output);
    cc_result_free(&rc);
    if (filesystem.vtable && filesystem.vtable->destroy) {
        filesystem.vtable->destroy(filesystem.self);
    }
}

/*
 * 解析并执行命令行文本。支持以下命令前缀：
 * /help、/heap、/clear、/debug on|off|、/profile on|off|sample|level|interval、
 * /thinking on|off、/lua <code>、/luafile <path>、/stream on|off、/exit。
 * 参数: ctx  - 上下文（运行时指针、事件来源），可为 NULL（默认 source=SYSTEM）
 * 参数: text - 命令行文本（不以 '/' 开头直接返回 NOT_HANDLED）
 * 返回: COMMAND_SERVICE_HANDLED 已处理，COMMAND_SERVICE_CLEAR 需清屏，
 *       COMMAND_SERVICE_NOT_HANDLED 非命令文本
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
        if (runtime) cc_agent_runtime_set_thinking_mode(runtime, 1);
        publish_status(source, "thinking on");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/thinking off") == 0) {
        if (runtime) cc_agent_runtime_set_thinking_mode(runtime, 0);
        publish_status(source, "thinking off");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strncmp(text, "/lua ", 5) == 0) {
        handle_lua_code(source, text + 5);
        return COMMAND_SERVICE_HANDLED;
    }
    if (strncmp(text, "/luafile ", 9) == 0) {
        handle_lua_file(source, trim_ascii(text + 9));
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/stream on") == 0 || strcmp(text, "/stream off") == 0) {
        publish_status(source, "stream follows route config");
        return COMMAND_SERVICE_HANDLED;
    }
    if (strcmp(text, "/exit") == 0 || strcmp(text, "exit") == 0) {
        publish_status(source, "console remains active");
        return COMMAND_SERVICE_HANDLED;
    }
    return COMMAND_SERVICE_NOT_HANDLED;
}
