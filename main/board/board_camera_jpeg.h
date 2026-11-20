#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 将紧密 RGB565 矩形借给当前目标硬件同步编码为 JPEG。
 * @param[in] raw 调用期稳定输入，覆盖整个像素矩形。
 * @param[in] raw_bytes 缓冲容量，须至少 width*height*2，额外尾部不参与编码。
 * @param[in] width 正宽度，当前编码硬件上限 16383 像素。
 * @param[in] height 正高度，上限 16383 像素。
 * @param[in] quality 一至一百，低于一取七十，高于一百取一百。
 * @param[out] out_jpeg 必填新/已清理槽，先置 NULL，成功由调用者 free。
 * @param[out] out_jpeg_size 必填独立槽，先置零，成功为有效输出字节数。
 * @return 成功或参数、尺寸、内存及驱动错误，失败输出为空。
 * @note 原生驱动串行共享硬件，200 ms 为编码事件等待配置，整体耗时还包含共享锁。
 */
esp_err_t board_camera_encode_rgb565_jpeg(uint8_t *raw,
                                          size_t raw_bytes,
                                          int width,
                                          int height,
                                          int quality,
                                          uint8_t **out_jpeg,
                                          size_t *out_jpeg_size);

/**
 * @brief 按需将 JPEG 二进制数据编码为独立 Base64 文本。
 * @param[in] jpeg 调用期间稳定的输入借用。
 * @param[in] jpeg_size 正字节数，检查输出容量溢出。
 * @param[out] out_base64 必填新/已清理槽，先置 NULL，成功由调用者 free。
 * @return 成功、参数/容量/内存错误或编码失败，失败没有部分输出。
 */
esp_err_t board_camera_jpeg_to_base64(const uint8_t *jpeg,
                                      size_t jpeg_size,
                                      char **out_base64);

#ifdef __cplusplus
}
#endif
