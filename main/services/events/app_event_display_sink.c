#include "app_event_bus_internal.h"

#include <string.h>

#include "esp_heap_caps.h"

#include "board_conversation.h"
#include "board_ui.h"

/**
 * @brief 将事件种类映射为 LCD 对话角色。
 * @param[in] kind 按值事件类型。
 * @return 用户、助手、思考、工具及错误各自对应角色，其他类型沿状态角色。
 */
static board_conversation_role_t role_for_event(app_event_kind_t kind)
{
    switch (kind) {
    case APP_EVENT_KIND_USER: return BOARD_CONV_USER;
    case APP_EVENT_KIND_ASSISTANT: return BOARD_CONV_ASSISTANT;
    case APP_EVENT_KIND_THINKING: return BOARD_CONV_THINKING;
    case APP_EVENT_KIND_TOOL: return BOARD_CONV_TOOL;
    case APP_EVENT_KIND_ERROR: return BOARD_CONV_ERROR;
    case APP_EVENT_KIND_STATUS:
    case APP_EVENT_KIND_RUN_STATE:
    case APP_EVENT_KIND_LOG:
    default:
        return BOARD_CONV_STATUS;
    }
}

/**
 * @brief 判断当前事件是否采用片段合并显示。
 * @param[in] kind 按值事件类型。
 * @return 助手及思考类型为 true，其余为 false。
 */
static bool is_stream_event(app_event_kind_t kind)
{
    return kind == APP_EVENT_KIND_ASSISTANT || kind == APP_EVENT_KIND_THINKING;
}

/**
 * @brief 释放一页拥有的记录引用及页存储。
 * @param[in] view 拥有页面，可 NULL，count 个 envelope 各持有本页的一次引用。
 * @post 所有页引用 release，页由 heap_caps_free 归还，调用者不得再次访问。
 */
static void display_view_free(app_event_display_view_t *view)
{
    if (!view) return;
    for (size_t i = 0; i < view->count; i++)
        app_event_envelope_release(view->items[i].envelope);
    heap_caps_free(view);
}

/**
 * @brief 清空现有显示并按页面顺序重建每条完整消息。
 * @param[in] view 调用期稳定页面借用，可 NULL，非空 envelope 覆盖调用期。
 * @post 每条消息独立结束片段，UI 内自行复制文本，页面及引用仍归消费泵。
 * @pre LVGL 任务持 UI 锁，调用期间页面未被其他任务释放。
 */
static void display_emit_view(const app_event_display_view_t *view)
{
    board_conversation_finish_fragment();
    board_conversation_clear();
    for (size_t i = 0; view && i < view->count; i++) {
        const char *text = view->items[i].envelope ? view->items[i].envelope->text : "";
        if (text[0]) board_conversation_append_fragment(role_for_event(view->items[i].kind), text);
        else board_conversation_append(role_for_event(view->items[i].kind), "");
        board_conversation_finish_fragment();
    }
}

/**
 * @brief 将一个事件交给流式或独立消息显示入口。
 * @param[in] kind 决定角色和流式语义的事件类型。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串。
 * @post 非流式事件先结束旧流，UI 在返回前复制所接纳的文本。
 * @pre LVGL 消费任务持 UI 锁，显示分配遵循 UI 的最佳努力规则。
 */
static void display_emit_direct(app_event_kind_t kind, const char *text)
{
    if (is_stream_event(kind)) {
        board_conversation_append_fragment(role_for_event(kind), text ? text : "");
    } else {
        board_conversation_finish_fragment();
        board_conversation_append(role_for_event(kind), text ? text : "");
    }
}

/**
 * @brief 在总线锁内退还一个已计入的显示 pending。
 * @post 非零 pending 减一，零值保持，调用者负责批末通知 idle。
 */
static void display_complete_one(void)
{
    app_event_bus_lock();
    if (g_app_event_bus.display_pending > 0) g_app_event_bus.display_pending--;
    app_event_bus_unlock();
}

/**
 * @brief 提交本批同类型片段并完成其累计 pending。
 * @param[in,out] batch 至少 APP_EVENT_TEXT_MAX 加一字节的独占栈区，末尾由本函数写 NUL。
 * @param[in,out] len 有效长度槽，输入不超过 APP_EVENT_TEXT_MAX，返回归零。
 * @param[in,out] pending 本批已消费消息数槽，每条完成后归零。
 * @param[in] kind 本批流式事件类型。
 * @post UI 完成调用期复制，所有批内 pending 退还，无栈借用残留。
 * @pre LVGL 任务持 UI 锁，len 和 pending 指针有效且调用期独占。
 */
