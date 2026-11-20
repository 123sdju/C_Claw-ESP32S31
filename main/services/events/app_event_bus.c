#include "app_event_bus.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_event_bus_internal.h"
#include "freertos/idf_additions.h"
#include "cc/util/cc_redaction.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    const char *text; /* 调用期原文借用或脱敏后唯一拥有的字符串。 */
    bool owned;      /* true 时由 free_event_text_view 释放。 */
} app_event_text_view_t;

app_event_bus_state_t g_app_event_bus;

static portMUX_TYPE s_access_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_access_open;
static unsigned s_access_users;
static portMUX_TYPE s_bus_lock_owner_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_bus_lock_owner;
static unsigned s_bus_lock_depth;
static portMUX_TYPE s_publish_owner_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_publish_owner;
static SemaphoreHandle_t s_publish_order_mutex;

/** @brief 在 owner 保持原语存活时等待已接纳 sink 工作退场。 @param[in] start 外层初始 tick。 @param[in] timeout_ticks 有限共享 tick 预算。 @return sink 计数归零为 true，等待到期为 false。 */
static bool app_event_bus_wait_idle_until(TickType_t start, TickType_t timeout_ticks);

/**
 * @brief 使用 64 位乘法向上转换有限毫秒，保留原生无限等待哨兵。
 * @param[in] timeout_ms 非零有限预算毫秒，零返回零。
 * @return 正预算至少一 tick，过长值截到 portMAX_DELAY-1。
 */
TickType_t app_event_timeout_ticks(uint32_t timeout_ms)
{
    if (!timeout_ms) return 0;
    uint64_t ticks = ((uint64_t)timeout_ms * (uint64_t)configTICK_RATE_HZ + 999U) / 1000U;
    uint64_t maximum = (uint64_t)portMAX_DELAY - 1U;
    if (!ticks) ticks = 1;
    return (TickType_t)(ticks > maximum ? maximum : ticks);
}

/** @brief 取得共享有限 tick 预算的剩余量。 @param[in] start 同一等待链起点。 @param[in] budget 有限 tick 数。 @return 到期为零，否则剩余 tick，调用者把同一结果交原生等待。 */
TickType_t app_event_remaining_ticks(TickType_t start, TickType_t budget)
{
    TickType_t elapsed = xTaskGetTickCount() - start;
    return elapsed >= budget ? 0 : budget - elapsed;
}

/** @brief 取得发布与清屏的串行顺序门。 @return 已取得为 true，当前任务递归发布、门不存在或取锁失败为 false。 @pre 外层持资源租约或独占生命周期，成功后必须同任务配对 end。 */
static bool app_event_publish_order_begin(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_publish_owner_mux);
    bool reentrant = s_publish_owner == current;
    portEXIT_CRITICAL(&s_publish_owner_mux);
    if (reentrant || !s_publish_order_mutex) return false;
    if (xSemaphoreTake(s_publish_order_mutex, portMAX_DELAY) != pdTRUE) return false;
    portENTER_CRITICAL(&s_publish_owner_mux);
    s_publish_owner = current;
    portEXIT_CRITICAL(&s_publish_owner_mux);
    return true;
}

/** @brief 归还当前任务持有的发布顺序门。 @post 当前 owner 清除并解锁，非 owner 调用无操作。 @pre 成功 begin 的任务在资源租约内调用。 */
static void app_event_publish_order_end(void)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_publish_owner_mux);
    bool release = s_publish_owner == current;
    if (release) s_publish_owner = NULL;
    portEXIT_CRITICAL(&s_publish_owner_mux);
    if (release && s_publish_order_mutex) xSemaphoreGive(s_publish_order_mutex);
}

/** @brief 在进程期临界区登记一次总线资源借用。 @return 入口开放时为 true 并新增租约，关闭时为 false。 @post 成功者必须 release，destroy 会等其归还后删除动态原语。 */
bool app_event_bus_acquire(void)
{
    portENTER_CRITICAL(&s_access_mux);
    bool allowed = s_access_open;
    if (allowed) s_access_users++;
    portEXIT_CRITICAL(&s_access_mux);
    return allowed;
}

/** @brief 归还一次已成功登记的总线租约。 @pre 调用者恰有一次尚未归还的 acquire。 @post 活跃租约减一，归还后不得继续访问动态总线资源。 */
void app_event_bus_release(void)
{
    portENTER_CRITICAL(&s_access_mux);
    s_access_users--;
    portEXIT_CRITICAL(&s_access_mux);
}

/**
 * @brief 关闭进程期接纳门并等待已登记的全部资源租约归还。
 * @param[in] start 外层等待链的初始 tick。
 * @param[in] timeout_ticks 有限共享预算。
 * @return 全部租约退出为 true，到期为 false，超时后入口继续关闭，owner 保留对象重试。
 */
static bool app_event_bus_close_access(TickType_t start, TickType_t timeout_ticks)
{
    for (;;) {
        portENTER_CRITICAL(&s_access_mux);
        s_access_open = false;
        unsigned users = s_access_users;
        portEXIT_CRITICAL(&s_access_mux);
        if (!users) return true;
        if (!app_event_remaining_ticks(start, timeout_ticks)) return false;
        vTaskDelay(1);
    }
}

/**
 * @brief 提供事件种类的稳定序列化名称。
 * @param[in] kind 按值事件种类。
 * @return 静态字符串借用，未知枚举返回 unknown。
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

/**
 * @brief 提供事件来源的稳定名称。
 * @param[in] source 按值来源。
 * @return 静态字符串借用，未知枚举返回 unknown。
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

/**
 * @brief 提供总线生命周期的稳定名称。
 * @param[in] lifecycle 按值状态。
 * @return 静态字符串借用，未知枚举返回 unknown。
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

/**
 * @brief 取得状态互斥锁，同任务通过 owner/depth 支持递归。
 * @pre 调用者持资源租约、writer 存活权或生命周期独占，保证 mutex 存活。
 * @post mutex 存在时阻塞取得并登记层数，不存在时直接返回；每层成功取得须配对 unlock。
 */
