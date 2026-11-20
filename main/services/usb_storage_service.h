#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "usb/msc_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USB_STORAGE_MOUNT_PATH "/usb0"

/** @brief 主机回调交给应用唯一 USB owner 的插拔通知。 */
typedef enum {
    USB_STORAGE_DEVICE_CONNECTED,
    USB_STORAGE_DEVICE_DISCONNECTED,
} usb_storage_event_id_t;

typedef struct {
    usb_storage_event_id_t id; /**< 连接使用地址，断开使用设备句柄。 */
    uint64_t generation; /**< 断开事件对应的本进程挂载序号，连接时由实际安装另行分配。 */
    union {
        uint8_t address;
        msc_host_device_handle_t handle;
    } data;
} usb_storage_event_t;

/**
 * @brief 创建或借回进程期四深度 usb_storage_event_t 队列。
 * @param[out] out_events 必填输出槽，先清 NULL，成功取得队列借用。
 * @return 成功、INVALID_ARG 或 NO_MEM，重复初始化成功返回已有队列。
 * @pre 唯一启动 owner 串行调用，主机和回调存活期间队列不得删除。
 */
esp_err_t usb_storage_service_init(QueueHandle_t *out_events);
/**
 * @brief 接纳安装 USB Host/MSC 的进程期任务，随后由回调发布插拔。
 * @param[in] events 非 NULL 且长期有效的 init 队列，owner 保管其完整寿命。
 * @return ESP_OK 仅表示任务创建成功，空队列为 INVALID_ARG，重复启动为 INVALID_STATE，创建失败为 ESP_FAIL。
 * @note 后台原生安装失败通过日志诊断，服务不提供运行期销毁 USB 主机栈的入口。
 */
esp_err_t usb_storage_service_start_host(QueueHandle_t events);
/**
 * @brief 安装地址对应的 MSC 并注册禁止自动格式化、max_files 为十二的 FAT 挂载。
 * @param[in] address 连接通知提供的 USB 设备地址。
 * @return 注册成功、已有挂载/安装/待清理的 INVALID_STATE 或原生错误，首个错误保留。
 * @note owner 串行执行挂载和卸载。成功后仍核对 current_generation，安装期间断开可使
 *       挂载不可用。回滚卸载失败保留设备并生成待清理事件，外层取出后重试 unmount。
 */
esp_err_t usb_storage_service_mount(uint8_t address);
/**
 * @brief 关闭当前设备的新应用 IO，为外层排空建立起点。
 * @param[in] handle 当前挂载或挂载回滚保留的设备句柄借用。
 * @return 匹配为 ESP_OK，无当前资源、安装中或不匹配为 NOT_FOUND。
 * @note 此入口不等待已有用户，owner 排空后才可调用 unmount。
 */
esp_err_t usb_storage_service_begin_unmount(msc_host_device_handle_t handle);
/**
 * @brief 消费匹配设备的 VFS 和 MSC 资源，部分失败保留剩余资源。
 * @param[in] handle 当前设备借用句柄，成功后该句柄失效。
 * @return 全部归还为 ESP_OK，不匹配为 NOT_FOUND，原生错误保持可重试状态。
 * @pre owner 已 begin_unmount 并排空全部已接纳文件和应用 IO，挂载/卸载操作串行。
 * @note 设备换盘的配置、会话、记忆、账本及 RAM 队列装配由外层完整重启负责。
 */
esp_err_t usb_storage_service_unmount(msc_host_device_handle_t handle);
/**
 * @brief 查询当前挂载是否仍开放应用 IO。
 * @return 已挂载且未开始卸载、未观察断开为 true，其余 false。
 * @note 状态快照不增加资源借用，实际操作沿 generation 和外层关闭屏障保护。
 */
bool usb_storage_service_is_mounted(void);
/**
 * @brief 原子取得本进程挂载身份及可用性。
 * @param[out] out_generation 可空数字槽，非空时无论是否可用都写当前计数。
 * @return 仅可用挂载为 true，断开通知观察后立即返回 false，早于 VFS 注销。
 */
bool usb_storage_service_current_generation(uint64_t *out_generation);
/**
 * @brief 取走队列满或挂载回滚保留的断开清理通知。
 * @param[out] out_handle 必填槽，参数有效时写设备借用或 NULL。
 * @param[out] out_generation 必填槽，参数有效时写对应计数或零。
 * @return 有待清理设备为 true，其余 false，空参数时保持输出。
 * @note 通知被消费，设备仍由服务保管，owner 核对身份并完成排空/卸载。
 */
bool usb_storage_service_take_pending_disconnect(msc_host_device_handle_t *out_handle, uint64_t *out_generation);
/**
 * @brief 取走队列满时保存的最近连接地址。
 * @param[out] out_event 必填值槽，有通知时清零并填写，无通知时保留。
 * @return 有通知为 true，无通知或空槽为 false，实际设备存续由后续安装核对。
 */
bool usb_storage_service_take_pending_connect(usb_storage_event_t *out_event);
/**
 * @brief 核对断开设备和本进程序号是否仍对应当前待清理资源。
 * @param[in] handle 通知中的借用句柄，NULL 不匹配。
 * @param[in] generation 通知记录的挂载序号。
 * @return 同一设备/序号且非安装中为 true，包含挂载回滚待清理状态。
 */
bool usb_storage_service_disconnect_is_current(msc_host_device_handle_t handle, uint64_t generation);

#ifdef __cplusplus
}
#endif
