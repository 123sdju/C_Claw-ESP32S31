#include "app_event_bus_internal.h"

#include <string.h>

/*
 * 将日志级别字符串（"trace"/"debug"/"info"/"warn"/"error"/"fatal"）解析为枚举值。
 * 不区分大小写，无法识别时默认返回 CC_LOG_INFO。
 *
 * 参数: text - 日志级别字符串指针
 * 返回: 对应的 cc_log_level_t 枚举值
 */
cc_log_level_t app_event_parse_log_level(const char *text)
{
    if (!text) return CC_LOG_INFO;
    if (strcmp(text, "trace") == 0) return CC_LOG_TRACE;
    if (strcmp(text, "debug") == 0) return CC_LOG_DEBUG;
    if (strcmp(text, "warn") == 0) return CC_LOG_WARN;
    if (strcmp(text, "error") == 0) return CC_LOG_ERROR;
    if (strcmp(text, "fatal") == 0) return CC_LOG_FATAL;
    return CC_LOG_INFO;
}

/*
 * 将日志级别枚举值转为对应的字符串名称，用于日志格式化输出。
 *
 * 参数: level - cc_log_level_t 枚举值
 * 返回: 对应的常量字符串指针
 */
const char *app_event_log_level_string(cc_log_level_t level)
{
    switch (level) {
    case CC_LOG_TRACE: return "trace";
    case CC_LOG_DEBUG: return "debug";
    case CC_LOG_INFO: return "info";
    case CC_LOG_WARN: return "warn";
    case CC_LOG_ERROR: return "error";
    case CC_LOG_FATAL: return "fatal";
    default: return "info";
    }
}

/*
 * 将配置中的字符串列表转换为 sink 掩码，支持 "serial"/"web"/"display"/"usb_log"
 * 四种 sink 标识。静态函数，仅供本地路由配置使用。
 *
 * 参数: list - 配置字符串列表指针
 * 返回: 组合后的 sink 掩码
 */
static app_event_sink_mask_t sink_mask_from_list(const cc_config_string_list_t *list)
{
    app_event_sink_mask_t mask = 0;
    if (!list) return mask;
    for (size_t i = 0; i < list->count; i++) {
        const char *sink = list->items[i];
        if (!sink) continue;
        if (strcmp(sink, "serial") == 0) mask |= APP_EVENT_SINK_SERIAL;
        else if (strcmp(sink, "web") == 0) mask |= APP_EVENT_SINK_WEB;
        else if (strcmp(sink, "display") == 0) mask |= APP_EVENT_SINK_DISPLAY;
        else if (strcmp(sink, "usb_log") == 0) mask |= APP_EVENT_SINK_USB_LOG;
    }
    return mask;
}

/*
 * 在持锁状态下查询指定事件种类的路由掩码，带越界保护。
 *
 * 参数: kind - 事件种类枚举值
 * 返回: 对应的 sink 掩码，越界时返回 0
 */
app_event_sink_mask_t app_event_route_for_kind_locked(app_event_kind_t kind)
{
    if ((unsigned)kind >= (sizeof(g_app_event_bus.route) / sizeof(g_app_event_bus.route[0]))) {
        return 0;
    }
    return g_app_event_bus.route[kind];
}

/*
 * 在持锁状态下根据 IO 配置填充所有事件种类的路由表。
 * chat 路由同时应用于 USER 和 ASSISTANT，status 路由同时应用于 STATUS 和 RUN_STATE。
 *
 * 参数: io - 运行时IO配置指针
 * 返回: 无
 */
void app_event_configure_routes_locked(const app_runtime_io_config_t *io)
{
    g_app_event_bus.route[APP_EVENT_KIND_LOG] = sink_mask_from_list(&io->routes.log);
    g_app_event_bus.route[APP_EVENT_KIND_USER] = sink_mask_from_list(&io->routes.chat);
    g_app_event_bus.route[APP_EVENT_KIND_ASSISTANT] = sink_mask_from_list(&io->routes.chat);
    g_app_event_bus.route[APP_EVENT_KIND_THINKING] = sink_mask_from_list(&io->routes.thinking);
    g_app_event_bus.route[APP_EVENT_KIND_TOOL] = sink_mask_from_list(&io->routes.tool);
    g_app_event_bus.route[APP_EVENT_KIND_STATUS] = sink_mask_from_list(&io->routes.status);
    g_app_event_bus.route[APP_EVENT_KIND_ERROR] = sink_mask_from_list(&io->routes.error);
    g_app_event_bus.route[APP_EVENT_KIND_RUN_STATE] = sink_mask_from_list(&io->routes.status);
    g_app_event_bus.route[APP_EVENT_KIND_PROFILE] = sink_mask_from_list(&io->routes.profile);
}