void app_event_bus_lock(void)
{
    SemaphoreHandle_t mutex = g_app_event_bus.mutex;
    if (!mutex) return;
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    portENTER_CRITICAL(&s_bus_lock_owner_mux);
    if (s_bus_lock_owner == current && s_bus_lock_depth > 0) {
        s_bus_lock_depth++;
        portEXIT_CRITICAL(&s_bus_lock_owner_mux);
        return;
    }
    portEXIT_CRITICAL(&s_bus_lock_owner_mux);

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return;
    portENTER_CRITICAL(&s_bus_lock_owner_mux);
    s_bus_lock_owner = current;
    s_bus_lock_depth = 1;
    portEXIT_CRITICAL(&s_bus_lock_owner_mux);
}

/**
 * @brief 归还当前任务的一层状态锁。
 * @pre 动态原语仍存活，调用者与成功 lock 配对。
 * @post 最外层归还原生 mutex，其他任务或未持锁时无操作。
 */
void app_event_bus_unlock(void)
{
    SemaphoreHandle_t mutex = g_app_event_bus.mutex;
    if (!mutex) return;
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    bool release = false;
    portENTER_CRITICAL(&s_bus_lock_owner_mux);
    if (s_bus_lock_owner == current && s_bus_lock_depth > 0) {
        s_bus_lock_depth--;
        if (s_bus_lock_depth == 0) {
            s_bus_lock_owner = NULL;
            release = true;
        }
    }
    portEXIT_CRITICAL(&s_bus_lock_owner_mux);
    if (release) xSemaphoreGive(mutex);
}

/**
 * @brief 在实际截断时去掉末尾不完整的 UTF-8 字符。
 * @param[in] text 至少 len 字节可读的文本借用，可为 NULL。
 * @param[in] len 候选前缀字节数。
 * @return 完整字符边界的前缀长度，空值返回零；依赖输入原本为有效 UTF-8，不执行全面校验。
 */
static size_t app_event_complete_utf8_prefix(const char *text, size_t len)
{
    if (!text || !len) return 0;
    size_t start = len;
    while (start && ((unsigned char)text[start - 1] & 0xc0U) == 0x80U) start--;
    if (!start) return 0;
    size_t lead = start - 1U;
    unsigned char byte = (unsigned char)text[lead];
    size_t width = byte >= 0xc2U && byte <= 0xdfU ? 2U :
                   byte >= 0xe0U && byte <= 0xefU ? 3U :
                   byte >= 0xf0U && byte <= 0xf4U ? 4U : 1U;
    return len - lead < width ? lead : len;
}

/**
 * @brief 优先 PSRAM 分配有界文本副本，失败再尝试内部 RAM。
 * @param[in] text 至少 len 字节可读的借用，NULL 按空串处理。
 * @param[in] len 待复制字节数，超过 APP_EVENT_TEXT_MAX 时按 UTF-8 边界截断。
 * @return 调用者用 heap_caps_free 释放的 NUL 结尾副本，两次分配失败为 NULL。
 */
