#pragma once

#include <stdbool.h>

/*
 * 初始化快照子系统，创建互斥锁。
 * 返回: true 成功，false 内存不足
 */
bool board_ui_snapshot_init(void);
/*
 * 清空 conversation 环形缓冲区中所有行。
 */
void board_ui_snapshot_clear_conversation(void);
/*
 * 清空 tool 环形缓冲区中所有行。
 */
void board_ui_snapshot_clear_tool(void);
/*
 * 清空 draw 环形缓冲区中所有行。
 */
void board_ui_snapshot_clear_draw(void);
/*
 * 更新状态栏文本快照。
 * 参数: text - 状态栏文本内容
 */
void board_ui_snapshot_set_status(const char *text);
/*
 * 向 boot 环形缓冲区追加一行日志。
 * 参数: line - 日志行内容
 */
void board_ui_snapshot_append_boot(const char *line);
/*
 * 向 conversation 环形缓冲区追加一行消息。
 * 参数: line - 消息内容
 */
void board_ui_snapshot_append_conversation(const char *line);
/*
 * 替换 conversation 环形缓冲区中最后一行（用于流式文本更新）。
 * 参数: line - 替换后的完整文本
 */
void board_ui_snapshot_replace_last_conversation(const char *line);
/*
 * 向 tool 环形缓冲区追加一行工具输出。
 * 参数: line - 工具输出内容
 */
void board_ui_snapshot_append_tool(const char *line);
/*
 * 向 draw 环形缓冲区追加格式化绘制日志。
 * 参数: fmt - printf 格式串
 *       ... - 可变参数
 */
void board_ui_snapshot_append_drawf(const char *fmt, ...);
/*
 * 生成所有环形缓冲区和状态的 JSON 快照字符串，由调用者负责释放。
 * 参数: ready - UI 就绪标志
 * 返回: JSON 字符串（需调用 free 释放），分配失败返回 NULL
 */
char *board_ui_screen_snapshot_json(bool ready);
