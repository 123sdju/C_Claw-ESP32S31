#include "app_event_bus_internal.h"

#include <string.h>

#include "esp_heap_caps.h"

#include "board_conversation.h"

/*
 * 将事件种类映射到 LCD 对话角色枚举，用于 board_conversation 系列 API。
 * STATUS、RUN_STATE、LOG 默认映射为 BOARD_CONV_STATUS。
 *
 * 参数: kind - 事件种类
 * 返回: 对应的 board_conversation_role_t 角色
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

/*
 * 判断事件是否为流式输出类型（ASSISTANT 或 THINKING），流式事件使用
 * append_fragment 增量追加而非整条替换。
 *
 * 参数: kind - 事件种类
 * 返回: true 表示流式事件
 */
static bool is_stream_event(app_event_kind_t kind)
{
    return kind == APP_EVENT_KIND_ASSISTANT || kind == APP_EVENT_KIND_THINKING;
}

/*
 * 直接将事件写入 LCD 对话区：流式事件调用 append_fragment，
 * 非流式事件先 finish_fragment 结束当前片段再 append 整条消息。
 *
 * 参数: kind - 事件种类
 * 参数: text - 事件文本内容
 * 返回: 无
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

static void display_complete_one(void)
{
    app_event_bus_lock();
    if (g_app_event_bus.display_pending > 0) g_app_event_bus.display_pending--;
    app_event_bus_unlock();
}

/*
 * 批量处理显示队列中的流式事件：合并同类型连续事件片段，减少
 * board_conversation_append_fragment 的调用次数以提升 LCD 绘制效率。
 * 非流式事件或 clear 消息会终止批量合并。
 *
 * 参数: kind - 事件种类
 * 参数: text - 当前事件的文本内容
 * 返回: 无
 */
static void display_emit_stream_batch(app_event_kind_t kind, const char *text)
{
    char batch[APP_EVENT_TEXT_MAX + 1];
    size_t len = 0;

    if (text && text[0]) {
        len = strlen(text);
        if (len > APP_EVENT_TEXT_MAX) {
            len = APP_EVENT_TEXT_MAX;
        }
        memcpy(batch, text, len);
    }
    batch[len] = '\0';

    while (true) {
        app_event_display_msg_t next;
        if (xQueuePeek(g_app_event_bus.display_queue, &next, 0) != pdTRUE ||
            next.clear ||
            next.kind != kind) {
            break;
        }

        if (xQueueReceive(g_app_event_bus.display_queue, &next, 0) != pdTRUE) {
            break;
        }

        const char *next_text = next.envelope ? next.envelope->text : "";
        size_t next_len = strlen(next_text);
        if (next_len > APP_EVENT_TEXT_MAX) {
            next_len = APP_EVENT_TEXT_MAX;
        }

        if (len > 0 && next_len > APP_EVENT_TEXT_MAX - len) {
            batch[len] = '\0';
            board_conversation_append_fragment(role_for_event(kind), batch);
            len = 0;
        }

        size_t copy_len = next_len;
        if (copy_len > APP_EVENT_TEXT_MAX - len) {
            copy_len = APP_EVENT_TEXT_MAX - len;
        }
        if (copy_len > 0) {
            memcpy(batch + len, next_text, copy_len);
            len += copy_len;
        }
        batch[len] = '\0';
        app_event_envelope_release(next.envelope);
        display_complete_one();
    }

    if (len > 0) {
        board_conversation_append_fragment(role_for_event(kind), batch);
    }
}

/*
 * 记录一次显示队列丢弃事件，更新 dropped / sink_dropped / display_queue_dropped 计数器。
 *
 * 参数: 无
 * 返回: 无
 */
static void display_note_drop(void)
{
    app_event_bus_lock();
    g_app_event_bus.dropped++;
    g_app_event_bus.sink_dropped++;
    g_app_event_bus.display_queue_dropped++;
    app_event_bus_unlock();
}

/*
 * 确保显示队列已创建（惰性初始化），优先尝试 PSRAM 带能力的队列创建，
 * 回退到普通队列创建。
 *
 * 参数: 无
 * 返回: true 表示队列可用，false 表示创建失败
 */
