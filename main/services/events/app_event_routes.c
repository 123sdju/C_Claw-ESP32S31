#include "app_event_bus_internal.h"

#include <string.h>

/**
 * @brief 按精确小写名称解析日志级别，保留 INFO 缺省。
 * @param[in] text 调用期借用的 NUL 字符串，可为 NULL。
 * @return trace/debug/warn/error/fatal 的对应级别，info、空值或未知拼写返回 INFO。
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

/** @brief 提供日志级别的稳定文本。 @param[in] level 按值级别。 @return 静态小写名称借用，未知值为 info。 */
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

/**
 * @brief 将精确 sink 名称列表折叠为路由位集合。
 * @param[in] list 配置列表借用，NULL 表示空路由，非零 count 须有对应 items 数组。
 * @return serial/web/display/usb_log 的组合位，NULL 项、重复项及未知名称分别忽略或合并。
 * @note 列表只在调用期读取，外层保持配置稳定，本函数不保留任何字符串。
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

/** @brief 在状态锁内读取指定种类路由。 @param[in] kind 按值种类，越界安全。 @return 已配置位集合，越界为零。 @pre 调用者持总线锁及有效资源租约，或处于独占初始化。 */
app_event_sink_mask_t app_event_route_for_kind_locked(app_event_kind_t kind)
{
    if ((unsigned)kind >= (sizeof(g_app_event_bus.route) / sizeof(g_app_event_bus.route[0]))) {
        return 0;
    }
    return g_app_event_bus.route[kind];
}

/**
 * @brief 从 IO 快照复制全部事件路由位，chat 与 status 各复用两个种类。
 * @param[in] io 必需的稳定配置借用，各列表须满足 count/items 约束。
 * @pre 调用者持状态锁，配置 owner 保持输入覆盖整个调用。
 * @post USER/ASSISTANT 共用 chat，STATUS/RUN_STATE 共用 status，其余按对应列表更新，无字符串借用遗留。
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

/** @brief 安装缺省串口回退路由。 @pre 调用者持状态锁。 @post THINKING 路由清零，其他现有种类仅含 SERIAL。 */
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

/**
 * @brief 将可识别 sink 位按固定顺序写为逗号分隔文本。
 * @param[in] mask 按值位集合，未知位忽略。
 * @param[out] out 可选可写缓冲，NULL 或零容量直接返回。
 * @param[in] out_size 缓冲字节数，含终止 NUL。
 * @post 有效缓冲始终 NUL 结尾，按 serial/web/display/usb_log 顺序尝试完整片段，容量不足片段跳过，末逗号移除。
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

/**
 * @brief 按配置和级别决定 USB 路由意图。
 * @param[in] kind 当前不参与判定的事件种类。
 * @param[in] level 按值日志级别，所有种类都应用 USB 阈值。
 * @param[in] route 已取得的路由位集合。
 * @return USB 配置启用、路由含 USB_LOG 且级别达到阈值时为 true。
 * @pre 调用者持状态锁，实际入队还需 sink 再检查生命周期与接纳状态。
 */
bool app_event_route_to_usb_log(app_event_kind_t kind, cc_log_level_t level, app_event_sink_mask_t route)
{
    return g_app_event_bus.usb_log_enabled &&
           (route & APP_EVENT_SINK_USB_LOG) &&
           level >= g_app_event_bus.usb_log_level;
}

/**
 * @brief 按串口开关、回退标志和 LOG 阈值决定路由。
 * @param[in] kind 仅 LOG 类型应用普通级别阈值。
 * @param[in] level 按值级别。
 * @param[in] route 已取得的路由位集合，须含 SERIAL 才能通过。
 * @return 串口或回退启用且满足路由/阈值时为 true，回退模式跳过 LOG 阈值。
 * @pre 调用者持总线状态锁。
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

/**
 * @brief 按显示配置及种类排除生成普通 LCD 路由意图。
 * @param[in] kind 按值种类，LOG/RUN_STATE/PROFILE 排除。
 * @param[in] route 已取得的路由位集合。
 * @return 显示启用、含 DISPLAY 且种类通过时为 true，boot 专用入口另行处理。
 * @pre 调用者持状态锁，队列接纳和 UI 可用性由 sink 再检查。
 */
bool app_event_route_to_display(app_event_kind_t kind, app_event_sink_mask_t route)
{
    return g_app_event_bus.display_enabled &&
           (route & APP_EVENT_SINK_DISPLAY) &&
           kind != APP_EVENT_KIND_LOG &&
           kind != APP_EVENT_KIND_RUN_STATE &&
           kind != APP_EVENT_KIND_PROFILE;
}
