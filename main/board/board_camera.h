#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define BOARD_CAMERA_WIDTH 240
#define BOARD_CAMERA_HEIGHT 240

typedef struct {
    uint8_t *jpeg_data;    /**< JPEG 原始数据缓冲区 */
    size_t jpeg_bytes;     /**< JPEG 数据字节数 */
    int width;             /**< 图像宽度（像素） */
    int height;            /**< 图像高度（像素） */
} board_camera_jpeg_t;

typedef struct {
    uint32_t preview_frames;   /**< 累计预览帧数 */
    uint32_t preview_drops;    /**< 累计丢弃帧数 */
    uint32_t preview_fps;      /**< 当前预览帧率（每秒更新） */
    size_t latest_jpeg_bytes;  /**< 最近一次 JPEG 编码字节数 */
    int width;                 /**< 摄像头输出宽度（像素） */
    int height;                /**< 摄像头输出高度（像素） */
} board_camera_stats_t;

typedef struct {
    void (*preview_update)(const void *rgb565, int width, int height, void *user_data); /**< 预览帧更新回调 */
    void (*stats_update)(const board_camera_stats_t *stats, void *user_data);            /**< 统计更新回调 */
    void *user_data;                                                                    /**< 用户数据，透传给回调函数 */
} board_camera_callbacks_t;

/**
 * 设置摄像头回调函数，传入 NULL 可清空所有回调。
 *
 * 参数:
 *   callbacks - 回调结构体指针（preview_update、stats_update、user_data）
 */
void board_camera_set_callbacks(const board_camera_callbacks_t *callbacks);
/**
 * 初始化摄像头模块：打开 V4L2 设备、配置格式、分配帧缓冲。
 *
 * 返回: ESP_OK 成功，否则 ESP 错误码。
 */
esp_err_t board_camera_init(void);
/**
 * 启动 V4L2 视频流并创建预览任务。
 *
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_STATE 未初始化，
 *       ESP_ERR_NO_MEM 任务创建失败。
 */
esp_err_t board_camera_start_preview(void);
/**
 * 停止 V4L2 视频流并等待预览任务退出。
 *
 * 返回: ESP_OK 成功。
 */
esp_err_t board_camera_stop_preview(void);
/**
 * 暂停或恢复摄像头预览（仅跳过帧处理，不停止 V4L2 流）。
 *
 * 参数:
 *   pause - true 暂停，false 恢复。
 */
void board_camera_pause_preview(bool pause);
/**
 * 查询摄像头预览是否正在运行。
 *
 * 返回: true 表示预览运行中。
 */
bool board_camera_preview_running(void);
/**
 * 获取当前摄像头统计信息快照。
 *
 * 参数:
 *   out_stats - 输出统计结构体（包含帧数、FPS、分辨率等）。
 */
void board_camera_get_stats(board_camera_stats_t *out_stats);
/**
 * 捕获当前帧并编码为 JPEG，不生成 Base64。
 *
 * 参数:
 *   quality   - JPEG 质量 (1-100)
 *   out_image - 输出结果结构体，使用后需调用 board_camera_jpeg_cleanup 释放。
 * 返回: ESP_OK 成功。
 */
esp_err_t board_camera_capture_jpeg(int quality, board_camera_jpeg_t *out_image);
/* 按需把 JPEG 编码为 Base64；out_base64 成功后由调用方 free。 */
esp_err_t board_camera_encode_base64(
    const uint8_t *jpeg_data,
    size_t jpeg_bytes,
    char **out_base64);
/**
 * 释放 board_camera_capture_jpeg 分配的 JPEG 资源。
 *
 * 参数:
 *   image - 之前由 board_camera_capture_jpeg 填充的结构体。
 */
void board_camera_jpeg_cleanup(board_camera_jpeg_t *image);
