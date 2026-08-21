#include "app_event_bus.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_event_bus_internal.h"
#include "cc/util/cc_redaction.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    const char *text;
    bool owned;
} app_event_text_view_t;

app_event_bus_state_t g_app_event_bus;

/*
 * 将事件种类枚举值转为可读的字符串名称，用于日志输出和JSON序列化。
 *
 * 参数: kind - 事件种类枚举值（LOG/USER/ASSISTANT/THINKING/TOOL/STATUS/ERROR/RUN_STATE/PROFILE）
 * 返回: 对应的常量字符串指针，未知类型返回 "unknown"
 */
const char *app_event_kind_string(app_event_kind_t kind)
{
    switch (kind) {
    case APP_EVENT_KIND_LOG: return "log";
    case APP_EVENT_KIND_USER: return "user";
    case APP_EVENT_KIND_ASSISTANT: return "assistant";
    case APP_EVENT_KIND_THINKING: return "thinking";
    case APP_EVENT_KIND_TOOL: return "tool";
    case APP_EVENT_KIND_STATUS: return "status";
    case APP_EVENT_KIND_ERROR: return "error";
    case APP_EVENT_KIND_RUN_STATE: return "run_state";
    case APP_EVENT_KIND_PROFILE: return "profile";
    default: return "unknown";
    }
}

/*
 * 将事件来源枚举值转为可读的字符串名称。
 *
 * 参数: source - 事件来源枚举值（SYSTEM/SERIAL/WEB/RUNTIME/TOOL）
 * 返回: 对应的常量字符串指针，未知来源返回 "unknown"
 */
const char *app_event_source_string(app_event_source_t source)
{
    switch (source) {
    case APP_EVENT_SOURCE_SYSTEM: return "system";
    case APP_EVENT_SOURCE_SERIAL: return "serial";
    case APP_EVENT_SOURCE_WEB: return "web";
    case APP_EVENT_SOURCE_RUNTIME: return "runtime";
    case APP_EVENT_SOURCE_TOOL: return "tool";
    default: return "unknown";
    }
}

/*
 * 将事件总线生命周期枚举值转为可读的字符串名称。
 *
 * 参数: lifecycle - 生命周期枚举值（UNINIT/FALLBACK_SERIAL/CONFIGURED/USB_LOG_READY/USB_LOG_FAILED）
 * 返回: 对应的常量字符串指针，未知状态返回 "unknown"
 */
const char *app_event_bus_lifecycle_string(app_event_bus_lifecycle_t lifecycle)
{
    switch (lifecycle) {
    case APP_EVENT_BUS_UNINIT: return "uninit";
    case APP_EVENT_BUS_FALLBACK_SERIAL: return "fallback_serial";
    case APP_EVENT_BUS_CONFIGURED: return "configured";
    case APP_EVENT_BUS_USB_LOG_READY: return "usb_log_ready";
    case APP_EVENT_BUS_USB_LOG_FAILED: return "usb_log_failed";
    case APP_EVENT_BUS_DRAINING: return "draining";
    case APP_EVENT_BUS_STOPPED: return "stopped";
    case APP_EVENT_BUS_FAILED: return "failed";
    default: return "unknown";
    }
}

/*
 * 获取事件总线全局互斥锁，阻塞直到获取成功。用于保护 g_app_event_bus 共享状态的所有读写操作。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_bus_lock(void)
{
    if (g_app_event_bus.mutex) {
        xSemaphoreTake(g_app_event_bus.mutex, portMAX_DELAY);
    }
}

/*
 * 释放事件总线全局互斥锁，与 app_event_bus_lock 配对使用。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_bus_unlock(void)
{
    if (g_app_event_bus.mutex) {
        xSemaphoreGive(g_app_event_bus.mutex);
    }
}

/*
 * 分配内存并复制指定长度的字符串，优先使用 PSRAM，回退到内部 RAM。
 * 长度超过 APP_EVENT_TEXT_MAX 时自动截断。返回的副本由调用者负责释放。
 *
 * 参数: text - 源字符串指针（可为NULL）
 * 参数: len  - 要复制的字节数
 * 返回: 堆上分配的以NULL结尾的字符串副本，失败返回 NULL
 */
char *app_event_strdup_len(const char *text, size_t len)
{
    if (len > APP_EVENT_TEXT_MAX) {
        len = APP_EVENT_TEXT_MAX;
    }
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        copy = heap_caps_malloc(len + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!copy) {
        return NULL;
    }
    if (text && len > 0) {
        memcpy(copy, text, len);
    }
    copy[len] = '\0';
    return copy;
}

/*
 * 分配内存并复制完整字符串，内部委托给 app_event_strdup_len。
 *
 * 参数: text - 源字符串指针（可为NULL，按空字符串处理）
 * 返回: 堆上分配的字符串副本，失败返回 NULL
 */
