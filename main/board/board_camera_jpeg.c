#include "board_camera_jpeg.h"

#include <stdlib.h>
#include <string.h>

#include "board_camera.h"
#include "driver/jpeg_encode.h"
#include "mbedtls/base64.h"
#include <stdint.h>

/*
 * 使用 ESP32 硬件 JPEG 编码器将 RGB565 原始数据编码为 JPEG。
 *
 * 流程：
 *  1. 参数校验（raw 非空、尺寸有效、输出指针有效）和质量裁剪 (1-100)。
 *  2. 创建 JPEG 编码器引擎 (jpeg_new_encoder_engine)，超时 200ms。
 *  3. 分配编码器输出内存 (jpeg_alloc_encoder_mem)。
 *  4. 配置编码参数：RGB565 输入、YUV422 子采样、指定质量/宽高。
 *  5. 调用 jpeg_encoder_process 执行编码。
 *  6. 释放编码器引擎，失败时释放已分配的输出内存。
 *
 * 参数:
 *   raw          - RGB565 原始像素数据
 *   raw_bytes    - 原始数据字节数
 *   width        - 图像宽度（像素）
 *   height       - 图像高度（像素）
 *   quality      - JPEG 质量 (1-100)，越界自动裁剪
 *   out_jpeg     - 输出 JPEG 数据指针（调用者负责 free）
 *   out_jpeg_size- 输出 JPEG 数据大小
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_ARG 参数无效，ESP_ERR_NO_MEM 内存不足，
 *       其他 ESP 错误码表示编码失败。
 */
esp_err_t board_camera_encode_rgb565_jpeg(uint8_t *raw,
                                          size_t raw_bytes,
                                          int width,
                                          int height,
                                          int quality,
                                          uint8_t **out_jpeg,
                                          size_t *out_jpeg_size)
{
    if (!raw || raw_bytes == 0 || width <= 0 || height <= 0 || !out_jpeg || !out_jpeg_size) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_jpeg = NULL;
    *out_jpeg_size = 0;

    if (quality < 1) {
        quality = 70;
    } else if (quality > 100) {
        quality = 100;
    }

    jpeg_encoder_handle_t jpeg_handle = NULL;
    jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };

    esp_err_t ret = jpeg_new_encoder_engine(&engine_cfg, &jpeg_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    jpeg_encode_memory_alloc_cfg_t out_mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };

    size_t jpeg_alloc_size = 0;
    uint8_t *jpeg = jpeg_alloc_encoder_mem(raw_bytes, &out_mem_cfg, &jpeg_alloc_size);

    if (!jpeg) {
        jpeg_del_encoder_engine(jpeg_handle);
        return ESP_ERR_NO_MEM;
    }

    jpeg_encode_cfg_t encode_cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = (uint32_t)quality,
        .width = width,
        .height = height,
    };

    uint32_t jpeg_size = 0;
    ret = jpeg_encoder_process(jpeg_handle,
                               &encode_cfg,
                               raw,
                               raw_bytes,
                               jpeg,
                               jpeg_alloc_size,
                               &jpeg_size);

    jpeg_del_encoder_engine(jpeg_handle);

    if (ret != ESP_OK) {
        free(jpeg);
        return ret;
    }

    *out_jpeg = jpeg;
    *out_jpeg_size = jpeg_size;
    return ESP_OK;
}

/*
 * 使用 mbedtls_base64 将 JPEG 二进制数据编码为 Base64 字符串。
 *
 * 两步编码：先调用 mbedtls_base64_encode 获取所需长度，再分配足够内存执行实际编码。
 * 输出字符串以 '\0' 结尾。
 *
 * 参数:
 *   jpeg       - JPEG 二进制数据指针
 *   jpeg_size  - JPEG 数据字节数
 *   out_base64 - 输出 Base64 字符串指针（调用者负责 free）
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_ARG 参数无效，ESP_ERR_NO_MEM 内存不足，
 *       ESP_FAIL Base64 编码失败。
 */
esp_err_t board_camera_jpeg_to_base64(const uint8_t *jpeg,
                                      size_t jpeg_size,
                                      char **out_base64)
{
    if (!jpeg || jpeg_size == 0 || !out_base64) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_base64 = NULL;

    size_t base64_len = 0;
    int ret = mbedtls_base64_encode(NULL, 0, &base64_len, jpeg, jpeg_size);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }

    if (base64_len == SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *base64 = malloc(base64_len + 1U);
    if (!base64) {
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_encode((unsigned char *)base64,
                                base64_len,
                                &base64_len,
                                jpeg,
                                jpeg_size);
    if (ret != 0) {
        free(base64);
        return ESP_FAIL;
    }

    base64[base64_len] = '\0';
    *out_base64 = base64;
    return ESP_OK;
}

/*
 * 释放 board_camera_jpeg_t 结构体占用的堆内存并清零。
 *
 * 释放 jpeg_data（原始 JPEG 数据），然后清零结构体避免野指针。
 *
 * 参数:
 *   image - 要清理的结构体指针，NULL 时直接返回。
 */
void board_camera_jpeg_cleanup(board_camera_jpeg_t *image)
{
    if (!image) {
        return;
    }

    free(image->jpeg_data);
    memset(image, 0, sizeof(*image));
}
