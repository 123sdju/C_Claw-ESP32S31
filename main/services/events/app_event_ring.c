#include "app_event_bus_internal.h"

#include <string.h>

#include "esp_heap_caps.h"

/*
 * 确保环形缓冲区已分配（惰性初始化），优先使用 PSRAM，回退到内部 RAM。
 * 在持锁状态下调用，分配大小固定为 APP_EVENT_RECENT_MAX。
 *
 * 参数: 无
 * 返回: true 表示缓冲区可用，false 表示内存分配失败
 */
bool app_event_ring_ensure_locked(void)
{
    if (g_app_event_bus.recent) return true;
    g_app_event_bus.recent = heap_caps_calloc(APP_EVENT_RECENT_MAX,
                                              sizeof(app_event_record_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_app_event_bus.recent) {
        g_app_event_bus.recent = heap_caps_calloc(APP_EVENT_RECENT_MAX,
                                                  sizeof(app_event_record_t),
                                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!g_app_event_bus.recent) {
        g_app_event_bus.recent_capacity = 0;
        return false;
    }
    g_app_event_bus.recent_capacity = APP_EVENT_RECENT_MAX;
    return true;
}

/*
 * 在持锁状态下向环形缓冲区追加一条事件记录。若缓冲区已满，
 * 覆盖最旧的记录并更新 overwrites 计数器。记录的 text 字段由调用者分配，
 * 所有权随追加转移给环形缓冲区。
 *
 * 参数: record - 要追加的事件记录指针
 * 返回: true 表示追加成功，false 表示缓冲区分配失败
 */
bool app_event_ring_append_locked(const app_event_record_t *record)
{
    if (!app_event_ring_ensure_locked()) {
        g_app_event_bus.dropped++;
        g_app_event_bus.sink_dropped++;
        g_app_event_bus.oom_dropped++;
        return false;
    }
    size_t index = 0;
    if (g_app_event_bus.recent_count < g_app_event_bus.recent_capacity) {
        index = (g_app_event_bus.recent_start + g_app_event_bus.recent_count) % g_app_event_bus.recent_capacity;
        g_app_event_bus.recent_count++;
    } else {
        index = g_app_event_bus.recent_start;
        g_app_event_bus.recent_start = (g_app_event_bus.recent_start + 1) % g_app_event_bus.recent_capacity;
        app_event_envelope_release(g_app_event_bus.recent[index].envelope);
        g_app_event_bus.dropped++;
        g_app_event_bus.ring_overwrites++;
    }
    g_app_event_bus.recent[index] = *record;
    return true;
}

/*
 * 在持锁状态下清空环形缓冲区，释放所有已存储记录的 text 字段内存，
 * 重置计数和起始位置，但不释放环形缓冲区本身。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_ring_clear_locked(void)
{
    for (size_t i = 0; g_app_event_bus.recent && i < g_app_event_bus.recent_capacity; i++) {
        app_event_envelope_release(g_app_event_bus.recent[i].envelope);
        g_app_event_bus.recent[i].envelope = NULL;
    }
    g_app_event_bus.recent_count = 0;
    g_app_event_bus.recent_start = 0;
}

/*
 * 对字符串进行 JSON 转义并追加到字符串构建器，包括对双引号、反斜杠、
 * 换行、回车、制表符和控制字符的转义处理。
 *
 * 参数: sb   - cc_string_builder 构建器指针
 * 参数: text - 要转义的原始文本
 * 返回: 无
 */
void app_event_json_escape_append(cc_string_builder_t *sb, const char *text)
{
    cc_string_builder_append_char(sb, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '\\': cc_string_builder_append(sb, "\\\\"); break;
        case '"': cc_string_builder_append(sb, "\\\""); break;
        case '\n': cc_string_builder_append(sb, "\\n"); break;
        case '\r': cc_string_builder_append(sb, "\\r"); break;
        case '\t': cc_string_builder_append(sb, "\\t"); break;
        default:
            if (*p < 0x20) cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p);
            else cc_string_builder_append_char(sb, (char)*p);
            break;
        }
    }
    cc_string_builder_append_char(sb, '"');
}

/*
 * 从环形缓冲区导出 JSON 格式的事件流快照，支持增量查询（since_seq）、
 * 数量限制（limit）和种类过滤（kind_mask）。返回的 JSON 字符串由调用者释放。
 * 内部先持锁快照复制事件数据，释放锁后再构建 JSON 字符串。
 *
 * 参数: since_seq - 增量序列号（仅返回 seq > since_seq 的事件，0 表示全部）
 * 参数: limit     - 最大返回条数
 * 参数: kind_mask - 事件种类掩码（0 表示不过滤）
 * 返回: 堆上分配的 JSON 字符串，失败返回 NULL
 */
