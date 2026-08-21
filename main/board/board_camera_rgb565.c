#include "board_camera_rgb565.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "board_camera_rgb565";

/*
 * 交换 RGB565 像素的红蓝通道。
 *
 * RGB565 位布局：RRRRRGGGGGGBBBBB。
 * 拆分为 R（高5位）、G（中6位）、B（低5位），重组为 B 在高位、R 在低位的格式。
 *
 * 参数:
 *   c - 原始 16 位 RGB565 像素值
 * 返回: 红蓝通道交换后的像素值。
 */
static inline uint16_t swap_rgb565_rb(uint16_t c)
{
    uint16_t r = (c >> 11) & 0x1f;
    uint16_t g = (c >> 5) & 0x3f;
    uint16_t b = c & 0x1f;

    return (uint16_t)((b << 11) | (g << 5) | r);
}

/*
 * 从 V4L2 源缓冲区逐行复制 RGB565 帧到目标缓冲区，支持 stride、字节交换和红蓝交换。
 *
 * 功能：
 *  - 支持 stride（src_stride_bytes），逐行复制而非整块 memcpy。
 *  - swap_bytes：将大端 RGB565 的两字节交换为小端。
 *  - swap_rb：交换每个像素的红蓝通道。
 *  - 不完整帧检测：当 bytesused 或 src_buffer_len 不足时丢弃并返回 ESP_ERR_INVALID_SIZE。
 *
 * 参数:
 *   dst          - 目标缓冲区（dst_line_bytes = width * 2）
 *   src          - 源 V4L2 缓冲区指针
 *   src_buffer_len - V4L2 缓冲区总长度
 *   bytesused    - V4L2 buf.bytesused，0 时回退使用 src_buffer_len
 *   cfg          - 复制配置（宽、高、src_stride、swap_bytes、swap_rb）
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_ARG 参数无效，ESP_ERR_INVALID_SIZE 帧不完整。
 */
esp_err_t board_camera_rgb565_copy(uint8_t *dst,
                                   const uint8_t *src,
                                   size_t src_buffer_len,
                                   size_t bytesused,
                                   const board_camera_rgb565_copy_config_t *cfg)
{
    if (!dst || !src || !cfg || cfg->width <= 0 || cfg->height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t dst_line_bytes = (size_t)cfg->width * 2;
    const size_t src_line_bytes = cfg->src_stride_bytes;
    const size_t required_src_bytes = src_line_bytes * (size_t)cfg->height;
    size_t available = bytesused ? bytesused : src_buffer_len;

    if (src_line_bytes < dst_line_bytes ||
        src_buffer_len < required_src_bytes ||
        available < required_src_bytes) {
        ESP_LOGW(TAG,
                 "drop incomplete RGB565 frame: bytesused=%u buf_len=%u required=%u stride=%u",
                 (unsigned)bytesused,
                 (unsigned)src_buffer_len,
                 (unsigned)required_src_bytes,
                 (unsigned)src_line_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    for (int y = 0; y < cfg->height; y++) {
        const uint8_t *src_line = src + (size_t)y * src_line_bytes;
        uint8_t *dst_line = dst + (size_t)y * dst_line_bytes;

        if (cfg->swap_bytes || cfg->swap_rb) {
            uint16_t *dst16 = (uint16_t *)dst_line;
            const uint16_t *src16 = (const uint16_t *)src_line;

            for (int x = 0; x < cfg->width; x++) {
                uint16_t c;

                if (cfg->swap_bytes) {
                    const uint8_t *p = &src_line[x * 2];
                    c = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
                } else {
                    c = src16[x];
                }

                if (cfg->swap_rb) {
                    c = swap_rgb565_rb(c);
                }

                dst16[x] = c;
            }
        } else {
            memcpy(dst_line, src_line, dst_line_bytes);
        }
    }

    return ESP_OK;
}
