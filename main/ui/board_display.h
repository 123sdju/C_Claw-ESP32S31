#pragma once

#include "esp_err.h"

/**
 * @brief 初始化并发布直接绘制入口，借用 Board Manager 的面板句柄。
 * @return ESP_OK 表示已取得可用面板，分配或板级错误透传，失败后 owner 可重试。
 * @pre 初始化 owner 串行调用，BSP 面板及绘制锁在发布后保留到进程结束。
 */
esp_err_t board_display_init(void);
/**
 * @brief 从固定 800×480 绘制区左上角提交 RGB565 图像。
 * @param[in] pixels 非 NULL 原始像素借用，源紧密排列且一行 width 乘二字节。
 * @param[in] width 源图正宽度，单位为像素。
 * @param[in] height 源图正高度，单位为像素。
 * @return 与 draw_rgb565_at 相同，成功仅表示提交完成。
 * @pre 源存储覆盖所读取的行及原生异步借用期，调用者在面板完成后回收。
 */
esp_err_t board_display_draw_rgb565(const void *pixels, int width, int height);
/**
 * @brief 在固定 800×480 绘制区提交图像，裁剪右边和底边并保留源行跨度。
 * @param[in] pixels 非 NULL RGB565 像素借用，一行原始 width 乘二字节。
 * @param[in] x 绘制区内的左上角水平像素坐标。
 * @param[in] y 绘制区内的左上角垂直像素坐标。
 * @param[in] width 原始源正宽度，单位为像素。
 * @param[in] height 原始源正高度，单位为像素。
 * @return 未初始化为 ESP_ERR_INVALID_STATE，参数或字节范围溢出为 ESP_ERR_INVALID_ARG，取锁失败为 ESP_FAIL，其他错误来自面板。
 * @pre 像素存储覆盖原生异步读取期，调用者负责完成通知，内部锁仅串行化提交。
 * @note 右边裁剪时逐行提交，后续行出错仍可能已有前缀行生效，返回错误也须完成已提交缓冲的回收屏障。
 */
esp_err_t board_display_draw_rgb565_at(const void *pixels, int x, int y, int width, int height);