char *app_event_strdup(const char *text)
{
    return app_event_strdup_len(text ? text : "", text ? strlen(text) : 0);
}

app_event_envelope_t *app_event_envelope_create(const char *text)
{
    const char *value = text ? text : "";
    size_t len = strlen(value);
    if (len > APP_EVENT_TEXT_MAX) len = APP_EVENT_TEXT_MAX;
    if (len > SIZE_MAX - sizeof(app_event_envelope_t) - 1U) return NULL;
    size_t bytes = sizeof(app_event_envelope_t) + len + 1U;
    app_event_envelope_t *envelope = heap_caps_malloc(
        bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!envelope) envelope = heap_caps_malloc(
        bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!envelope) return NULL;
    envelope->refs = 1U;
    envelope->len = len;
    memcpy(envelope->text, value, len);
    envelope->text[len] = '\0';
    return envelope;
}

void app_event_envelope_retain(app_event_envelope_t *envelope)
{
    if (envelope) (void)__atomic_add_fetch(&envelope->refs, 1U, __ATOMIC_ACQ_REL);
}

void app_event_envelope_release(app_event_envelope_t *envelope)
{
    if (!envelope) return;
    if (__atomic_sub_fetch(&envelope->refs, 1U, __ATOMIC_ACQ_REL) == 0U) {
        heap_caps_free(envelope);
    }
}

/*
 * 安全地将源字符串拷贝到目标缓冲区，自动截断超长内容。
 *
 * 参数: dst      - 目标缓冲区指针
 * 参数: dst_size - 目标缓冲区大小（字节）
 * 参数: src      - 源字符串指针（可为NULL，按空字符串处理）
 * 返回: 无
 */
void app_event_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

/*
 * 将一段文本追加到已有字符串末尾，超出缓冲区大小时拒绝追加。
 *
 * 参数: out      - 输出缓冲区（已有NULL结尾的字符串）
 * 参数: out_size - 缓冲区总大小（字节）
 * 参数: text     - 要追加的文本
 * 返回: true 表示追加成功，false 表示缓冲区不足
 */
bool app_event_append_text(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0 || !text) return false;
    size_t out_len = strlen(out);
    size_t text_len = strlen(text);
    if (out_len + text_len >= out_size) return false;
    memcpy(out + out_len, text, text_len + 1);
    return true;
}

/*
 * 拼接目录路径和文件名，自动补充路径分隔符 '/'。
 *
 * 参数: out      - 输出缓冲区
 * 参数: out_size - 缓冲区大小（字节）
 * 参数: dir      - 目录路径
 * 参数: name     - 文件名
 * 返回: true 表示拼接成功，false 表示缓冲区不足或参数无效
 */
bool app_event_join_path(char *out, size_t out_size, const char *dir, const char *name)
{
    if (!out || out_size == 0 || !dir || !name) return false;
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    bool needs_slash = dir_len > 0 && dir[dir_len - 1] != '/';
    size_t total = dir_len + (needs_slash ? 1U : 0U) + name_len;
    if (total >= out_size) return false;
    memcpy(out, dir, dir_len);
    size_t pos = dir_len;
    if (needs_slash) out[pos++] = '/';
    memcpy(out + pos, name, name_len);
    out[total] = '\0';
    return true;
}

/*
 * 将无符号32位整数转换为十进制字符串并追加到输出缓冲区。
 *
 * 参数: out      - 输出缓冲区（已有NULL结尾的字符串）
 * 参数: out_size - 缓冲区大小（字节）
 * 参数: value    - 要转换的 uint32_t 值
 * 返回: true 表示追加成功，false 表示缓冲区不足
 */
bool app_event_append_uint32(char *out, size_t out_size, uint32_t value)
{
    char buf[11];
    size_t pos = sizeof(buf);
    buf[--pos] = '\0';
    do {
        buf[--pos] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value > 0U && pos > 0U);
    return app_event_append_text(out, out_size, &buf[pos]);
}

/*
 * 根据事件种类返回串口输出时的前缀标签，用于 printf 格式化。
 * LOG 和 PROFILE 类型使用 "log" 前缀，其余类型使用对应短标签。
 *
 * 参数: kind - 事件种类枚举值
 * 返回: 对应的前缀字符串常量
 */
static const char *serial_prefix_for_event(app_event_kind_t kind)
{
    switch (kind) {
    case APP_EVENT_KIND_USER: return "user";
    case APP_EVENT_KIND_ASSISTANT: return "assistant";
    case APP_EVENT_KIND_THINKING: return "thinking";
    case APP_EVENT_KIND_TOOL: return "tool";
    case APP_EVENT_KIND_STATUS: return "status";
    case APP_EVENT_KIND_ERROR: return "error";
    case APP_EVENT_KIND_RUN_STATE: return "run";
    case APP_EVENT_KIND_LOG:
    default:
        return "log";
    }
}

