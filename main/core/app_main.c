#include "app_lifecycle.h"

/**
 * @brief 从 ESP-IDF 主任务进入 CClaw 应用生命周期编排。
 * @post 正常启动后驻留在 app_lifecycle_run 的 USB 热插拔循环中。
 *       若启动编排在失败清理后返回，本入口随之返回。
 * @note 生命周期编排负责初始化全局组合根及服务，本入口仅完成调用交接。
 */
void app_main(void)
{
    app_lifecycle_run();
}
