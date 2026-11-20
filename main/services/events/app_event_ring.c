#include "app_event_bus_internal.h"

#include <string.h>

#include "esp_heap_caps.h"

/**
 * @brief 惰性分配 APP_EVENT_RECENT_MAX 个清零记录槽，先尝试 PSRAM 再内部 RAM。
 * @pre 调用者持状态锁及有效资源租约，或处于独占初始化。
 * @return 已有或新分配数组可用为 true，两种分配均失败为 false 并将容量置零，可再次重试。
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

/**
 * @brief 在槽位及引用文本预算内追加记录，必要时淘汰最旧项。
 * @param[in] record 必需记录借用，envelope 为有效拥有对象借用或 NULL，调用者继续保管自身引用。
 * @pre 调用者持状态锁，记录字段覆盖本次复制，非空 envelope 由本模块构造且长度可表示。
 * @return 成功保留独立 envelope 引用为 true，数组 OOM 或单条超过文本预算为 false，统计按对应原因饱和增加。
 * @note recent_bytes 计 envelope 头、正文及 NUL，默认上限 512 KiB，独立 view、sink 和快照引用另计寿命。
 */
bool app_event_ring_append_locked(const app_event_record_t *record)
{
    if (!app_event_ring_ensure_locked()) {
        app_event_counter_increment(&g_app_event_bus.dropped);
        app_event_counter_increment(&g_app_event_bus.sink_dropped);
        app_event_counter_increment(&g_app_event_bus.oom_dropped);
        return false;
    }
    size_t bytes = record->envelope ? sizeof(*record->envelope) + record->envelope->len + 1U : 0;
    if (bytes > APP_EVENT_RECENT_MAX_BYTES) {
        app_event_counter_increment(&g_app_event_bus.dropped);
        app_event_counter_increment(&g_app_event_bus.ring_overwrites);
        return false;
    }
    while (g_app_event_bus.recent_count &&
           (g_app_event_bus.recent_count == g_app_event_bus.recent_capacity ||
            g_app_event_bus.recent_bytes > APP_EVENT_RECENT_MAX_BYTES - bytes)) {
        size_t index = g_app_event_bus.recent_start;
        g_app_event_bus.recent_start = (g_app_event_bus.recent_start + 1) % g_app_event_bus.recent_capacity;
        g_app_event_bus.recent_count--;
        app_event_envelope_t *old = g_app_event_bus.recent[index].envelope;
        if (old) g_app_event_bus.recent_bytes -= sizeof(*old) + old->len + 1U;
        g_app_event_bus.recent[index].envelope = NULL;
        app_event_envelope_release(old);
        app_event_counter_increment(&g_app_event_bus.dropped);
        app_event_counter_increment(&g_app_event_bus.ring_overwrites);
    }
    size_t index = (g_app_event_bus.recent_start + g_app_event_bus.recent_count) % g_app_event_bus.recent_capacity;
    g_app_event_bus.recent_count++;
    app_event_envelope_retain(record->envelope);
    g_app_event_bus.recent_bytes += bytes;
    g_app_event_bus.recent[index] = *record;
    return true;
}

/**
 * @brief 清空普通近期记录和独立历史页面，归还各自 envelope 引用。
 * @pre 调用者持状态锁，资源由当前 owner 保持有效。
 * @post 两份视图计数、近期字节数和起点归零，槽数组继续保留，已有 publisher/sink/快照引用仍有效。
 */
void app_event_ring_clear_locked(void)
{
    for (size_t i = 0; i < g_app_event_bus.view_count; i++) {
        app_event_envelope_release(g_app_event_bus.view[i].envelope);
        g_app_event_bus.view[i].envelope = NULL;
    }
    g_app_event_bus.view_count = 0;
    g_app_event_bus.view_latest_seq = 0;
    g_app_event_bus.view_active = false;
    for (size_t i = 0; g_app_event_bus.recent && i < g_app_event_bus.recent_capacity; i++) {
        app_event_envelope_release(g_app_event_bus.recent[i].envelope);
        g_app_event_bus.recent[i].envelope = NULL;
    }
    g_app_event_bus.recent_count = 0;
    g_app_event_bus.recent_bytes = 0;
    g_app_event_bus.recent_start = 0;
}