char *app_event_strdup_len(const char *text, size_t len)
{
    if (!text) len = 0;
    if (len > APP_EVENT_TEXT_MAX) {
        len = app_event_complete_utf8_prefix(text, APP_EVENT_TEXT_MAX);
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

/**
 * @brief 测量 NUL 文本后复制有界副本。
 * @param[in] text 调用期字符串借用，NULL 按空串处理。
 * @return 至多 APP_EVENT_TEXT_MAX 字节的拥有副本，heap_caps_free 释放，OOM 为 NULL。
 */
char *app_event_strdup(const char *text)
{
    return app_event_strdup_len(text ? text : "", text ? strlen(text) : 0);
}

/** @brief 创建保留精确长度的引用计数正文。 @param[in] value 必需可读 len 字节借用，可包含零长度。 @param[in] len 正文字节数，本入口不截断。 @return 初始一个引用的拥有对象，长度溢出或两类堆均 OOM 为 NULL，最终由 envelope_release 释放。 */
static app_event_envelope_t *app_event_envelope_create_len(const char *value, size_t len)
{
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

/** @brief 将普通事件文本复制到有界引用正文。 @param[in] text NUL 字符串借用，NULL 按空串处理，长文本在 UTF-8 边界截至 2048 字节。 @return 一个拥有引用，分配失败为 NULL。 */
app_event_envelope_t *app_event_envelope_create(const char *text)
{
    const char *value = text ? text : "";
    size_t len = strlen(value);
    if (len > APP_EVENT_TEXT_MAX)
        len = app_event_complete_utf8_prefix(value, APP_EVENT_TEXT_MAX);
    return app_event_envelope_create_len(value, len);
}

/** @brief 为 ring、快照或队列新增正文引用。 @param[in,out] envelope 仍有有效引用的对象，NULL 无操作。 @post 原子增加一个引用，调用者必须配对 release；持有者数量受有界队列和快照约束。 */
void app_event_envelope_retain(app_event_envelope_t *envelope)
{
    if (envelope) (void)__atomic_add_fetch(&envelope->refs, 1U, __ATOMIC_ACQ_REL);
}

/** @brief 归还正文引用并在最后一个归还时释放。 @param[in,out] envelope 交接一个拥有引用，NULL 无操作。 @post 引用归还后调用者不得再用该引用访问对象。 */
void app_event_envelope_release(app_event_envelope_t *envelope)
{
    if (!envelope) return;
    if (__atomic_sub_fetch(&envelope->refs, 1U, __ATOMIC_ACQ_REL) == 0U) {
        heap_caps_free(envelope);
    }
}

/**
 * @brief 复制允许重叠的文本并按 UTF-8 边界截断。
 * @param[out] dst 输出缓冲，NULL 或零容量无操作，可与 src 重叠。
 * @param[in] dst_size 输出字节容量，含终止 NUL。
 * @param[in] src 调用期 NUL 字符串借用，NULL 按空串。
 * @post 有效输出始终 NUL 结尾，截断时避免保留半个有效 UTF-8 字符。
 */
void app_event_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    const char *text = src ? src : "";
    size_t length = 0;
    while (length < dst_size - 1U && text[length]) length++;
    if (text[length]) length = app_event_complete_utf8_prefix(text, length);
    memmove(dst, text, length);
    dst[length] = '\0';
}

/**
 * @brief 在容量充足时一次追加完整文本。
 * @param[in,out] out 已有 NUL 字符串的可写缓冲，NULL 无效。
 * @param[in] out_size 总字节容量，含 NUL。
 * @param[in] text 必需 NUL 字符串借用，与输出写入区不重叠。
 * @return 追加成功为 true，参数或容量不足为 false 并保留原输出。
 */
bool app_event_append_text(char *out, size_t out_size, const char *text)
{
    if (!out || out_size == 0 || !text) return false;
    size_t out_len = strlen(out);
    size_t text_len = strlen(text);
    if (out_len >= out_size || text_len >= out_size - out_len) return false;
    memcpy(out + out_len, text, text_len + 1);
    return true;
}

/**
 * @brief 拼接目录和名称，仅在非空目录末尾缺失时补一个斜杠。
 * @param[out] out 与两个输入不重叠的输出缓冲。
 * @param[in] out_size 缓冲字节数，含 NUL。
 * @param[in] dir 必需 NUL 目录借用，允许空串。
 * @param[in] name 必需 NUL 名称借用，允许空串。
 * @return 完整输出为 true，参数或容量不足为 false，失败保留原输出。
 */
bool app_event_join_path(char *out, size_t out_size, const char *dir, const char *name)
{
    if (!out || out_size == 0 || !dir || !name) return false;
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    if (dir_len >= out_size || name_len >= out_size - dir_len) return false;
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

/**
 * @brief 通过局部十进制缓冲追加无符号整数。
 * @param[in,out] out 已有 NUL 字符串的输出缓冲。
 * @param[in] out_size 总字节容量，含 NUL。
 * @param[in] value 按值 uint32_t 整数。
 * @return 完整追加为 true，参数或容量不足为 false 并保留原输出。
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

/**
 * @brief 选择串口行的短标签。
 * @param[in] kind 按值事件种类。
 * @return 静态字符串借用，LOG、PROFILE 和未知值共用 log。
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

/**
 * @brief 创建脱敏文本视图，分配失败时使用固定失败提示。
 * @param[in] text 调用期原文借用，NULL 按空串。
 * @return 成功拥有 cc_redact_secrets 输出并标 owned，失败借用静态提示；交给 free_event_text_view 清理。
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
    } else {
        view.text = "[REDACTION_FAILED]";
    }
    return view;
}

/**
 * @brief 释放视图独占的脱敏文本。
 * @param[in,out] view 可为 NULL，owned 为 true 时交接拥有权。
 * @post 拥有文本释放且槽清零，借用视图原样保留。
 */
static void free_event_text_view(app_event_text_view_t *view)
{
    if (view && view->owned) {
        free((void *)view->text);
        view->text = NULL;
        view->owned = false;
    }
}

/**
 * @brief 对 ASCII 英文字母折叠大小写。
 * @param[in] c 按值字节。
 * @return A 到 Z 转为小写，其余字节保留。
 */
static char app_event_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

/**
 * @brief 以 ASCII 不区分大小写方式搜索维护关键词。
 * @param[in] text NUL 文本借用，可为 NULL。
 * @param[in] needle NUL 子串借用，可为 NULL。
 * @return 找到非空子串为 true，任一 NULL、空 needle 或未找到为 false。
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

/**
 * @brief 匹配现有 OTA 维护关键词集合。
 * @param[in] text NUL 文本借用，可为 NULL。
 * @return 含 ota/firmware/partition/reboot/upload/progress 任一 ASCII 忽略大小写子串为 true。
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

/**
 * @brief 判定维护模式事件白名单。
 * @param[in] kind 按值种类，ERROR 无条件通过。
 * @param[in] source 按值来源，当前策略未使用。
 * @param[in] level LOG/STATUS 达到 WARN 时通过。
 * @param[in] text NUL 文本借用，可为 NULL，LOG/STATUS 可按关键词通过。
 * @return 满足上述任一条件为 true。
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

/**
 * @brief 在状态锁内执行维护过滤并累计饱和统计。
 * @param[in] kind 按值种类。
 * @param[in] source 按值来源。
 * @param[in] level 按值级别。
 * @param[in] text 调用期文本借用，可为 NULL。
 * @return 已计数并应丢弃为 true，非维护模式或白名单通过为 false。
 * @pre 调用者持状态锁。
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
    app_event_counter_increment(&g_app_event_bus.dropped);
    app_event_counter_increment(&g_app_event_bus.sink_dropped);
    app_event_counter_increment(&g_app_event_bus.maintenance_skipped);
    return true;
}

/**
 * @brief 在发布顺序门内脱敏、复制并投递一次事件。
 * @param[in] kind 按值种类，未知值仍可保留并按 unknown 序列化。
 * @param[in] source 按值来源，串口用户输入避免回显。
 * @param[in] level 按值级别，交路由阈值处理。
 * @param[in] text 调用期 NUL 文本借用，NULL 按空串。
 * @param[in] boot_status true 使用 LCD 启动层专用投递。
 * @pre 外层持资源租约和发布顺序门。
 * @post 正文引用分别交 ring/队列持有，局部引用释放；维护过滤或 OOM 时计数后返回。序号耗尽会清空近期显示并从 1 开始，clear_generation 通知轮询端刷新。
 */
static void app_event_publish_active(app_event_kind_t kind,
                       app_event_source_t source,
                       cc_log_level_t level,
                       const char *text,
                       bool boot_status)
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
        app_event_counter_increment(&g_app_event_bus.dropped);
        app_event_counter_increment(&g_app_event_bus.sink_dropped);
        app_event_counter_increment(&g_app_event_bus.oom_dropped);
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
    bool reset_sequence = g_app_event_bus.next_seq == UINT32_MAX;
    if (reset_sequence) {
        app_event_ring_clear_locked();
        g_app_event_bus.clear_generation++;
        g_app_event_bus.next_seq = 0;
    }
    bool serial_enabled = g_app_event_bus.serial_enabled;
    record.seq = ++g_app_event_bus.next_seq;
    app_event_counter_increment(&g_app_event_bus.published);
    seq = record.seq;
    route = (maintenance_mode || draining) ? APP_EVENT_SINK_SERIAL :
            app_event_route_for_kind_locked(kind);
    /* 发布者保留自身引用覆盖解锁后的 sink 投递，ring 另持引用并可被清空。 */
    (void)app_event_ring_append_locked(&record);
    bool to_serial = maintenance_mode ? true : app_event_route_to_serial(kind, level, route);
    bool to_display = (maintenance_mode || draining) ? false :
                      app_event_route_to_display(kind, route);
    bool to_usb = (maintenance_mode || draining) ? false :
                  app_event_route_to_usb_log(kind, level, route);
    if (to_serial && kind == APP_EVENT_KIND_USER && source == APP_EVENT_SOURCE_SERIAL) {
        to_serial = false;
    }
    app_event_bus_unlock();

    if (reset_sequence) {
        app_event_display_clear();
        if (serial_enabled) {
            fputs("\033[2J\033[H", stdout);
            fflush(stdout);
        }
    }
    if (to_serial) {
        if (kind == APP_EVENT_KIND_LOG) {
            printf("%s\r\n", envelope->text);
        } else {
            printf("[%s] %s\r\n", serial_prefix_for_event(kind), envelope->text);
        }
        fflush(stdout);
    }

    if (boot_status && !maintenance_mode && !draining) {
        app_event_display_emit_boot(kind, envelope);
    } else if (to_display) {
        app_event_display_emit(kind, envelope);
    }

    if (to_usb) {
        app_event_usb_enqueue(envelope, seq, kind, level);
    }
    app_event_envelope_release(envelope);
    free_event_text_view(&safe);
}

/** @brief 包装资源租约与发布顺序，关闭入口时仅脱敏串口输出。 @param[in] kind 按值种类。 @param[in] source 按值来源。 @param[in] level 按值级别。 @param[in] text 调用期文本借用，NULL 按空串。 @param[in] boot_status 是否投递启动显示层。 @post 所有租约与门配对归还，同任务递归发布直接丢弃以免日志重入。 */
static void app_event_publish_ordered(app_event_kind_t kind, app_event_source_t source,
                                      cc_log_level_t level, const char *text, bool boot_status)
{
    if (!app_event_bus_acquire()) {
        char *safe = cc_redact_secrets(text ? text : "");
        if (safe) { printf("[%s] %s\r\n", serial_prefix_for_event(kind), safe); free(safe); }
        else printf("[error] event redaction allocation failed\r\n");
        fflush(stdout);
        return;
    }
    if (!app_event_publish_order_begin()) {
        /* 发布过程引发的日志不再递归进入同一总线。 */
        app_event_bus_release();
        return;
    }
    app_event_publish_active(kind, source, level, text, boot_status);
    app_event_publish_order_end();
    app_event_bus_release();
}

/** @brief 按普通路由发布事件。 @param[in] kind 按值种类。 @param[in] source 按值来源。 @param[in] level 按值级别。 @param[in] text 调用期文本借用，NULL 按空串。 @post 完成同步串口投递与异步入队尝试，正文由各拥有引用维持寿命。 */
void app_event_publish(app_event_kind_t kind, app_event_source_t source,
                       cc_log_level_t level, const char *text)
{ app_event_publish_ordered(kind,source,level,text,false); }

/** @brief 按发布顺序投递启动状态，LCD 启动层独立于普通显示路由。 @param[in] kind 按值种类。 @param[in] source 按值来源。 @param[in] level 按值级别。 @param[in] text 调用期文本借用，NULL 按空串。 @post 与清屏保持顺序，维护或退场状态仍按普通核心规则过滤。 */
void app_event_publish_boot_status(app_event_kind_t kind, app_event_source_t source,
                                   cc_log_level_t level, const char *text)
{ app_event_publish_ordered(kind,source,level,text,true); }

/**
 * @brief 在栈上格式化有界正文后普通发布。
 * @param[in] kind 按值种类。
 * @param[in] source 按值来源。
 * @param[in] level 按值级别。
 * @param[in] fmt printf 格式借用，NULL 无操作。
 * @param[in] ... 与格式匹配的参数，指针覆盖格式化调用。
 * @post 格式化失败无发布，超长输出截至完整 UTF-8 前缀再脱敏发布。
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
    int written = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (written < 0) return;
    if (written >= (int)sizeof(text))
        text[app_event_complete_utf8_prefix(text, APP_EVENT_TEXT_MAX)] = '\0';
    app_event_publish(kind, source, level, text);
}

/**
 * @brief 按发布顺序清除近期 ring、固定历史页和 LCD，并清屏串口终端。
 * @post clear_generation 增加并允许从 MAX 回零，轮询 Web 比较代数变化刷新；持久化会话保持原有记录。入口关闭或同任务发布重入时无操作。
 */
void app_event_bus_clear_conversation(void)
{
    if (!app_event_bus_acquire()) return;
    if (!app_event_publish_order_begin()) {
        app_event_bus_release();
        return;
    }
    app_event_display_clear();
    app_event_bus_lock();
    app_event_ring_clear_locked();
    g_app_event_bus.clear_generation++;
    bool serial_enabled = g_app_event_bus.serial_enabled;
    app_event_bus_unlock();
    if (serial_enabled) {
        /* 使用 VT 清屏控制序列清除兼容串口终端的当前显示。 */
        fputs("\033[2J\033[H", stdout);
        fflush(stdout);
    }
    app_event_publish_order_end();
    app_event_bus_release();
}

/**
 * @brief 先构造完整脱敏历史页，再按发布顺序提交 Web、LCD 和串口视图。
 * @param[in] items count 项调用期借用，count 为零时可 NULL，每项 text 可 NULL。
 * @param[in] count 条数，上限 16，含各 NUL 的原文与脱敏正文预算均为 32 KiB。
 * @return 成功 ESP_OK；参数、大小、OOM、退场或显示队列失败返回对应错误，提交前失败保持旧可见页。
 * @note 正文独立复制，全部临时引用在退出时释放。提交会换 clear_generation，剩余序号不足时先从零起算，页首获得 1。
 */
esp_err_t app_event_bus_replace_conversation(const app_event_view_item_t *items, size_t count)
{
    if (count > APP_EVENT_VIEW_MAX_ITEMS || (count && !items)) return ESP_ERR_INVALID_ARG;
    app_event_view_record_t records[APP_EVENT_VIEW_MAX_ITEMS] = {0};
    size_t total_bytes = 0;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < count; i++) {
        if (items[i].kind < 0 || items[i].kind >= APP_EVENT_KIND_COUNT) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        const char *text = items[i].text ? items[i].text : "";
        size_t remaining = APP_EVENT_VIEW_MAX_BYTES - total_bytes;
        size_t len = 0;
        while (len < remaining && text[len]) len++;
        if (len == remaining) { err = ESP_ERR_INVALID_SIZE; break; }
        char *safe = cc_redact_secrets(text);
        if (!safe) { err = ESP_ERR_NO_MEM; break; }
        len = strlen(safe);
        if (len >= remaining) {
            free(safe);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        records[i].kind = items[i].kind;
        records[i].envelope = app_event_envelope_create_len(safe, len);
        free(safe);
        if (!records[i].envelope) { err = ESP_ERR_NO_MEM; break; }
        total_bytes += len + 1U;
    }
    if (err != ESP_OK) goto done;
    if (!app_event_bus_acquire()) { err = ESP_ERR_INVALID_STATE; goto done; }
    if (!app_event_publish_order_begin()) {
        app_event_bus_release();
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    app_event_bus_lock();
    bool accepting = !g_app_event_bus.maintenance_mode &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_DRAINING &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_STOPPED &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_FAILED;
    bool display_enabled = g_app_event_bus.display_enabled;
    bool serial_enabled = g_app_event_bus.serial_enabled;
    bool ring_ok = accepting && app_event_ring_ensure_locked();
    app_event_bus_unlock();
    if (display_enabled && !app_event_display_available()) display_enabled=false;
    if (!accepting) err = ESP_ERR_INVALID_STATE;
    else if (!ring_ok) err = ESP_ERR_NO_MEM;
    else if (display_enabled) err = app_event_display_replace(records, count);
    if (err == ESP_OK) {
        app_event_bus_lock();
        app_event_ring_clear_locked();
        g_app_event_bus.clear_generation++;
        if (count > (size_t)(UINT32_MAX - g_app_event_bus.next_seq))
            g_app_event_bus.next_seq = 0;
        for (size_t i = 0; i < count; i++) {
            app_event_record_t record = {
                .seq = ++g_app_event_bus.next_seq,
                .timestamp_ms = (int64_t)pdTICKS_TO_MS(xTaskGetTickCount()),
                .kind = records[i].kind,
                .source = APP_EVENT_SOURCE_RUNTIME,
                .level = CC_LOG_INFO,
                .envelope = records[i].envelope,
            };
            app_event_counter_increment(&g_app_event_bus.published);
            (void)app_event_ring_append_locked(&record);
            g_app_event_bus.view[i] = record;
            app_event_envelope_retain(record.envelope);
        }
        g_app_event_bus.view_count = count;
        g_app_event_bus.view_latest_seq = g_app_event_bus.next_seq;
        g_app_event_bus.view_active = true;
        app_event_bus_unlock();
        if (serial_enabled) {
            fputs("\033[2J\033[H", stdout);
            for (size_t i = 0; i < count; i++)
                printf("[%s] %s\r\n", serial_prefix_for_event(records[i].kind), records[i].envelope->text);
            fflush(stdout);
        }
    }
    app_event_publish_order_end();
    app_event_bus_release();
done:
    for (size_t i = 0; i < count; i++) app_event_envelope_release(records[i].envelope);
    return err;
}

/**
 * @brief 事务式补齐状态锁、idle 事件组和发布顺序门。
 * @pre 生命周期 owner 独占初始化，尚未向外发布新原语。
 * @return 全部可用为 true，任一创建失败为 false，释放本次候选并保留已有原语供重试或 destroy。
 */
static bool initialize_primitives(void)
{
    SemaphoreHandle_t mutex = g_app_event_bus.mutex;
    EventGroupHandle_t idle_events = g_app_event_bus.idle_events;
    SemaphoreHandle_t order_mutex = s_publish_order_mutex;
    if (!mutex) mutex = xSemaphoreCreateMutex();
    if (!idle_events) idle_events = xEventGroupCreate();
    if (!order_mutex) order_mutex = xSemaphoreCreateMutex();
    if (!mutex || !idle_events || !order_mutex) {
        if (mutex && !g_app_event_bus.mutex) vSemaphoreDelete(mutex);
        if (idle_events && !g_app_event_bus.idle_events) vEventGroupDelete(idle_events);
        if (order_mutex && !s_publish_order_mutex) vSemaphoreDelete(order_mutex);
        return false;
    }
    if (!g_app_event_bus.idle_events)
        xEventGroupSetBits(idle_events, APP_EVENT_BUS_IDLE_BIT);
    g_app_event_bus.mutex = mutex;
    g_app_event_bus.idle_events = idle_events;
    s_publish_order_mutex = order_mutex;
    return true;
}

/**
 * @brief 将 IO 标量和路由复制入状态后启动或停止 USB 输出。
 * @param[in] io 必需稳定配置借用，整个调用期间由配置 owner 保持有效。
 * @return 成功 ESP_OK；配置无效在修改前返回参数/大小错误，未初始化返回 INVALID_STATE，文件或任务错误透传且配置已复制，USB 启动失败尝试串口回退。
 * @pre 外层持资源租约，生命周期 owner 串行调用配置和启停。
 */
static esp_err_t app_event_bus_apply_config_active(const app_runtime_io_config_t *io)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) {
        return ESP_ERR_INVALID_STATE;
    }
    const char *directory = io->usb_log.dir && io->usb_log.dir[0] ?
        io->usb_log.dir : "/usb0/cclaw/logs";
    if (strlen(directory) >= sizeof(g_app_event_bus.usb_dir)) return ESP_ERR_INVALID_SIZE;
    const cc_config_string_list_t *lists[] = {
        &io->routes.log, &io->routes.chat, &io->routes.thinking, &io->routes.tool,
        &io->routes.status, &io->routes.error, &io->routes.profile,
    };
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        if (lists[i]->count && !lists[i]->items) return ESP_ERR_INVALID_ARG;
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
                        directory);
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

/**
 * @brief 创建总线原语、两个 sink 和近期 ring，再打开资源接纳门。
 * @param[in] io 调用期配置借用，可为 NULL 以启用串口回退。
 * @return 成功 ESP_OK，分配或配置/USB 启动失败返回对应错误；已建资源仍由总线拥有，可重试或 destroy。
 * @pre 生命周期 owner 独占，与 apply/stop/destroy 串行，不能在运行中的旧 owner 之外并发初始化。
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
    portENTER_CRITICAL(&s_access_mux);
    s_access_open = true;
    portEXIT_CRITICAL(&s_access_mux);
    app_event_log_hooks_install();
    if (io) {
        return app_event_bus_apply_config(io);
    }
    app_event_bus_start_serial_fallback_debug();
    return ESP_OK;
}

/**
 * @brief 在生命周期 owner 的共享预算内关闭异步 sink 接纳并等待退场。
 * @param[in] start 外层初始 tick。
 * @param[in] timeout_ticks 有限共享 tick 预算。
 * @return 完整停止为 ESP_OK，原语不足为 INVALID_STATE，等待或 USB 退场失败保留资源并返回错误。
 */
static esp_err_t app_event_bus_stop_active(TickType_t start, TickType_t timeout_ticks)
{
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) {
        return ESP_ERR_INVALID_STATE;
    }
    app_event_bus_lock();
    if (g_app_event_bus.lifecycle == APP_EVENT_BUS_STOPPED) {
        app_event_bus_unlock();
        return ESP_OK;
    }
    g_app_event_bus.lifecycle = APP_EVENT_BUS_DRAINING;
    app_event_bus_unlock();

    if (!app_event_bus_wait_idle_until(start, timeout_ticks)) {
        app_event_bus_lock();
        g_app_event_bus.lifecycle = APP_EVENT_BUS_FAILED;
        app_event_bus_unlock();
        return ESP_ERR_TIMEOUT;
    }
    app_event_bus_lock();
    g_app_event_bus.usb_log_enabled = false;
    g_app_event_bus.display_enabled = false;
    app_event_bus_unlock();
    esp_err_t err = app_event_usb_stop_until(start, timeout_ticks);
    app_event_bus_lock();
    g_app_event_bus.lifecycle = err == ESP_OK ? APP_EVENT_BUS_STOPPED : APP_EVENT_BUS_FAILED;
    app_event_bus_unlock();
    return err;
}