/*
 * 对事件文本进行敏感信息脱敏处理，调用 cc_redact_secrets 替换密钥等敏感内容。
 * 返回一个文本视图结构，owned 标记为 true 时表示文本在堆上分配，需要手动释放。
 *
 * 参数: text - 原始事件文本
 * 返回: 脱敏后的文本视图，包含指向原文或新分配脱敏文本的指针
 */
static app_event_text_view_t redacted_event_text(const char *text)
{
    app_event_text_view_t view = {
        .text = text ? text : "",
        .owned = false,
    };
    char *redacted = cc_redact_secrets(view.text);
    if (redacted) {
        view.text = redacted;
        view.owned = true;
    }
    return view;
}

/*
 * 释放由 redacted_event_text 分配的脱敏文本内存（仅当 owned 为 true 时）。
 *
 * 参数: view - 文本视图指针
 * 返回: 无
 */
static void free_event_text_view(app_event_text_view_t *view)
{
    if (view && view->owned) {
        free((void *)view->text);
        view->text = NULL;
        view->owned = false;
    }
}

/*
 * 将 ASCII 大写字母转为小写，其他字符原样返回。
 *
 * 参数: c - 输入字符
 * 返回: 小写字母或原字符
 */
static char app_event_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/*
 * 不区分大小写地检查文本中是否包含指定子串，采用朴素逐位置匹配算法。
 *
 * 参数: text   - 被搜索的文本
 * 参数: needle - 要查找的子串
 * 返回: true 表示找到匹配，false 表示未找到或参数无效
 */
static bool app_event_text_contains_ci(const char *text, const char *needle)
{
    if (!text || !needle || !needle[0]) {
        return false;
    }
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (needle[i] && p[i] &&
               app_event_ascii_lower(p[i]) == app_event_ascii_lower(needle[i])) {
            i++;
        }
        if (!needle[i]) {
            return true;
        }
    }
    return false;
}

/*
 * 判断事件文本内容是否与 OTA 维护相关（如固件升级、分区操作、重启等关键词）。
 * 维护模式下相关的事件不会被丢弃。
 *
 * 参数: text - 事件文本内容
 * 返回: true 表示文本与维护相关，false 表示不相关
 */
static bool app_event_maintenance_text_is_relevant(const char *text)
{
    return app_event_text_contains_ci(text, "ota") ||
           app_event_text_contains_ci(text, "firmware") ||
           app_event_text_contains_ci(text, "partition") ||
           app_event_text_contains_ci(text, "reboot") ||
           app_event_text_contains_ci(text, "upload") ||
           app_event_text_contains_ci(text, "progress");
}

/*
 * 判断维护模式下某事件是否允许通过。ERROR 类型无条件放行，
 * WARN 及以上级别的 LOG/STATUS 事件放行，与维护关键词相关的 LOG/STATUS 事件也放行。
 *
 * 参数: kind   - 事件种类
 * 参数: source - 事件来源（当前未使用）
 * 参数: level  - 日志级别
 * 参数: text   - 事件文本内容
 * 返回: true 表示允许通过，false 表示应被丢弃
 */
static bool app_event_maintenance_allows(app_event_kind_t kind,
                                         app_event_source_t source,
                                         cc_log_level_t level,
                                         const char *text)
{
    (void)source;
    if (kind == APP_EVENT_KIND_ERROR) {
        return true;
    }
    if (level >= CC_LOG_WARN &&
        (kind == APP_EVENT_KIND_LOG || kind == APP_EVENT_KIND_STATUS)) {
        return true;
    }
    if ((kind == APP_EVENT_KIND_LOG || kind == APP_EVENT_KIND_STATUS) &&
        app_event_maintenance_text_is_relevant(text)) {
        return true;
    }
    return false;
}

/*
 * 在已持锁状态下判断维护模式是否应丢弃该事件，并更新丢弃计数器。
 * 非维护模式下直接返回 false（不丢弃）。
 *
 * 参数: kind   - 事件种类
 * 参数: source - 事件来源
 * 参数: level  - 日志级别
 * 参数: text   - 事件文本内容
 * 返回: true 表示该事件被丢弃，false 表示继续处理
 */
static bool app_event_maintenance_drop_locked(app_event_kind_t kind,
                                              app_event_source_t source,
                                              cc_log_level_t level,
                                              const char *text)
{
    if (!g_app_event_bus.maintenance_mode) {
        return false;
    }
    if (app_event_maintenance_allows(kind, source, level, text)) {
        return false;
    }
    g_app_event_bus.dropped++;
    g_app_event_bus.sink_dropped++;
    g_app_event_bus.maintenance_skipped++;
    return true;
}

/*
 * 事件发布核心入口：进行敏感信息脱敏、文本复制、维护模式过滤，
 * 然后根据路由规则将事件分发到串口、显示端和USB日志三个 sink。
 * 同时将事件写入环形缓冲区供后续查询。
 *
 * 参数: kind   - 事件种类
 * 参数: source - 事件来源
 * 参数: level  - 日志级别
 * 参数: text   - 事件文本内容
 * 返回: 无
 */