/**
 * @brief 将字符串连同双引号编码为 JSON 字段值，逐次消费追加诊断。
 * @param[in,out] sb 独占的已初始化构造器，失败由 sticky 状态保管。
 * @param[in] text 调用期 NUL 字符串借用，NULL 按空串处理，原始非控制字节保持原样。
 * @post 引号、反斜杠和控制字节完成转义，最终调用者以 take 判定整个 JSON 是否可交付。
 */
void app_event_json_escape_append(cc_string_builder_t *sb, const char *text)
{
    app_event_discard_result(cc_string_builder_append_char(sb, '"'));
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '\\': app_event_discard_result(cc_string_builder_append(sb, "\\\\")); break;
        case '"': app_event_discard_result(cc_string_builder_append(sb, "\\\"")); break;
        case '\n': app_event_discard_result(cc_string_builder_append(sb, "\\n")); break;
        case '\r': app_event_discard_result(cc_string_builder_append(sb, "\\r")); break;
        case '\t': app_event_discard_result(cc_string_builder_append(sb, "\\t")); break;
        default:
            if (*p < 0x20) app_event_discard_result(cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p));
            else app_event_discard_result(cc_string_builder_append_char(sb, (char)*p));
            break;
        }
    }
    app_event_discard_result(cc_string_builder_append_char(sb, '"'));
}

/**
 * @brief 编码一条已固定 envelope 引用的事件记录。
 * @param[in,out] sb 独占构造器，失败保留 sticky 状态并消费诊断。
 * @param[in] record 必需快照记录借用，envelope 引用保持至编码结束，NULL envelope 输出空文本。
 * @post 元数据和文本追加到同一 JSON 构造器，无记录引用交接。
 */
static void app_event_json_append_record(cc_string_builder_t *sb, const app_event_record_t *record)
{
    app_event_discard_result(cc_string_builder_appendf(sb, "{\"seq\":%u,\"time_ms\":%lld,\"kind\":",
                              (unsigned)record->seq, (long long)record->timestamp_ms));
    app_event_json_escape_append(sb, app_event_kind_string(record->kind));
    app_event_discard_result(cc_string_builder_append(sb, ",\"source\":"));
    app_event_json_escape_append(sb, app_event_source_string(record->source));
    app_event_discard_result(cc_string_builder_append(sb, ",\"level\":"));
    app_event_json_escape_append(sb, app_event_log_level_string(record->level));
    app_event_discard_result(cc_string_builder_append(sb, ",\"text\":"));
    app_event_json_escape_append(sb, record->envelope ? record->envelope->text : "");
    app_event_discard_result(cc_string_builder_append_char(sb, '}'));
}

/**
 * @brief 在同一锁期固定统计、近期事件和历史页面，再在锁外编码 JSON。
 * @param[in] since_seq 零表示全量，其他值筛选较新序号，大于当前最新值时返回 cursor_reset。
 * @param[in] limit 最大近期条数，非正或超过槽位容量时归一为该容量，保留匹配项中最新的一段。
 * @param[in] kind_mask 零忽略种类过滤，非零按有效种类位筛选，未知种类跳过。
 * @return 调用方 free 的 JSON 字符串，分配或构造失败返回 NULL，所有快照引用均在返回前归还。
 * @pre 外层持有资源租约，独立 view 最多 16 项，其包含条件由增量缺口、截取及换代共同决定。
 */
