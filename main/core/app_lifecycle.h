#pragma once

#include <stdint.h>

#include "esp_err.h"

/*
 * 应用生命周期主循环入口。
 *
 * 负责完整的启动序列：初始化上下文和事件总线、等待 USB 配置、启动
 * Agent 运行时和交互服务、初始化各传输层（串口、WiFi、Web 控制台）、
 * 启动摄像头预览，然后进入主事件循环处理 USB 热插拔。
 * 此函数在 app_main() 中调用且永不返回。
 */
void app_lifecycle_run(void);

/* OTA 成功和显式重启共用的逆序 drain/stop/destroy 编排。 */
esp_err_t app_lifecycle_prepare_for_reboot(uint32_t timeout_ms);