void app_event_publish(app_event_kind_t kind,
                       app_event_source_t source,
                       cc_log_level_t level,
                       const char *text)
{
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) {
        char *redacted = cc_redact_secrets(text ? text : "");
        if (redacted) {
            printf("[%s] %s\r\n", serial_prefix_for_event(kind), redacted);
            free(redacted);
        } else {
            printf("[error] event redaction allocation failed\r\n");
        }
        fflush(stdout);
        return;
    }
    app_event_bus_lock();
    bool drop_for_maintenance = app_event_maintenance_drop_locked(kind, source, level, text);
    app_event_bus_unlock();
    if (drop_for_maintenance) {
        return;
    }

    app_event_text_view_t safe = redacted_event_text(text);
    app_event_envelope_t *envelope = app_event_envelope_create(safe.text);
    if (!envelope) {
        app_event_bus_lock();
        g_app_event_bus.dropped++;
        g_app_event_bus.sink_dropped++;
        g_app_event_bus.oom_dropped++;
        app_event_bus_unlock();
        free_event_text_view(&safe);
        return;
    }

    app_event_sink_mask_t route = 0;
    uint32_t seq = 0;
    app_event_record_t record;
    memset(&record, 0, sizeof(record));
    record.kind = kind;
    record.source = source;
    record.level = level;
    record.timestamp_ms = (int64_t)pdTICKS_TO_MS(xTaskGetTickCount());
    record.envelope = envelope;

    app_event_bus_lock();
    if (app_event_maintenance_drop_locked(kind, source, level, safe.text)) {
        app_event_bus_unlock();
        app_event_envelope_release(envelope);
        free_event_text_view(&safe);
        return;
    }
    bool maintenance_mode = g_app_event_bus.maintenance_mode;
    bool draining = g_app_event_bus.lifecycle == APP_EVENT_BUS_DRAINING ||
                    g_app_event_bus.lifecycle == APP_EVENT_BUS_STOPPED ||
                    g_app_event_bus.lifecycle == APP_EVENT_BUS_FAILED;
    record.seq = ++g_app_event_bus.next_seq;
    g_app_event_bus.published++;
    seq = record.seq;
    route = (maintenance_mode || draining) ? APP_EVENT_SINK_SERIAL :
            app_event_route_for_kind_locked(kind);
    bool stored = app_event_ring_append_locked(&record);
    bool to_serial = maintenance_mode ? true : app_event_route_to_serial(kind, level, route);
    bool to_display = (maintenance_mode || draining) ? false :
                      app_event_route_to_display(kind, route);
    bool to_usb = (maintenance_mode || draining) ? false :
                  app_event_route_to_usb_log(kind, level, route);
    if (to_serial && kind == APP_EVENT_KIND_USER && source == APP_EVENT_SOURCE_SERIAL) {
        to_serial = false;
    }
    app_event_bus_unlock();

    if (to_serial) {
        if (kind == APP_EVENT_KIND_LOG) {
            printf("%s\r\n", envelope->text);
        } else {
            printf("[%s] %s\r\n", serial_prefix_for_event(kind), envelope->text);
        }
        fflush(stdout);
    }

    if (to_display) {
        app_event_display_emit(kind, envelope);
    }

    if (to_usb) {
        app_event_usb_enqueue(envelope, seq, kind, level);
    }
    if (!stored) {
        app_event_envelope_release(envelope);
    }
    free_event_text_view(&safe);
}

/*
 * 格式化字符串后发布事件，委托给 app_event_publish。
 * 格式化缓冲区上限为 APP_EVENT_TEXT_MAX，超长内容自动截断。
 *
 * 参数: kind   - 事件种类
 * 参数: source - 事件来源
 * 参数: level  - 日志级别
 * 参数: fmt    - printf 格式字符串
 * 参数: ...    - 可变参数列表
 * 返回: 无
 */
