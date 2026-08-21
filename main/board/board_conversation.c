#include "board_conversation.h"

#include <stdarg.h>
#include <stdio.h>

#include "board_ui.h"

/*
 * 初始化板级 UI 并间接初始化对话子系统。
 * 返回: ESP_OK 成功，否则为 ESP 错误码
 */
esp_err_t board_conversation_init(void)
{
    return board_ui_init();
}

/*
 * 清空对话面板上所有消息和绘制内容。
 */
void board_conversation_clear(void)
{
    board_ui_conversation_clear();
}

/*
 * 向对话面板追加一条消息，作为新消息块结束之前的流式片段。
 * 参数: role - 对话角色（USER/ASSISTANT/THINKING/TOOL/ERROR/STATUS）
 *       text - 消息正文
 */
void board_conversation_append(board_conversation_role_t role, const char *text)
{
    board_ui_conversation_append(role, text);
}

/*
 * 向对话面板追加流式片段，同角色连续文本会合并到同一标签内。
 * 参数: role - 对话角色
 *       text - 片段正文
 */
void board_conversation_append_fragment(board_conversation_role_t role, const char *text)
{
    board_ui_conversation_append_fragment(role, text);
}

/*
 * 结束当前流式片段，后续 append 会创建新的消息标签。
 */
void board_conversation_finish_fragment(void)
{
    board_ui_conversation_finish_fragment();
}

/*
 * 格式化追加对话消息（类似 printf）。
 * 参数: role - 对话角色
 *       fmt  - printf 格式串
 *       ...  - 可变参数
 */
void board_conversation_appendf(board_conversation_role_t role, const char *fmt, ...)
{
    if (!fmt) {
        return;
    }

    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    board_ui_conversation_append(role, buf);
}

/*
 * 在对话框覆盖层（overlay）上绘制矩形。
 * 参数: x/y/width/height - 矩形位置和尺寸
 *       color - 16 位 RGB565 颜色
 */
void board_conversation_draw_rect(int x, int y, int width, int height, uint16_t color)
{
    board_ui_conversation_draw_rect(x, y, width, height, color);
}

/*
 * 在对话框覆盖层上绘制文本。
 * 参数: x/y   - 文本位置
 *       role  - 对话角色，决定文本颜色
 *       text  - 文本内容
 */
void board_conversation_draw_text(int x, int y, board_conversation_role_t role, const char *text)
{
    board_ui_conversation_draw_text(x, y, role, text);
}

/*
 * 触发 LVGL 重绘对话面板区域。
 */
void board_conversation_request_flush(void)
{
    board_ui_conversation_request_flush();
}
