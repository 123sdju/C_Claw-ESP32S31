#pragma once

#include "app_event_bus.h"
#include "cc/core/cc_message.h"

/* 同一稳定时间顺序切片的显示游标，非零字节偏移位于脱敏后正文的 UTF-8 边界。 */
typedef struct {
    size_t message_index;
    size_t byte_offset;
} app_history_cursor_t;

typedef struct {
    app_event_view_item_t items[APP_EVENT_VIEW_MAX_ITEMS];
    size_t count;
    app_history_cursor_t next;
    bool has_more;
} app_history_page_t;

/** @brief 构建先脱敏再分页的历史显示副本。
 * @param[in] messages 调用期稳定的时间顺序消息切片，count 为零可为 NULL。
 * @param[in] count 切片条数。
 * @param[in] start 同一切片的下标及脱敏后正文的字节偏移。
 * @param[in] max_items 条数预算，范围 1 到 APP_EVENT_VIEW_MAX_ITEMS。
 * @param[in] max_bytes 含每条 NUL 的字节预算，范围 5 到 APP_EVENT_VIEW_MAX_BYTES。
 * @param[out] out_page 必填且原先无拥有成员，先清零，成功后调用 cleanup 释放正文。
 * @return ESP_OK 或参数、UTF-8 容量、摘要及分配错误，失败清理部分输出。
 * @pre 消息和输出禁止并发修改。next 只用于原切片，更早存储页按消息 ID 另行取得。
 */
esp_err_t app_history_page_build(const cc_message_t *messages, size_t count,
                                 app_history_cursor_t start, size_t max_items,
                                 size_t max_bytes, app_history_page_t *out_page);
/** @brief 释放显示页正文并清零。
 * @param[in,out] page 可为 NULL 的独占页，读者必须已退出，结构本身仍归调用者。
 * @post 已清理空页可再次调用。
 */
void app_history_page_cleanup(app_history_page_t *page);
/** @brief 定位以最新消息结束的有界尾页。
 * @param[in] messages 稳定时间顺序消息，count 为零可为 NULL。
 * @param[in] count 消息条数。
 * @param[in] max_items 条数预算，范围 1 到视图上限。
 * @param[in] max_bytes 包含逐条 NUL 的字节预算，范围 5 到视图上限。
 * @param[out] out_cursor 必填，成功为脱敏后 UTF-8 边界游标，错误时可能已写部分结果。
 * @return ESP_OK 或参数、摘要、分配、字符容量错误。
 * @pre 调用期禁止修改切片，临时文本均由函数归还。
 */
esp_err_t app_history_tail_cursor(const cc_message_t *messages, size_t count,
                                  size_t max_items, size_t max_bytes,
                                  app_history_cursor_t *out_cursor);