void app_event_publishf(app_event_kind_t kind,
                        app_event_source_t source,
                        cc_log_level_t level,
                        const char *fmt,
                        ...)
{
    if (!fmt) return;
    char text[APP_EVENT_TEXT_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    app_event_publish(kind, source, level, text);
}

/*
 * 清除 LCD 显示端的对话内容和事件环形缓冲区，通常在开始新对话时调用。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_bus_clear_conversation(void)
{
    if (!g_app_event_bus.mutex) return;
    app_event_display_clear();
    app_event_bus_lock();
    app_event_ring_clear_locked();
    app_event_bus_unlock();
}

/*
 * 在 app_event_bus_init() 中显式创建事件总线同步原语。
 *
 * 参数: 无
 * 返回: true 表示互斥锁可用，false 表示创建失败
 */
static bool initialize_primitives(void)
{
    if (!g_app_event_bus.mutex) {
        g_app_event_bus.mutex = xSemaphoreCreateMutex();
    }
    if (!g_app_event_bus.idle_events) {
        g_app_event_bus.idle_events = xEventGroupCreate();
        if (g_app_event_bus.idle_events) {
            xEventGroupSetBits(g_app_event_bus.idle_events, APP_EVENT_BUS_IDLE_BIT);
        }
    }
    return g_app_event_bus.mutex != NULL && g_app_event_bus.idle_events != NULL;
}

/*
 * 将运行时 IO 配置应用到事件总线：设置串口/显示/USB日志开关及日志级别，
 * 配置事件路由规则，安装ESP日志钩子，启动USB日志任务。
 *
 * 参数: io - 运行时IO配置指针
 * 返回: ESP_OK 表示成功，ESP_ERR_INVALID_ARG 表示参数无效，ESP_ERR_NO_MEM 表示内存不足
 */
esp_err_t app_event_bus_apply_config(const app_runtime_io_config_t *io)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) {
        return ESP_ERR_INVALID_STATE;
    }

    app_event_bus_lock();
    g_app_event_bus.serial_enabled = io->serial.enabled != 0;
    g_app_event_bus.display_enabled = io->display.enabled != 0;
    g_app_event_bus.serial_debug_enabled = app_event_parse_log_level(io->serial.log_level) <= CC_LOG_DEBUG;
    g_app_event_bus.serial_log_level = app_event_parse_log_level(io->serial.log_level);
    g_app_event_bus.serial_fallback_debug_enabled = false;
    g_app_event_bus.usb_log_enabled = io->usb_log.enabled != 0;
    g_app_event_bus.usb_log_level = app_event_parse_log_level(io->usb_log.level);
    g_app_event_bus.usb_max_bytes = io->usb_log.max_bytes ? io->usb_log.max_bytes : 512 * 1024;
    g_app_event_bus.usb_max_files = io->usb_log.max_files;
    app_event_copy_text(g_app_event_bus.usb_dir,
                        sizeof(g_app_event_bus.usb_dir),
                        io->usb_log.dir && io->usb_log.dir[0] ?
                            io->usb_log.dir : "/usb0/cclaw/logs");
    app_event_configure_routes_locked(io);
    g_app_event_bus.config_applied = true;
    g_app_event_bus.lifecycle = APP_EVENT_BUS_CONFIGURED;
    app_event_bus_unlock();

    app_event_log_hooks_install();
    app_event_log_hooks_apply_esp_level();

    if (!g_app_event_bus.usb_log_enabled) {
        return app_event_usb_stop(2000U);
    }

    esp_err_t err = app_event_usb_start();
    if (err != ESP_OK) {
        app_event_bus_lock();
        g_app_event_bus.lifecycle = APP_EVENT_BUS_USB_LOG_FAILED;
        app_event_bus_unlock();
        app_event_bus_start_serial_fallback_debug();
        return err;
    }
    return ESP_OK;
}

/*
 * 初始化事件总线：创建互斥锁和环形缓冲区，安装日志钩子。
 * 若提供了 IO 配置则应用配置，否则启动串口回退调试模式。
 *
 * 参数: io - 运行时IO配置指针（可为NULL，表示使用串口回退模式）
 * 返回: ESP_OK 表示成功，ESP_ERR_NO_MEM 表示内存不足
 */
esp_err_t app_event_bus_init(const app_runtime_io_config_t *io)
{
    if (!initialize_primitives()) return ESP_ERR_NO_MEM;
    esp_err_t sink_err = app_event_usb_init();
    if (sink_err != ESP_OK) return sink_err;
    sink_err = app_event_display_init();
    if (sink_err != ESP_OK) return sink_err;
    app_event_bus_lock();
    if (g_app_event_bus.lifecycle == APP_EVENT_BUS_UNINIT) {
        g_app_event_bus.lifecycle = APP_EVENT_BUS_FALLBACK_SERIAL;
    }
    bool ring_ok = app_event_ring_ensure_locked();
    app_event_bus_unlock();
    if (!ring_ok) return ESP_ERR_NO_MEM;
    app_event_log_hooks_install();
    if (io) {
        return app_event_bus_apply_config(io);
    }
    app_event_bus_start_serial_fallback_debug();
    return ESP_OK;
}

