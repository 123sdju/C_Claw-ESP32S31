#include "board_display.h"

#include <stdbool.h>

#include "dev_display_lcd.h"
#include "esp_board_manager_includes.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board_display";

static dev_display_lcd_handles_t *s_lcd;
static SemaphoreHandle_t s_draw_mutex;
static bool s_board_manager_ready;
static bool s_display_ready;

/*
 * 初始化 LCD 显示驱动，通过 esp_board_manager 获取 display_lcd 设备句柄并创建绘制互斥锁。
 * 返回: ESP_OK 成功，ESP_ERR_NOT_FOUND 设备不在配置中，ESP_ERR_NO_MEM 互斥锁创建失败
 */
esp_err_t board_display_init(void)
{
    if (s_board_manager_ready) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing display_lcd device");
    esp_err_t ret = esp_board_manager_init_device_by_name("display_lcd");
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "display_lcd init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    if (!s_draw_mutex) {
        s_draw_mutex = xSemaphoreCreateMutex();
        if (!s_draw_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_board_manager_ready = true;

    if (!esp_board_manager_check_name("display_lcd")) {
        ESP_LOGW(TAG, "display_lcd is not present in generated board config");
        return ESP_ERR_NOT_FOUND;
    }

    ret = esp_board_manager_get_device_handle("display_lcd", (void **)&s_lcd);
    if (ret != ESP_OK || !s_lcd || !s_lcd->panel_handle) {
        ESP_LOGE(TAG, "Failed to get display_lcd handle: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    s_display_ready = true;
    ESP_LOGI(TAG, "display_lcd handle acquired");
    return ESP_OK;
}

/*
 * 在 LCD (0,0) 绘制 RGB565 位图，委托 draw_rgb565_at 实现。
 * 参数: pixels - RGB565 像素数据
 *       width  - 位图宽度
 *       height - 位图高度
 * 返回: ESP_OK 成功，否则为 ESP 错误码
 */
esp_err_t board_display_draw_rgb565(const void *pixels, int width, int height)
{
    return board_display_draw_rgb565_at(pixels, 0, 0, width, height);
}

/*
 * 在 LCD 指定位置绘制 RGB565 位图，自动裁剪超出 800x480 屏幕范围的区域，持互斥锁保护并发绘制。
 * 参数: pixels - RGB565 像素数据
 *       x/y/width/height - 绘制区域位置和尺寸
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_STATE 设备未就绪，ESP_ERR_INVALID_ARG 参数无效
 */
esp_err_t board_display_draw_rgb565_at(const void *pixels, int x, int y, int width, int height)
{
    if (!s_display_ready || !s_lcd || !s_lcd->panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pixels || x < 0 || y < 0 || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x >= 800 || y >= 480) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x + width > 800) {
        width = 800 - x;
    }
    if (y + height > 480) {
        height = 480 - y;
    }

    if (s_draw_mutex) {
        xSemaphoreTake(s_draw_mutex, portMAX_DELAY);
    }
    esp_err_t ret = esp_lcd_panel_draw_bitmap(s_lcd->panel_handle, x, y, x + width, y + height, pixels);
    if (s_draw_mutex) {
        xSemaphoreGive(s_draw_mutex);
    }
    return ret;
}
