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
static bool s_usb_host_started;
static msc_host_device_handle_t s_msc_device;
static msc_host_vfs_handle_t s_vfs_handle;
static bool s_usb_mounted;
static bool s_usb_unmounting;
static bool s_usb_mounting;
static bool s_usb_disconnected;
static uint64_t s_usb_generation = 1;
static msc_host_device_handle_t s_pending_disconnect;
static uint64_t s_pending_disconnect_generation;
static bool s_pending_connect;
static uint8_t s_pending_connect_address;
static portMUX_TYPE s_usb_state_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief 在状态临界区推进挂载身份，耗尽后关闭本盘可用性。
 * @pre 调用者持有 s_usb_state_mux，临界区内不执行驱动或文件系统操作。
 * @post 未到 UINT64_MAX 时加一，到顶后保持计数并标记断开，后续挂载无法重新开放 IO。
 */
static void advance_usb_generation_locked(void)
{
    if (s_usb_generation != UINT64_MAX) s_usb_generation++;
    else s_usb_disconnected = true;
}

/**
 * @brief 根据当前构建优先创建 PSRAM 栈任务，失败时尝试内部栈。
 * @param[in] task 必填任务入口，内核可在创建返回前执行。
 * @param[in] name 调用期有效名称借用。
 * @param[in] stack_bytes 正栈字节数，遵循 ESP-IDF 创建 API 单位。
 * @param[in,out] arg 借给任务的上下文，覆盖任务寿命，可为 NULL。
 * @param[in] priority 原生优先级。
 * @param[out] out_handle 可空句柄槽，成功时借出由任务自删的原生身份。
 * @return pdPASS 成功，其余为原生失败，创建者保留其他资源。
 * @note WithCaps 创建通过配对的删除入口归还栈与任务控制块。
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

/**
 * @brief 在启动失败退场路径通过配对 API 删除当前任务。
 * @pre 本任务已结束 USB 原生资源及共享状态访问，持有的驱动资源已经清理或登记保留。
 * @post 不再执行应用逻辑，内核负责回收任务栈和控制块。
 */
static void usb_storage_service_task_delete_self(void)
{
#if usb_storage_service_STACK_IN_PSRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

/**
 * @brief 同步接收 MSC 插拔，立即关闭断盘 IO 准入并零等待提交拥有的事件值。
 * @param[in] event 原生回调期借用事件，可为 NULL，设备句柄继续由 MSC 管理。
 * @param[in] arg 未使用的可空原生上下文。
 * @post 相关断开推进 generation 并登记补偿槽，连接队列满时保存最近地址供 owner 取回。
 * @note 队列和本文件状态沿进程期保留，回调只在短临界区改元数据，驱动和 VFS 由 owner 操作。
 */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;

    if (!event || !s_usb_events) {
        return;
    }

    usb_storage_event_t msg = {0};
    if (event->event == MSC_DEVICE_CONNECTED) {
        msg.id = USB_STORAGE_DEVICE_CONNECTED;
        msg.data.address = event->device.address;
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        msg.id = USB_STORAGE_DEVICE_DISCONNECTED;
        msg.data.handle = event->device.handle;
        portENTER_CRITICAL(&s_usb_state_mux);
        /* 安装期即使 owner 尚未公布实际句柄，已观察到的断开也使本次挂载失效。 */
        bool relevant = s_usb_mounting || (s_usb_mounted && event->device.handle == s_msc_device);
        if (relevant) {
            advance_usb_generation_locked();
            s_usb_disconnected = true;
            s_pending_disconnect = event->device.handle;
            s_pending_disconnect_generation = s_usb_generation;
        }
        msg.generation = s_usb_generation;
        portEXIT_CRITICAL(&s_usb_state_mux);
    } else {
        return;
    }

    if (xQueueSend(s_usb_events, &msg, 0) != pdTRUE) {
        if (msg.id == USB_STORAGE_DEVICE_CONNECTED) {
            portENTER_CRITICAL(&s_usb_state_mux);
            s_pending_connect_address = msg.data.address;
            s_pending_connect = true;
            portEXIT_CRITICAL(&s_usb_state_mux);
        }
        ESP_LOGW(TAG, "USB event queue is full");
    }
}

