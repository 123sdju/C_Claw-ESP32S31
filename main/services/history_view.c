#include "history_view.h"
#include "cc/util/cc_redaction.h"

#include <stdlib.h>
#include <string.h>

/** @brief 将历史角色映射为显示事件种类。
 * @param[in] role 消息角色，接受未知枚举值。
 * @return USER/ASSISTANT/TOOL 对应种类，SYSTEM 及未知值映射 STATUS。
 */
static app_event_kind_t history_role(cc_message_role_t role)
{
    switch (role) {
    case CC_ROLE_USER: return APP_EVENT_KIND_USER;
    case CC_ROLE_ASSISTANT: return APP_EVENT_KIND_ASSISTANT;
    case CC_ROLE_TOOL: return APP_EVENT_KIND_TOOL;
    case CC_ROLE_SYSTEM: return APP_EVENT_KIND_STATUS;
    default: return APP_EVENT_KIND_STATUS;
    }
}

/** @brief 消费页面中独有的正文副本并清零页面。
 * @param[in,out] page 可为 NULL 的独占页面，count 以内正文须由构建器分配。
 * @post 所有正文 free，游标和标志归零，可重复清理空页面。
 * @pre 页面读者均已退出，调用者保管页面结构本身。
 */
void app_history_page_cleanup(app_history_page_t *page)
{
    if (!page) return;
    for (size_t i = 0; i < page->count; i++) free((void *)page->items[i].text);
    memset(page, 0, sizeof(*page));
}

/** @brief 从最新消息向前选择受限尾页的起点。
 * @param[in] messages 调用期稳定的时间顺序消息切片，count 为零时可为 NULL。
 * @param[in] count 切片消息条数。
 * @param[in] max_items 最大显示条数，范围 1 到 APP_EVENT_VIEW_MAX_ITEMS。
 * @param[in] max_bytes 正文总字节预算，包含逐项 NUL，范围 5 到视图上限。
 * @param[out] out_cursor 必填游标，偏移按脱敏后正文计算并对齐 UTF-8 边界。
 * @return 成功为 ESP_OK，参数错误为 INVALID_ARG，分配失败为 NO_MEM，摘要失败为 FAIL，无法容纳字符为 INVALID_SIZE。
 * @note 参数有效后先写切片末尾，后续错误可能留下部分游标，调用者仅在成功时使用。
 * @pre 消息在调用期间禁止并发修改，函数只分配临时摘要并自行归还。
 */
esp_err_t app_history_tail_cursor(const cc_message_t *messages, size_t count,
                                  size_t max_items, size_t max_bytes,
                                  app_history_cursor_t *out_cursor)
{
    if (!out_cursor || (!messages && count) || !max_items || max_items>APP_EVENT_VIEW_MAX_ITEMS ||
        max_bytes<5U || max_bytes>APP_EVENT_VIEW_MAX_BYTES) return ESP_ERR_INVALID_ARG;
    *out_cursor=(app_history_cursor_t){count,0};
    size_t remaining=max_bytes, selected=0;
    for (size_t i=count;i>0 && selected<max_items && remaining;i--) {
        char *summary=NULL;
        cc_result_t rc=cc_message_get_text_summary(&messages[i-1U],&summary);
        if (rc.code!=CC_OK || !summary) {
            esp_err_t err=rc.code==CC_ERR_OUT_OF_MEMORY ? ESP_ERR_NO_MEM : ESP_FAIL;
            cc_result_free(&rc); free(summary); return err;
        }
        cc_result_free(&rc);
        char *redacted=cc_redact_secrets(summary); free(summary);
        if (!redacted) return ESP_ERR_NO_MEM;
        size_t length=strlen(redacted);
        if (length<remaining) {
            remaining-=length+1U; selected++;
            *out_cursor=(app_history_cursor_t){i-1U,0};
            free(redacted); continue;
        }
        if (remaining>1U) {
            size_t offset=length-(remaining-1U);
            while (offset<length && ((unsigned char)redacted[offset] & 0xc0U)==0x80U) offset++;
            if (offset<length) *out_cursor=(app_history_cursor_t){i-1U,offset};
            else if (!selected) { free(redacted); return ESP_ERR_INVALID_SIZE; }
        }
        free(redacted); break;
    }
    return ESP_OK;
}

