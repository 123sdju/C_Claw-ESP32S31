#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * @brief 串行介质挂载/卸载工作与关闭屏障。
 *
 * 生命周期 owner 可能在 close 后再次醒来，拥有者继续保管 guard 和 mutex。
 * 只有排空持有者、等待者及未来入口后才能 destroy，close 成功只确认受保护区间已退出。
 */
typedef struct app_usb_owner_guard {
    SemaphoreHandle_t mutex; /**< 保护介质操作的非递归 mutex，初始化和销毁由外层独占。 */
    int closed; /**< release/acquire 访问的接纳标志，关闭后 enter 返回拒绝。 */
} app_usb_owner_guard_t;

/**
 * @brief 初始化独占 guard 并开放操作接纳。
 * @param[in,out] guard 已清零或已完成销毁的对象，mutex 必须为 NULL。
 * @return 成功为 ESP_OK，参数或既有 mutex 无效为 ESP_ERR_INVALID_ARG，分配失败为 ESP_ERR_NO_MEM。
 */
esp_err_t app_usb_owner_guard_init(app_usb_owner_guard_t *guard);
/**
 * @brief 等待 mutex，取得锁后再次检查接纳状态。
 * @param[in,out] guard 已初始化且保持存活的对象。
 * @param[in] timeout_ms 等待毫秒值，零立即尝试，转换为零 tick 的正值提升为一 tick。
 * @return 持锁且开放为真，其他情况为假，成功后由同一任务配对 leave。
 */
bool app_usb_owner_guard_enter(app_usb_owner_guard_t *guard, uint32_t timeout_ms);
/**
 * @brief 完成一次成功 enter 的介质操作区间。
 * @param[in,out] guard 对应存活对象，空对象或空 mutex 直接结束调用。
 * @pre 由成功 enter 并持锁的同一任务配对调用。
 * @post 本次 mutex 持有区间结束。
 */
void app_usb_owner_guard_leave(app_usb_owner_guard_t *guard);
/**
 * @brief 关闭接纳并等待当前受保护区间结束。
 * @param[in,out] guard 已初始化的对象，超时也保管 closed=1 及 mutex。
 * @param[in] timeout_ms 等待毫秒值，零立即尝试，转换为零 tick 的正值提升为一 tick。
 * @return 取得并归还 mutex 为 ESP_OK，未初始化为 ESP_ERR_INVALID_STATE，等锁失败为 ESP_ERR_TIMEOUT。
 * @note 可在持有者退出后重试，成功后仍保管对象供拒绝尚未返回的入口调用。
 */
esp_err_t app_usb_owner_guard_close(app_usb_owner_guard_t *guard, uint32_t timeout_ms);
/**
 * @brief 排空所有访问后销毁 mutex。
 * @param[in,out] guard 要销毁的对象，空对象或空 mutex 直接结束调用。
 * @pre 已排除全部持有者、等待者和未来调用，例如 owner 任务创建前的启动回滚。
 * @post 有效 mutex 已释放并置空，closed 为一。
 */
void app_usb_owner_guard_destroy(app_usb_owner_guard_t *guard);