/**
 * @brief 异步安装 USB Host 和 MSC 后常驻处理主机库事件。
 * @param[in] arg 未使用的可空任务参数。
 * @post 正常路径常驻，安装失败记录错误并自删，清理失败保留启动标志以防重复安装。
 * @note MSC 创建自己的后台任务派发回调，本任务和队列为进程期资源。start_host 成功只
 *       表示本任务已创建，原生安装结果通过日志诊断，设备插拔由事件队列交给应用 owner。
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
        portENTER_CRITICAL(&s_usb_state_mux);
        s_usb_host_started = false;
        portEXIT_CRITICAL(&s_usb_state_mux);
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
        esp_err_t cleanup_err = usb_host_uninstall();
        if (cleanup_err != ESP_OK)
            ESP_LOGE(TAG, "usb_host_uninstall failed: %s", esp_err_to_name(cleanup_err));
        portENTER_CRITICAL(&s_usb_state_mux);
        s_usb_host_started = cleanup_err != ESP_OK;
        portEXIT_CRITICAL(&s_usb_state_mux);
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

/**
 * @brief 由唯一启动 owner 创建或借回进程期四深度插拔事件队列。
 * @param[out] out_events 必填句柄槽，参数有效后先置 NULL，成功借出队列。
 * @return 成功或 INVALID_ARG/NO_MEM，失败不交付句柄，已存在队列可重复取得。
 * @pre 初始化与主机创建由 owner 串行执行，返回队列在主机及全部回调存活期间不得删除。
 */
esp_err_t usb_storage_service_init(QueueHandle_t *out_events)
{
    if (!out_events) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_events = NULL;
    if (s_usb_events) {
        *out_events = s_usb_events;
        return ESP_OK;
    }
    s_usb_events = xQueueCreate(4, sizeof(usb_storage_event_t));
    if (!s_usb_events) {
        return ESP_ERR_NO_MEM;
    }
    *out_events = s_usb_events;
    return ESP_OK;
}

/**
 * @brief 绑定进程期事件队列并接纳唯一 USB Host 后台任务。
 * @param[in] events 非 NULL 稳定队列句柄，容量和元素类型须符合 init 的契约。
 * @return 任务创建成功为 ESP_OK，空队列为 INVALID_ARG，已启动为 INVALID_STATE，创建失败为 ESP_FAIL。
 * @note 成功只接纳任务，原生 Host/MSC 后续安装失败由任务记录。owner 保管队列直到进程结束，
 *       本接口不提供关闭进程期 USB 栈的操作，设备换盘由应用完整重启重新装配。
 */
esp_err_t usb_storage_service_start_host(QueueHandle_t events)
{
    if (!events) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_usb_state_mux);
    bool started = s_usb_host_started;
    if (!started) {
        s_usb_events = events;
        s_usb_host_started = true;
    }
    portEXIT_CRITICAL(&s_usb_state_mux);
    if (started) return ESP_ERR_INVALID_STATE;
    BaseType_t task_ok = usb_storage_service_create_task(usb_host_task, "usb_host", 4096, NULL, 2, NULL);
    if (task_ok != pdPASS) {
        portENTER_CRITICAL(&s_usb_state_mux);
        s_usb_host_started = false;
        portEXIT_CRITICAL(&s_usb_state_mux);
    }
    return task_ok == pdPASS ? ESP_OK : ESP_FAIL;
}

/**
 * @brief 安装指定地址的 MSC 设备并以禁止自动格式化的 FAT 配置注册 /usb0。
 * @param[in] address 主机事件提供的设备地址。
 * @return 完整注册成功为 ESP_OK，已有挂载/安装/清理为 INVALID_STATE，其他为原生安装或 VFS 错误。
 * @note 应用 owner 串行挂载和卸载，回调可并发使本次 generation 失效。成功后仍须查询当前
 *       可用性，安装期间断开会登记实际句柄的清理事件。VFS 失败后的卸载再失败时保留设备
 *       句柄及补偿事件，后续 unmount 重试，首个 VFS 错误继续返回。不开启磁盘身份绑定。
 */