esp_err_t app_event_bus_stop(uint32_t timeout_ms)
{
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) {
        return ESP_ERR_INVALID_STATE;
    }
    if (timeout_ms == 0) timeout_ms = 2000U;
    app_event_bus_lock();
    if (g_app_event_bus.lifecycle == APP_EVENT_BUS_STOPPED) {
        app_event_bus_unlock();
        return ESP_OK;
    }
    g_app_event_bus.lifecycle = APP_EVENT_BUS_DRAINING;
    app_event_bus_unlock();

    if (!app_event_bus_wait_idle(timeout_ms)) {
        app_event_bus_lock();
        g_app_event_bus.lifecycle = APP_EVENT_BUS_FAILED;
        app_event_bus_unlock();
        return ESP_ERR_TIMEOUT;
    }
    app_event_bus_lock();
    g_app_event_bus.usb_log_enabled = false;
    g_app_event_bus.display_enabled = false;
    app_event_bus_unlock();
    esp_err_t err = app_event_usb_stop(timeout_ms);
    app_event_bus_lock();
    g_app_event_bus.lifecycle = err == ESP_OK ? APP_EVENT_BUS_STOPPED : APP_EVENT_BUS_FAILED;
    app_event_bus_unlock();
    return err;
}

esp_err_t app_event_bus_destroy(uint32_t timeout_ms)
{
    if (!g_app_event_bus.mutex) return ESP_OK;
    esp_err_t err = app_event_bus_stop(timeout_ms);
    if (err != ESP_OK) return err;

    app_event_log_hooks_uninstall();
    app_event_display_discard_all();
    if (g_app_event_bus.usb_queue) {
        app_event_usb_log_msg_t msg;
        while (xQueueReceive(g_app_event_bus.usb_queue, &msg, 0) == pdTRUE) {
            app_event_envelope_release(msg.envelope);
        }
    }

    app_event_bus_lock();
    app_event_ring_clear_locked();
    app_event_record_t *recent = g_app_event_bus.recent;
    g_app_event_bus.recent = NULL;
    g_app_event_bus.recent_capacity = 0;
    g_app_event_bus.lifecycle = APP_EVENT_BUS_UNINIT;
    app_event_bus_unlock();
    heap_caps_free(recent);

    QueueHandle_t usb_queue = g_app_event_bus.usb_queue;
    QueueHandle_t display_queue = g_app_event_bus.display_queue;
    EventGroupHandle_t idle_events = g_app_event_bus.idle_events;
    SemaphoreHandle_t file_mutex = g_app_event_bus.file_mutex;
    SemaphoreHandle_t mutex = g_app_event_bus.mutex;
    g_app_event_bus.usb_queue = NULL;
    g_app_event_bus.display_queue = NULL;
    g_app_event_bus.idle_events = NULL;
    g_app_event_bus.file_mutex = NULL;
    g_app_event_bus.mutex = NULL;
    if (usb_queue) vQueueDelete(usb_queue);
    if (display_queue) vQueueDelete(display_queue);
    if (idle_events) vEventGroupDelete(idle_events);
    if (file_mutex) vSemaphoreDelete(file_mutex);
    if (mutex) vSemaphoreDelete(mutex);
    memset(&g_app_event_bus, 0, sizeof(g_app_event_bus));
    return ESP_OK;
}

/*
 * 启动串口调试回退模式：停止USB日志，强制启用串口输出和DEBUG级别，
 * 将 LOG/STATUS/ERROR 路由到串口，确保在没有USB存储时也能通过串口查看日志。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_bus_start_serial_fallback_debug(void)
{
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) return;
    (void)app_event_usb_stop(2000U);
    app_event_bus_lock();
    g_app_event_bus.usb_log_enabled = false;
    g_app_event_bus.usb_log_started = false;
    g_app_event_bus.serial_enabled = true;
    g_app_event_bus.serial_fallback_debug_enabled = true;
    g_app_event_bus.serial_debug_enabled = true;
    g_app_event_bus.serial_log_level = CC_LOG_DEBUG;
    if (g_app_event_bus.config_applied) {
        g_app_event_bus.route[APP_EVENT_KIND_LOG] |= APP_EVENT_SINK_SERIAL;
        g_app_event_bus.route[APP_EVENT_KIND_STATUS] |= APP_EVENT_SINK_SERIAL;
        g_app_event_bus.route[APP_EVENT_KIND_ERROR] |= APP_EVENT_SINK_SERIAL;
    } else {
        app_event_configure_fallback_routes_locked();
    }
    if (g_app_event_bus.lifecycle != APP_EVENT_BUS_USB_LOG_FAILED) {
        g_app_event_bus.lifecycle = APP_EVENT_BUS_FALLBACK_SERIAL;
    }
    app_event_bus_unlock();
    app_event_log_hooks_install();
    app_event_log_hooks_apply_esp_level();
}

/*
 * 标记系统时间已通过 NTP 同步，触发 USB 日志任务将启动时的临时文件名
 * 重命名为基于真实时间戳的文件名。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_bus_note_time_synced(void)
{
    if (!g_app_event_bus.mutex) return;
    app_event_bus_lock();
    g_app_event_bus.rename_pending = true;
    app_event_bus_unlock();
}

/*
 * 设置串口调试开关，启用时串口日志级别降至 DEBUG，关闭时恢复为 INFO。
 * 同时清除串口回退标记并同步更新 ESP 日志级别。
 *
 * 参数: enabled - true 启用调试输出，false 关闭
 * 返回: 无
 */
