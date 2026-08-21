#include "app_lifecycle.h"

/*
 * ESP-IDF 应用主入口。
 *
 * FreeRTOS 启动后第一个用户任务从此函数开始执行，
 * 直接委托给 app_lifecycle_run() 进入生命周期主循环。
 * 此函数永不返回。
 */
void app_main(void)
{
    app_lifecycle_run();
}
