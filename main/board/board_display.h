#pragma once

#include "esp_err.h"

/*
 * 初始化 LCD 显示驱动，获取 esp_board_manager 中注册的 display_lcd 设备句柄并创建绘制互斥锁。
 * 返回: ESP_OK 成功，否则为 ESP 错误码
 */
esp_err_t board_display_init(void);
/*
 * 在 LCD 上从 (0,0) 绘制 RGB565 位图。
 * 参数: pixels - RGB565 像素数据缓冲区
 *       width  - 位图宽度
 *       height - 位图高度
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_STATE 设备未就绪，ESP_ERR_INVALID_ARG 参数无效
 */
esp_err_t board_display_draw_rgb565(const void *pixels, int width, int height);
/*
 * 在 LCD 指定位置绘制 RGB565 位图，自动裁剪超出屏幕区域。
 * 参数: pixels - RGB565 像素数据缓冲区
 *       x/y/width/height - 绘制区域位置和尺寸
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_STATE 设备未就绪，ESP_ERR_INVALID_ARG 参数无效
 */
esp_err_t board_display_draw_rgb565_at(const void *pixels, int x, int y, int width, int height);
