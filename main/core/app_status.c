#include "app_status.h"

#include <string.h>

#include "board_camera.h"
#include "esp_timer.h"
#include "usb_storage_service.h"

/**
 * @brief 按顺序采集上电时间、USB、摄像头和网络的观测快照。
 * @param[out] out_status 调用者独占的输出存储，NULL 时直接返回。
 * @post 非空输出先归零，再填充各子系统查询结果，结构体内数据均按值保存。
 * @note 各查询独立同步，摄像头运行标志与统计也分次读取，因此字段可能来自相邻时刻。
 *       network_service_get_status 锁竞争时提供保守状态，此接口沿用该观测语义。
 *       采集可能等待子系统锁，适合任务上下文调用。
 */
void app_status_collect(app_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->uptime_ms = (uint64_t)(esp_timer_get_time() / 1000);
    out_status->usb_mounted = usb_storage_service_is_mounted();
    out_status->camera_preview_running = board_camera_preview_running();
    board_camera_get_stats(&out_status->camera);
    network_service_get_status(&out_status->network);
}