static void display_flush_stream(char *batch, size_t *len, unsigned *pending, app_event_kind_t kind)
{
    if (*len) {
        batch[*len] = '\0';
        board_conversation_append_fragment(role_for_event(kind), batch);
        *len = 0;
    }
    while (*pending) { display_complete_one(); (*pending)--; }
}

/**
 * @brief 锁内饱和增加总丢弃、sink 丢弃与显示队列丢弃统计。
 * @post 三项统计在同一总线临界区更新，到 UINT32_MAX 后保持上限。
 */
static void display_note_drop(void)
{
    app_event_bus_lock();
    app_event_counter_increment(&g_app_event_bus.dropped);
    app_event_counter_increment(&g_app_event_bus.sink_dropped);
    app_event_counter_increment(&g_app_event_bus.display_queue_dropped);
    app_event_bus_unlock();
}

/**
 * @brief 由总线初始化 owner 创建进程内显示队列，优先能力堆再回退普通队列。
 * @return 已有或新队列可用为 ESP_OK，两种创建均失败为 ESP_ERR_NO_MEM。
 * @pre 总线 owner 串行初始化，未对外开放生产和消费，队列最终由总线销毁路径按来源释放。
 */
esp_err_t app_event_display_init(void)
{
    if (!g_app_event_bus.display_queue) {
#if configSUPPORT_STATIC_ALLOCATION
        g_app_event_bus.display_queue = xQueueCreateWithCaps(APP_EVENT_DISPLAY_QUEUE_DEPTH,
                                                             sizeof(app_event_display_msg_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        g_app_event_bus.display_queue_with_caps = g_app_event_bus.display_queue != NULL;
#endif
        if (!g_app_event_bus.display_queue) {
            g_app_event_bus.display_queue = xQueueCreate(APP_EVENT_DISPLAY_QUEUE_DEPTH,
                                                         sizeof(app_event_display_msg_t));
        }
    }
    if (!g_app_event_bus.display_queue) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/**
 * @brief 非阻塞消费至多 APP_EVENT_DISPLAY_PUMP_MAX 条，合并可容纳的同类型流式消息。
 * @post 每条页或 envelope 引用归还且 pending 完成，批末通知总线可能排空。
 * @pre 调用者已持总线生命周期 lease，LVGL 任务串行持 UI 锁消费。
 * @note 超过栈批容量的单个流式 envelope 走直接片段入口，保持源文本并避免批区越界。
 */
static void app_event_display_pump_active(void)
{
    if (!g_app_event_bus.display_queue) {
        return;
    }

    app_event_display_msg_t msg;
    unsigned processed = 0;
    char batch[APP_EVENT_TEXT_MAX + 1];
    size_t batch_len = 0;
    unsigned batch_pending = 0;
    app_event_kind_t batch_kind = APP_EVENT_KIND_STATUS;
    while (processed++ < APP_EVENT_DISPLAY_PUMP_MAX) {
        if (xQueueReceive(g_app_event_bus.display_queue, &msg, 0) != pdTRUE) {
            break;
        }
        bool buffered_stream = !msg.view && !msg.clear && !msg.boot && is_stream_event(msg.kind) &&
            (!msg.envelope || msg.envelope->len <= APP_EVENT_TEXT_MAX);
        if (buffered_stream) {
            const char *text = msg.envelope ? msg.envelope->text : "";
            size_t len = msg.envelope ? msg.envelope->len : 0;
            if (batch_len && (batch_kind != msg.kind || len > APP_EVENT_TEXT_MAX - batch_len))
                display_flush_stream(batch, &batch_len, &batch_pending, batch_kind);
            batch_kind = msg.kind;
            if (len) {
                memcpy(batch + batch_len, text, len);
                batch_len += len;
            }
            batch_pending++;
        } else if (msg.view) {
            display_flush_stream(batch, &batch_len, &batch_pending, batch_kind);
            display_emit_view(msg.view);
        } else if (msg.clear) {
            display_flush_stream(batch, &batch_len, &batch_pending, batch_kind);
            board_conversation_finish_fragment();
            board_conversation_clear();
            board_ui_boot_log_clear();
        } else if (msg.boot) {
            display_flush_stream(batch, &batch_len, &batch_pending, batch_kind);
            board_ui_boot_log_append(role_for_event(msg.kind),msg.envelope ? msg.envelope->text : "");
        } else {
            display_flush_stream(batch, &batch_len, &batch_pending, batch_kind);
            display_emit_direct(msg.kind,
                msg.envelope ? msg.envelope->text : "");
        }
        display_view_free(msg.view);
        app_event_envelope_release(msg.envelope);
        if (!buffered_stream) display_complete_one();
    }
    display_flush_stream(batch, &batch_len, &batch_pending, batch_kind);
    app_event_bus_notify_idle_if_drained();
}

/**
 * @brief 查询板级 UI 是否已经完整发布。
 * @return board_ui_ready 的原子就绪值，查询不接纳显示提交。
 */
bool app_event_display_available(void)
{
    return board_ui_ready();
}

/**
 * @brief 取得总线 lease 后执行一次显示消费批次。
 * @post lease 获取失败跳过，否则覆盖整批消费及引用回收后归还。
 * @pre LVGL 消费任务持 UI 锁，生命周期 owner 关闭入口并等待 lease 后才能销毁队列。
 */
void app_event_display_pump(void)
{
    if (!app_event_bus_acquire()) return;
    app_event_display_pump_active();
    app_event_bus_release();
}

/**
 * @brief 非阻塞丢弃队列内尚未消费的消息，逐条归还资源及 pending。
 * @post 空队列或缺失队列直接结束，否则清除当前可读项并通知 idle。
 * @pre owner 已关闭生产或调用者持发布顺序锁及生命周期 lease，消费端允许同时取走另一条消息。
 */
void app_event_display_discard_all(void)
{
    if (!g_app_event_bus.display_queue) return;
    app_event_display_msg_t msg;
    while (xQueueReceive(g_app_event_bus.display_queue, &msg, 0) == pdTRUE) {
        display_view_free(msg.view);
        app_event_envelope_release(msg.envelope);
        display_complete_one();
    }
    app_event_bus_notify_idle_if_drained();
}

/**
 * @brief 检查显示接纳状态后保留 envelope 引用并提交一次队列消息。
 * @param[in] kind 按值事件类型，流式类型有短背压预算。
 * @param[in] envelope 调用期稳定借用，接纳后为队列新增一次引用，NULL 接纳失败计丢弃。
 * @param[in] boot 是否发往启动日志入口。
 * @post 队列成功拥有新增引用及 pending，提交失败归还两者并记丢弃，调用者原引用保持。
 * @pre 调用者已持生命周期 lease 和发布顺序责任，队列在 lease 退场前有效。
 */
static void app_event_display_emit_active(app_event_kind_t kind, app_event_envelope_t *envelope, bool boot)
{
    if (!app_event_display_available()) return;
    app_event_bus_lock();
    bool accepting = g_app_event_bus.lifecycle != APP_EVENT_BUS_DRAINING &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_STOPPED &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_FAILED &&
        !g_app_event_bus.maintenance_mode && g_app_event_bus.display_queue;
    if (accepting) g_app_event_bus.display_pending++;
    app_event_bus_unlock();
    if (!accepting) return;
    if (!envelope || !g_app_event_bus.display_queue) {
        display_complete_one();
        display_note_drop();
        return;
    }
    app_event_envelope_retain(envelope);
    app_event_display_msg_t msg = {
        .kind = kind,
        .envelope = envelope,
        .clear = false,
        .boot = boot,
    };
    UBaseType_t waiting = uxQueueMessagesWaiting(g_app_event_bus.display_queue);
    app_event_bus_lock();
    if ((uint32_t)waiting > g_app_event_bus.max_display_queue_waiting) {
        g_app_event_bus.max_display_queue_waiting = (uint32_t)waiting;
    }
    app_event_bus_unlock();
    TickType_t wait = is_stream_event(kind) ? app_event_timeout_ticks(5) : 0;
    app_event_bus_mark_sink_busy();
    if (xQueueSend(g_app_event_bus.display_queue, &msg, wait) != pdTRUE) {
        app_event_envelope_release(envelope);
        display_complete_one();
        display_note_drop();
        app_event_bus_notify_idle_if_drained();
    }
}

/**
 * @brief 为普通事件取得生命周期 lease 后尝试异步显示。
 * @param[in] kind 按值事件类型。
 * @param[in] envelope 有效 envelope 借用，允许 NULL，调用者保留自己的引用。
 * @post 已接纳消息新增队列引用，关闭或未就绪时跳过，失败资源由本层回收。
 * @pre 发布 owner 串行化需要保序的提交，envelope 内容在引用存活期不可修改。
 */
void app_event_display_emit(app_event_kind_t kind, app_event_envelope_t *envelope)
{
    if (!app_event_bus_acquire()) return;
    app_event_display_emit_active(kind, envelope, false);
    app_event_bus_release();
}

/**
 * @brief 为启动日志取得生命周期 lease 后尝试异步显示。
 * @param[in] kind 决定启动文本颜色的事件类型。
 * @param[in] envelope 稳定借用，可 NULL，接纳时新增一次队列引用。
 * @post 原引用保持，队列满时回收新增引用并记录丢弃。
 * @pre 发布 owner 维护提交顺序，envelope 覆盖调用期且引用存活期间不可修改。
 */
void app_event_display_emit_boot(app_event_kind_t kind, app_event_envelope_t *envelope)
{
    if (!app_event_bus_acquire()) return;
    app_event_display_emit_active(kind, envelope, true);
    app_event_bus_release();
}

/**
 * @brief 复制完整显示页面并替换尚未消费的旧显示队列。
 * @param[in] items count 个稳定记录的调用期数组借用，count 非零时必填，envelope 可 NULL。
 * @param[in] count 页面记录数，不得超过 APP_EVENT_VIEW_MAX_ITEMS，零表示提交空页面。
 * @return 参数错误为 INVALID_ARG，关闭或未就绪为 INVALID_STATE，副本 OOM 为 NO_MEM，入队失败为 TIMEOUT，否则 ESP_OK。
 * @post 成功队列拥有页面和各 envelope 新引用，失败全部回收，原数组及原引用保持。
 * @pre 调用者持发布顺序锁，生命周期 lease 由本函数覆盖，消费端可与旧消息丢弃并发。
 */
esp_err_t app_event_display_replace(const app_event_view_record_t *items, size_t count)
{
    if (count > APP_EVENT_VIEW_MAX_ITEMS || (count && !items)) return ESP_ERR_INVALID_ARG;
    if (!app_event_display_available()) return ESP_ERR_INVALID_STATE;
    if (!app_event_bus_acquire()) return ESP_ERR_INVALID_STATE;
    size_t bytes = sizeof(app_event_display_view_t) + count * sizeof(app_event_view_record_t);
    app_event_display_view_t *view = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!view) view = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!view) { app_event_bus_release(); return ESP_ERR_NO_MEM; }
    view->count = count;
    for (size_t i = 0; i < count; i++) {
        view->items[i] = items[i];
        app_event_envelope_retain(view->items[i].envelope);
    }
    app_event_bus_lock();
    bool accepting = g_app_event_bus.display_enabled && g_app_event_bus.display_queue &&
        !g_app_event_bus.maintenance_mode && g_app_event_bus.lifecycle != APP_EVENT_BUS_DRAINING &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_STOPPED && g_app_event_bus.lifecycle != APP_EVENT_BUS_FAILED;
    if (accepting) g_app_event_bus.display_pending++;
    app_event_bus_unlock();
    if (!accepting) {
        display_view_free(view);
        app_event_bus_release();
        return ESP_ERR_INVALID_STATE;
    }
    app_event_bus_mark_sink_busy();
    /* 发布顺序锁排斥其他生产者，LVGL 消费者并发只会减少已有队列项。 */
    app_event_display_discard_all();
    app_event_display_msg_t msg = {.view = view};
    if (xQueueSend(g_app_event_bus.display_queue, &msg, 0) != pdTRUE) {
        display_view_free(view);
        display_complete_one();
        display_note_drop();
        app_event_bus_notify_idle_if_drained();
        app_event_bus_release();
        return ESP_ERR_TIMEOUT;
    }
    app_event_bus_release();
    return ESP_OK;
}

/**
 * @brief 清除尚未消费旧消息后提交一个异步清屏标记。
 * @post 已接纳标记计入 pending，消费时结束流并清除对话和启动显示，提交失败退还 pending 并记丢弃。
 * @pre 调用者持发布顺序锁，本函数取得生命周期 lease，UI 未就绪或总线关闭时跳过。
 */
void app_event_display_clear(void)
{
    if (!app_event_display_available()) return;
    if (!app_event_bus_acquire()) return;
    app_event_bus_lock();
    bool accepting = g_app_event_bus.display_queue &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_DRAINING &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_STOPPED &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_FAILED;
    if (accepting) g_app_event_bus.display_pending++;
    app_event_bus_unlock();
    if (!accepting) { app_event_bus_release(); return; }
    app_event_display_discard_all();
    app_event_display_msg_t msg = {
        .kind = APP_EVENT_KIND_STATUS,
        .envelope = NULL,
        .clear = true,
    };
    app_event_bus_mark_sink_busy();
    if (xQueueSend(g_app_event_bus.display_queue, &msg, 0) != pdTRUE) {
        display_complete_one();
        display_note_drop();
        app_event_bus_notify_idle_if_drained();
    }
    app_event_bus_release();
}
