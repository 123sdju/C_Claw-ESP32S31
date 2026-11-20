#include "board_lcd.h"

#include <string.h>
#include "sdkconfig.h"
#include "dev_display_lcd.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_manager_includes.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "freertos/FreeRTOS.h"
#include "soc/soc_caps.h"

#if defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT)
#include "dev_lcd_touch.h"
#elif defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT)
#include "dev_lcd_touch_i2c.h"
#endif

#ifndef UI_STABLE_LCD_SPI_INTERNAL_DMA
#define UI_STABLE_LCD_SPI_INTERNAL_DMA 0
#endif

static const char *TAG = "board_lcd";

/**
 * @brief 将板级交换轴和镜像组合映射为 adapter 的四种旋转值。
 * @param[in] cfg 已初始化 Board Manager 借出的非 NULL 配置，只读且覆盖调用期。
 * @return 对应的 0、90、180 或 270 度 adapter 枚举。
 */
static esp_lv_adapter_rotation_t lcd_rotation(const dev_display_lcd_config_t *cfg)
{
    if (cfg->swap_xy) return cfg->mirror_x ? ESP_LV_ADAPTER_ROTATE_90 : ESP_LV_ADAPTER_ROTATE_270;
    return (cfg->mirror_x || cfg->mirror_y) ? ESP_LV_ADAPTER_ROTATE_180 : ESP_LV_ADAPTER_ROTATE_0;
}

/**
 * @brief 按 RGB 面板预先配置的帧缓冲数选择双缓冲或三缓冲局部刷新策略。
 * @param[in] cfg 稳定的非 NULL 板级配置借用，sub_type 必须有效。
 * @return RGB 且至少三个缓冲为三缓冲，两缓冲为双缓冲，其余返回 NONE。
 * @note 实际帧缓冲由 BSP 创建，本函数只选择 adapter 策略。
 */
static esp_lv_adapter_tear_avoid_mode_t lcd_tear_mode(const dev_display_lcd_config_t *cfg)
{
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
    if (strcmp(cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0) {
        uint8_t buffers = cfg->sub_cfg.rgb.panel_config.num_fbs;
        if (buffers >= 3) return ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
        if (buffers == 2) return ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL;
    }
#endif
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
}

/**
 * @brief 以板级尺寸和旋转注册 LVGL display，并记录实际 RGB/PPA 选择。
 * @param[in] cfg 非 NULL 只读板级配置借用，设备生命周期覆盖注册后的显示使用。
 * @param[in] lcd 非 NULL 面板及 I/O 句柄借用，BSP 保持原生句柄有效。
 * @param[out] out_lcd 有效结果对象，更新 is_rgb 与 ppa_accel_enabled，display 由外层写入。
 * @return adapter 拥有的 display 借用，未编译 RGB 支持或原生注册失败返回 NULL。
 * @note adapter 复制栈上配置结构，面板和 I/O 对象继续借用，失败时诊断字段仍可能已写入。
 */
static lv_display_t *register_display(const dev_display_lcd_config_t *cfg,
    const dev_display_lcd_handles_t *lcd, board_lcd_t *out_lcd)
{
    esp_lv_adapter_rotation_t rotation = lcd_rotation(cfg);
    esp_lv_adapter_display_config_t display;
    out_lcd->is_rgb = strcmp(cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0;
    if (out_lcd->is_rgb) {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
        /* 板级配置可能位于只读区，帧缓冲数已由 BSP 在设备初始化时确定。 */
        display = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(lcd->panel_handle, lcd->io_handle,
            cfg->lcd_width, cfg->lcd_height, rotation);
        display.tear_avoid_mode = lcd_tear_mode(cfg);
#else
        return NULL;
#endif
    } else {
#if UI_STABLE_LCD_SPI_INTERNAL_DMA || !defined(CONFIG_SPIRAM)
        display = ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(lcd->panel_handle,
            lcd->io_handle, cfg->lcd_width, cfg->lcd_height, rotation);
#else
        display = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(lcd->panel_handle,
            lcd->io_handle, cfg->lcd_width, cfg->lcd_height, rotation);
#endif
    }
    display.profile.enable_ppa_accel = false;
    out_lcd->ppa_accel_enabled = display.profile.enable_ppa_accel;
    ESP_LOGI(TAG, "LVGL display %ux%u type=%s rgb=%d tear=%d spi_internal_dma=%d ppa_runtime=%d",
        cfg->lcd_width, cfg->lcd_height, cfg->sub_type, out_lcd->is_rgb,
        display.tear_avoid_mode, UI_STABLE_LCD_SPI_INTERNAL_DMA, out_lcd->ppa_accel_enabled);
    return esp_lv_adapter_register_display(&display);
}

/**
 * @brief 初始化板级显示及 adapter，尽力注册触摸和背光后启动 LVGL 任务。
 * @param[out] out_lcd 必填可写输出，入口清零，只有 ESP_OK 时可向 UI 发布其中句柄。
 * @return NULL 输出为 INVALID_ARG，面板、adapter、配置和启动错误透传，注册失败为 ESP_FAIL。
 * @pre 启动 owner 串行调用，BSP 设备、adapter 与输入句柄覆盖 UI 后续使用期。
 * @note 触摸和背光故障允许显示降级。失败可能保留已初始化的 BSP、adapter 或已注册 display，外层管理退场或重试。
 * @post 成功输出借用 adapter 的 display，输出对象存储仍由调用者拥有。
 */
esp_err_t board_lcd_init(board_lcd_t *out_lcd)
{
    if (!out_lcd) return ESP_ERR_INVALID_ARG;
    memset(out_lcd, 0, sizeof(*out_lcd));
    esp_err_t err = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "lcd_touch init failed: %s", esp_err_to_name(err));
    if (esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS)) {
        err = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            ESP_LOGW(TAG, "lcd_brightness init failed: %s", esp_err_to_name(err));
    }

    esp_lv_adapter_config_t adapter = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter.task_priority = 4;
    adapter.task_stack_size = 10 * 1024;
    adapter.task_core_id = 1;
    adapter.tick_period_ms = 5;
    adapter.task_max_delay_ms = 100;
#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
    adapter.stack_in_psram = true;
#endif
    err = esp_lv_adapter_init(&adapter);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    void *lcd_handle = NULL;
    err = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (err != ESP_OK || !lcd_handle) return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    dev_display_lcd_config_t *lcd_config = NULL;
    err = esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, (void **)&lcd_config);
    if (err != ESP_OK || !lcd_config) return err == ESP_OK ? ESP_ERR_NOT_FOUND : err;
    out_lcd->display = register_display(lcd_config, lcd_handle, out_lcd);
    if (!out_lcd->display) return ESP_FAIL;

#if defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT) || defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT)
    void *touch_handle = NULL;
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle) == ESP_OK && touch_handle) {
#if defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT)
        dev_lcd_touch_handles_t *touch = touch_handle;
#else
        dev_lcd_touch_i2c_handles_t *touch = touch_handle;
#endif
        const esp_lv_adapter_touch_config_t config =
            ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(out_lcd->display, touch->touch_handle);
        (void)esp_lv_adapter_register_touch(&config);
    }
#endif
    err = esp_lv_adapter_start();
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}