esp_err_t usb_storage_service_mount(uint8_t address)
{
    portENTER_CRITICAL(&s_usb_state_mux);
    bool unavailable = s_usb_mounted || s_usb_unmounting || s_usb_mounting;
    uint64_t start_generation = s_usb_generation;
    if (!unavailable) { s_usb_mounting = true; s_usb_disconnected = false; }
    portEXIT_CRITICAL(&s_usb_state_mux);
    if (unavailable) {
        ESP_LOGW(TAG, "A USB storage device is already mounted at %s", USB_STORAGE_MOUNT_PATH);
        return ESP_ERR_INVALID_STATE;
    }

    msc_host_device_handle_t device = NULL;
    esp_err_t err = msc_host_install_device(address, &device);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_usb_state_mux);
        s_usb_mounting = false;
        portEXIT_CRITICAL(&s_usb_state_mux);
        ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
        return err;
    }
    portENTER_CRITICAL(&s_usb_state_mux);
    s_msc_device = device;
    portEXIT_CRITICAL(&s_usb_state_mux);

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 8192,
    };

    err = msc_host_vfs_register(device, USB_STORAGE_MOUNT_PATH, &mount_config, &s_vfs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
        esp_err_t cleanup_err = msc_host_uninstall_device(device);
        portENTER_CRITICAL(&s_usb_state_mux);
        s_usb_mounting = false;
        if (cleanup_err == ESP_OK) {
            s_msc_device = NULL;
        } else {
            s_usb_unmounting = true;
            s_usb_disconnected = true;
            advance_usb_generation_locked();
            s_pending_disconnect = device;
            s_pending_disconnect_generation = s_usb_generation;
        }
        portEXIT_CRITICAL(&s_usb_state_mux);
        if (cleanup_err != ESP_OK)
            ESP_LOGE(TAG, "USB mount rollback retained device: %s", esp_err_to_name(cleanup_err));
        return err;
    }

    portENTER_CRITICAL(&s_usb_state_mux);
    s_usb_mounted = true;
    s_usb_unmounting = false;
    s_usb_mounting = false;
    if (s_usb_generation != start_generation || s_usb_disconnected) {
        s_usb_disconnected = true;
        s_pending_disconnect = device;
    }
    advance_usb_generation_locked();
    if (s_usb_disconnected) {
        s_pending_disconnect = device;
        s_pending_disconnect_generation = s_usb_generation;
    }
    portEXIT_CRITICAL(&s_usb_state_mux);
    ESP_LOGI(TAG, "USB storage mounted at %s", USB_STORAGE_MOUNT_PATH);
    return ESP_OK;
}

/**
 * @brief 对当前设备关闭新的应用 IO 准入，为外层排空所有借用者建立起点。
 * @param[in] handle 借用的当前设备句柄，也可对应挂载回滚后待清理设备。
 * @return 匹配且不在安装中为 ESP_OK，空/过期句柄或无当前资源为 NOT_FOUND。
 * @note 此处只标记 unmounting，外层随后排空文件、runtime 和各存储借用再执行 unmount。
 */
esp_err_t usb_storage_service_begin_unmount(msc_host_device_handle_t handle)
{
    portENTER_CRITICAL(&s_usb_state_mux);
    bool matches = handle && handle == s_msc_device && !s_usb_mounting &&
                   (s_usb_mounted || s_usb_unmounting);
    if (matches) s_usb_unmounting = true;
    portEXIT_CRITICAL(&s_usb_state_mux);
    return matches ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/**
 * @brief 在外层完成排空后按 VFS、MSC 顺序消费当前设备资源。
 * @param[in] handle 当前设备或挂载回滚保留设备的借用句柄。
 * @return 全部归还为 ESP_OK，不匹配为 NOT_FOUND，原生失败保留未归还部分供重试。
 * @pre 唯一 USB owner 串行调用，先 begin_unmount 并排空已接纳的应用 IO 与文件句柄。
 * @note 成功注销的 VFS 不重复注销，设备卸载失败保留句柄。成功后清理相关补偿事件，
 *       下一次挂载采用新 generation，配置/记忆的全新装配由应用重启流程负责。
 */
esp_err_t usb_storage_service_unmount(msc_host_device_handle_t handle)
{
    portENTER_CRITICAL(&s_usb_state_mux);
    bool matches = handle && handle == s_msc_device && !s_usb_mounting &&
                   (s_usb_mounted || s_usb_unmounting);
    portEXIT_CRITICAL(&s_usb_state_mux);
    if (!matches) {
        return ESP_ERR_NOT_FOUND;
    }

    if (s_vfs_handle) {
        esp_err_t err = msc_host_vfs_unregister(s_vfs_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "msc_host_vfs_unregister failed: %s", esp_err_to_name(err));
            return err;
        }
        s_vfs_handle = NULL;
    }

    if (s_msc_device) {
        esp_err_t err = msc_host_uninstall_device(s_msc_device);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "msc_host_uninstall_device failed: %s", esp_err_to_name(err));
            return err;
        }
        portENTER_CRITICAL(&s_usb_state_mux);
        s_msc_device = NULL;
        portEXIT_CRITICAL(&s_usb_state_mux);
    }

    portENTER_CRITICAL(&s_usb_state_mux);
    s_usb_mounted = false;
    s_usb_unmounting = false;
    s_usb_disconnected = false;
    if (s_pending_disconnect == handle) {
        s_pending_disconnect = NULL;
        s_pending_disconnect_generation = 0;
    }
    portEXIT_CRITICAL(&s_usb_state_mux);
    ESP_LOGI(TAG, "USB storage device uninstalled");
    return ESP_OK;
}

