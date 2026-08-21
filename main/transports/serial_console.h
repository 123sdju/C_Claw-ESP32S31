#pragma once

#include <stdbool.h>

/*
 * 启动串口控制台：初始化 UART 硬件并创建轮询任务。
 * 返回: true 表示启动成功或已在运行中
 */
bool serial_console_start(void);
/*
 * 查询串口控制台任务是否正在运行。
 * 返回: true 表示任务正在运行
 */
bool serial_console_running(void);
