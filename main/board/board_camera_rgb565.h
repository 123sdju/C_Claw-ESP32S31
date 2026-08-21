#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    int width;               /**< 图像宽度（像素） */
    int height;              /**< 图像高度（像素） */
    size_t src_stride_bytes; /**< V4L2 源缓冲区每行字节数（含填充） */
    bool swap_bytes;         /**< 是否执行 RGB565 字节交换（大端↔小端） */
    bool swap_rb;            /**< 是否交换红蓝通道 */
} board_camera_rgb565_copy_config_t;

/**
 * 从 V4L2 源缓冲区逐行复制 RGB565 帧到目标缓冲区，支持 stride、字节交换和红蓝交换。
 *
 * 参数:
 *   dst          - 目标缓冲区
 *   src          - 源 V4L2 缓冲区
 *   src_buffer_len - 源缓冲区长度
 *   bytesused    - V4L2 bytesused 字段值
 *   cfg          - 复制配置（宽高、stride、字节交换、红蓝交换标志）
 * 返回: ESP_OK 成功。
 */
esp_err_t board_camera_rgb565_copy(uint8_t *dst,
                                   const uint8_t *src,
                                   size_t src_buffer_len,
                                   size_t bytesused,
                                   const board_camera_rgb565_copy_config_t *cfg);
