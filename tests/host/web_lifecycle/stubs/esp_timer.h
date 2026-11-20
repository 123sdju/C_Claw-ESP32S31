#pragma once
#include <stdint.h>
/** @brief 取得主机单调时钟供真实请求期限逻辑使用。 @return 自单调时钟原点起的微秒数。 */
int64_t esp_timer_get_time(void);
