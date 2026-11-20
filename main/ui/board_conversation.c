#include "board_conversation.h"

#include <stdarg.h>
#include <stdio.h>

#include "board_ui.h"

/**
 * @brief 通过板级 UI 的正常入口初始化显示及对话面板。
 * @return board_ui_init 的 ESP 错误码，成功时 UI 已发布为可用。
 * @pre 初始化 owner 串行调用，BSP 与 LVGL 资源由板级 UI 管理。
 */
esp_err_t board_conversation_init(void)
{
    return board_ui_init();
}

/**
 * @brief 委托 UI 锁内清除对话、覆盖层及工具显示内容。
 * @post 已就绪时清理显示与对应快照，尚未就绪时跳过。
 */
void board_conversation_clear(void)
{
    board_ui_conversation_clear();
}

/**
 * @brief 结束当前流后追加一个独立消息标签。
 * @param[in] role 按值消息角色，决定前缀和颜色。
 * @param[in] text 调用期 NUL 正文借用，NULL 作为空串。
 * @post UI 内复制受预算限制的显示文本，未就绪或分配失败时尽力跳过，无输入借用残留。
 */
void board_conversation_append(board_conversation_role_t role, const char *text)
{
    board_ui_conversation_append(role, text);
}

/**
 * @brief 将片段交给 UI 的同角色流式合并缓冲。
 * @param[in] role 按值角色，变化时结束旧流。
 * @param[in] text 调用期 NUL 文本借用，NULL 或空串忽略。
 * @post UI 锁内复制片段并按刷新节奏更新标签，超预算截取前缀，失败时保留已接纳正文。
 */
void board_conversation_append_fragment(board_conversation_role_t role, const char *text)
{
    board_ui_conversation_append_fragment(role, text);
}

/**
 * @brief 委托 UI 提交最后一次流式刷新并结束当前合并缓冲。
 * @post UI 已就绪时归还拼接缓冲，后续片段从新标签开始，已有标签继续由 UI 持有。
 */
void board_conversation_finish_fragment(void)
{
    board_ui_conversation_finish_fragment();
}

/**
 * @brief 用 384 字节栈缓冲格式化一条独立消息后委托 UI 复制。
 * @param[in] role 按值显示角色。
 * @param[in] fmt 调用期 printf 格式借用，NULL 忽略。
 * @param[in] ... 与格式匹配的参数及调用期字符串借用。
 * @post 非负格式化结果最多保留 383 字节，负结果不发布，无栈地址跨调用借用。
 */
void board_conversation_appendf(board_conversation_role_t role, const char *fmt, ...)
{
    if (!fmt) {
        return;
    }

    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (written >= 0) board_ui_conversation_append(role, buf);
}

/**
 * @brief 委托 UI 在对话覆盖层创建矩形。
 * @param[in] x 相对对话绘制区的水平像素坐标。
 * @param[in] y 相对对话绘制区的垂直像素坐标。
 * @param[in] width 请求宽度，单位为像素。
 * @param[in] height 请求高度，单位为像素。
 * @param[in] color RGB565 颜色值。
 * @post UI 负责锁和位置裁剪，未就绪或无有效可见区域时跳过。
 */
void board_conversation_draw_rect(int x, int y, int width, int height, uint16_t color)
{
    board_ui_conversation_draw_rect(x, y, width, height, color);
}

/**
 * @brief 委托 UI 在对话覆盖层创建文本标签。
 * @param[in] x 相对绘制区的水平像素坐标。
 * @param[in] y 相对绘制区的垂直像素坐标。
 * @param[in] role 决定文字颜色的角色值。
 * @param[in] text 调用期 NUL 文本借用，NULL 按 UI 的空文本规则处理。
 * @post UI 锁内复制显示文本，调用结束不持有输入借用。
 */
void board_conversation_draw_text(int x, int y, board_conversation_role_t role, const char *text)
{
    board_ui_conversation_draw_text(x, y, role, text);
}

/**
 * @brief 委托 UI 请求对话覆盖层重绘。
 * @post 已就绪时设置 LVGL 失效区域，由 LVGL 后续刷新，不等待物理屏完成。
 */
void board_conversation_request_flush(void)
{
    board_ui_conversation_request_flush();
}
