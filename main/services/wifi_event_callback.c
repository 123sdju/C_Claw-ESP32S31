#include "wifi_event_callback.h"

#include <limits.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 进程期锁与计数跨服务销毁保留，只有持有已登记回调借用时才能访问其上下文。
 * 注销先关闭新借用，再等已接纳回调返回，等待方本身只访问这些永久字段。 */
static portMUX_TYPE s_callback_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_app_event_callback_t s_callback;
static void *s_callback_data;
static unsigned s_active_callbacks;
static unsigned s_detach_waiters;
static bool s_detaching;

/**
 * @brief 将注销毫秒预算向上换算为有限 tick，防止原生宏乘法回绕。
 * @param[in] timeout_ms 非负毫秒数，零保持为零。
 * @return 正预算至少一个 tick，最大 portMAX_DELAY-1，保持有限等待。
 */
static TickType_t callback_timeout_ticks(uint32_t timeout_ms)
{
    uint64_t ticks = ((uint64_t)timeout_ms * configTICK_RATE_HZ + 999U) / 1000U;
    if (ticks >= (uint64_t)portMAX_DELAY) ticks = (uint64_t)portMAX_DELAY - 1U;
    return (TickType_t)ticks;
}

/**
 * @brief 在空闲且已排空的唯一回调槽中成对注册函数和借用上下文。
 * @param[in] callback 必填同步回调，NULL 返回参数错误。
 * @param[in,out] user_data 可空借用上下文，须覆盖所有已经取得该注册快照的调用。
 * @return 安装成功为 ESP_OK，空函数为 INVALID_ARG，已有注册或正在注销为 INVALID_STATE。
 * @note 进程期 portMUX 只保护登记，不在锁内调用外部函数。注册和最终释放由 owner 协调。
 */
esp_err_t wifi_app_set_event_callback(wifi_app_event_callback_t callback, void *user_data)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_callback_lock);
    bool available = !s_callback && !s_detaching && s_active_callbacks == 0;
    if (available) { s_callback=callback; s_callback_data=user_data; }
    portEXIT_CRITICAL(&s_callback_lock);
    return available ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/**
 * @brief 取得函数与上下文配对借用，锁外同步派发，再归还在途计数。
 * @param[in] event 按值转发的 Wi-Fi 事件编号。
 * @param[in] status 调用者拥有的状态快照借用，可为 NULL，仅在本次回调期间有效。
 * @post 未注册或计数饱和时跳过，已接纳回调返回后借用结束，末个回调可自动归还注销槽。
 * @note 可由多个线程并发派发，接收者负责共享上下文同步。回调不能等待自己退出。
 */
void wifi_event_callback_dispatch(wifi_app_event_t event, const wifi_app_status_t *status)
{
    wifi_app_event_callback_t callback=NULL;
    void *user_data=NULL;
    portENTER_CRITICAL(&s_callback_lock);
    if (s_callback && s_active_callbacks < UINT_MAX) {
        callback=s_callback; user_data=s_callback_data; s_active_callbacks++;
    }
    portEXIT_CRITICAL(&s_callback_lock);
    if (!callback) return;
    callback(event,status,user_data);
    portENTER_CRITICAL(&s_callback_lock);
    s_active_callbacks--;
    if (!s_active_callbacks && !s_detach_waiters && s_detaching) s_detaching=false;
    portEXIT_CRITICAL(&s_callback_lock);
}

/**
 * @brief 关闭新回调接纳并按有限预算等待已接纳调用全部返回。
 * @param[in] timeout_ms 毫秒总预算，零仅检查当前状态，不等待。
 * @return 已排空为 ESP_OK，未排空为 TIMEOUT，等待者计数饱和为 INVALID_STATE。
 * @note 超时后仍关闭接纳，末个回调返回时自动回收槽位。原 userdata owner 须继续
 *       保管借用数据并完成排空交接后释放，销毁重试由外层串行协调。
 */
esp_err_t wifi_app_clear_event_callback(uint32_t timeout_ms)
{
    TickType_t started=xTaskGetTickCount();
    TickType_t timeout=callback_timeout_ticks(timeout_ms);
    portENTER_CRITICAL(&s_callback_lock);
    if (s_detach_waiters==UINT_MAX) {
        portEXIT_CRITICAL(&s_callback_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_detach_waiters++;
    s_callback=NULL; s_callback_data=NULL; s_detaching=true;
    portEXIT_CRITICAL(&s_callback_lock);
    for (;;) {
        bool timed_out=!timeout_ms || xTaskGetTickCount()-started>=timeout;
        portENTER_CRITICAL(&s_callback_lock);
        bool done=s_active_callbacks==0;
        bool expired=!done && timed_out;
        if (done || expired) {
            s_detach_waiters--;
            if (done && !s_detach_waiters) s_detaching=false;
        }
        portEXIT_CRITICAL(&s_callback_lock);
        if (done) return ESP_OK;
        if (expired) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
}
