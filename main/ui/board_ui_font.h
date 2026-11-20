#pragma once

#include <stdbool.h>

#include "lvgl.h"

/* 外部字体文件默认路径（USB 存储器） */
#define BOARD_UI_FONT_PATH_DEFAULT "A:/usb0/cclaw/fonts/default.bin"

/**
 * @brief 获取当前选定的外部字体或内置回退字体。
 * @return 字体借用，不得直接释放，外部字体借用止于后续重绑和退休释放事务。
 * @pre UI owner 以 LVGL 锁排斥字体状态修改。
 */
const lv_font_t *board_ui_font_current(void);
/**
 * @brief 复制下次加载使用的字体路径。
 * @param[in] path NUL 路径借用，NULL 或空串恢复默认，最多保留 159 字节。
 * @post 当前字体保持有效，后续加载使用复制后的路径。
 * @pre owner 排斥并发路径更新和加载，超长输入会按既有配置容量截断。
 */
void board_ui_font_set_path(const char *path);
/**
 * @brief 加载外部字体并转交待退休旧字体，使 UI 可在一次锁内完成重绑。
 * @param[out] out_retired_font 必填拥有输出槽，替换或失败回退时交付旧字体，同路径缓存命中置 NULL。
 * @return 外部字体可用为 true，加载失败为 false 并回退内置字体，NULL 输出槽失败且不变更状态。
 * @pre UI owner 持 LVGL 锁，文件和文件系统覆盖整个加载调用，禁止并发路径更新。
 * @post 调用者在解锁前将全部旧字体使用者重绑到 current，再释放退休字体。
 */
bool board_ui_font_load_external(lv_font_t **out_retired_font);

/**
 * @brief 释放 load_external 交付的已退休字体。
 * @param[in,out] font 拥有槽，可 NULL，字体须已脱离全部标签和绘制任务。
 * @post 原生字体及复制表释放，槽置 NULL，空槽调用无副作用。
 * @pre UI owner 持 LVGL 锁，禁止传入当前字体或内置字体。
 */
void board_ui_font_release_retired(lv_font_t **font);
