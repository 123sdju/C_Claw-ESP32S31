#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int width;               /**< 图像宽度（像素） */
    int height;              /**< 图像高度（像素） */
    size_t src_stride_bytes; /**< V4L2 源缓冲区每行字节数（含填充） */
    bool swap_bytes;         /**< 是否执行 RGB565 字节交换（大端↔小端） */
    bool swap_rb;            /**< 是否交换红蓝通道 */
} board_camera_rgb565_copy_config_t;

/**
 * @brief 校验完整 RGB565 帧后逐行复制，支持字节和红蓝通道转换。
 * @param[out] dst 独占目标，容量至少 width*height*2，与源范围互不重叠。
 * @param[in] src 本次调用稳定的源借用，允许非对齐地址。
 * @param[in] src_buffer_len 源缓冲实际字节容量。
 * @param[in] bytesused 驱动有效字节数，覆盖 stride*height 且不超过实际容量。
 * @param[in] cfg 稳定的正宽高、源步长和转换标志，步长至少 width*2。
 * @return 成功、参数或尺寸错误，失败发生在首次写目标之前。
 */
esp_err_t board_camera_rgb565_copy(uint8_t *dst,
                                   const uint8_t *src,
                                   size_t src_buffer_len,
                                   size_t bytesused,
                                   const board_camera_rgb565_copy_config_t *cfg);

#ifdef __cplusplus
}
#endif