/** @brief 以单一有限预算停止异步输出。 @param[in] timeout_ms 毫秒预算，零使用 2000。 @return 停止结果，失败对象保留可重试。 @pre 生命周期 owner 串行调用 init/apply/stop/destroy，发布和查询可并发。 */
esp_err_t app_event_bus_stop(uint32_t timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    return app_event_bus_stop_active(start, app_event_timeout_ticks(timeout_ms ? timeout_ms : 2000U));
}

/**
 * @brief 先停止 sink，再关闭资源接纳并等待所有已登记调用归还后销毁。
 * @param[in] timeout_ms 毫秒总预算，零使用 2000，覆盖 drain、writer 完成和外部租约退场。
 * @return 完整释放或未初始化为 ESP_OK，停止/关闭失败原样保留剩余资源并返回错误供重试。
 * @pre 生命周期 owner 串行调用，禁止从持有本总线租约的同步回调内销毁自身。
 * @note native FILE 与短状态锁自身受后端调度影响，有限预算约束可等待阶段；writer 完成位覆盖 SetBits 尾部，kernel 自删回收可随后完成。
 */
esp_err_t app_event_bus_destroy(uint32_t timeout_ms)
{
    if (!g_app_event_bus.mutex) return ESP_OK;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = app_event_timeout_ticks(timeout_ms ? timeout_ms : 2000U);
    esp_err_t err = app_event_bus_stop_active(start, timeout_ticks);
    if (err != ESP_OK) return err;

    /* 先停止新 sink 接纳，再关闭租约门，等 publisher、reader 和 LVGL pump 归还后释放原语。 */
    if (!app_event_bus_close_access(start, timeout_ticks))
        return ESP_ERR_TIMEOUT;
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
    SemaphoreHandle_t publish_order_mutex = s_publish_order_mutex;
    g_app_event_bus.usb_queue = NULL;
    g_app_event_bus.display_queue = NULL;
    g_app_event_bus.idle_events = NULL;
    g_app_event_bus.file_mutex = NULL;
    g_app_event_bus.mutex = NULL;
    s_publish_order_mutex = NULL;
    if (usb_queue) {
#if configSUPPORT_STATIC_ALLOCATION
        if (g_app_event_bus.usb_queue_with_caps) vQueueDeleteWithCaps(usb_queue);
        else
#endif
            vQueueDelete(usb_queue);
    }
    if (display_queue) {
#if configSUPPORT_STATIC_ALLOCATION
        if (g_app_event_bus.display_queue_with_caps) vQueueDeleteWithCaps(display_queue);
        else
#endif
            vQueueDelete(display_queue);
    }
    if (idle_events) vEventGroupDelete(idle_events);
    if (file_mutex) vSemaphoreDelete(file_mutex);
    if (mutex) vSemaphoreDelete(mutex);
    if (publish_order_mutex) vSemaphoreDelete(publish_order_mutex);
    memset(&g_app_event_bus, 0, sizeof(g_app_event_bus));
    return ESP_OK;
}

