#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/** @brief 启动调用的显示结果，仅成功返回后可发布给 UI。 */
typedef struct {
    lv_display_t *display; /**< adapter 拥有的借用句柄，BSP 和 adapter 生命周期覆盖 UI 使用。 */
    bool is_rgb; /**< 实际面板是否使用 RGB 子类型。 */
    bool ppa_accel_enabled; /**< 本次 adapter 配置的 PPA 开关，当前初始化显式关闭。 */
} board_lcd_t;

/**
 * @brief 初始化面板与 LVGL adapter，尽力装配可选触摸和背光。
 * @param[out] out_lcd 必填可写结果，入口清零，成功后取得借用的 display 及模式信息。
 * @return ESP_OK 表示显示入口已建立，NULL 输出为 INVALID_ARG，其他错误来自板级或 adapter 初始化。
 * @pre 启动 owner 排斥并发初始化，BSP 设备和 adapter 覆盖全部显示与输入使用期。
 * @note 失败可能保留部分 BSP 或 adapter 资源，外层管理其退场和重试，触摸与背光错误允许降级。
 */
esp_err_t board_lcd_init(board_lcd_t *out_lcd);
