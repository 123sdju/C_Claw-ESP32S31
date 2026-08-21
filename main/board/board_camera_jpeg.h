#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * 使用 ESP32 硬件 JPEG 编码器将 RGB565 数据编码为 JPEG。
 *
 * 参数:
 *   raw          - RGB565 原始数据
 *   raw_bytes    - 原始数据字节数
 *   width        - 图像宽度
 *   height       - 图像高度
 *   quality      - JPEG 质量 (1-100)
 *   out_jpeg     - 输出 JPEG 数据（调用者负责 free）
 *   out_jpeg_size- 输出 JPEG 大小
 * 返回: ESP_OK 成功。
 */
esp_err_t board_camera_encode_rgb565_jpeg(uint8_t *raw,
                                          size_t raw_bytes,
                                          int width,
                                          int height,
                                          int quality,
                                          uint8_t **out_jpeg,
                                          size_t *out_jpeg_size);

/**
 * 将 JPEG 二进制数据编码为 Base64 字符串。
 *
 * 参数:
 *   jpeg       - JPEG 数据
 *   jpeg_size  - JPEG 数据大小
 *   out_base64 - 输出 Base64 字符串（调用者负责 free）
 * 返回: ESP_OK 成功。
 */
esp_err_t board_camera_jpeg_to_base64(const uint8_t *jpeg,
                                      size_t jpeg_size,
                                      char **out_base64);