/*
 * 在持锁状态下设置串口回退路由：将除 THINKING 外的所有事件种类路由到串口，
 * THINKING 路由设为 0（不输出）。在 IO 配置不可用时调用。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_configure_fallback_routes_locked(void)
{
    g_app_event_bus.route[APP_EVENT_KIND_LOG] = APP_EVENT_SINK_SERIAL;
    g_app_event_bus.route[APP_EVENT_KIND_USER] = APP_EVENT_SINK_SERIAL;
    g_app_event_bus.route[APP_EVENT_KIND_ASSISTANT] = APP_EVENT_SINK_SERIAL;
    g_app_event_bus.route[APP_EVENT_KIND_THINKING] = 0;
    g_app_event_bus.route[APP_EVENT_KIND_TOOL] = APP_EVENT_SINK_SERIAL;
    g_app_event_bus.route[APP_EVENT_KIND_STATUS] = APP_EVENT_SINK_SERIAL;
    g_app_event_bus.route[APP_EVENT_KIND_ERROR] = APP_EVENT_SINK_SERIAL;
    g_app_event_bus.route[APP_EVENT_KIND_RUN_STATE] = APP_EVENT_SINK_SERIAL;
    g_app_event_bus.route[APP_EVENT_KIND_PROFILE] = APP_EVENT_SINK_SERIAL;
}

/*
 * 将 sink 掩码转换为逗号分隔的可读字符串（如 "serial,web,display"），
 * 写入 out 缓冲区，末尾逗号自动去除。
 *
 * 参数: mask     - sink 掩码
 * 参数: out      - 输出缓冲区
 * 参数: out_size - 输出缓冲区大小
 * 返回: 无
 */
void app_event_sink_mask_string(app_event_sink_mask_t mask, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (mask & APP_EVENT_SINK_SERIAL) app_event_append_text(out, out_size, "serial,");
    if (mask & APP_EVENT_SINK_WEB) app_event_append_text(out, out_size, "web,");
    if (mask & APP_EVENT_SINK_DISPLAY) app_event_append_text(out, out_size, "display,");
    if (mask & APP_EVENT_SINK_USB_LOG) app_event_append_text(out, out_size, "usb_log,");
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] == ',') out[len - 1] = '\0';
}

/*
 * 判断事件是否应路由到 USB 日志文件，需同时满足：USB日志已启用、
 * 路由包含 USB_LOG 位、日志级别不低于 USB 日志最低级别。
 *
 * 参数: kind  - 事件种类
 * 参数: level - 日志级别
 * 参数: route - 该事件的路由掩码
 * 返回: true 表示应写入 USB 日志文件
 */
bool app_event_route_to_usb_log(app_event_kind_t kind, cc_log_level_t level, app_event_sink_mask_t route)
{
    return g_app_event_bus.usb_log_enabled &&
           (route & APP_EVENT_SINK_USB_LOG) &&
           level >= g_app_event_bus.usb_log_level;
}

/*
 * 判断事件是否应路由到串口输出，需同时满足：串口已启用（或处于回退调试模式）、
 * 路由包含 SERIAL 位。对于 LOG 类型事件还需检查日志级别是否满足串口级别阈值。
 *
 * 参数: kind  - 事件种类
 * 参数: level - 日志级别
 * 参数: route - 该事件的路由掩码
 * 返回: true 表示应向串口输出
 */
bool app_event_route_to_serial(app_event_kind_t kind, cc_log_level_t level, app_event_sink_mask_t route)
{
    if (!g_app_event_bus.serial_enabled && !g_app_event_bus.serial_fallback_debug_enabled) return false;
    if (!(route & APP_EVENT_SINK_SERIAL)) return false;
    if (kind == APP_EVENT_KIND_LOG &&
        !g_app_event_bus.serial_fallback_debug_enabled &&
        level < g_app_event_bus.serial_log_level) {
        return false;
    }
    return true;
}

/*
 * 判断事件是否应路由到 LCD 显示屏，需同时满足：显示屏已启用、
 * 路由包含 DISPLAY 位、且事件种类不是 LOG/RUN_STATE/PROFILE。
 *
 * 参数: kind  - 事件种类
 * 参数: route - 该事件的路由掩码
 * 返回: true 表示应输出到显示屏
 */
bool app_event_route_to_display(app_event_kind_t kind, app_event_sink_mask_t route)
{
    return g_app_event_bus.display_enabled &&
           (route & APP_EVENT_SINK_DISPLAY) &&
           kind != APP_EVENT_KIND_LOG &&
           kind != APP_EVENT_KIND_RUN_STATE &&
           kind != APP_EVENT_KIND_PROFILE;
}