esp_err_t app_event_display_init(void)
{
    if (!g_app_event_bus.display_queue) {
#if configSUPPORT_STATIC_ALLOCATION
        g_app_event_bus.display_queue = xQueueCreateWithCaps(APP_EVENT_DISPLAY_QUEUE_DEPTH,
                                                             sizeof(app_event_display_msg_t),
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

/*
 * 显示队列消费泵：每调用最多处理 APP_EVENT_DISPLAY_PUMP_MAX 条消息，
 * 清空消息分派到 board_conversation_clear，流式事件走批量合并路径，
 * 非流式事件走直接写入路径。应在 LVGL 任务上下文中周期性调用。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_display_pump(void)
{
    if (!g_app_event_bus.display_queue) {
        return;
    }

    app_event_display_msg_t msg;
    unsigned processed = 0;
    while (processed++ < APP_EVENT_DISPLAY_PUMP_MAX) {
        if (xQueueReceive(g_app_event_bus.display_queue, &msg, 0) != pdTRUE) {
            app_event_bus_notify_idle_if_drained();
            return;
        }
        if (msg.clear) {
            board_conversation_finish_fragment();
            board_conversation_clear();
        } else if (is_stream_event(msg.kind)) {
            display_emit_stream_batch(msg.kind,
                msg.envelope ? msg.envelope->text : "");
        } else {
            display_emit_direct(msg.kind,
                msg.envelope ? msg.envelope->text : "");
        }
        app_event_envelope_release(msg.envelope);
        display_complete_one();
    }
    app_event_bus_notify_idle_if_drained();
}

void app_event_display_discard_all(void)
{
    if (!g_app_event_bus.display_queue) return;
    app_event_display_msg_t msg;
    while (xQueueReceive(g_app_event_bus.display_queue, &msg, 0) == pdTRUE) {
        app_event_envelope_release(msg.envelope);
        display_complete_one();
    }
    app_event_bus_notify_idle_if_drained();
}

/*
 * 将事件放入显示端消费队列：流式事件使用 5ms 的发送超时做背压，
 * 非流式事件使用非阻塞发送。队列满时记录丢弃。
 *
 * 参数: kind - 事件种类
 * 参数: text - 事件文本内容
 * 返回: 无
 */
void app_event_display_emit(app_event_kind_t kind, app_event_envelope_t *envelope)
{
    if (!envelope || !g_app_event_bus.display_queue) {
        display_note_drop();
        return;
    }
    app_event_envelope_retain(envelope);
    app_event_display_msg_t msg = {
        .kind = kind,
        .envelope = envelope,
        .clear = false,
    };
    UBaseType_t waiting = uxQueueMessagesWaiting(g_app_event_bus.display_queue);
    app_event_bus_lock();
    if ((uint32_t)waiting > g_app_event_bus.max_display_queue_waiting) {
        g_app_event_bus.max_display_queue_waiting = (uint32_t)waiting;
    }
    g_app_event_bus.display_pending++;
    app_event_bus_unlock();
    TickType_t wait = is_stream_event(kind) ? pdMS_TO_TICKS(5) : 0;
    app_event_bus_mark_sink_busy();
    if (xQueueSend(g_app_event_bus.display_queue, &msg, wait) != pdTRUE) {
        app_event_envelope_release(envelope);
        display_complete_one();
        display_note_drop();
        app_event_bus_notify_idle_if_drained();
    }
}

/*
 * 向显示队列发送清空消息，pump 消费后将调用 board_conversation_clear
 * 清除 LCD 对话区的所有内容。使用 50ms 超时发送。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_display_clear(void)
{
    if (!g_app_event_bus.display_queue) {
        display_note_drop();
        return;
    }
    app_event_display_msg_t msg = {
        .kind = APP_EVENT_KIND_STATUS,
        .envelope = NULL,
        .clear = true,
    };
    app_event_bus_lock();
    g_app_event_bus.display_pending++;
    app_event_bus_unlock();
    app_event_bus_mark_sink_busy();
    if (xQueueSend(g_app_event_bus.display_queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
        display_complete_one();
        display_note_drop();
        app_event_bus_notify_idle_if_drained();
    }
}
