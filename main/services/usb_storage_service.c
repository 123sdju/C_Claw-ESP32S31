#include "usb_storage_service.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define usb_storage_service_STACK_IN_PSRAM 1
#else
#define usb_storage_service_STACK_IN_PSRAM 0
#endif

static const char *TAG = "usb_storage";

static QueueHandle_t s_usb_events;
static msc_host_device_handle_t s_msc_device;
static msc_host_vfs_handle_t s_vfs_handle;
static bool s_usb_mounted;

/*
 * 创建 FreeRTOS 任务，优先 PSRAM 堆栈。
 */
static BaseType_t usb_storage_service_create_task(TaskFunction_t task,
                                         const char *name,
                                         uint32_t stack_bytes,
                                         void *arg,
                                         UBaseType_t priority,
                                         TaskHandle_t *out_handle)
{
#if usb_storage_service_STACK_IN_PSRAM
    BaseType_t ok = xTaskCreateWithCaps(task,
                                        name,
                                        stack_bytes,
                                        arg,
                                        priority,
                                        out_handle,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) return ok;

    ESP_LOGW(TAG, "PSRAM stack allocation failed for %s, retrying internal", name);
    return xTaskCreateWithCaps(task,
                               name,
                               stack_bytes,
                               arg,
                               priority,
                               out_handle,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(task, name, stack_bytes, arg, priority, out_handle);
#endif
}

/*
 * 删除当前任务自身（根据配置选择带或不带 PSRAM 标记的 vTaskDelete）。
 */
static void usb_storage_service_task_delete_self(void)
{
#if usb_storage_service_STACK_IN_PSRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

/*
 * MSC 设备插拔事件回调：将设备连接/断开事件写入 FreeRTOS 队列。
 */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;

    if (!s_usb_events) {
        return;
    }

    usb_storage_event_t msg = {0};
    if (event->event == MSC_DEVICE_CONNECTED) {
        msg.id = USB_STORAGE_DEVICE_CONNECTED;
        msg.data.address = event->device.address;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        msg.id = USB_STORAGE_DEVICE_DISCONNECTED;
        msg.data.handle = event->device.handle;
    } else {
        return;
    }

    if (xQueueSend(s_usb_events, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "USB event queue is full");
    }
}

/*
 * USB Host 主任务：安装 USB Host 和 MSC 驱动，循环处理 USB 主机库事件。
 */
static void usb_host_task(void *arg)
{
    (void)arg;

    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LOWMED,
    };

    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install failed: %s", esp_err_to_name(err));
        usb_storage_service_task_delete_self();
        return;
    }

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };

    err = msc_host_install(&msc_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install failed: %s", esp_err_to_name(err));
        usb_host_uninstall();
        usb_storage_service_task_delete_self();
        return;
    }

    while (true) {
        uint32_t event_flags = 0;
        err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_host_lib_handle_events failed: %s", esp_err_to_name(err));
            continue;
        }

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/*
 * 初始化 USB 存储服务：创建插拔事件队列（4 深度）。
 * 返回: ESP_OK 成功
 */
esp_err_t usb_storage_service_init(QueueHandle_t *out_events)
{
    if (!out_events) {
        return ESP_ERR_INVALID_ARG;
    }
    s_usb_events = xQueueCreate(4, sizeof(usb_storage_event_t));
    if (!s_usb_events) {
        return ESP_ERR_NO_MEM;
    }
    *out_events = s_usb_events;
    return ESP_OK;
}

/*
 * 启动 USB 主机驱动任务：绑定事件队列，创建 FreeRTOS 任务运行 USB MSC 主机栈，
 * 用于检测外部 USB 存储设备的插拔事件。
 * 参数: events - FreeRTOS 队列句柄，用于接收 USB 主机事件通知
 * 返回: ESP_OK 成功 / ESP_ERR_INVALID_ARG events 为空 / ESP_FAIL 任务创建失败
 */
esp_err_t usb_storage_service_start_host(QueueHandle_t events)
{
    if (!events) {
        return ESP_ERR_INVALID_ARG;
    }
    s_usb_events = events;
    BaseType_t task_ok = usb_storage_service_create_task(usb_host_task, "usb_host", 4096, NULL, 2, NULL);
    return task_ok == pdPASS ? ESP_OK : ESP_FAIL;
}

/*
 * 挂载指定 USB MSC 设备到 /usb0（FAT 文件系统）。
 */
esp_err_t usb_storage_service_mount(uint8_t address)
{
    if (s_usb_mounted) {
        ESP_LOGW(TAG, "A USB storage device is already mounted at %s", USB_STORAGE_MOUNT_PATH);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = msc_host_install_device(address, &s_msc_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 8192,
    };

    err = msc_host_vfs_register(s_msc_device, USB_STORAGE_MOUNT_PATH, &mount_config, &s_vfs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
        msc_host_uninstall_device(s_msc_device);
        s_msc_device = NULL;
        return err;
    }

    s_usb_mounted = true;
    ESP_LOGI(TAG, "USB storage mounted at %s", USB_STORAGE_MOUNT_PATH);
    return ESP_OK;
}

/*
 * 卸载 USB MSC 存储设备：先注销 VFS 文件系统注册，再卸载 MSC 设备驱动，
 * 最后将挂载状态标记为 false。传入的 handle 与当前设备不匹配时直接返回。
 * 参数: handle - 待卸载的 MSC 设备句柄
 * 返回: 无
 */
void usb_storage_service_unmount(msc_host_device_handle_t handle)
{
    if (!s_usb_mounted || handle != s_msc_device) {
        return;
    }

    if (s_vfs_handle) {
        esp_err_t err = msc_host_vfs_unregister(s_vfs_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "msc_host_vfs_unregister failed: %s", esp_err_to_name(err));
        }
        s_vfs_handle = NULL;
    }

    if (s_msc_device) {
        esp_err_t err = msc_host_uninstall_device(s_msc_device);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "msc_host_uninstall_device failed: %s", esp_err_to_name(err));
        }
        s_msc_device = NULL;
    }

    s_usb_mounted = false;
    ESP_LOGI(TAG, "USB storage disconnected; loaded config remains in RAM");
}

/*
 * 查询 USB 存储设备是否已挂载。
 * 参数: 无
 * 返回: true 已挂载 / false 未挂载
 */
bool usb_storage_service_is_mounted(void)
{
    return s_usb_mounted;
}
