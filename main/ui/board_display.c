#include "board_display.h"

#include <stdbool.h>
#include <stdint.h>

#include "dev_display_lcd.h"
#include "esp_board_manager_includes.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "board_display";

static dev_display_lcd_handles_t *s_lcd;
static SemaphoreHandle_t s_draw_mutex;
static bool s_display_ready;

/**
 * @brief 初始化并借用 Board Manager 的 LCD 面板，完整取得句柄后发布绘制入口。
 * @return ESP_OK 为可绘制，锁 OOM 为 ESP_ERR_NO_MEM，设备缺失为 ESP_ERR_NOT_FOUND，其他错误透传。
 * @pre 单一 owner 串行初始化，Board Manager 面板及本模块进程期锁覆盖全部后续绘制。
 * @note 部分初始化失败保留已建立的板级设备和锁，允许 owner 重试取得句柄。
 */
esp_err_t board_display_init(void)
{
    if (__atomic_load_n(&s_display_ready, __ATOMIC_ACQUIRE)) {
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
    if (!esp_board_manager_check_name("display_lcd")) {
        ESP_LOGW(TAG, "display_lcd is not present in generated board config");
        return ESP_ERR_NOT_FOUND;
    }

    ret = esp_board_manager_get_device_handle("display_lcd", (void **)&s_lcd);
    if (ret != ESP_OK || !s_lcd || !s_lcd->panel_handle) {
        ESP_LOGE(TAG, "Failed to get display_lcd handle: %s", esp_err_to_name(ret));
        return ret == ESP_OK ? ESP_FAIL : ret;
    }

    __atomic_store_n(&s_display_ready, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "display_lcd handle acquired");
    return ESP_OK;
}

/**
 * @brief 从屏幕左上角提交紧密排列的 RGB565 位图。
 * @param[in] pixels 原始像素借用，不可 NULL，源行跨度为 width 乘二字节。
 * @param[in] width 源图宽度，正像素数。
 * @param[in] height 源图高度，正像素数。
 * @return 与 draw_rgb565_at 相同的状态，成功表示原生绘制提交完成。
 * @pre 像素存储覆盖源区域及原生异步读取期，调用者负责面板完成通知和回收时点。
 */
esp_err_t board_display_draw_rgb565(const void *pixels, int width, int height)
{
    return board_display_draw_rgb565_at(pixels, 0, 0, width, height);
}

/**
 * @brief 在固定 800×480 绘制区提交位图，右边裁剪时逐行保留原始源跨度。
 * @param[in] pixels 源 RGB565 借用，不可 NULL，每行原始 width 个像素。
 * @param[in] x 左上角水平像素坐标，须在绘制区内。
 * @param[in] y 左上角垂直像素坐标，须在绘制区内。
 * @param[in] width 原始源宽度，须为正值，裁剪不改变源行跨度。
 * @param[in] height 原始源高度，须为正值，绘制区之外的底部行跳过。
 * @return 未就绪为 INVALID_STATE，参数或字节范围溢出为 INVALID_ARG，锁失败为 ESP_FAIL，否则返回首个原生错误或 ESP_OK。
 * @note 锁只串行化提交。逐行中途失败时此前行可能已经提交，像素借用仍须覆盖这些提交的异步完成期。
 */
esp_err_t board_display_draw_rgb565_at(const void *pixels, int x, int y, int width, int height)
{
    if (!__atomic_load_n(&s_display_ready, __ATOMIC_ACQUIRE) || !s_lcd || !s_lcd->panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!pixels || x < 0 || y < 0 || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x >= 800 || y >= 480) {
        return ESP_ERR_INVALID_ARG;
    }
    int source_width = width;
    if (width > 800 - x) {
        width = 800 - x;
    }
    if (height > 480 - y) {
        height = 480 - y;
    }

    if ((size_t)source_width > SIZE_MAX / 2U) return ESP_ERR_INVALID_ARG;
    size_t stride = (size_t)source_width * 2U;
    if ((size_t)height > SIZE_MAX / stride) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_draw_mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    esp_err_t ret = ESP_OK;
    if (width == source_width) {
        ret = esp_lcd_panel_draw_bitmap(s_lcd->panel_handle, x, y, x + width, y + height, pixels);
    } else {
        for (int row = 0; row < height; row++) {
            const uint8_t *source = (const uint8_t *)pixels + (size_t)row * stride;
            ret = esp_lcd_panel_draw_bitmap(s_lcd->panel_handle, x, y + row, x + width, y + row + 1, source);
            if (ret != ESP_OK) break;
        }
    }
    xSemaphoreGive(s_draw_mutex);
    return ret;
}
