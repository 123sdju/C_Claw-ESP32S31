#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* 外部字体文件默认路径（USB 存储器） */
#define BOARD_UI_FONT_PATH_DEFAULT "A:/usb0/cclaw/fonts/default.bin"

/*
 * 获取当前 UI 使用的字体指针。若外部字体加载失败则返回 LVGL 内置字体。
 * 返回: 当前有效的 lv_font_t 指针
 */
const lv_font_t *board_ui_font_current(void);
/*
 * 设置外部字体文件路径。下次调用 load_external 将从此路径加载。
 * 参数: path - 字体文件路径，若为 NULL 或空则恢复默认路径
 */
void board_ui_font_set_path(const char *path);
/*
 * 从文件系统加载外部二进制字体。成功加载后自动切换为当前字体。
 * 返回: true 加载成功，false 失败（回退到内置字体）
 */
bool board_ui_font_load_external(void);