/** @brief 先脱敏再按字符边界构建拥有正文的显示页。
 * @param[in] messages 稳定时间顺序消息切片，count 为零时可为 NULL。
 * @param[in] count 消息条数。
 * @param[in] start 同一切片及同一脱敏规则中的消息下标和字节偏移。
 * @param[in] max_items 页内条数上限，范围 1 到视图条数上限。
 * @param[in] max_bytes 正文含逐项 NUL 的字节预算，范围 5 到视图字节上限。
 * @param[out] out_page 必填空槽，先清零，成功正文由调用者 cleanup。
 * @return 成功为 ESP_OK，参数/偏移无效、字符容量不足、分配或摘要失败分别返回错误，失败清理所有部分输出。
 * @pre 输出原先无拥有正文，消息只读借用覆盖调用；has_more 时用 next 和原切片继续。
 */
esp_err_t app_history_page_build(const cc_message_t *messages, size_t count,
                                 app_history_cursor_t start, size_t max_items,
                                 size_t max_bytes, app_history_page_t *out_page)
{
    if (!out_page) return ESP_ERR_INVALID_ARG;
    memset(out_page, 0, sizeof(*out_page));
    if ((!messages && count) || start.message_index > count ||
        (start.message_index == count && start.byte_offset) || !max_items ||
        max_items > APP_EVENT_VIEW_MAX_ITEMS || max_bytes < 5U || max_bytes > APP_EVENT_VIEW_MAX_BYTES)
        return ESP_ERR_INVALID_ARG;

    size_t index = start.message_index, offset = start.byte_offset, used = 0;
    esp_err_t err = ESP_OK;
    while (index < count && out_page->count < max_items && used < max_bytes) {
        char *summary = NULL;
        cc_result_t rc = cc_message_get_text_summary(&messages[index], &summary);
        if (rc.code != CC_OK || !summary) {
            err = rc.code == CC_ERR_OUT_OF_MEMORY ? ESP_ERR_NO_MEM : ESP_FAIL;
            cc_result_free(&rc);
            free(summary);
            break;
        }
        cc_result_free(&rc);
        char *redacted = cc_redact_secrets(summary);
        free(summary);
        if (!redacted) { err = ESP_ERR_NO_MEM; break; }
        summary = redacted;
        size_t length = strlen(summary);
        if (offset > length || (offset && offset == length) ||
            (offset < length && ((unsigned char)summary[offset] & 0xc0U) == 0x80U)) {
            free(summary);
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        size_t remaining = max_bytes - used;
        size_t left = length - offset;
        size_t take = left < remaining - 1U ? left : remaining - 1U;
        if (take < left) {
            while (take && ((unsigned char)summary[offset + take] & 0xc0U) == 0x80U) take--;
        }
        if (left && !take) {
            free(summary);
            if (!out_page->count) err = ESP_ERR_INVALID_SIZE;
            break;
        }
        char *text = malloc(take + 1U);
        if (!text) { free(summary); err = ESP_ERR_NO_MEM; break; }
        memcpy(text, summary + offset, take);
        text[take] = '\0';
        out_page->items[out_page->count].kind = history_role(messages[index].role);
        out_page->items[out_page->count].text = text;
        out_page->count++;
        used += take + 1U;
        free(summary);
        if (take < left) { offset += take; break; }
        index++;
        offset = 0;
    }
    if (err != ESP_OK) { app_history_page_cleanup(out_page); return err; }
    out_page->next.message_index = index;
    out_page->next.byte_offset = offset;
    out_page->has_more = index < count;
    return ESP_OK;
}