/**
 * @brief 停止 USB 后启用串口 DEBUG 回退并更新日志 hook 级别。
 * @return USB 在 5000 毫秒内完成退场为 ESP_OK，原语不足或停止失败透传且不提交回退标志。
 * @pre 外层持资源租约，生命周期 owner 串行调用，已有路由仅补 LOG/STATUS/ERROR 的 SERIAL 位，未配置则安装缺省路由。
 */
static esp_err_t app_event_bus_start_serial_fallback_debug_active(void)
{
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) return ESP_ERR_INVALID_STATE;
    esp_err_t stop_err = app_event_usb_stop(5000U);
    if (stop_err != ESP_OK) return stop_err;
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
    return ESP_OK;
}

/**
 * @brief 登记一次 USB 时间命名请求。
 * @post 状态锁内设置 rename_pending，writer 后续检查真实时间和已命名标志；入口关闭时无操作。
 */
void app_event_bus_note_time_synced(void)
{
    if (!app_event_bus_acquire()) return;
    app_event_bus_lock();
    g_app_event_bus.rename_pending = true;
    app_event_bus_unlock();
    app_event_bus_release();
}

/**
 * @brief 在资源租约内更新调试标志及日志阈值。
 * @param[in] enabled true 设 DEBUG，false 设 INFO。
 * @post 清除自动回退标志并同步 ESP 日志级别，串口 enabled 开关和路由保持现值。
 */
