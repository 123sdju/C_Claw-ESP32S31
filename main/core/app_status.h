#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "board_camera.h"
#include "network_service.h"

/* 应用运行时状态快照，由 app_status_collect() 周期性采集。 */
typedef struct {
    uint64_t uptime_ms;                    /* 上电后的毫秒数 */
    bool usb_mounted;                      /* USB 大容量存储是否已挂载 */
    bool camera_preview_running;           /* 摄像头预览流是否运行中 */
    board_camera_stats_t camera;           /* 摄像头统计数据（帧数、FPS 等） */
    network_service_status_t network;      /* 网络连接状态（WiFi、IP 等） */
} app_status_t;

/*
 * 从各子系统采集当前运行时状态，填充到 out_status。
 *
 * 参数:
 *   out_status - 输出参数，接收状态快照。为 NULL 则无操作。
 */
void app_status_collect(app_status_t *out_status);
