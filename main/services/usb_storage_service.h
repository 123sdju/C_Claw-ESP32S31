#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "usb/msc_host.h"

#define USB_STORAGE_MOUNT_PATH "/usb0"

typedef enum {
    USB_STORAGE_DEVICE_CONNECTED,
    USB_STORAGE_DEVICE_DISCONNECTED,
} usb_storage_event_id_t;

typedef struct {
    usb_storage_event_id_t id;
    union {
        uint8_t address;
        msc_host_device_handle_t handle;
    } data;
} usb_storage_event_t;

/*
 * 初始化 USB 存储服务：创建插拔事件队列（4 深度，usb_storage_event_t）。
 * 参数: out_events - 输出队列句柄
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_ARG/ESP_ERR_NO_MEM 失败
 */
esp_err_t usb_storage_service_init(QueueHandle_t *out_events);
/*
 * 启动 USB Host 任务：安装 USB Host 和 MSC 驱动，进入主事件循环等待插拔事件。
 * 参数: events - 事件队列句柄（由 usb_storage_service_init 创建）
 * 返回: ESP_OK 任务创建成功，ESP_FAIL 失败
 */
esp_err_t usb_storage_service_start_host(QueueHandle_t events);
/*
 * 挂载指定地址的 USB MSC 设备到 /usb0 路径（FAT 文件系统，max_files=4）。
 * 参数: address - USB 设备地址
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_STATE 已挂载，其他为 ESP-IDF 错误码
 */
esp_err_t usb_storage_service_mount(uint8_t address);
/*
 * 卸载 USB MSC 设备：注销 VFS、卸载设备、清除挂载标志。
 * 参数: handle - MSC 设备句柄
 */
void usb_storage_service_unmount(msc_host_device_handle_t handle);
/*
 * 查询当前是否有 USB 存储设备处于挂载状态。
 * 返回: true 已挂载，false 未挂载
 */
bool usb_storage_service_is_mounted(void);
