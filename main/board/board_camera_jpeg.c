#include "board_camera_jpeg.h"

#include <stdlib.h>
#include <string.h>

#include "board_camera.h"
#include "driver/jpeg_encode.h"
#include "mbedtls/base64.h"
#include <stdint.h>

/* 当前目标 JPEG 编码硬件的宽高各支持十四位，由 soc/jpeg_struct.h 的 pic_size 定义。 */
#define BOARD_CAMERA_JPEG_MAX_DIMENSION 16383U

/**
 * @brief 将紧密 RGB565 矩形同步编码为 YUV422 子采样的 JPEG。
 * @param[in] raw 调用期稳定的紧密像素借用，硬件完成前保持有效。
 * @param[in] raw_bytes 输入缓冲容量，必须覆盖 width*height*2，仅编码矩形部分。
 * @param[in] width 正像素宽度，最大 16383，符合当前硬件十四位限制。
 * @param[in] height 正像素高度，最大 16383。
 * @param[in] quality 一至一百，低于一采用七十，高于一百采用一百。
 * @param[out] out_jpeg 独立的新/已清理指针槽，存在时先置 NULL，成功交付 free 兼容数据。
 * @param[out] out_jpeg_size 独立大小槽，存在时先置零，成功为有效 JPEG 字节数。
 * @return 成功或参数、尺寸、分配、驱动/释放错误，失败归还本次输出分配。
 * @note 每次独立创建引擎，原生共享编解码锁负责硬件串行。200 ms 配置用于原生
 *       编码事件等待，锁等待和整个调用耗时遵循当前 IDF 驱动实现。
 */
esp_err_t board_camera_encode_rgb565_jpeg(uint8_t *raw,
                                          size_t raw_bytes,
                                          int width,
                                          int height,
                                          int quality,
                                          uint8_t **out_jpeg,
                                          size_t *out_jpeg_size)
{
    if (out_jpeg) *out_jpeg = NULL;
    if (out_jpeg_size) *out_jpeg_size = 0;
    if (!raw || raw_bytes == 0 || width <= 0 || height <= 0 || !out_jpeg || !out_jpeg_size) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((unsigned)width > BOARD_CAMERA_JPEG_MAX_DIMENSION ||
        (unsigned)height > BOARD_CAMERA_JPEG_MAX_DIMENSION ||
        (size_t)width > SIZE_MAX / 2U ||
        (size_t)height > SIZE_MAX / ((size_t)width * 2U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t input_bytes = (size_t)width * 2U * (size_t)height;
    if (raw_bytes < input_bytes || input_bytes > UINT32_MAX) return ESP_ERR_INVALID_SIZE;

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
    uint8_t *jpeg = jpeg_alloc_encoder_mem(input_bytes, &out_mem_cfg, &jpeg_alloc_size);

    if (!jpeg) {
        jpeg_del_encoder_engine(jpeg_handle);
        return ESP_ERR_NO_MEM;
    }
    if (jpeg_alloc_size < input_bytes || jpeg_alloc_size > UINT32_MAX) {
        jpeg_del_encoder_engine(jpeg_handle);
        free(jpeg);
        return ESP_ERR_INVALID_SIZE;
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
                               (uint32_t)input_bytes,
                               jpeg,
                               (uint32_t)jpeg_alloc_size,
                               &jpeg_size);

    esp_err_t release_ret = jpeg_del_encoder_engine(jpeg_handle);
    if (ret == ESP_OK) ret = release_ret;
    if (ret == ESP_OK && (jpeg_size == 0 || jpeg_size > jpeg_alloc_size)) ret = ESP_ERR_INVALID_SIZE;

    if (ret != ESP_OK) {
        free(jpeg);
        return ret;
    }

    *out_jpeg = jpeg;
    *out_jpeg_size = jpeg_size;
    return ESP_OK;
}

/**
 * @brief 两次调用原生 Base64 编码器，精确分配含终止符的输出空间。
 * @param[in] jpeg 调用期稳定的二进制借用。
 * @param[in] jpeg_size 正输入字节数，原生长度计算溢出时拒绝。
 * @param[out] out_base64 必需新/已清理槽，存在时先置 NULL，成功由调用者 free。
 * @return 成功、参数/尺寸/分配错误或原生编码失败，失败不发布部分文本。
 * @note 所有状态和分配归本次调用，输入缓冲稳定时可并发执行。
 */
esp_err_t board_camera_jpeg_to_base64(const uint8_t *jpeg,
                                      size_t jpeg_size,
                                      char **out_base64)
{
    if (out_base64) *out_base64 = NULL;
    if (!jpeg || jpeg_size == 0 || !out_base64) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t base64_len = 0;
    int ret = mbedtls_base64_encode(NULL, 0, &base64_len, jpeg, jpeg_size);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }

    if (base64_len == 0 || base64_len == SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t capacity = base64_len;
    char *base64 = malloc(capacity);
    if (!base64) {
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_encode((unsigned char *)base64,
                                capacity,
                                &base64_len,
                                jpeg,
                                jpeg_size);
    if (ret != 0 || base64_len >= capacity) {
        free(base64);
        return ESP_FAIL;
    }

    base64[base64_len] = '\0';
    *out_base64 = base64;
    return ESP_OK;
}

/**
 * @brief 消费 JPEG 数据并清零记录，保留调用者的结构体存储。
 * @param[in,out] image 独占且已初始化的记录，允许 NULL，数据须兼容 free。
 * @post 数据归还且字段归零，空记录可重复清理。
 * @note 拥有者先结束所有数据借用，按值复制记录时须明确唯一释放责任。
 */
void board_camera_jpeg_cleanup(board_camera_jpeg_t *image)
{
    if (!image) {
        return;
    }

    free(image->jpeg_data);
    memset(image, 0, sizeof(*image));
}
