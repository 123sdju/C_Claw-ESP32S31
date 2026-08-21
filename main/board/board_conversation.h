#pragma once

#include <stdint.h>

#include "esp_err.h"

/* 对话消息角色枚举 */
typedef enum {
    BOARD_CONV_USER,      /* 用户消息 */
    BOARD_CONV_ASSISTANT, /* AI 助手回复 */
    BOARD_CONV_THINKING,  /* AI 思考过程 */
    BOARD_CONV_TOOL,      /* 工具调用输出 */
    BOARD_CONV_STATUS,    /* 系统状态消息 */
    BOARD_CONV_ERROR,     /* 错误消息 */
} board_conversation_role_t;

/*
 * 初始化板级 UI 并间接初始化对话子系统。
 * 返回: ESP_OK 成功，否则为 ESP 错误码
 */
esp_err_t board_conversation_init(void);
/*
 * 清空对话面板上所有消息和绘制内容。
 */
void board_conversation_clear(void);
/*
 * 向对话面板追加一条消息，作为新消息块结束之前的流式片段。
 * 参数: role - 对话角色
 *       text - 消息正文
 */
void board_conversation_append(board_conversation_role_t role, const char *text);
/*
 * 向对话面板追加流式片段，同角色连续文本会合并到同一标签内。
 * 参数: role - 对话角色
 *       text - 片段正文
 */
void board_conversation_append_fragment(board_conversation_role_t role, const char *text);
/*
 * 结束当前流式片段，后续 append 会创建新的消息标签。
 */
void board_conversation_finish_fragment(void);
/*
 * 格式化追加对话消息（类似 printf）。
 * 参数: role - 对话角色
 *       fmt  - printf 格式串
 *       ...  - 可变参数
 */
void board_conversation_appendf(board_conversation_role_t role, const char *fmt, ...);
/*
 * 在对话框覆盖层（overlay）上绘制矩形。
 * 参数: x/y/width/height - 矩形位置和尺寸
 *       color - 16 位 RGB565 颜色
 */
void board_conversation_draw_rect(int x, int y, int width, int height, uint16_t color);
/*
 * 在对话框覆盖层上绘制文本。
 * 参数: x/y - 文本位置
 *       role - 对话角色，决定文本颜色
 *       text - 文本内容
 */
void board_conversation_draw_text(int x, int y, board_conversation_role_t role, const char *text);
/*
 * 触发 LVGL 重绘对话面板区域。
 */
void board_conversation_request_flush(void);