void app_event_bus_set_serial_debug(bool enabled)
{
    if (!g_app_event_bus.mutex) return;
    app_event_bus_lock();
    g_app_event_bus.serial_debug_enabled = enabled;
    g_app_event_bus.serial_fallback_debug_enabled = false;
    g_app_event_bus.serial_log_level = enabled ? CC_LOG_DEBUG : CC_LOG_INFO;
    app_event_bus_unlock();
    app_event_log_hooks_apply_esp_level();
}

/*
 * 设置 OTA 维护模式：启用时大量无关日志被丢弃以节省资源，仅保留错误和
 * 与 OTA/固件相关的关键日志。状态变更时通过串口输出提示信息。
 *
 * 参数: enabled - true 启用维护模式，false 关闭
 * 参数: reason  - 进入维护模式的原因描述（可为NULL）
 * 返回: 无
 */
void app_event_bus_set_maintenance_mode(bool enabled, const char *reason)
{
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) {
        return;
    }

    char previous_reason[64] = {0};
    char current_reason[64] = {0};
    bool changed = false;
    app_event_bus_lock();
    bool was_enabled = g_app_event_bus.maintenance_mode;
    app_event_copy_text(previous_reason,
                        sizeof(previous_reason),
                        g_app_event_bus.maintenance_reason);
    g_app_event_bus.maintenance_mode = enabled;
    app_event_copy_text(g_app_event_bus.maintenance_reason,
                        sizeof(g_app_event_bus.maintenance_reason),
                        enabled ? (reason ? reason : "") : "");
    app_event_copy_text(current_reason,
                        sizeof(current_reason),
                        g_app_event_bus.maintenance_reason);
    changed = was_enabled != enabled ||
              strcmp(previous_reason, current_reason) != 0;
    app_event_bus_unlock();

    app_event_log_hooks_apply_esp_level();

    if (changed) {
        printf("[status] OTA maintenance mode %s%s%s\r\n",
               enabled ? "enabled" : "disabled",
               enabled && current_reason[0] ? ": " : "",
               enabled && current_reason[0] ? current_reason : "");
        fflush(stdout);
    }
}

/*
 * 查询串口调试是否已启用（不含回退模式），无锁读取全局标志。
 *
 * 参数: 无
 * 返回: true 表示串口调试已启用
 */
bool app_event_bus_serial_debug_enabled(void)
{
    if (!g_app_event_bus.mutex) return false;
    app_event_bus_lock();
    bool enabled = g_app_event_bus.serial_debug_enabled;
    app_event_bus_unlock();
    return enabled;
}

/*
 * 查询维护模式是否启用，持锁读取确保原子性。
 *
 * 参数: 无
 * 返回: true 表示当前处于维护模式
 */
bool app_event_bus_maintenance_mode_enabled(void)
{
    bool enabled = false;
    app_event_bus_lock();
    enabled = g_app_event_bus.maintenance_mode;
    app_event_bus_unlock();
    return enabled;
}

/*
 * 等待 USB 和显示端事件队列排空，用于安全关机前确保所有日志已落盘。
 * sink drain 时通过事件组唤醒等待方，避免固定间隔轮询。
 *
 * 参数: timeout_ms - 超时时间（毫秒），0 使用默认有限超时
 * 返回: true 表示队列已排空，false 表示超时
 */
bool app_event_bus_wait_idle(uint32_t timeout_ms)
{
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) return false;
    TickType_t start = xTaskGetTickCount();
    if (timeout_ms == 0) timeout_ms = 5000U;
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while (true) {
        app_event_bus_lock();
        uint32_t usb_pending = g_app_event_bus.usb_pending;
        uint32_t display_pending = g_app_event_bus.display_pending;
        app_event_bus_unlock();
        if (usb_pending == 0 && display_pending == 0) {
            xEventGroupSetBits(g_app_event_bus.idle_events, APP_EVENT_BUS_IDLE_BIT);
            return true;
        }
        xEventGroupClearBits(g_app_event_bus.idle_events, APP_EVENT_BUS_IDLE_BIT);
        app_event_bus_lock();
        usb_pending = g_app_event_bus.usb_pending;
        display_pending = g_app_event_bus.display_pending;
        app_event_bus_unlock();
        if (usb_pending == 0 && display_pending == 0) {
            xEventGroupSetBits(g_app_event_bus.idle_events, APP_EVENT_BUS_IDLE_BIT);
            return true;
        }
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout_ticks) {
            return false;
        }
        TickType_t wait_ticks = timeout_ticks - elapsed;
        EventBits_t bits = xEventGroupWaitBits(g_app_event_bus.idle_events,
                                               APP_EVENT_BUS_IDLE_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               wait_ticks);
        if ((bits & APP_EVENT_BUS_IDLE_BIT) == 0) {
            return false;
        }
    }
}

