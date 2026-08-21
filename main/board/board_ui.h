#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_conversation.h"
#include "esp_err.h"

/* 摄像头性能统计 */
typedef struct {
    uint32_t preview_frames;    /* 预览帧累计计数 */
    uint32_t preview_drops;     /* 丢失帧累计计数 */
    uint32_t preview_fps;       /* 当前预览帧率 */
    size_t   latest_jpeg_bytes; /* 最近一帧 JPEG 大小 */
    int      width;             /* 预览宽度 */
    int      height;            /* 预览高度 */
} board_ui_camera_stats_t;

/* UI 回调函数集，由外部模块提供配置读写能力 */
typedef struct {
    char *(*load_config_json)(void *user_data);       /* 读取配置 JSON 字符串（需调用者释放） */
    bool  (*save_config_json)(const char *json, void *user_data); /* 保存配置 JSON 字符串 */
    void  *user_data;                                  /* 回调上下文数据 */
} board_ui_callbacks_t;

/*
 * 注册 UI 回调函数集。必须在 board_ui_init 之前调用。
 * 参数: callbacks - 回调函数指针，NULL 则清空所有回调
 */
void board_ui_set_callbacks(const board_ui_callbacks_t *callbacks);

/*
 * 初始化 LVGL 显示系统（LCD、触摸、背光），创建 800x480 UI 布局。
 * 返回: ESP_OK 成功，否则为 ESP 错误码
 */
esp_err_t board_ui_init(void);
/*
 * 返回 UI 子系统是否已初始化完成。
 * 返回: true 已就绪
 */
bool board_ui_ready(void);

/*
 * 清空对话面板上所有消息以及 tool area、overlay 叠加层内容。
 */
void board_ui_conversation_clear(void);
/*
 * 向对话面板追加一条完整消息（结束之前流式片段）。
 * 参数: role - 对话角色
 *       text - 消息正文
 */
void board_ui_conversation_append(board_conversation_role_t role, const char *text);
/*
 * 向对话面板追加流式文本片段。同角色连续调用会合并到同一标签。
 * 参数: role - 对话角色
 *       text - 片段文本
 */
void board_ui_conversation_append_fragment(board_conversation_role_t role, const char *text);
/*
 * 结束当前流式片段，下次 append_fragment 将创建新标签。
 */
void board_ui_conversation_finish_fragment(void);
/*
 * 在对话面板覆盖层上绘制填充矩形（像素坐标相对于 overlay）。
 * 参数: x/y/width/height - 矩形位置和尺寸
 *       color - 16 位 RGB565 颜色值
 */
void board_ui_conversation_draw_rect(int x, int y, int width, int height, uint16_t color);
/*
 * 在对话面板覆盖层上绘制文本。
 * 参数: x/y  - 文本左上角坐标
 *       role - 角色（决定颜色）
 *       text - 文本内容
 */
void board_ui_conversation_draw_text(int x, int y, board_conversation_role_t role, const char *text);
/*
 * 触发 LVGL 重绘对话区域（root/conversation/overlay/tool panel）。
 */
void board_ui_conversation_request_flush(void);

/*
 * 清空 Tool Area 中所有内容（通过命令队列异步执行）。
 */
void board_ui_tool_area_clear(void);
/*
 * 向 Tool Area 追加一行文本（通过命令队列异步执行）。
 * 参数: role - 对话角色
 *       text - 文本内容
 */
void board_ui_tool_area_append(board_conversation_role_t role, const char *text);
/*
 * 在 Tool Area 中绘制填充矩形（通过命令队列异步执行）。
 * 参数: x/y/width/height - 矩形位置和尺寸
 *       color - 16 位 RGB565 颜色值
 */
void board_ui_tool_area_draw_rect(int x, int y, int width, int height, uint16_t color);
/*
 * 在 Tool Area 中绘制文本（通过命令队列异步执行）。
 * 参数: x/y  - 文本位置
 *       role - 角色（决定颜色）
 *       text - 文本内容
 */
void board_ui_tool_area_draw_text(int x, int y, board_conversation_role_t role, const char *text);
/*
 * 触发 Tool Area 刷新（通过命令队列异步执行）。
 */
void board_ui_tool_area_request_flush(void);

/*
 * 向引导日志界面追加一行日志条目。
 * 参数: role - 消息角色
 *       text - 日志文本
 */
void board_ui_boot_log_append(board_conversation_role_t role, const char *text);
/*
 * 删除引导日志覆盖层，切换回主 UI 界面。
 */
void board_ui_boot_log_finish(void);

/*
 * 更新摄像头预览帧。摄像头任务不应持有 LVGL 锁，此函数仅写入后台缓冲区。
 * 参数: rgb565 - 240x240 RGB565 像素数据
 *       width  - 图像宽度（必须等于 240）
 *       height - 图像高度（必须等于 240）
 */
void board_ui_camera_preview_update(const void *rgb565, int width, int height);
/*
 * 更新摄像头性能统计数据。
 * 参数: stats - 指向统计数据的指针，NULL 则忽略
 */
void board_ui_camera_stats_update(const board_ui_camera_stats_t *stats);
/*
 * 设置配置加载状态，触发外部字体加载并应用到所有 UI 对象。
 * 参数: loaded   - 配置是否已加载
 *       provider - 模型提供商名称
 *       model    - 模型名称
 */
void board_ui_set_config_loaded(bool loaded, const char *provider, const char *model);
/*
 * 设置 USB 大容量存储挂载状态。
 * 参数: mounted - 是否已挂载
 */
void board_ui_set_usb_mounted(bool mounted);
/*
 * 设置 Wi-Fi 连接状态和详情（显示在状态栏中）。
 * 参数: connected - Wi-Fi 是否已连接
 *       detail    - IP 地址等详情字符串
 */
void board_ui_set_wifi_state(bool connected, const char *detail);
/*
 * 设置需要重启标志（配置变更后提醒）。
 * 参数: required - 是否需要重启
 */
void board_ui_set_runtime_restart_required(bool required);
/*
 * OTA 准备阶段：显示 OTA 覆盖层、固件名称和预期大小。
 * 参数: name          - 固件名称
 *       expected_size - 预期固件大小（字节）
 *       message       - 附加状态消息
 */
void board_ui_ota_prepare(const char *name, size_t expected_size, const char *message);
/*
 * OTA 写入开始：显示 OTA 覆盖层并初始化进度条。
 * 参数: name          - 固件名称
 *       expected_size - 预期固件大小（字节）
 */
void board_ui_ota_begin(const char *name, size_t expected_size);
/*
 * OTA 进度更新：刷新进度条（最多每 250ms 更新一次）。
 * 参数: bytes_written - 已写入字节数
 *       expected_size - 预期总大小
 */
void board_ui_ota_progress(size_t bytes_written, size_t expected_size);
/*
 * OTA 结束：显示成功/失败结果消息。
 * 参数: ok      - 是否成功
 *       message - 结果消息
 */
void board_ui_ota_end(bool ok, const char *message);
/*
 * 为重启做准备：显示黑色覆盖层并强制 LVGL 刷新一帧后延迟 250ms。
 */
void board_ui_prepare_for_reboot(void);
/*
 * 显示配置编辑模态框，加载当前 JSON 配置并构建表单 UI。
 */
void board_ui_show_config_editor(void);
