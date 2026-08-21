#include "app_status.h"

#include <string.h>

#include "board_camera.h"
#include "esp_timer.h"
#include "usb_storage_service.h"

/*
 * 采集运行时状态快照到 out_status 结构体。
 *
 * 参数:
 *   out_status - 输出参数，接收当前状态快照。
 *
 * 从各子系统采集最新指标：通过 esp_timer_get_time() 获取上电时间、
 * 从 usb_storage_service 查询 USB 挂载状态、从 board_camera 获取摄像头
 * 运行帧率和统计数据、从 network_service 获取网络连接状态。
 * 调用方可以周期性调用此函数来更新 app_context_t 中的 status 字段。
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
