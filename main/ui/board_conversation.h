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

/**
 * @brief 初始化板级 UI 和对话显示。
 * @return ESP_OK 表示已就绪，否则透传板级初始化错误。
 * @pre owner 串行初始化并维持 BSP 与 LVGL 的进程期资源。
 */
esp_err_t board_conversation_init(void);
/**
 * @brief 清空对话、覆盖层、工具显示及对应显示快照。
 * @post 内部取得 LVGL 锁完成清理，UI 尚未就绪时跳过。
 */
void board_conversation_clear(void);
/**
 * @brief 结束当前流后追加独立消息标签。
 * @param[in] role 消息角色，决定前缀和颜色。
 * @param[in] text 调用期 NUL 正文借用，NULL 作为空串。
 * @post 内部锁内复制显示文本，完整消息前缀和正文最多保留 511 字节，分配失败尽力跳过。
 */
void board_conversation_append(board_conversation_role_t role, const char *text);
/**
 * @brief 将片段复制进同角色合并流，角色变化时结束旧流。
 * @param[in] role 消息角色。
 * @param[in] text 调用期 NUL 文本借用，NULL 或空串忽略。
 * @post 已接纳正文受 UI 总预算约束并按节奏刷新，分配失败保持此前正文，无输入借用残留。
 */
void board_conversation_append_fragment(board_conversation_role_t role, const char *text);
/**
 * @brief 请求最后一次流式刷新并结束当前合并缓冲。
 * @post 拼接缓冲归还，标签仍归 UI，后续片段创建新标签，未就绪时跳过。
 */
void board_conversation_finish_fragment(void);
/**
 * @brief 格式化一条独立对话消息。
 * @param[in] role 消息角色。
 * @param[in] fmt 调用期 printf 格式借用，NULL 忽略。
 * @param[in] ... 与格式匹配的参数及调用期字符串借用。
 * @post 非负格式化结果最多保留 383 字节后交 UI 复制，格式化失败跳过。
 */
void board_conversation_appendf(board_conversation_role_t role, const char *fmt, ...);
/**
 * @brief 在对话覆盖层请求矩形显示。
 * @param[in] x 相对绘制区水平像素坐标。
 * @param[in] y 相对绘制区垂直像素坐标。
 * @param[in] width 宽度，单位为像素。
 * @param[in] height 高度，单位为像素。
 * @param[in] color RGB565 颜色值。
 * @post 内部持锁裁剪并创建对象，无有效区域或 UI 未就绪时跳过。
 */
void board_conversation_draw_rect(int x, int y, int width, int height, uint16_t color);
/**
 * @brief 在对话覆盖层请求文本显示。
 * @param[in] x 相对绘制区水平像素坐标。
 * @param[in] y 相对绘制区垂直像素坐标。
 * @param[in] role 决定文本颜色的角色值。
 * @param[in] text 调用期 NUL 文本借用，UI 在锁内复制，NULL 作为空串。
 * @post 文本对象归 UI，未就绪或无法显示时跳过。
 */
void board_conversation_draw_text(int x, int y, board_conversation_role_t role, const char *text);
/**
 * @brief 请求 LVGL 重绘对话覆盖层。
 * @post 设置失效区域后返回，物理屏异步完成由显示驱动管理。
 */
void board_conversation_request_flush(void);
