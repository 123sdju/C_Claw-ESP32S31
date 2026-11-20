#include "board_camera_rgb565.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "board_camera_rgb565";

/**
 * @brief 对按值 RGB565 像素交换高低两个五位通道，保留中间绿色六位。
 * @param[in] c 原像素位值，与其内存字节序无关。
 * @return 交换后的独立十六位像素值，无资源或共享状态。
 */
static inline uint16_t swap_rgb565_rb(uint16_t c)
{
    uint16_t r = (c >> 11) & 0x1f;
    uint16_t g = (c >> 5) & 0x3f;
    uint16_t b = c & 0x1f;

    return (uint16_t)((b << 11) | (g << 5) | r);
}

/**
 * @brief 校验整帧后逐行复制 RGB565，按配置转换字节序及红蓝通道。
 * @param[out] dst 调用者独占缓冲，容量至少 width*height*2，须与源范围不重叠。
 * @param[in] src 调用期间稳定的源借用，允许非对齐地址及奇数步长。
 * @param[in] src_buffer_len 源实际容量。
 * @param[in] bytesused 驱动报告的有效字节数，须覆盖全部含填充行且不超过容量。
 * @param[in] cfg 调用期间稳定的宽高、源步长及转换标志借用。
 * @return 成功、参数错误或完整性/容量溢出错误，参数与尺寸失败保留原目标字节。
 * @note 无转换时按行 memcpy，转换时按字节访问并输出小端 RGB565。
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
    if ((size_t)cfg->width > SIZE_MAX / 2U || src_line_bytes < dst_line_bytes ||
        src_line_bytes > SIZE_MAX / (size_t)cfg->height) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t required_src_bytes = src_line_bytes * (size_t)cfg->height;
    size_t available = bytesused;

    if (src_buffer_len < required_src_bytes ||
        available < required_src_bytes || available > src_buffer_len) {
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
            for (int x = 0; x < cfg->width; x++) {
                uint16_t c;
                size_t offset = (size_t)x * 2U;
                const uint8_t *p = src_line + offset;

                if (cfg->swap_bytes) {
                    c = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
                } else {
                    c = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
                }

                if (cfg->swap_rb) {
                    c = swap_rgb565_rb(c);
                }

                /* 逐字节输出小端像素，保持奇数行步长和非对齐源地址可用。 */
                dst_line[offset] = (uint8_t)c;
                dst_line[offset + 1U] = (uint8_t)(c >> 8);
            }
        } else {
            memcpy(dst_line, src_line, dst_line_bytes);
        }
    }

    return ESP_OK;
}
