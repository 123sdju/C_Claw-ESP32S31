#pragma once

#include "wifi_app.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 同步借出一份 Wi-Fi 事件状态给已注册接收者，返回前结束该次借用。
 * @param[in] event 按值转发的事件编号。
 * @param[in] status 可空状态借用，调用者保持其在整个同步回调期间稳定。
 * @post 已登记调用完成后归还在途计数，无注册或计数饱和时跳过。
 * @note 派发可并发，接收者同步 userdata，共享注册的销毁通过 clear 关闭并排空。
 */
void wifi_event_callback_dispatch(wifi_app_event_t event, const wifi_app_status_t *status);

#ifdef __cplusplus
}
#endif