static char *app_event_bus_json_active(uint32_t since_seq, int limit, uint32_t kind_mask)
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
    uint32_t clear_generation = 0;
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
    size_t omitted = 0;
    app_event_record_t view[APP_EVENT_VIEW_MAX_ITEMS] = {0};
    size_t view_count = 0;
    uint32_t view_seq = 0;
    bool view_active = false;
    app_event_bus_lock();
    app_event_ring_ensure_locked();
    latest = g_app_event_bus.next_seq;
    clear_generation = g_app_event_bus.clear_generation;
    bool cursor_reset = since_seq > latest;
    uint32_t effective_since = cursor_reset ? 0 : since_seq;
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
    view_active = g_app_event_bus.view_active;
    view_seq = g_app_event_bus.view_latest_seq;
    for (size_t i = 0; view_active && i < g_app_event_bus.view_count; i++) {
        const app_event_record_t *rec = &g_app_event_bus.view[i];
        if (kind_mask != 0 && ((unsigned)rec->kind >= APP_EVENT_KIND_COUNT ||
            !(kind_mask & APP_EVENT_KIND_MASK(rec->kind)))) continue;
        view[view_count] = *rec;
        app_event_envelope_retain(view[view_count].envelope);
        view_count++;
    }
    for (size_t i = 0; i < g_app_event_bus.recent_count; i++) {
        size_t index = (g_app_event_bus.recent_start + i) % g_app_event_bus.recent_capacity;
        app_event_record_t *rec = &g_app_event_bus.recent[index];
        if (rec->seq <= effective_since) continue;
        if (kind_mask != 0 && ((unsigned)rec->kind >= APP_EVENT_KIND_COUNT ||
            !(kind_mask & APP_EVENT_KIND_MASK(rec->kind)))) continue;
        if (count >= (size_t)limit) {
            omitted++;
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
    bool gap = !cursor_reset && since_seq != 0 && oldest > 0 && since_seq < oldest - 1;
    bool include_view = view_active &&
        (since_seq == 0 || since_seq < view_seq || gap || omitted || cursor_reset);

    cc_string_builder_t sb;
    cc_result_t initialized = cc_string_builder_init(&sb);
    bool ready = initialized.code == CC_OK;
    cc_result_free(&initialized);
    if (!ready) {
        for (size_t i = 0; i < count; i++) app_event_envelope_release(snapshot[i].envelope);
        for (size_t i = 0; i < view_count; i++) app_event_envelope_release(view[i].envelope);
        heap_caps_free(snapshot);
        return NULL;
    }
    app_event_discard_result(cc_string_builder_appendf(&sb,
                              "{\"latest_seq\":%u,\"oldest_seq\":%u,\"clear_generation\":%u,\"capacity\":%u,"
                              "\"dropped\":%u,\"ring_overwrites\":%u,\"sink_dropped\":%u,"
                              "\"published\":%u,\"maintenance_skipped\":%u,\"oom_dropped\":%u,"
                              "\"usb_queue_dropped\":%u,\"display_queue_dropped\":%u,"
                              "\"max_usb_queue_waiting\":%u,\"max_display_queue_waiting\":%u,"
                              "\"gap\":%s,\"truncated\":%s,\"omitted\":%u,\"cursor_reset\":%s,"
                              "\"view_seq\":%u,\"view\":",
                              (unsigned)latest,
                              (unsigned)oldest,
                              (unsigned)clear_generation,
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
                              (unsigned)max_display_queue_waiting,
                              gap ? "true" : "false",
                              omitted ? "true" : "false",
                              (unsigned)omitted,
                              cursor_reset ? "true" : "false",
                              (unsigned)view_seq));
    if (include_view) {
        app_event_discard_result(cc_string_builder_append_char(&sb, '['));
        for (size_t i = 0; i < view_count; i++) {
            if (i) app_event_discard_result(cc_string_builder_append_char(&sb, ','));
            app_event_json_append_record(&sb, &view[i]);
        }
        app_event_discard_result(cc_string_builder_append_char(&sb, ']'));
    } else {
        app_event_discard_result(cc_string_builder_append(&sb, "null"));
    }
    app_event_discard_result(cc_string_builder_append(&sb, ",\"events\":["));
    for (size_t i = 0; i < count; i++) {
        if (i > 0) app_event_discard_result(cc_string_builder_append_char(&sb, ','));
        app_event_json_append_record(&sb, &snapshot[i]);
    }
    app_event_discard_result(cc_string_builder_append(&sb, "]}"));
    for (size_t i = 0; i < count; i++) app_event_envelope_release(snapshot[i].envelope);
    for (size_t i = 0; i < view_count; i++) app_event_envelope_release(view[i].envelope);
    heap_caps_free(snapshot);
    return cc_string_builder_take(&sb);
}

/**
 * @brief 为 Web 等调用者导出拥有型事件快照，关闭接纳后直接失败。
 * @param[in] since_seq 增量游标，零全量，序号换代由 cursor_reset 与 clear_generation 表达。
 * @param[in] limit 近期结果数量，按当前固定槽位归一。
 * @param[in] kind_mask 种类位过滤，零表示全部。
 * @return 成功 JSON 由调用者 free，未初始化、关闭或分配/构造失败返回 NULL，函数内部归还租约。
 */
char *app_event_bus_json(uint32_t since_seq, int limit, uint32_t kind_mask)
{
    if (!app_event_bus_acquire()) return NULL;
    char *json = app_event_bus_json_active(since_seq, limit, kind_mask);
    app_event_bus_release();
    return json;
}