static void app_event_bus_set_serial_debug_active(bool enabled)
{
    if (!g_app_event_bus.mutex) return;
    app_event_bus_lock();
    g_app_event_bus.serial_debug_enabled = enabled;
    g_app_event_bus.serial_fallback_debug_enabled = false;
    g_app_event_bus.serial_log_level = enabled ? CC_LOG_DEBUG : CC_LOG_INFO;
    app_event_bus_unlock();
    app_event_log_hooks_apply_esp_level();
}

/**
 * @brief 在状态锁内切换维护过滤并复制原因，再同步日志级别。
 * @param[in] enabled 目标维护状态。
 * @param[in] reason 调用期 NUL 借用，可 NULL，启用时截入 64 字节状态槽，关闭时清空。
 * @post 状态或原因改变时串口输出通知，后续事件按维护白名单过滤并仅投递串口；已接纳 sink 工作继续。
 * @pre 外层持资源租约。
 */
static void app_event_bus_set_maintenance_mode_active(bool enabled, const char *reason)
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

/**
 * @brief 通过资源租约和状态锁读取串口调试快照。
 * @return 当前 serial_debug_enabled，自动回退也会设置此位，入口关闭返回 false。
 */
bool app_event_bus_serial_debug_enabled(void)
{
    if (!app_event_bus_acquire()) return false;
    app_event_bus_lock();
    bool enabled = g_app_event_bus.serial_debug_enabled;
    app_event_bus_unlock();
    app_event_bus_release();
    return enabled;
}