char *app_event_bus_json(uint32_t since_seq, int limit, uint32_t kind_mask)
{
    size_t max_recent = g_app_event_bus.recent_capacity ? g_app_event_bus.recent_capacity : APP_EVENT_RECENT_MAX;
    if (limit <= 0 || limit > (int)max_recent) {
        limit = (int)max_recent;
    }

    app_event_record_t *snapshot = heap_caps_calloc((size_t)limit,
                                                    sizeof(app_event_record_t),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snapshot) {
        snapshot = heap_caps_calloc((size_t)limit,
                                    sizeof(app_event_record_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!snapshot) return NULL;

    uint32_t latest = 0;
    uint32_t oldest = 0;
    uint32_t dropped = 0;
    uint32_t ring_overwrites = 0;
    uint32_t sink_dropped = 0;
    uint32_t published = 0;
    uint32_t maintenance_skipped = 0;
    uint32_t oom_dropped = 0;
    uint32_t usb_queue_dropped = 0;
    uint32_t display_queue_dropped = 0;
    uint32_t max_usb_queue_waiting = 0;
    uint32_t max_display_queue_waiting = 0;
    size_t capacity = 0;
    size_t count = 0;
    app_event_bus_lock();
    app_event_ring_ensure_locked();
    latest = g_app_event_bus.next_seq;
    capacity = g_app_event_bus.recent_capacity;
    if (g_app_event_bus.recent_count > 0 && g_app_event_bus.recent_capacity > 0) {
        oldest = g_app_event_bus.recent[g_app_event_bus.recent_start].seq;
    }
    dropped = g_app_event_bus.dropped;
    ring_overwrites = g_app_event_bus.ring_overwrites;
    sink_dropped = g_app_event_bus.sink_dropped;
    published = g_app_event_bus.published;
    maintenance_skipped = g_app_event_bus.maintenance_skipped;
    oom_dropped = g_app_event_bus.oom_dropped;
    usb_queue_dropped = g_app_event_bus.usb_queue_dropped;
    display_queue_dropped = g_app_event_bus.display_queue_dropped;
    max_usb_queue_waiting = g_app_event_bus.max_usb_queue_waiting;
    max_display_queue_waiting = g_app_event_bus.max_display_queue_waiting;
    for (size_t i = 0; i < g_app_event_bus.recent_count; i++) {
        size_t index = (g_app_event_bus.recent_start + i) % g_app_event_bus.recent_capacity;
        app_event_record_t *rec = &g_app_event_bus.recent[index];
        if (rec->seq <= since_seq) continue;
        if (kind_mask != 0 && !(kind_mask & APP_EVENT_KIND_MASK(rec->kind))) continue;
        if (count >= (size_t)limit) {
            app_event_envelope_release(snapshot[0].envelope);
            for (size_t j = 1; j < count; j++) snapshot[j - 1] = snapshot[j];
            snapshot[count - 1].envelope = NULL;
            count--;
        }
        snapshot[count] = *rec;
        app_event_envelope_retain(snapshot[count].envelope);
        count++;
    }
    app_event_bus_unlock();

    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) {
        for (size_t i = 0; i < count; i++) app_event_envelope_release(snapshot[i].envelope);
        heap_caps_free(snapshot);
        return NULL;
    }
    cc_string_builder_appendf(&sb,
                              "{\"latest_seq\":%u,\"oldest_seq\":%u,\"capacity\":%u,"
                              "\"dropped\":%u,\"ring_overwrites\":%u,\"sink_dropped\":%u,"
                              "\"published\":%u,\"maintenance_skipped\":%u,\"oom_dropped\":%u,"
                              "\"usb_queue_dropped\":%u,\"display_queue_dropped\":%u,"
                              "\"max_usb_queue_waiting\":%u,\"max_display_queue_waiting\":%u,"
                              "\"events\":[",
                              (unsigned)latest,
                              (unsigned)oldest,
                              (unsigned)capacity,
                              (unsigned)dropped,
                              (unsigned)ring_overwrites,
                              (unsigned)sink_dropped,
                              (unsigned)published,
                              (unsigned)maintenance_skipped,
                              (unsigned)oom_dropped,
                              (unsigned)usb_queue_dropped,
                              (unsigned)display_queue_dropped,
                              (unsigned)max_usb_queue_waiting,
                              (unsigned)max_display_queue_waiting);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) cc_string_builder_append_char(&sb, ',');
        cc_string_builder_appendf(&sb,
                                  "{\"seq\":%u,\"time_ms\":%lld,\"kind\":",
                                  (unsigned)snapshot[i].seq,
                                  (long long)snapshot[i].timestamp_ms);
        app_event_json_escape_append(&sb, app_event_kind_string(snapshot[i].kind));
        cc_string_builder_append(&sb, ",\"source\":");
        app_event_json_escape_append(&sb, app_event_source_string(snapshot[i].source));
        cc_string_builder_append(&sb, ",\"level\":");
        app_event_json_escape_append(&sb, app_event_log_level_string(snapshot[i].level));
        cc_string_builder_append(&sb, ",\"text\":");
        app_event_json_escape_append(&sb,
            snapshot[i].envelope ? snapshot[i].envelope->text : "");
        cc_string_builder_append_char(&sb, '}');
    }
    cc_string_builder_append(&sb, "]}");
    for (size_t i = 0; i < count; i++) app_event_envelope_release(snapshot[i].envelope);
    heap_caps_free(snapshot);
    return cc_string_builder_take(&sb);
}