void app_event_bus_mark_sink_busy(void)
{
    if (g_app_event_bus.idle_events) {
        xEventGroupClearBits(g_app_event_bus.idle_events, APP_EVENT_BUS_IDLE_BIT);
    }
}

void app_event_bus_notify_idle_if_drained(void)
{
    if (!g_app_event_bus.idle_events) return;
    app_event_bus_lock();
    bool idle = g_app_event_bus.usb_pending == 0 &&
                g_app_event_bus.display_pending == 0;
    app_event_bus_unlock();
    if (idle) {
        xEventGroupSetBits(g_app_event_bus.idle_events, APP_EVENT_BUS_IDLE_BIT);
    }
}

/*
 * 获取事件总线运行状态快照：生命周期、计数器、队列水位、各 sink 路由配置、
 * 当前日志文件路径等，填充到 out_status 结构体中。所有读取在持锁下完成。
 *
 * 参数: out_status - 状态输出结构体指针
 * 返回: 无
 */
void app_event_bus_get_status(app_event_bus_status_t *out_status)
{
    if (!out_status) return;
    memset(out_status, 0, sizeof(*out_status));
    app_event_bus_lock();
    out_status->lifecycle = g_app_event_bus.lifecycle;
    out_status->serial_debug_enabled = g_app_event_bus.serial_debug_enabled;
    out_status->maintenance_mode = g_app_event_bus.maintenance_mode;
    out_status->usb_log_enabled = g_app_event_bus.usb_log_enabled;
    out_status->usb_log_started = g_app_event_bus.usb_log_started;
    out_status->usb_log_fallback_serial = g_app_event_bus.serial_fallback_debug_enabled;
    out_status->dropped = g_app_event_bus.dropped;
    out_status->ring_overwrites = g_app_event_bus.ring_overwrites;
    out_status->sink_dropped = g_app_event_bus.sink_dropped;
    out_status->published = g_app_event_bus.published;
    out_status->maintenance_skipped = g_app_event_bus.maintenance_skipped;
    out_status->oom_dropped = g_app_event_bus.oom_dropped;
    out_status->usb_queue_dropped = g_app_event_bus.usb_queue_dropped;
    out_status->display_queue_dropped = g_app_event_bus.display_queue_dropped;
    out_status->max_usb_queue_waiting = g_app_event_bus.max_usb_queue_waiting;
    out_status->max_display_queue_waiting = g_app_event_bus.max_display_queue_waiting;
    out_status->latest_seq = g_app_event_bus.next_seq;
    out_status->capacity = g_app_event_bus.recent_capacity;
    if (g_app_event_bus.recent_count > 0 && g_app_event_bus.recent_capacity > 0) {
        out_status->oldest_seq = g_app_event_bus.recent[g_app_event_bus.recent_start].seq;
    }
    out_status->usb_bytes_written = g_app_event_bus.usb_bytes_written;
    app_event_copy_text(out_status->usb_log_path,
                        sizeof(out_status->usb_log_path),
                        g_app_event_bus.usb_current_path);
    app_event_copy_text(out_status->maintenance_reason,
                        sizeof(out_status->maintenance_reason),
                        g_app_event_bus.maintenance_reason);
    app_event_sink_mask_string(g_app_event_bus.route[APP_EVENT_KIND_LOG],
                               out_status->log_route,
                               sizeof(out_status->log_route));
    app_event_sink_mask_string(g_app_event_bus.route[APP_EVENT_KIND_USER],
                               out_status->chat_route,
                               sizeof(out_status->chat_route));
    app_event_sink_mask_string(g_app_event_bus.route[APP_EVENT_KIND_THINKING],
                               out_status->thinking_route,
                               sizeof(out_status->thinking_route));
    app_event_sink_mask_string(g_app_event_bus.route[APP_EVENT_KIND_TOOL],
                               out_status->tool_route,
                               sizeof(out_status->tool_route));
    app_event_sink_mask_string(g_app_event_bus.route[APP_EVENT_KIND_STATUS],
                               out_status->status_route,
                               sizeof(out_status->status_route));
    app_event_sink_mask_string(g_app_event_bus.route[APP_EVENT_KIND_ERROR],
                               out_status->error_route,
                               sizeof(out_status->error_route));
    app_event_sink_mask_string(g_app_event_bus.route[APP_EVENT_KIND_PROFILE],
                               out_status->profile_route,
                               sizeof(out_status->profile_route));
    app_event_bus_unlock();
}

/*
 * 返回当前 USB 日志文件的完整路径，无锁直接读取全局状态中的字符串。
 *
 * 参数: 无
 * 返回: 指向当前日志路径的常量字符串指针
 */
void app_event_bus_copy_current_log_path(char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0) return;
    out_path[0] = '\0';
    if (!g_app_event_bus.mutex) return;
    app_event_bus_lock();
    app_event_copy_text(out_path, out_size, g_app_event_bus.usb_current_path);
    app_event_bus_unlock();
}