/**
 * @brief 通过资源租约和状态锁读取维护模式。
 * @return 当前维护位，入口关闭返回 false。
 */
bool app_event_bus_maintenance_mode_enabled(void)
{
    if (!app_event_bus_acquire()) return false;
    bool enabled = false;
    app_event_bus_lock();
    enabled = g_app_event_bus.maintenance_mode;
    app_event_bus_unlock();
    app_event_bus_release();
    return enabled;
}

/**
 * @brief 在共享预算内等待 USB/display 的已接纳计数归零。
 * @param[in] start 等待链初始 tick。
 * @param[in] timeout_ticks 有限 tick 总预算。
 * @return 观察两类计数归零为 true，原语不足或到期为 false；普通运行时后续发布仍可新增工作。
 * @pre 资源租约或生命周期独占覆盖等待，事件组仅提示重新核对计数。
 */
static bool app_event_bus_wait_idle_until(TickType_t start, TickType_t timeout_ticks)
{
    if (!g_app_event_bus.mutex || !g_app_event_bus.idle_events) return false;
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
        TickType_t wait_ticks = app_event_remaining_ticks(start, timeout_ticks);
        if (!wait_ticks) {
            return false;
        }
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

/** @brief 为独立 idle 等待建立有限预算。 @param[in] timeout_ms 毫秒数，零使用 5000。 @return 已登记的两类 sink 工作归零为 true，等待到期为 false。 @pre 外层持资源租约。 */
static bool app_event_bus_wait_idle_active(uint32_t timeout_ms)
{
    return app_event_bus_wait_idle_until(xTaskGetTickCount(),
        app_event_timeout_ticks(timeout_ms ? timeout_ms : 5000U));
}

/** @brief 清除 idle 提示，标记已登记的异步工作。 @pre 先在状态锁内增加 pending，且资源租约或 writer 存活权覆盖事件组。 @post 有事件组时清 idle 位。 */
void app_event_bus_mark_sink_busy(void)
{
    if (g_app_event_bus.idle_events) {
        xEventGroupClearBits(g_app_event_bus.idle_events, APP_EVENT_BUS_IDLE_BIT);
    }
}

/** @brief 核对两个 pending 计数并在均零时发布 idle 提示。 @pre 调用者保证原语存活，等待者还须自行再次核对计数。 @post 当前无已登记工作时置 idle 位。 */
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

/**
 * @brief 在单次状态锁内复制运行统计和已拥有路径。
 * @param[out] out_status 可选输出，NULL 无操作，有效槽先整体清零。
 * @post 接纳门关闭时保留全零，成功时结构无外部借用；累计计数和 USB 字节饱和，近期 bytes 含正文头与 NUL。
 */
void app_event_bus_get_status(app_event_bus_status_t *out_status)
{
    if (!out_status) return;
    memset(out_status, 0, sizeof(*out_status));
    if (!app_event_bus_acquire()) return;
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
    out_status->retained_count = g_app_event_bus.recent_count;
    out_status->retained_bytes = g_app_event_bus.recent_bytes;
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
    app_event_bus_release();
}

/**
 * @brief 在状态锁内复制 USB 当前路径，避免泄漏共享字符串借用。
 * @param[out] out_path 可选缓冲，NULL 或零容量无操作。
 * @param[in] out_size 输出字节容量，含 NUL。
 * @post 有效输出先置空，入口关闭保留空串，容量不足按 UTF-8 边界截断。
 */
void app_event_bus_copy_current_log_path(char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0) return;
    out_path[0] = '\0';
    if (!app_event_bus_acquire()) return;
    app_event_bus_lock();
    app_event_copy_text(out_path, out_size, g_app_event_bus.usb_current_path);
    app_event_bus_unlock();
    app_event_bus_release();
}

