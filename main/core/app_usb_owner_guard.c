#include "app_usb_owner_guard.h"
#include <stddef.h>

/**
 * @brief 为已清零或已完成销毁的 guard 创建互斥锁，并开放介质操作接纳。
 * @param[in,out] guard 独占初始化的对象，mutex 须为 NULL。
 * @return 成功为 ESP_OK，空对象或已有 mutex 为 ESP_ERR_INVALID_ARG，锁分配失败为 ESP_ERR_NO_MEM。
 */
esp_err_t app_usb_owner_guard_init(app_usb_owner_guard_t *guard)
{
    if (!guard || guard->mutex) return ESP_ERR_INVALID_ARG;
    guard->mutex=xSemaphoreCreateMutex();
    if (!guard->mutex) return ESP_ERR_NO_MEM;
    __atomic_store_n(&guard->closed,0,__ATOMIC_RELEASE);
    return ESP_OK;
}

/**
 * @brief 在关闭标志前后双重检查下取得介质操作互斥锁。
 * @param[in,out] guard 已初始化且保持存活的 guard，调用可能等待其 mutex。
 * @param[in] timeout_ms 等锁毫秒值，零为立即尝试，转换为零 tick 的正值提升为一 tick。
 * @return 成功持锁且仍开放时为真，由同一任务调用 leave。参数无效、已关闭或超时为假。
 * @note 取得 mutex 后再次检查 closed，使关门前已开始等待的调用也遵守关闭状态。
 */
bool app_usb_owner_guard_enter(app_usb_owner_guard_t *guard, uint32_t timeout_ms)
{
    if (!guard || !guard->mutex || __atomic_load_n(&guard->closed,__ATOMIC_ACQUIRE)) return false;
    TickType_t wait=pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms && !wait) wait=1;
    if (xSemaphoreTake(guard->mutex,wait)!=pdTRUE) return false;
    if (__atomic_load_n(&guard->closed,__ATOMIC_ACQUIRE)) {
        xSemaphoreGive(guard->mutex);
        return false;
    }
    return true;
}

/**
 * @brief 归还一次成功 enter 取得的互斥锁。
 * @param[in,out] guard 对应的存活 guard，空对象或空 mutex 直接结束调用。
 * @pre 有效对象由成功 enter 的同一任务配对调用一次。
 * @post 当前任务完成本次受保护介质操作区间。
 */
void app_usb_owner_guard_leave(app_usb_owner_guard_t *guard)
{
    if (guard && guard->mutex) xSemaphoreGive(guard->mutex);
}

/**
 * @brief 先原子关闭新接纳，再等待当前持锁者完成受保护区间。
 * @param[in,out] guard 已初始化的 guard，进入等待前发布 closed=1。
 * @param[in] timeout_ms 等锁毫秒值，零为立即尝试，转换为零 tick 的正值提升为一 tick。
 * @return 取得并归还锁为 ESP_OK，未初始化为 ESP_ERR_INVALID_STATE，等锁失败为 ESP_ERR_TIMEOUT。
 * @note 超时保管关闭状态和 mutex，可在当前持有者 leave 后重试。成功后仍可能存在
 *       尚未返回的拒绝调用或等待者，对象和 mutex 的销毁由外层另行排空。
 */
esp_err_t app_usb_owner_guard_close(app_usb_owner_guard_t *guard, uint32_t timeout_ms)
{
    if (!guard || !guard->mutex) return ESP_ERR_INVALID_STATE;
    __atomic_store_n(&guard->closed,1,__ATOMIC_RELEASE);
    TickType_t wait=pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms && !wait) wait=1;
    if (xSemaphoreTake(guard->mutex,wait)!=pdTRUE) return ESP_ERR_TIMEOUT;
    xSemaphoreGive(guard->mutex);
    return ESP_OK;
}

/**
 * @brief 在全部使用者排空后释放 mutex，并保管关闭状态。
 * @param[in,out] guard 要销毁的对象，空对象或空 mutex 直接结束调用。
 * @pre 外层保证当前没有持有者、等待者或会继续访问本对象的任务。
 * @post 有效 mutex 已删除并置空，closed 发布为一，未来使用先独占重新初始化。
 */
void app_usb_owner_guard_destroy(app_usb_owner_guard_t *guard)
{
    if (!guard || !guard->mutex) return;
    vSemaphoreDelete(guard->mutex);
    guard->mutex=NULL;
    __atomic_store_n(&guard->closed,1,__ATOMIC_RELEASE);
}
