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

/**
 * @brief 采集时间、USB 挂载、摄像头和网络的状态值。
 * @param[out] out_status 由调用者独占的输出结构体，NULL 时直接返回。
 * @post 非空输出先清零再填充，调用者可按值保存或复制。
 * @note 子系统分次查询，各字段允许存在时间偏差。查询可能等待子系统锁，
 *       应从任务上下文调用，并在跨任务共享输出时由调用者同步。
 */
void app_status_collect(app_status_t *out_status);