/**
 * @brief 查询当前盘是否可接纳应用 IO。
 * @return 已注册挂载且未开始卸载、未观察断开为 true，其余为 false。
 * @note 仅为状态快照，实际 IO 必须结合 generation 和外层借用/关闭屏障。
 */
bool usb_storage_service_is_mounted(void)
{
    return usb_storage_service_current_generation(NULL);
}

/**
 * @brief 在同一临界区取得挂载身份及应用 IO 可用状态。
 * @param[out] out_generation 可空数字槽，非空时无论是否可用均写当前计数。
 * @return 当前挂载可用为 true，安装/清理/断开等状态为 false。
 * @note generation 是本次进程挂载序号，身份核对不延长底层资源寿命。
 */
bool usb_storage_service_current_generation(uint64_t *out_generation)
{
    portENTER_CRITICAL(&s_usb_state_mux);
    bool available = s_usb_mounted && !s_usb_unmounting && !s_usb_disconnected;
    if (out_generation) *out_generation = s_usb_generation;
    portEXIT_CRITICAL(&s_usb_state_mux);
    return available;
}

/**
 * @brief 取走断开补偿槽，恢复队列满或挂载回滚需要清理的设备通知。
 * @param[out] out_handle 必填借用句柄槽，参数有效时写当前待清理句柄或 NULL。
 * @param[out] out_generation 必填计数槽，参数有效时写对应 generation 或零。
 * @return 有待清理句柄为 true，其余为 false，空输出参数保持其他输出原值。
 * @note 取走只消费补偿通知，设备仍由服务保管，owner 核对当前身份后串行排空与卸载。
 */
bool usb_storage_service_take_pending_disconnect(msc_host_device_handle_t *out_handle, uint64_t *out_generation)
{
    if (!out_handle || !out_generation) return false;
    portENTER_CRITICAL(&s_usb_state_mux);
    *out_handle = s_pending_disconnect;
    *out_generation = s_pending_disconnect_generation;
    s_pending_disconnect = NULL;
    s_pending_disconnect_generation = 0;
    portEXIT_CRITICAL(&s_usb_state_mux);
    return *out_handle != NULL;
}

/**
 * @brief 取走因队列满保留的最近连接地址。
 * @param[out] out_event 必填值输出，有补偿时清零后填写连接事件和当前 generation。
 * @return 成功取走为 true，无事件或空槽为 false，此时保持已有输出内容。
 * @note 只保留最近地址，实际设备仍须由 owner 安装，地址可能已失效并由原生安装返回错误。
 */
bool usb_storage_service_take_pending_connect(usb_storage_event_t *out_event)
{
    if (!out_event) return false;
    portENTER_CRITICAL(&s_usb_state_mux);
    bool pending = s_pending_connect;
    if (pending) {
        memset(out_event, 0, sizeof(*out_event));
        out_event->id = USB_STORAGE_DEVICE_CONNECTED;
        out_event->data.address = s_pending_connect_address;
        out_event->generation = s_usb_generation;
        s_pending_connect = false;
    }
    portEXIT_CRITICAL(&s_usb_state_mux);
    return pending;
}

/**
 * @brief 核对断开通知是否仍指向当前待卸载设备和同一挂载身份。
 * @param[in] handle 通知中的借用设备句柄，NULL 不匹配。
 * @param[in] generation 通知中的本进程序号。
 * @return 设备匹配、资源存在且不在安装中、序号相同为 true，其余 false。
 * @note 包含 VFS 注册失败后保留的卸载状态，返回值不表示该盘可进行应用 IO。
 */
bool usb_storage_service_disconnect_is_current(msc_host_device_handle_t handle, uint64_t generation)
{
    portENTER_CRITICAL(&s_usb_state_mux);
    bool current = handle && handle == s_msc_device && !s_usb_mounting &&
                   (s_usb_mounted || s_usb_unmounting) && generation == s_usb_generation;
    portEXIT_CRITICAL(&s_usb_state_mux);
    return current;
}