/** @brief 在资源租约内应用生命周期 owner 的 IO 快照。 @param[in] io 必需调用期配置借用。 @return 入口关闭为 INVALID_STATE，否则透传配置与 USB 启停结果，失败时已提交状态由 active 契约说明。 @pre 与 init/stop/destroy/回退串行。 */
esp_err_t app_event_bus_apply_config(const app_runtime_io_config_t *io)
{
    if (!app_event_bus_acquire()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = app_event_bus_apply_config_active(io);
    app_event_bus_release();
    return err;
}

/** @brief 为生命周期 owner 登记租约后切换串口回退。 @return 入口关闭为 INVALID_STATE，否则返回 USB 停止和回退结果。 @pre 与配置和启停串行。 */
esp_err_t app_event_bus_start_serial_fallback_debug(void)
{
    if (!app_event_bus_acquire()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = app_event_bus_start_serial_fallback_debug_active();
    app_event_bus_release();
    return err;
}

/** @brief 在租约内更新串口调试阈值。 @param[in] enabled 目标开关。 @post 成功接纳时应用 DEBUG/INFO 并清自动回退位，入口关闭无操作。 */
void app_event_bus_set_serial_debug(bool enabled)
{
    if (!app_event_bus_acquire()) return;
    app_event_bus_set_serial_debug_active(enabled);
    app_event_bus_release();
}

/** @brief 在租约内设置维护过滤。 @param[in] enabled 目标状态。 @param[in] reason 调用期原因借用，可为 NULL。 @post 接纳成功时复制原因并更新过滤，入口关闭无操作。 */
void app_event_bus_set_maintenance_mode(bool enabled, const char *reason)
{
    if (!app_event_bus_acquire()) return;
    app_event_bus_set_maintenance_mode_active(enabled, reason);
    app_event_bus_release();
}

/** @brief 在资源租约内等待异步工作计数归零。 @param[in] timeout_ms 毫秒预算，零为 5000。 @return 接纳关闭、原语不足或到期为 false，观察已接纳工作归零为 true；文件 fflush 由 writer 周期或 stop 另行完成。 */
bool app_event_bus_wait_idle(uint32_t timeout_ms)
{
    if (!app_event_bus_acquire()) return false;
    bool idle = app_event_bus_wait_idle_active(timeout_ms);
    app_event_bus_release();
    return idle;
}
