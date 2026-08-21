#include "board_ui.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "app_event_bus.h"
#include "board_ui_font.h"
#include "board_ui_json.h"
#include "board_ui_snapshot.h"
#include "cc/util/cc_json.h"
#include "dev_display_lcd.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_board_manager_includes.h"
#include "esp_cache.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_memory_utils.h"
#include "esp_system.h"
#if __has_include("esp_freertos_hooks.h")
#include "esp_freertos_hooks.h"
#define UI_HAVE_ESP_FREERTOS_HOOKS 1
#else
#define UI_HAVE_ESP_FREERTOS_HOOKS 0
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "soc/soc_caps.h"

#if defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT)
#include "dev_lcd_touch.h"
#elif defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT)
#include "dev_lcd_touch_i2c.h"
#endif

#define UI_LCD_WIDTH 800
#define UI_LCD_HEIGHT 480
#define UI_PREVIEW_W 240
#define UI_PREVIEW_H 240
#define UI_CONV_X 250
#define UI_STATUS_H 34
#define UI_PANEL_PAD 6
#define UI_STATUS_REFRESH_MS 3000U
#define UI_STATUS_SCROLL_MS 45000U
#ifndef UI_PERF_UPDATE_MS
#define UI_PERF_UPDATE_MS 1000U
#endif
#ifndef UI_ENABLE_CPU_LOAD_IDLE_HOOK
#define UI_ENABLE_CPU_LOAD_IDLE_HOOK 1
#endif
#ifdef CONFIG_CACHE_L1_DCACHE_LINE_SIZE
#define UI_CACHE_ALIGN CONFIG_CACHE_L1_DCACHE_LINE_SIZE
#else
#define UI_CACHE_ALIGN 64U
#endif
#define UI_PREVIEW_BYTES (UI_PREVIEW_W * UI_PREVIEW_H * 2)
#ifndef UI_PREVIEW_BUFFER_COUNT
#define UI_PREVIEW_BUFFER_COUNT 5
#endif
#ifndef UI_PREVIEW_STRICT_PSRAM_FOR_RGB_LCD
#define UI_PREVIEW_STRICT_PSRAM_FOR_RGB_LCD 1
#endif
#define UI_CONFIG_FIELD_MAX 16
#define UI_BOOT_LOG_MAX_LINES 80U

/*
 * Conversation history policy:
 * Keep a longer scrollback and prune only the oldest LVGL label objects when the
 * limit is exceeded. This avoids clearing the whole panel after tool calls while
 * still preventing unbounded LVGL object growth.
 */
#ifndef UI_CONV_MAX_MESSAGES
#define UI_CONV_MAX_MESSAGES 160U
#endif

#ifndef UI_CONV_BG_COLOR
#define UI_CONV_BG_COLOR 0x080c10
#endif

#ifndef UI_ROOT_BG_COLOR
#define UI_ROOT_BG_COLOR 0x0b1014
#endif

/*
 * Streaming text updates are high-frequency. Scrolling/layout on every token can
 * leave stale glyphs on RGB LCD partial-refresh paths, so only scroll
 * periodically while still updating the current label immediately.
 */
#ifndef UI_STREAM_SCROLL_INTERVAL_MS
#define UI_STREAM_SCROLL_INTERVAL_MS 50U
#endif

/*
 * Dedicated tool drawing area. screen_draw writes here instead of the main
 * conversation/overlay, so tool calls cannot leave stale text in the chat log.
 * The area lives in the left column under the camera preview and Settings button.
 */
#ifndef UI_TOOL_AREA_ENABLED
#define UI_TOOL_AREA_ENABLED 1
#endif

#ifndef UI_TOOL_AREA_MAX_ITEMS
#define UI_TOOL_AREA_MAX_ITEMS 32U
#endif

#ifndef UI_TOOL_BG_COLOR
#define UI_TOOL_BG_COLOR 0x101820
#endif

#ifndef UI_TOOL_BORDER_COLOR
#define UI_TOOL_BORDER_COLOR 0x304050
#endif

#define UI_TOOL_X UI_PANEL_PAD
#define UI_TOOL_Y (UI_STATUS_H + UI_PANEL_PAD + UI_PREVIEW_H + 8 + 38 + 8)
#define UI_TOOL_W (UI_CONV_X - 2 * UI_PANEL_PAD)
#define UI_TOOL_H (UI_LCD_HEIGHT - UI_TOOL_Y - UI_PANEL_PAD)
#define UI_TOOL_TITLE_H 26
#define UI_TOOL_INNER_PAD 4

/*
 * Tool Area uses the same CJK font as the main UI, but that font can have a
 * taller descent than LVGL's default Latin font. Give every tool label extra
 * vertical padding/min-height and place appended items using the previous
 * object's real bottom instead of a fixed 24px row. This prevents the lower
 * part of Chinese glyphs from being clipped.
 */
#ifndef UI_TOOL_LABEL_PAD_TOP
#define UI_TOOL_LABEL_PAD_TOP 3
#endif
#ifndef UI_TOOL_LABEL_PAD_BOTTOM
#define UI_TOOL_LABEL_PAD_BOTTOM 6
#endif
#ifndef UI_TOOL_LABEL_PAD_X
#define UI_TOOL_LABEL_PAD_X 3
#endif
#ifndef UI_TOOL_LABEL_MIN_H
#define UI_TOOL_LABEL_MIN_H 30
#endif
#ifndef UI_TOOL_LABEL_ROW_GAP
#define UI_TOOL_LABEL_ROW_GAP 6
#endif
#ifndef UI_TOOL_LABEL_LINE_SPACE
#define UI_TOOL_LABEL_LINE_SPACE 3
#endif

#define UI_TOOL_CANVAS_W (UI_TOOL_W - 2 * UI_TOOL_INNER_PAD)
#define UI_TOOL_CANVAS_H (UI_TOOL_H - UI_TOOL_TITLE_H - 2 * UI_TOOL_INNER_PAD)


/*
 * Screen-performance-first defaults:
 * - Keep SPI LCD on the original high-throughput PSRAM draw-buffer path by default.
 *   Set UI_STABLE_LCD_SPI_INTERNAL_DMA to 1 only if boot instability remains.
 * - Keep the preview image source in internal DMA-capable memory when possible,
 *   so LVGL image rendering reads faster while the LCD draw buffer still stays in PSRAM.
 * - Do not start Wi-Fi connect synchronously inside the LVGL event callback.
 */
#ifndef UI_STABLE_LCD_SPI_INTERNAL_DMA
#define UI_STABLE_LCD_SPI_INTERNAL_DMA 0
#endif

/*
 * Preview image source placement:
 * - RGB LCD: keep lv_image source in internal RAM by default. The RGB/LVGL
 *   refresh path may access this pointer while flash/PSRAM cache is disabled,
 *   and a PSRAM image source can trigger Cache access error.
 * - SPI LCD: allow PSRAM first by default to reduce internal RAM pressure.
 */
#ifndef UI_PREVIEW_PSRAM_FOR_RGB_LCD
#define UI_PREVIEW_PSRAM_FOR_RGB_LCD 1
#endif

#ifndef UI_PREVIEW_PSRAM_FOR_SPI_LCD
#define UI_PREVIEW_PSRAM_FOR_SPI_LCD 1
#endif

#ifndef UI_PREVIEW_SOURCE_DMA_WRITES
#define UI_PREVIEW_SOURCE_DMA_WRITES 1
#endif

/*
 * Limit only the camera preview widget update rate.
 * This does not limit the LCD/LVGL refresh path itself. It prevents the camera
 * preview task from continuously memcpy-ing 115 KB frames and starving IDLE1.
 */
#ifndef UI_PREVIEW_MAX_FPS
#define UI_PREVIEW_MAX_FPS 24U
#endif

#ifndef UI_PREVIEW_YIELD_AFTER_UPDATE
#define UI_PREVIEW_YIELD_AFTER_UPDATE 1
#endif

#ifndef UI_PREVIEW_TIMER_MS
#define UI_PREVIEW_TIMER_MS 50U
#endif

#ifndef UI_EVENT_DISPLAY_TIMER_MS
#define UI_EVENT_DISPLAY_TIMER_MS 20U
#endif

#ifndef UI_TOOL_CMD_TIMER_MS
#define UI_TOOL_CMD_TIMER_MS 20U
#endif

#ifndef UI_TOOL_CMD_QUEUE_DEPTH
#define UI_TOOL_CMD_QUEUE_DEPTH 32U
#endif

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define UI_TASK_STACK_IN_PSRAM 1
#else
#define UI_TASK_STACK_IN_PSRAM 0
#endif

static const char *TAG = "board_ui";

static lv_display_t *s_disp;
static lv_indev_t *s_touch;
static lv_obj_t *s_root;
static lv_obj_t *s_status_label;
static lv_obj_t *s_preview;
static lv_obj_t *s_conversation;
static lv_obj_t *s_overlay;
static lv_obj_t *s_tool_panel;
static lv_obj_t *s_tool_area;
static lv_obj_t *s_boot_layer;
static lv_obj_t *s_boot_log;
static lv_obj_t *s_ota_layer;
static lv_obj_t *s_ota_title;
static lv_obj_t *s_ota_bar;
static lv_obj_t *s_ota_detail;
static lv_obj_t *s_shutdown_layer;
static lv_obj_t *s_config_modal;
static lv_obj_t *s_keyboard;

static uint8_t *s_preview_buf[UI_PREVIEW_BUFFER_COUNT];
static lv_image_dsc_t s_preview_dsc[UI_PREVIEW_BUFFER_COUNT];
static uint8_t s_preview_front_index;
static uint8_t s_preview_write_index;
static volatile int s_preview_pending_index = -1;
static portMUX_TYPE s_preview_mux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t *s_last_msg_label;
static board_conversation_role_t s_last_role;
static lv_obj_t *s_stream_msg_label;
static board_conversation_role_t s_stream_role;
static bool s_stream_active;
static board_ui_camera_stats_t s_camera_stats;
static bool s_ready;
static bool s_usb_mounted;
static bool s_config_loaded;
static bool s_wifi_connected;
static bool s_restart_required;
static bool s_ppa_accel_enabled;
static bool s_lcd_is_rgb;
static char s_wifi_detail[64];
static char s_provider[32];
static char s_model[64];

static bool s_preview_src_sync_warned;
static bool s_preview_dst_sync_warned;
static TickType_t s_preview_last_update_tick;
static TickType_t s_last_stream_scroll_tick;
static TickType_t s_ota_last_update_tick;
static volatile bool s_ota_active;
static QueueHandle_t s_tool_cmd_queue;

static board_ui_callbacks_t s_callbacks;

/*
 * UI performance counters.
 * - screen_fps is counted from LVGL display refresh-ready events.
 * - CPU load is an approximate per-core load based on FreeRTOS idle hooks.
 *   It is intentionally lightweight and does not require FreeRTOS runtime stats.
 */
static volatile uint32_t s_screen_refresh_count;
static uint32_t s_screen_fps;
static uint32_t s_screen_refresh_last_count;
static TickType_t s_perf_last_update_tick;

#if UI_HAVE_ESP_FREERTOS_HOOKS && UI_ENABLE_CPU_LOAD_IDLE_HOOK
static volatile uint32_t s_idle_count[SOC_CPU_CORES_NUM];
static uint32_t s_idle_last_count[SOC_CPU_CORES_NUM];
static uint32_t s_idle_baseline_count[SOC_CPU_CORES_NUM];
static uint32_t s_cpu_load_pct[SOC_CPU_CORES_NUM];
static bool s_cpu_load_hooks_registered;
#endif

typedef struct {
    const char *path;
    lv_obj_t *textarea;
} config_field_t;

typedef enum {
    UI_TOOL_CMD_CLEAR,
    UI_TOOL_CMD_APPEND,
    UI_TOOL_CMD_RECT,
    UI_TOOL_CMD_TEXT,
    UI_TOOL_CMD_FLUSH,
} tool_cmd_type_t;

typedef struct {
    tool_cmd_type_t type;
    board_conversation_role_t role;
    int x;
    int y;
    int width;
    int height;
    uint16_t color;
    char text[384];
} tool_cmd_t;

static config_field_t s_fields[UI_CONFIG_FIELD_MAX];
static size_t s_field_count;

/* 工具命令队列确保创建（前向声明）。 */
static bool tool_cmd_queue_ensure(void);
/* 工具命令队列定时器回调（前向声明）。 */
static void tool_cmd_timer_cb(lv_timer_t *timer);

/*
 * 获取 UI 互斥锁（阻塞等待）。
 */
static void ui_lock(void)
{
    esp_lv_adapter_lock(-1);
}

/*
 * 释放 UI 互斥锁。
 */
static void ui_unlock(void)
{
    esp_lv_adapter_unlock();
}

/*
 * 将 16 位 RGB565 颜色转换为 32 位 RGB888 值。
 * 参数: c - RGB565 颜色值
 * 返回: RGB888 颜色值（0x00RRGGBB）
 */
static uint32_t rgb565_to_rgb888(uint16_t c)
{
    uint8_t r = (uint8_t)(((c >> 11) & 0x1f) << 3);
    uint8_t g = (uint8_t)(((c >> 5) & 0x3f) << 2);
    uint8_t b = (uint8_t)((c & 0x1f) << 3);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/*
 * 格式化字节数为人可读字符串（B/KB/MB）。
 * 参数: bytes    - 字节数
 *       out      - 输出缓冲区
 *       out_size - 输出缓冲区大小
 */
static void format_bytes(size_t bytes, char *out, size_t out_size)
{
    const char *unit = "B";
    double value = (double)bytes;

    if (bytes >= 1024 * 1024) {
        value /= 1024.0 * 1024.0;
        unit = "MB";
    } else if (bytes >= 1024) {
        value /= 1024.0;
        unit = "KB";
    }

    if (strcmp(unit, "B") == 0) {
        snprintf(out, out_size, "%uB", (unsigned)bytes);
    } else {
        snprintf(out, out_size, "%.2f%s", value, unit);
    }
}

/*
 * 格式化堆内存使用情况：已用/总量 百分比。
 * 参数: caps     - heap_caps 内存类型标志
 *       out      - 输出缓冲区
 *       out_size - 输出缓冲区大小
 */
static void format_heap_usage(uint32_t caps, char *out, size_t out_size)
{
    size_t total = heap_caps_get_total_size(caps);
    size_t free_size = heap_caps_get_free_size(caps);
    size_t used = total > free_size ? total - free_size : 0;
    unsigned used_pct = total ? (unsigned)((used * 100U + total / 2U) / total) : 0;
    char used_text[16];
    char total_text[16];

    format_bytes(used, used_text, sizeof(used_text));
    format_bytes(total, total_text, sizeof(total_text));
    snprintf(out, out_size, "%s/%s %u%%", used_text, total_text, used_pct);
}

/*
 * 根据对话角色返回对应颜色。
 * 参数: role - 对话角色枚举
 * 返回: LVGL 颜色值
 */
static lv_color_t role_color(board_conversation_role_t role)
{
    switch (role) {
    case BOARD_CONV_USER: return lv_color_hex(0x50b4ff);
    case BOARD_CONV_ASSISTANT: return lv_color_hex(0xe6e6d2);
    case BOARD_CONV_THINKING: return lv_color_hex(0xaa96ff);
    case BOARD_CONV_TOOL: return lv_color_hex(0xffbe50);
    case BOARD_CONV_ERROR: return lv_color_hex(0xff5a5a);
    case BOARD_CONV_STATUS:
    default: return lv_color_hex(0x78e696);
    }
}

/*
 * 根据对话角色返回角色前缀字符串（USER/ASSIST/THINK/TOOL/ERROR/STATUS）。
 * 参数: role - 对话角色枚举
 * 返回: 角色前缀字符串
 */
static const char *role_prefix(board_conversation_role_t role)
{
    switch (role) {
    case BOARD_CONV_USER: return "USER";
    case BOARD_CONV_ASSISTANT: return "ASSIST";
    case BOARD_CONV_THINKING: return "THINK";
    case BOARD_CONV_TOOL: return "TOOL";
    case BOARD_CONV_ERROR: return "ERROR";
    case BOARD_CONV_STATUS:
    default: return "STATUS";
    }
}

/*
 * 将当前活跃字体应用到 LVGL 对象样式。
 * 参数: obj - LVGL 对象
 */
static void apply_font(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_set_style_text_font(obj, board_ui_font_current(), 0);
    }
}

/*
 * 递归遍历 LVGL 对象树，逐一应用当前字体。
 * 参数: obj - 根对象
 */
static void apply_font_tree(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }

    apply_font(obj);
    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; i++) {
        apply_font_tree(lv_obj_get_child(obj, i));
    }
}

/*
 * 将消息样式和字体应用到指定 LVGL 标签对象。
 * 参数: label - 标签对象
 *       role  - 角色（决定文本颜色）
 */
static void apply_message_label_style(lv_obj_t *label, board_conversation_role_t role)
{
    if (!label) {
        return;
    }

    apply_font(label);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, role_color(role), 0);

    /*
     * Make each label paint an opaque background. This prevents stale glyphs
     * when streaming text is updated quickly on RGB LCD partial-refresh paths.
     */
    lv_obj_set_style_bg_color(label, lv_color_hex(UI_CONV_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(label, 2, 0);
    lv_obj_set_style_pad_bottom(label, 2, 0);
    lv_obj_set_style_pad_left(label, 2, 0);
    lv_obj_set_style_pad_right(label, 2, 0);
}

/*
 * 清空 overlay 覆盖层中所有绘制对象。
 */
static void conversation_overlay_clear_locked(void)
{
    if (!s_overlay) {
        return;
    }

    lv_obj_clean(s_overlay);
    lv_obj_invalidate(s_overlay);
}

/*
 * 将整数值限制在 [min_v, max_v] 范围内。
 * 参数: v     - 输入值
 *       min_v - 最小值
 *       max_v - 最大值
 * 返回: 限制后的值
 */
static int clamp_int(int v, int min_v, int max_v)
{
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

/*
 * 清空 Tool Area 中所有子对象。
 */
static void tool_area_clear_locked(void)
{
#if UI_TOOL_AREA_ENABLED
    if (!s_tool_area) {
        return;
    }

    lv_obj_clean(s_tool_area);
    lv_obj_invalidate(s_tool_area);
#else
    (void)0;
#endif
}

/*
 * 当 Tool Area 子对象超过 UI_TOOL_AREA_MAX_ITEMS 时，删除最旧项。
 */
static void tool_area_prune_locked(void)
{
#if UI_TOOL_AREA_ENABLED
    if (!s_tool_area) {
        return;
    }

    while (lv_obj_get_child_count(s_tool_area) > UI_TOOL_AREA_MAX_ITEMS) {
        lv_obj_t *oldest = lv_obj_get_child(s_tool_area, 0);
        if (!oldest) {
            break;
        }
        lv_obj_delete(oldest);
    }
#endif
}

/*
 * 将工具标签样式应用到 LVGL 标签（CJK 兼容 padding）。
 * 参数: label - 标签对象
 *       role  - 角色（决定文本颜色）
 */
static void apply_tool_label_style(lv_obj_t *label, board_conversation_role_t role)
{
    if (!label) {
        return;
    }

    apply_font(label);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, role_color(role), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(UI_TOOL_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);

    /* Extra vertical room avoids clipping the lower half of CJK glyphs. */
    lv_obj_set_style_pad_top(label, UI_TOOL_LABEL_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(label, UI_TOOL_LABEL_PAD_BOTTOM, 0);
    lv_obj_set_style_pad_left(label, UI_TOOL_LABEL_PAD_X, 0);
    lv_obj_set_style_pad_right(label, UI_TOOL_LABEL_PAD_X, 0);
    lv_obj_set_style_min_height(label, UI_TOOL_LABEL_MIN_H, 0);
    lv_obj_set_style_text_line_space(label, UI_TOOL_LABEL_LINE_SPACE, 0);
}

/*
 * 计算 Tool Area 中下一个追加项的 y 坐标（基于上一子对象底部+间隙）。
 * 返回: 下一个子对象的 y 坐标
 */
static int tool_area_next_y_locked(void)
{
#if UI_TOOL_AREA_ENABLED
    if (!s_tool_area) {
        return 0;
    }

    uint32_t count = lv_obj_get_child_count(s_tool_area);
    if (count == 0) {
        return 2;
    }

    lv_obj_t *last = lv_obj_get_child(s_tool_area, count - 1);
    if (!last) {
        return 2;
    }

    /*
     * Do not clamp to the visible canvas height here. This is a scrollable
     * container, so appended labels are allowed to extend below the visible
     * area and lv_obj_scroll_to_view() will bring them into view. Clamping was
     * causing new rows to overlap at the bottom and clip glyph descenders.
     */
    lv_obj_update_layout(last);
    return lv_obj_get_y(last) + lv_obj_get_height(last) + UI_TOOL_LABEL_ROW_GAP;
#else
    return 0;
#endif
}

/*
 * 当对话消息数超过 UI_CONV_MAX_MESSAGES 时删除最旧消息。
 */
static void conversation_prune_old_messages_locked(void)
{
    if (!s_conversation) {
        return;
    }

    while (lv_obj_get_child_count(s_conversation) > UI_CONV_MAX_MESSAGES) {
        lv_obj_t *oldest = lv_obj_get_child(s_conversation, 0);
        if (!oldest) {
            break;
        }

        if (oldest == s_last_msg_label) {
            s_last_msg_label = NULL;
            s_last_stream_scroll_tick = 0;
        }
        if (oldest == s_stream_msg_label) {
            s_stream_msg_label = NULL;
            s_stream_active = false;
        }

        lv_obj_delete(oldest);
    }
}

/*
 * 结束当前流式片段状态（重置 stream 跟踪变量）。
 */
static void conversation_finish_fragment_locked(void)
{
    s_stream_msg_label = NULL;
    s_stream_active = false;
}


/*
 * LVGL 显示刷新回调：递增屏幕刷新计数器（用于 FPS 统计）。
 */
static void display_refresh_event_cb(lv_event_t *event)
{
    (void)event;
    s_screen_refresh_count++;
}

#if UI_HAVE_ESP_FREERTOS_HOOKS && UI_ENABLE_CPU_LOAD_IDLE_HOOK

/* CPU0 空闲钩子：递增空闲计数器以估算 CPU 负载。 */
static bool ui_idle_hook_cpu0(void)
{
    s_idle_count[0]++;
    return true;
}

/* CPU1 空闲钩子：递增空闲计数器以估算 CPU 负载。 */
#if SOC_CPU_CORES_NUM > 1
static bool ui_idle_hook_cpu1(void)
{
    s_idle_count[1]++;
    return true;
}
#endif

/* 为每个 CPU 核心注册空闲钩子，用于计算 CPU 负载。 */
static void ui_perf_register_idle_hooks(void)
{
    if (s_cpu_load_hooks_registered) {
        return;
    }

    esp_err_t ret0 = esp_register_freertos_idle_hook_for_cpu(ui_idle_hook_cpu0, 0);
    if (ret0 != ESP_OK && ret0 != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "idle hook cpu0 register failed: %s", esp_err_to_name(ret0));
    }

#if SOC_CPU_CORES_NUM > 1
    esp_err_t ret1 = esp_register_freertos_idle_hook_for_cpu(ui_idle_hook_cpu1, 1);
    if (ret1 != ESP_OK && ret1 != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "idle hook cpu1 register failed: %s", esp_err_to_name(ret1));
    }
#endif

    s_cpu_load_hooks_registered = true;
}
#endif

/*
 * 更新 UI 性能指标（屏幕 FPS、CPU 负载近似值）。
 */
static void ui_perf_update_metrics(void)
{
    /*
     * Use FreeRTOS ticks instead of esp_timer_get_time().
     * Some ESP-IDF master/ESP32-S31 combinations used by this project do not
     * expose esp_timer.h to this component, so keep the UI perf counter
     * dependency-free. Millisecond resolution is enough for status-bar FPS.
     */
    TickType_t now_tick = xTaskGetTickCount();

    if (s_perf_last_update_tick == 0) {
        s_perf_last_update_tick = now_tick;
        s_screen_refresh_last_count = s_screen_refresh_count;
#if UI_HAVE_ESP_FREERTOS_HOOKS && UI_ENABLE_CPU_LOAD_IDLE_HOOK
        for (int i = 0; i < SOC_CPU_CORES_NUM; i++) {
            s_idle_last_count[i] = s_idle_count[i];
        }
#endif
        return;
    }

    TickType_t elapsed_ticks = now_tick - s_perf_last_update_tick;
    uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(elapsed_ticks);
    if (elapsed_ms < UI_PERF_UPDATE_MS) {
        return;
    }

    uint32_t refresh_now = s_screen_refresh_count;
    uint32_t refresh_delta = refresh_now - s_screen_refresh_last_count;
    s_screen_fps = elapsed_ms ?
                   (uint32_t)(((uint64_t)refresh_delta * 1000ULL + elapsed_ms / 2ULL) / elapsed_ms) :
                   0U;
    s_screen_refresh_last_count = refresh_now;

#if UI_HAVE_ESP_FREERTOS_HOOKS && UI_ENABLE_CPU_LOAD_IDLE_HOOK
    for (int i = 0; i < SOC_CPU_CORES_NUM; i++) {
        uint32_t idle_now = s_idle_count[i];
        uint32_t idle_delta = idle_now - s_idle_last_count[i];
        s_idle_last_count[i] = idle_now;

        /*
         * Use the highest observed idle-loop count as the 0%% CPU baseline.
         * This self-calibrates after boot and remains stable enough for UI display.
         */
        if (idle_delta > s_idle_baseline_count[i]) {
            s_idle_baseline_count[i] = idle_delta;
        }

        if (s_idle_baseline_count[i] > 0) {
            uint32_t idle_pct = (idle_delta >= s_idle_baseline_count[i]) ?
                                100U :
                                (uint32_t)(((uint64_t)idle_delta * 100ULL) / s_idle_baseline_count[i]);
            s_cpu_load_pct[i] = idle_pct >= 100U ? 0U : (100U - idle_pct);
        }
    }
#endif

    s_perf_last_update_tick = now_tick;
}

/*
 * 格式化 CPU 使用率为状态栏显示字符串（格式 "c0=N% c1=N%" 或 "n/a"）。
 * 参数: out      - 输出缓冲区
 *       out_size - 输出缓冲区大小
 */
static void format_cpu_usage(char *out, size_t out_size)
{
#if UI_HAVE_ESP_FREERTOS_HOOKS && UI_ENABLE_CPU_LOAD_IDLE_HOOK
#if SOC_CPU_CORES_NUM > 1
    snprintf(out, out_size, "c0=%u%% c1=%u%%", (unsigned)s_cpu_load_pct[0], (unsigned)s_cpu_load_pct[1]);
#else
    snprintf(out, out_size, "c0=%u%%", (unsigned)s_cpu_load_pct[0]);
#endif
#else
    snprintf(out, out_size, "n/a");
#endif
}

/* 格式化追加对话消息并写入对话面板。 */
static void ui_appendf(board_conversation_role_t role, const char *fmt, ...)
{
    char text[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    board_ui_conversation_append(role, text);
}

/*
 * 确定 LCD 旋转方向。
 * 参数: cfg - LCD 配置结构体
 * 返回: LVGL LCD 适配器旋转枚举
 */
static esp_lv_adapter_rotation_t lcd_rotation(const dev_display_lcd_config_t *cfg)
{
    if (cfg && cfg->swap_xy) return cfg->mirror_x ? ESP_LV_ADAPTER_ROTATE_90 : ESP_LV_ADAPTER_ROTATE_270;
    return (cfg && (cfg->mirror_x || cfg->mirror_y)) ? ESP_LV_ADAPTER_ROTATE_180 : ESP_LV_ADAPTER_ROTATE_0;
}

/*
 * 确定 LCD 撕裂避免模式（RGB 双/三缓冲）。
 * 参数: cfg - LCD 配置结构体
 * 返回: 撕裂避免模式枚举
 */
static esp_lv_adapter_tear_avoid_mode_t lcd_tear_mode(const dev_display_lcd_config_t *cfg)
{
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
    if (cfg && strcmp(cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0) {
        uint8_t fbs = cfg->sub_cfg.rgb.panel_config.num_fbs;
        if (fbs >= 3) return ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL;
        if (fbs == 2) return ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL;
    }
#endif
    return ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
}

/* 使用 LVGL 适配器注册显示驱动（支持 RGB/SPI LCD 配置）。 */
static lv_display_t *register_display(dev_display_lcd_config_t *cfg, dev_display_lcd_handles_t *lcd)
{
    esp_lv_adapter_rotation_t rot = lcd_rotation(cfg);
    esp_lv_adapter_display_config_t disp_cfg;

    s_lcd_is_rgb = cfg && strcmp(cfg->sub_type, ESP_BOARD_DEVICE_LCD_SUB_TYPE_RGB) == 0;

    if (s_lcd_is_rgb) {
#if CONFIG_ESP_BOARD_DEV_DISPLAY_LCD_SUB_RGB_SUPPORT
        /*
         * Do not modify cfg->sub_cfg.rgb.panel_config here.
         * Board Manager can place the device config in flash/rodata; writing it
         * causes Store access fault. RGB framebuffer count must be configured in
         * the board definition before the LCD panel is initialized.
         */
        disp_cfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(lcd->panel_handle, lcd->io_handle,
                                                             cfg->lcd_width, cfg->lcd_height, rot);
        disp_cfg.tear_avoid_mode = lcd_tear_mode(cfg);
#else
        return NULL;
#endif
    } else {
#if UI_STABLE_LCD_SPI_INTERNAL_DMA
        disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(lcd->panel_handle, lcd->io_handle,
                                                                           cfg->lcd_width, cfg->lcd_height, rot);
#else
#ifdef CONFIG_SPIRAM
        disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_DEFAULT_CONFIG(lcd->panel_handle, lcd->io_handle,
                                                                        cfg->lcd_width, cfg->lcd_height, rot);
#else
        disp_cfg = ESP_LV_ADAPTER_DISPLAY_SPI_WITHOUT_PSRAM_DEFAULT_CONFIG(lcd->panel_handle, lcd->io_handle,
                                                                           cfg->lcd_width, cfg->lcd_height, rot);
#endif
#endif
    }

    disp_cfg.profile.enable_ppa_accel = false;
    s_ppa_accel_enabled = disp_cfg.profile.enable_ppa_accel;

    ESP_LOGI(TAG,
             "LVGL display %ux%u type=%s rgb=%d tear=%d spi_internal_dma=%d cap gdma=%d dma2d=%d ppa_runtime=%d jpeg=%d",
             cfg->lcd_width, cfg->lcd_height, cfg->sub_type, s_lcd_is_rgb ? 1 : 0,
             disp_cfg.tear_avoid_mode, UI_STABLE_LCD_SPI_INTERNAL_DMA ? 1 : 0,
             SOC_GDMA_SUPPORTED, SOC_DMA2D_SUPPORTED, s_ppa_accel_enabled ? 1 : 0, SOC_JPEG_CODEC_SUPPORTED);
    return esp_lv_adapter_register_display(&disp_cfg);
}

/* 状态栏定时器回调：每 3 秒刷新状态栏中的芯片信息、内存、FPS、WiFi 等状态。 */
static void status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_ota_active) {
        return;
    }
    ui_perf_update_metrics();

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    char heap_text[40];
    char int_text[40];
    char psram_text[40];
    char min_text[16];
    char cpu_text[32];
    char wifi_text[80];
    char web_text[96];
    format_heap_usage(MALLOC_CAP_8BIT, heap_text, sizeof(heap_text));
    format_heap_usage(MALLOC_CAP_INTERNAL, int_text, sizeof(int_text));
    format_heap_usage(MALLOC_CAP_SPIRAM, psram_text, sizeof(psram_text));
    format_bytes(esp_get_minimum_free_heap_size(), min_text, sizeof(min_text));
    format_cpu_usage(cpu_text, sizeof(cpu_text));

    web_text[0] = '\0';
    snprintf(wifi_text,
             sizeof(wifi_text),
             "%s",
             s_wifi_connected ? (s_wifi_detail[0] ? s_wifi_detail : "connected") : "disabled");

    char text[1024];
    snprintf(text, sizeof(text),
             "chip=ESP32S31 cores=%d chip_rev=%u.%u flash=%uMB "
             "heap_8bit=%s heap_min=%s heap_internal=%s heap_psram=%s "
             "screen_fps=%" PRIu32 " cpu=%s camera_fps=%" PRIu32 " camera_drops=%" PRIu32 " "
             "usb=%s wifi=%s web=%s config=%s restart_required=%s model=%s/%s "
             "lcd_rgb=%d hw_gdma=%d hw_dma2d=%d lvgl_ppa=%d hw_jpeg=%d",
             chip.cores, chip.revision / 100, chip.revision % 100,
             (unsigned)(flash_size / (1024 * 1024)),
             heap_text, min_text, int_text, psram_text,
             s_screen_fps, cpu_text, s_camera_stats.preview_fps, s_camera_stats.preview_drops,
             s_usb_mounted ? "mounted" : "unmounted",
             wifi_text,
             web_text[0] ? web_text : "n/a",
             s_config_loaded ? "loaded" : "waiting",
             s_restart_required ? "yes" : "no",
             s_provider, s_model,
             s_lcd_is_rgb ? 1 : 0, SOC_GDMA_SUPPORTED, SOC_DMA2D_SUPPORTED,
             s_ppa_accel_enabled ? 1 : 0, SOC_JPEG_CODEC_SUPPORTED);
    lv_label_set_text(s_status_label, text);
    board_ui_snapshot_set_status(text);
}

/*
 * 创建状态栏控件（全宽、单行无限循环滚动文本），每 3 秒自动刷新。
 * 参数: parent - 父容器对象
 */
static void create_status_bar(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_LCD_WIDTH, UI_STATUS_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_ROOT_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    s_status_label = lv_label_create(bar);
    apply_font(s_status_label);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_status_label, UI_STATUS_SCROLL_MS, 0);
    lv_obj_set_size(s_status_label, UI_LCD_WIDTH - 2 * UI_PANEL_PAD, UI_STATUS_H - 4);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, UI_PANEL_PAD, 2);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xc8d0d8), 0);
    lv_obj_set_style_bg_color(s_status_label, lv_color_hex(UI_ROOT_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_status_label, LV_OPA_COVER, 0);
    status_timer_cb(NULL);
    lv_timer_create(status_timer_cb, UI_STATUS_REFRESH_MS, NULL);
}

/*
 * 分配摄像头预览缓冲区，优先 PSRAM，回退内部 RAM。
 * 参数: allow_psram - 是否允许 PSRAM 分配
 *       name       - 日志标识名称
 * 返回: 对齐分配的缓冲区指针，失败返回 NULL
 */
static uint8_t *preview_buffer_alloc(bool allow_psram, const char *name)
{
    uint8_t *buf = NULL;

    if (allow_psram) {
        buf = heap_caps_aligned_alloc(UI_CACHE_ALIGN,
                                      UI_PREVIEW_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT |
                                      MALLOC_CAP_CACHE_ALIGNED);
        if (!buf) {
            ESP_LOGW(TAG, "%s PSRAM alloc failed, bytes=%u", name, (unsigned)UI_PREVIEW_BYTES);
        }
    }

    if (!buf) {
        bool strict_rgb_psram = s_lcd_is_rgb && UI_PREVIEW_PSRAM_FOR_RGB_LCD &&
                                UI_PREVIEW_STRICT_PSRAM_FOR_RGB_LCD;
        if (strict_rgb_psram) {
            ESP_LOGE(TAG, "%s strict RGB PSRAM alloc failed; no internal fallback", name);
            return NULL;
        }

        /*
         * Fallback path. This is mainly for SPI LCD or debugging. For RGB LCD,
         * UI_PREVIEW_STRICT_PSRAM_FOR_RGB_LCD keeps this source out of internal
         * RAM by default.
         */
        buf = heap_caps_aligned_alloc(UI_CACHE_ALIGN,
                                      UI_PREVIEW_BYTES,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT |
                                      MALLOC_CAP_CACHE_ALIGNED);
        if (!buf) {
            ESP_LOGE(TAG, "%s internal fallback alloc failed, bytes=%u", name, (unsigned)UI_PREVIEW_BYTES);
            return NULL;
        }
    }

    memset(buf, 0, UI_PREVIEW_BYTES);
    esp_err_t sync_ret = esp_cache_msync(buf, UI_PREVIEW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (sync_ret != ESP_OK && sync_ret != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "%s initial cache sync failed: %s", name, esp_err_to_name(sync_ret));
    }

    ESP_LOGI(TAG, "%s=%p bytes=%u lcd_rgb=%d psram_allowed=%d place=%s",
             name,
             buf,
             (unsigned)UI_PREVIEW_BYTES,
             s_lcd_is_rgb ? 1 : 0,
             allow_psram ? 1 : 0,
             esp_ptr_external_ram(buf) ? "PSRAM" : "INTERNAL");
    return buf;
}

/* 预览帧定时器：将摄像头任务写入的待处理帧切换到显示缓冲区并刷新。 */
static void preview_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_ready || !s_preview) {
        return;
    }

    int pending = -1;
    portENTER_CRITICAL(&s_preview_mux);
    pending = s_preview_pending_index;
    if (pending >= 0 && pending < UI_PREVIEW_BUFFER_COUNT && s_preview_buf[pending]) {
        s_preview_pending_index = -1;
        s_preview_front_index = (uint8_t)pending;
    } else {
        pending = -1;
    }
    portEXIT_CRITICAL(&s_preview_mux);

    if (pending < 0) {
        return;
    }

    lv_image_set_src(s_preview, &s_preview_dsc[pending]);
    lv_obj_invalidate(s_preview);
}

/* 事件显示定时器回调：泵送应用事件队列。 */
static void event_display_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    app_event_display_pump();
}

/*
 * 创建摄像头预览区域（240x240），使用双缓冲帧机制。
 * 参数: parent - 父容器对象
 */
static void create_preview(lv_obj_t *parent)
{
    bool allow_psram = (!s_lcd_is_rgb && UI_PREVIEW_PSRAM_FOR_SPI_LCD) ||
                       (s_lcd_is_rgb && UI_PREVIEW_PSRAM_FOR_RGB_LCD);

    s_preview_front_index = 0;
    s_preview_write_index = UI_PREVIEW_BUFFER_COUNT > 1 ? 1 : 0;
    s_preview_pending_index = -1;

    for (uint8_t i = 0; i < UI_PREVIEW_BUFFER_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof(name), "preview_buf[%u]", (unsigned)i);
        s_preview_buf[i] = preview_buffer_alloc(allow_psram, name);
        if (!s_preview_buf[i]) {
            for (uint8_t j = 0; j < i; j++) {
                heap_caps_free(s_preview_buf[j]);
                s_preview_buf[j] = NULL;
            }
            ESP_LOGE(TAG, "preview double buffer alloc failed; preview disabled, bytes=%u count=%u",
                     (unsigned)UI_PREVIEW_BYTES, (unsigned)UI_PREVIEW_BUFFER_COUNT);
            s_preview = lv_label_create(parent);
            apply_font(s_preview);
            lv_label_set_text(s_preview, "Preview disabled");
            lv_obj_align(s_preview, LV_ALIGN_TOP_LEFT, UI_PANEL_PAD, UI_STATUS_H + UI_PANEL_PAD);
            return;
        }

        memset(&s_preview_dsc[i], 0, sizeof(s_preview_dsc[i]));
        s_preview_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_preview_dsc[i].header.w = UI_PREVIEW_W;
        s_preview_dsc[i].header.h = UI_PREVIEW_H;
        s_preview_dsc[i].data_size = UI_PREVIEW_BYTES;
        s_preview_dsc[i].data = s_preview_buf[i];
    }

    s_preview = lv_image_create(parent);
    lv_image_set_src(s_preview, &s_preview_dsc[s_preview_front_index]);
    lv_obj_set_size(s_preview, UI_PREVIEW_W, UI_PREVIEW_H);
    lv_obj_align(s_preview, LV_ALIGN_TOP_LEFT, UI_PANEL_PAD, UI_STATUS_H + UI_PANEL_PAD);
    lv_timer_create(preview_timer_cb, UI_PREVIEW_TIMER_MS, NULL);
}

/*
 * 创建对话面板（flex 列布局、自动滚动条、深色背景）。
 * 参数: parent - 父容器对象
 */
static void create_conversation(lv_obj_t *parent)
{
    s_conversation = lv_obj_create(parent);
    lv_obj_set_size(s_conversation, UI_LCD_WIDTH - UI_CONV_X - UI_PANEL_PAD,
                    UI_LCD_HEIGHT - UI_STATUS_H - UI_PANEL_PAD * 2);
    lv_obj_align(s_conversation, LV_ALIGN_TOP_LEFT, UI_CONV_X, UI_STATUS_H + UI_PANEL_PAD);
    lv_obj_set_style_bg_color(s_conversation, lv_color_hex(UI_CONV_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_conversation, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_conversation, lv_color_hex(0x24303a), 0);
    lv_obj_set_style_border_opa(s_conversation, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_conversation, 4, 0);
    lv_obj_set_style_pad_all(s_conversation, 4, 0);
    lv_obj_set_style_pad_row(s_conversation, 4, 0);
    lv_obj_set_flex_flow(s_conversation, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_conversation, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_conversation, LV_DIR_VER);
}

/*
 * 创建对话覆盖层（透明、位于对话面板上方，用于绘制矩形/文本标注）。
 * 参数: parent - 父容器对象
 */
static void create_overlay(lv_obj_t *parent)
{
    s_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, UI_LCD_WIDTH - UI_CONV_X - UI_PANEL_PAD,
                    UI_LCD_HEIGHT - UI_STATUS_H - UI_PANEL_PAD * 2);
    lv_obj_align(s_overlay, LV_ALIGN_TOP_LEFT, UI_CONV_X, UI_STATUS_H + UI_PANEL_PAD);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
}

/*
 * 创建工具绘制区域（位于预览窗口下方，带标题和滚动画布）。
 * 参数: parent - 父容器对象
 */
static void create_tool_area(lv_obj_t *parent)
{
#if UI_TOOL_AREA_ENABLED
    s_tool_panel = lv_obj_create(parent);
    lv_obj_set_pos(s_tool_panel, UI_TOOL_X, UI_TOOL_Y);
    lv_obj_set_size(s_tool_panel, UI_TOOL_W, UI_TOOL_H);
    lv_obj_set_style_bg_color(s_tool_panel, lv_color_hex(UI_TOOL_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_tool_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_tool_panel, lv_color_hex(UI_TOOL_BORDER_COLOR), 0);
    lv_obj_set_style_border_opa(s_tool_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_tool_panel, 4, 0);
    lv_obj_set_style_pad_all(s_tool_panel, 0, 0);
    lv_obj_clear_flag(s_tool_panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(s_tool_panel);
    apply_font(title);
    lv_label_set_text(title, "Tool Area");
    lv_obj_set_style_text_color(title, lv_color_hex(0x9ed0ff), 0);
    lv_obj_set_style_bg_color(title, lv_color_hex(UI_TOOL_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(title, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(title, 2, 0);
    lv_obj_set_style_pad_bottom(title, 4, 0);
    lv_obj_set_pos(title, UI_TOOL_INNER_PAD, 0);
    lv_obj_set_width(title, UI_TOOL_CANVAS_W);
    lv_obj_set_height(title, UI_TOOL_TITLE_H);

    s_tool_area = lv_obj_create(s_tool_panel);
    lv_obj_remove_style_all(s_tool_area);
    lv_obj_set_pos(s_tool_area, UI_TOOL_INNER_PAD, UI_TOOL_TITLE_H);
    lv_obj_set_size(s_tool_area, UI_TOOL_CANVAS_W, UI_TOOL_CANVAS_H);
    lv_obj_set_style_bg_color(s_tool_area, lv_color_hex(UI_TOOL_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_tool_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tool_area, 0, 0);
    lv_obj_set_scrollbar_mode(s_tool_area, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_tool_area, LV_DIR_VER);
    lv_obj_clear_flag(s_tool_area, LV_OBJ_FLAG_CLICKABLE);
#else
    (void)parent;
#endif
}

/*
 * 创建引导日志全屏覆盖层（深色背景、flex 布局、可滚动日志区）。
 */
static void create_boot_log_layer(void)
{
    s_boot_layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_boot_layer);
    lv_obj_set_size(s_boot_layer, UI_LCD_WIDTH, UI_LCD_HEIGHT);
    lv_obj_set_style_bg_color(s_boot_layer, lv_color_hex(0x05080c), 0);
    lv_obj_set_style_bg_opa(s_boot_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_boot_layer, 12, 0);
    lv_obj_set_style_pad_row(s_boot_layer, 8, 0);
    lv_obj_set_flex_flow(s_boot_layer, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(s_boot_layer);
    apply_font(title);
    lv_label_set_text(title, "C-Claw Boot Log");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe8eef4), 0);

    s_boot_log = lv_obj_create(s_boot_layer);
    lv_obj_set_width(s_boot_log, LV_PCT(100));
    lv_obj_set_flex_grow(s_boot_log, 1);
    lv_obj_set_style_bg_color(s_boot_log, lv_color_hex(0x080c10), 0);
    lv_obj_set_style_bg_opa(s_boot_log, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_boot_log, lv_color_hex(0x24303a), 0);
    lv_obj_set_style_radius(s_boot_log, 4, 0);
    lv_obj_set_style_pad_all(s_boot_log, 8, 0);
    lv_obj_set_style_pad_row(s_boot_log, 4, 0);
    lv_obj_set_flex_flow(s_boot_log, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_boot_log, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_boot_log, LV_DIR_VER);
}

/*
 * 创建 OTA 更新全屏覆盖层（标题、进度条、详情文本），默认隐藏。
 */
static void create_ota_layer_locked(void)
{
    if (s_ota_layer) {
        return;
    }

    s_ota_layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_ota_layer);
    lv_obj_set_size(s_ota_layer, UI_LCD_WIDTH, UI_LCD_HEIGHT);
    lv_obj_set_style_bg_color(s_ota_layer, lv_color_hex(0x05080c), 0);
    lv_obj_set_style_bg_opa(s_ota_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_ota_layer, 32, 0);
    lv_obj_set_flex_flow(s_ota_layer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ota_layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_ota_layer, LV_OBJ_FLAG_HIDDEN);

    s_ota_title = lv_label_create(s_ota_layer);
    apply_font(s_ota_title);
    lv_label_set_text(s_ota_title, "OTA update");
    lv_obj_set_style_text_color(s_ota_title, lv_color_hex(0xe8eef4), 0);

    s_ota_bar = lv_bar_create(s_ota_layer);
    lv_obj_set_size(s_ota_bar, UI_LCD_WIDTH - 160, 28);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_bar_set_value(s_ota_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x203040), 0);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x56c878), LV_PART_INDICATOR);

    s_ota_detail = lv_label_create(s_ota_layer);
    apply_font(s_ota_detail);
    lv_label_set_long_mode(s_ota_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ota_detail, UI_LCD_WIDTH - 120);
    lv_obj_set_style_text_align(s_ota_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ota_detail, lv_color_hex(0xaab6c0), 0);
    lv_label_set_text(s_ota_detail, "Waiting");
}

/*
 * 创建关机/重启全屏黑色覆盖层，默认隐藏。
 */
static void create_shutdown_layer_locked(void)
{
    if (s_shutdown_layer) {
        return;
    }

    s_shutdown_layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_shutdown_layer);
    lv_obj_set_size(s_shutdown_layer, UI_LCD_WIDTH, UI_LCD_HEIGHT);
    lv_obj_set_style_bg_color(s_shutdown_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_shutdown_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_shutdown_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_shutdown_layer, LV_OBJ_FLAG_HIDDEN);
}

/*
 * 更新 OTA 进度条和详情文本（需持有 UI 锁）。
 * 参数: bytes_written - 已写入字节数
 *       expected_size - 预期总大小
 *       prefix        - 进度前缀文本（如 "Writing"）
 */
static void ota_progress_update_locked(size_t bytes_written, size_t expected_size, const char *prefix)
{
    create_ota_layer_locked();
    if (!s_ota_layer) {
        return;
    }

    unsigned pct = expected_size ? (unsigned)((bytes_written * 100U) / expected_size) : 0;
    if (pct > 100U) pct = 100U;
    char written[16];
    char expected[16];
    char text[160];
    format_bytes(bytes_written, written, sizeof(written));
    format_bytes(expected_size, expected, sizeof(expected));
    snprintf(text,
             sizeof(text),
             "%s %u%%\n%s / %s\nDo not power off",
             prefix ? prefix : "Writing",
             pct,
             written,
             expected);
    lv_obj_clear_flag(s_ota_layer, LV_OBJ_FLAG_HIDDEN);
    if (s_ota_bar) {
        lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x56c878), LV_PART_INDICATOR);
        lv_bar_set_value(s_ota_bar, (int32_t)pct, LV_ANIM_OFF);
    }
    if (s_ota_detail) lv_label_set_text(s_ota_detail, text);
    lv_obj_invalidate(s_ota_layer);
}

/* 文本框聚焦回调：连接虚拟键盘到目标文本框。 */
static void textarea_focus_cb(lv_event_t *event)
{
    if (!s_keyboard) return;
    lv_keyboard_set_textarea(s_keyboard, lv_event_get_target(event));
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

/*
 * 向配置表单添加一个输入字段（标题标签 + 文本框，可选密码模式）。
 * 参数: parent - 父容器
 *       title  - 字段标签文本
 *       path   - JSON 配置路径（如 "model.provider"）
 *       value  - 初始值
 *       secret - 是否启用密码模式
 */
static void add_field(lv_obj_t *parent, const char *title, const char *path, const char *value, bool secret)
{
    lv_obj_t *label = lv_label_create(parent);
    apply_font(label);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0xaab4be), 0);

    lv_obj_t *ta = lv_textarea_create(parent);
    apply_font(ta);
    lv_obj_set_width(ta, 360);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, value ? value : "");
    if (secret) lv_textarea_set_password_mode(ta, true);
    lv_obj_add_event_cb(ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL);

    if (s_field_count < UI_CONFIG_FIELD_MAX) {
        s_fields[s_field_count++] = (config_field_t){path, ta};
    }
}

/* 判断配置路径是否为布尔类型字段。 */
static bool is_bool_path(const char *path)
{
    return strcmp(path, "network.sta.enabled") == 0 ||
           strcmp(path, "network.ap.enabled") == 0 ||
           strcmp(path, "network.mdns.enabled") == 0 ||
           strcmp(path, "runtime.stream") == 0 ||
           strcmp(path, "runtime.thinking") == 0 ||
           strcmp(path, "runtime.web_enabled") == 0;
}

/* 判断配置路径是否为整数类型字段。 */
static bool is_int_path(const char *path)
{
    return strcmp(path, "model.max_tokens") == 0 ||
           strcmp(path, "runtime.max_steps") == 0 ||
           strcmp(path, "runtime.queue_depth") == 0 ||
           strcmp(path, "runtime.max_input_bytes") == 0 ||
           strcmp(path, "runtime.web_port") == 0 ||
           strcmp(path, "runtime.event_limit") == 0 ||
           strcmp(path, "network.sta.connect_timeout_ms") == 0 ||
           strcmp(path, "network.sta.max_retry") == 0 ||
           strcmp(path, "network.ap.channel") == 0 ||
           strcmp(path, "network.ap.max_connections") == 0 ||
           strcmp(path, "camera.default_quality") == 0;
}

/* 判断文本值是否为 true（"1"/"true"/"on"/"yes"）。 */
static bool text_is_true(const char *text)
{
    return text && (strcmp(text, "1") == 0 || strcmp(text, "true") == 0 || strcmp(text, "on") == 0 || strcmp(text, "yes") == 0);
}

/*
 * 保存配置按钮回调：从表单收集值并按路径写回 JSON 文件。
 */
static void save_config_cb(lv_event_t *event)
{
    (void)event;
    cc_json_value_t *root = NULL;
    char *json_source = s_callbacks.load_config_json ? s_callbacks.load_config_json(s_callbacks.user_data) : NULL;
    cc_result_t rc = cc_json_parse(json_source ? json_source : "{}", &root);
    free(json_source);
    if (rc.code != CC_OK || !root || !cc_json_is_object(root)) {
        ui_appendf(BOARD_CONV_ERROR, "config open failed: %s", rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        cc_json_destroy(root);
        return;
    }
    cc_result_free(&rc);

    for (size_t i = 0; i < s_field_count; i++) {
        const char *path = s_fields[i].path;
        const char *text = lv_textarea_get_text(s_fields[i].textarea);
        if (is_int_path(path)) {
            board_ui_json_set_path_number(root, path, atoi(text ? text : "0"));
        } else if (strcmp(path, "model.temperature") == 0) {
            board_ui_json_set_path_number(root, path, atof(text ? text : "0"));
        } else if (is_bool_path(path)) {
            board_ui_json_set_path_bool(root, path, text_is_true(text));
        } else {
            board_ui_json_set_path_string(root, path, text);
        }
    }

    char *json = cc_json_stringify(root);
    if (!json) {
        cc_json_destroy(root);
        board_ui_conversation_append(BOARD_CONV_ERROR, "config save failed: OOM");
        return;
    }

    bool saved = s_callbacks.save_config_json ? s_callbacks.save_config_json(json, s_callbacks.user_data) : false;
    free(json);
    if (!saved) {
        board_ui_conversation_append(BOARD_CONV_ERROR, "config save failed");
        cc_json_destroy(root);
        return;
    }

    board_ui_conversation_append(BOARD_CONV_STATUS, "config saved; services updated");
    cc_json_destroy(root);
}
/* 关闭配置编辑器模态框，删除键盘和模态框对象。 */
static void close_config_cb(lv_event_t *event)
{
    (void)event;
    if (s_keyboard) {
        lv_obj_delete(s_keyboard);
        s_keyboard = NULL;
    }
    if (!s_config_modal) return;
    lv_obj_delete(s_config_modal);
    s_config_modal = NULL;
    s_field_count = 0;
}

/* Settings 按钮点击回调：调用配置编辑器。 */
static void settings_btn_cb(lv_event_t *event)
{
    (void)event;
    board_ui_show_config_editor();
}

/*
 * 创建 Settings 按钮（位于预览面板下方）。
 * 参数: parent - 父容器对象
 */
static void create_settings_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 96, 38);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, UI_PANEL_PAD, UI_STATUS_H + UI_PANEL_PAD + UI_PREVIEW_H + 8);
    lv_obj_add_event_cb(btn, settings_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    apply_font(label);
    lv_label_set_text(label, "Settings");
    lv_obj_center(label);
}

/*
 * 注册 UI 回调函数集（配置读写接口）。
 * 参数: callbacks - 回调函数集指针
 */
void board_ui_set_callbacks(const board_ui_callbacks_t *callbacks)
{
    if (callbacks) {
        s_callbacks = *callbacks;
    } else {
        memset(&s_callbacks, 0, sizeof(s_callbacks));
    }
}

/*
 * 初始化 LVGL UI 系统：初始化 LCD/触摸/背光、注册显示驱动、创建 800x480 布局。
 * 返回: ESP_OK 成功，否则为 ESP 错误码
 */
esp_err_t board_ui_init(void)
{
    board_ui_snapshot_init();

    if (s_ready) return ESP_OK;

    esp_err_t ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "display_lcd init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_TOUCH);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "lcd_touch init failed: %s", esp_err_to_name(ret));
    }
    if (esp_board_manager_check_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS)) {
        ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_LCD_BRIGHTNESS);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "lcd_brightness init failed: %s", esp_err_to_name(ret));
        }
    } else {
        ESP_LOGD(TAG, "lcd_brightness is not defined; skipping brightness device init");
    }

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_priority = 4;
    adapter_cfg.task_stack_size = 10 * 1024;
    adapter_cfg.task_core_id = 1;
    adapter_cfg.tick_period_ms = 5;
    adapter_cfg.task_max_delay_ms = 100;
#if UI_TASK_STACK_IN_PSRAM
    adapter_cfg.stack_in_psram = true;
#endif
    ret = esp_lv_adapter_init(&adapter_cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    void *lcd_handle = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, &lcd_handle);
    if (ret != ESP_OK || !lcd_handle) return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;

    dev_display_lcd_config_t *lcd_cfg = NULL;
    ret = esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD, (void **)&lcd_cfg);
    if (ret != ESP_OK || !lcd_cfg) return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;

    s_disp = register_display(lcd_cfg, (dev_display_lcd_handles_t *)lcd_handle);
    if (!s_disp) return ESP_FAIL;

    lv_display_add_event_cb(s_disp, display_refresh_event_cb, LV_EVENT_REFR_READY, NULL);
#if UI_HAVE_ESP_FREERTOS_HOOKS && UI_ENABLE_CPU_LOAD_IDLE_HOOK
    ui_perf_register_idle_hooks();
#endif

#if defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_SUPPORT)
    void *touch_handle = NULL;
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle) == ESP_OK && touch_handle) {
        dev_lcd_touch_handles_t *touch = (dev_lcd_touch_handles_t *)touch_handle;
        const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, touch->touch_handle);
        s_touch = esp_lv_adapter_register_touch(&touch_cfg);
    }
#elif defined(CONFIG_ESP_BOARD_DEV_LCD_TOUCH_I2C_SUPPORT)
    void *touch_handle = NULL;
    if (esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_LCD_TOUCH, &touch_handle) == ESP_OK && touch_handle) {
        dev_lcd_touch_i2c_handles_t *touch = (dev_lcd_touch_i2c_handles_t *)touch_handle;
        const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(s_disp, touch->touch_handle);
        s_touch = esp_lv_adapter_register_touch(&touch_cfg);
    }
#endif

    ret = esp_lv_adapter_start();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    ui_lock();
    s_root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, UI_LCD_WIDTH, UI_LCD_HEIGHT);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_ROOT_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    create_status_bar(s_root);
    create_preview(s_root);
    create_settings_button(s_root);
    create_tool_area(s_root);
    create_conversation(s_root);
    create_overlay(s_root);
    create_boot_log_layer();
    (void)tool_cmd_queue_ensure();
    lv_timer_create(tool_cmd_timer_cb, UI_TOOL_CMD_TIMER_MS, NULL);
    lv_timer_create(event_display_timer_cb, UI_EVENT_DISPLAY_TIMER_MS, NULL);
    ui_unlock();

    s_ready = true;
    return ESP_OK;
}

/* 返回 UI 就绪状态。 */
bool board_ui_ready(void)
{
    return s_ready;
}

/*
 * 清空对话面板：清除 snapshot 缓冲、conversation/overlay/tool area 中所有 LVGL 对象。
 */
void board_ui_conversation_clear(void)
{
    if (!s_ready) return;

    board_ui_snapshot_clear_conversation();
    board_ui_snapshot_clear_tool();
    board_ui_snapshot_clear_draw();

    ui_lock();

    lv_obj_clean(s_conversation);
    conversation_overlay_clear_locked();
    tool_area_clear_locked();
    s_last_msg_label = NULL;
    s_stream_msg_label = NULL;
    s_stream_active = false;
    s_last_stream_scroll_tick = 0;

    lv_obj_invalidate(s_conversation);

    ui_unlock();
}

/*
 * 向对话面板追加一条完整消息（非流式），结束之前流式片段。
 * 参数: role - 对话角色
 *       text - 消息正文
 */
void board_ui_conversation_append(board_conversation_role_t role, const char *text)
{
    if (!s_ready) return;

    ui_lock();
    conversation_finish_fragment_locked();

    lv_obj_t *label = lv_label_create(s_conversation);
    apply_message_label_style(label, role);

    char line[512];
    snprintf(line, sizeof(line), "%s: %s", role_prefix(role), text ? text : "");
    lv_label_set_text(label, line);
    board_ui_snapshot_append_conversation(line);

    s_last_role = role;
    s_last_msg_label = label;
    s_last_stream_scroll_tick = xTaskGetTickCount();

    conversation_prune_old_messages_locked();

    lv_obj_update_layout(s_conversation);
    lv_obj_scroll_to_view(label, LV_ANIM_OFF);

    ui_unlock();
}

/*
 * 向对话面板追加流式文本片段。同角色连续调用合并到同一标签，定期滚动。
 * 参数: role - 对话角色
 *       text - 片段文本
 */
void board_ui_conversation_append_fragment(board_conversation_role_t role, const char *text)
{
    if (!s_ready || !text || !text[0]) return;

    ui_lock();

    if (!s_stream_active || !s_stream_msg_label || s_stream_role != role) {
        lv_obj_t *label = lv_label_create(s_conversation);
        apply_message_label_style(label, role);

        const char *prefix = role_prefix(role);
        size_t prefix_len = strlen(prefix);
        size_t text_len = strlen(text);
        size_t line_len = prefix_len + 2 + text_len;
        char *line = heap_caps_malloc(line_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        bool line_heap_caps = line != NULL;
        if (!line) {
            line = malloc(line_len + 1);
        }
        if (line) {
            memcpy(line, prefix, prefix_len);
            memcpy(line + prefix_len, ": ", 2);
            memcpy(line + prefix_len + 2, text, text_len + 1);
            lv_label_set_text(label, line);
            board_ui_snapshot_append_conversation(line);
            if (line_heap_caps) {
                heap_caps_free(line);
            } else {
                free(line);
            }
        } else {
            char fallback[512];
            snprintf(fallback, sizeof(fallback), "%s: %s", prefix, text);
            lv_label_set_text(label, fallback);
            board_ui_snapshot_append_conversation(fallback);
        }

        s_last_role = role;
        s_last_msg_label = label;
        s_stream_role = role;
        s_stream_msg_label = label;
        s_stream_active = true;
        s_last_stream_scroll_tick = xTaskGetTickCount();

        conversation_prune_old_messages_locked();
        lv_obj_update_layout(s_conversation);
        lv_obj_scroll_to_view(label, LV_ANIM_OFF);
        ui_unlock();
        return;
    }

    /* Refresh only the active streaming label, not the whole panel. */
    lv_obj_invalidate(s_stream_msg_label);

    const char *old = lv_label_get_text(s_stream_msg_label);
    size_t old_len = old ? strlen(old) : 0;
    size_t add_len = strlen(text);

    char *joined = heap_caps_malloc(old_len + add_len + 1,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!joined) {
        joined = malloc(old_len + add_len + 1);
    }

    if (joined) {
        memcpy(joined, old ? old : "", old_len);
        memcpy(joined + old_len, text, add_len + 1);

        lv_label_set_text(s_stream_msg_label, joined);
        board_ui_snapshot_replace_last_conversation(joined);
        free(joined);

        apply_message_label_style(s_stream_msg_label, role);
        lv_obj_set_height(s_stream_msg_label, LV_SIZE_CONTENT);
        lv_obj_invalidate(s_stream_msg_label);

        TickType_t now = xTaskGetTickCount();
        if (s_last_stream_scroll_tick == 0 ||
            (now - s_last_stream_scroll_tick) >= pdMS_TO_TICKS(UI_STREAM_SCROLL_INTERVAL_MS)) {
            conversation_prune_old_messages_locked();
            lv_obj_update_layout(s_conversation);
            lv_obj_scroll_to_view(s_stream_msg_label, LV_ANIM_OFF);
            s_last_stream_scroll_tick = now;
        }
    }

    ui_unlock();
}

/*
 * 结束当前流式片段，强制最终布局和滚动。
 */
void board_ui_conversation_finish_fragment(void)
{
    if (!s_ready) return;

    ui_lock();
    if (s_stream_active && s_stream_msg_label) {
        lv_obj_set_height(s_stream_msg_label, LV_SIZE_CONTENT);
        lv_obj_update_layout(s_conversation);
        lv_obj_scroll_to_view(s_stream_msg_label, LV_ANIM_OFF);
    }
    conversation_finish_fragment_locked();
    ui_unlock();
}

/*
 * 向引导日志界面追加一行日志（带角色颜色）。
 * 参数: role - 消息角色
 *       text - 日志文本
 */
void board_ui_boot_log_append(board_conversation_role_t role, const char *text)
{
    if (!s_ready) return;

    ui_lock();
    if (!s_boot_log) {
        ui_unlock();
        board_ui_conversation_append(role, text);
        return;
    }

    while (lv_obj_get_child_count(s_boot_log) >= UI_BOOT_LOG_MAX_LINES) {
        lv_obj_t *oldest = lv_obj_get_child(s_boot_log, 0);
        if (!oldest) break;
        lv_obj_delete(oldest);
    }

    lv_obj_t *label = lv_label_create(s_boot_log);
    apply_message_label_style(label, role);

    char line[512];
    snprintf(line, sizeof(line), "%s: %s", role_prefix(role), text ? text : "");
    lv_label_set_text(label, line);
    board_ui_snapshot_append_boot(line);
    lv_obj_scroll_to_view(label, LV_ANIM_OFF);
    ui_unlock();
}

/*
 * 删除引导日志覆盖层，切换回主 UI。
 */
void board_ui_boot_log_finish(void)
{
    if (!s_ready) return;

    ui_lock();
    if (s_boot_layer) {
        lv_obj_delete(s_boot_layer);
        s_boot_layer = NULL;
        s_boot_log = NULL;
    }
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(s_root);
    lv_obj_invalidate(s_conversation);
    ui_unlock();
}

/*
 * 在对话覆盖层上绘制填充矩形（像素坐标相对于 overlay 区域）。
 * 参数: x/y/width/height - 矩形位置和尺寸
 *       color - RGB565 颜色值
 */
void board_ui_conversation_draw_rect(int x, int y, int width, int height, uint16_t color)
{
    if (!s_ready || width <= 0 || height <= 0) return;
    board_ui_snapshot_append_drawf("conversation.rect x=%d y=%d w=%d h=%d color=0x%04x",
                                   x,
                                   y,
                                   width,
                                   height,
                                   (unsigned)color);
    ui_lock();
    lv_obj_t *obj = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(rgb565_to_rgb888(color)), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    ui_unlock();
}

/*
 * 在对话覆盖层上绘制文本（角色决定颜色，带不透明背景防撕裂）。
 * 参数: x/y  - 文本位置
 *       role - 对话角色
 *       text - 文本内容
 */
void board_ui_conversation_draw_text(int x, int y, board_conversation_role_t role, const char *text)
{
    if (!s_ready) return;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    board_ui_snapshot_append_drawf("conversation.text x=%d y=%d role=%s text=%s",
                                   x,
                                   y,
                                   role_prefix(role),
                                   text ? text : "");

    ui_lock();

    lv_obj_t *label = lv_label_create(s_overlay);
    apply_message_label_style(label, role);
    lv_obj_set_pos(label, x, y);

    int width = UI_LCD_WIDTH - UI_CONV_X - x - UI_PANEL_PAD;
    if (width < 1) width = 1;
    lv_obj_set_width(label, width);

    lv_label_set_text(label, text ? text : "");

    ui_unlock();
}

/* 触发 LVGL 重绘 root、conversation 和 overlay 区域。 */
void board_ui_conversation_request_flush(void)
{
    if (!s_ready) return;
    ui_lock();
    lv_obj_invalidate(s_root);
    lv_obj_invalidate(s_conversation);
    if (s_overlay) lv_obj_invalidate(s_overlay);
    if (s_tool_panel) lv_obj_invalidate(s_tool_panel);
    if (s_tool_area) lv_obj_invalidate(s_tool_area);
    ui_unlock();
}

/*
 * 确保工具命令队列已创建（PSRAM 优先，回退到内部 RAM）。
 * 返回: true 队列可用
 */
static bool tool_cmd_queue_ensure(void)
{
    if (s_tool_cmd_queue) {
        return true;
    }
#if configSUPPORT_STATIC_ALLOCATION
    s_tool_cmd_queue = xQueueCreateWithCaps(UI_TOOL_CMD_QUEUE_DEPTH,
                                            sizeof(tool_cmd_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
    if (!s_tool_cmd_queue) {
        s_tool_cmd_queue = xQueueCreate(UI_TOOL_CMD_QUEUE_DEPTH, sizeof(tool_cmd_t));
    }
    return s_tool_cmd_queue != NULL;
}

/*
 * 将工具命令入队（非阻塞发送到 xQueue）。
 * 参数: cmd - 指向工具命令的指针
 */
static void tool_cmd_enqueue(const tool_cmd_t *cmd)
{
    if (!cmd || !s_ready || !tool_cmd_queue_ensure()) {
        return;
    }
    (void)xQueueSend(s_tool_cmd_queue, cmd, 0);
}

/* 清空 Tool Area 内容并清除对应快照缓冲区。 */
static void tool_area_clear_cmd_locked(void)
{
    board_ui_snapshot_clear_tool();
    tool_area_clear_locked();
}

/* 在 Tool Area 中追加一行文本（需持有 UI 锁）。 */
static void tool_area_append_cmd_locked(board_conversation_role_t role, const char *text)
{
#if UI_TOOL_AREA_ENABLED
    if (!s_tool_area) {
        return;
    }

    int y = tool_area_next_y_locked();
    lv_obj_t *label = lv_label_create(s_tool_area);
    apply_tool_label_style(label, role);
    lv_obj_set_pos(label, 0, y);
    lv_obj_set_width(label, UI_TOOL_CANVAS_W);

    char line[384];
    snprintf(line, sizeof(line), "%s: %s", role_prefix(role), text ? text : "");
    lv_label_set_text(label, line);
    board_ui_snapshot_append_tool(line);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_update_layout(label);

    tool_area_prune_locked();
    lv_obj_scroll_to_view(label, LV_ANIM_OFF);
    lv_obj_invalidate(s_tool_area);
#else
    (void)role;
    (void)text;
#endif
}

/* 在 Tool Area 中绘制矩形（需持有 UI 锁）。 */
static void tool_area_rect_cmd_locked(int x, int y, int width, int height, uint16_t color)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    board_ui_snapshot_append_drawf("tool.rect x=%d y=%d w=%d h=%d color=0x%04x",
                                   x,
                                   y,
                                   width,
                                   height,
                                   (unsigned)color);

#if UI_TOOL_AREA_ENABLED
    if (!s_tool_area) {
        return;
    }

    x = clamp_int(x, 0, UI_TOOL_CANVAS_W - 1);
    y = clamp_int(y, 0, UI_TOOL_CANVAS_H - 1);
    width = clamp_int(width, 1, UI_TOOL_CANVAS_W - x);
    height = clamp_int(height, 1, UI_TOOL_CANVAS_H - y);

    lv_obj_t *obj = lv_obj_create(s_tool_area);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(rgb565_to_rgb888(color)), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    tool_area_prune_locked();
    lv_obj_invalidate(s_tool_area);
#endif
}

/* 在 Tool Area 中绘制文本（需持有 UI 锁）。 */
static void tool_area_text_cmd_locked(int x, int y, board_conversation_role_t role, const char *text)
{
    board_ui_snapshot_append_drawf("tool.text x=%d y=%d role=%s text=%s",
                                   x,
                                   y,
                                   role_prefix(role),
                                   text ? text : "");

#if UI_TOOL_AREA_ENABLED
    if (!s_tool_area) {
        return;
    }

    x = clamp_int(x, 0, UI_TOOL_CANVAS_W - 1);
    y = clamp_int(y, 0, UI_TOOL_CANVAS_H - 1);

    lv_obj_t *label = lv_label_create(s_tool_area);
    apply_tool_label_style(label, role);
    lv_obj_set_pos(label, x, y);

    int label_w = UI_TOOL_CANVAS_W - x;
    if (label_w < 1) label_w = 1;
    lv_obj_set_width(label, label_w);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_update_layout(label);

    tool_area_prune_locked();
    lv_obj_scroll_to_view(label, LV_ANIM_OFF);
    lv_obj_invalidate(s_tool_area);
#endif
}

/* Tool Area 刷新命令（触发重绘）。 */
static void tool_area_flush_cmd_locked(void)
{
    if (s_tool_panel) lv_obj_invalidate(s_tool_panel);
    if (s_tool_area) lv_obj_invalidate(s_tool_area);
}

/*
 * 处理 Tool Area 命令队列：每 20ms 批量消费队列中所有命令并在 LVGL 线程执行。
 */
static void tool_cmd_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_tool_cmd_queue) {
        return;
    }

    tool_cmd_t cmd;
    unsigned processed = 0;
    while (processed++ < UI_TOOL_CMD_QUEUE_DEPTH &&
           xQueueReceive(s_tool_cmd_queue, &cmd, 0) == pdTRUE) {
        switch (cmd.type) {
        case UI_TOOL_CMD_CLEAR:
            tool_area_clear_cmd_locked();
            break;
        case UI_TOOL_CMD_APPEND:
            tool_area_append_cmd_locked(cmd.role, cmd.text);
            break;
        case UI_TOOL_CMD_RECT:
            tool_area_rect_cmd_locked(cmd.x, cmd.y, cmd.width, cmd.height, cmd.color);
            break;
        case UI_TOOL_CMD_TEXT:
            tool_area_text_cmd_locked(cmd.x, cmd.y, cmd.role, cmd.text);
            break;
        case UI_TOOL_CMD_FLUSH:
            tool_area_flush_cmd_locked();
            break;
        default:
            break;
        }
    }
}

/* 通过命令队列异步清空 Tool Area 内容。 */
void board_ui_tool_area_clear(void)
{
    if (!s_ready) return;
    const tool_cmd_t cmd = {
        .type = UI_TOOL_CMD_CLEAR,
    };
    tool_cmd_enqueue(&cmd);
}

/* 通过命令队列异步追加文本到 Tool Area。 */
void board_ui_tool_area_append(board_conversation_role_t role, const char *text)
{
    if (!s_ready) return;

    tool_cmd_t cmd = {
        .type = UI_TOOL_CMD_APPEND,
        .role = role,
    };
    snprintf(cmd.text, sizeof(cmd.text), "%s", text ? text : "");
    tool_cmd_enqueue(&cmd);
}

/* 通过命令队列异步在 Tool Area 绘制矩形。 */
void board_ui_tool_area_draw_rect(int x, int y, int width, int height, uint16_t color)
{
    if (!s_ready || width <= 0 || height <= 0) return;
    const tool_cmd_t cmd = {
        .type = UI_TOOL_CMD_RECT,
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .color = color,
    };
    tool_cmd_enqueue(&cmd);
}

/* 通过命令队列异步在 Tool Area 绘制文本。 */
void board_ui_tool_area_draw_text(int x, int y, board_conversation_role_t role, const char *text)
{
    if (!s_ready) return;
    tool_cmd_t cmd = {
        .type = UI_TOOL_CMD_TEXT,
        .role = role,
        .x = x,
        .y = y,
    };
    snprintf(cmd.text, sizeof(cmd.text), "%s", text ? text : "");
    tool_cmd_enqueue(&cmd);
}

/* 通过命令队列异步触发 Tool Area 刷新。 */
void board_ui_tool_area_request_flush(void)
{
    if (!s_ready) return;
    const tool_cmd_t cmd = {
        .type = UI_TOOL_CMD_FLUSH,
    };
    tool_cmd_enqueue(&cmd);
}

/*
 * 摄像头预览帧更新（摄像头任务直接调用，通过双缓冲机制避免 LVGL 锁）。
 * 参数: rgb565 - 240x240 RGB565 像素数据
 *       width  - 必须等于 UI_PREVIEW_W (240)
 *       height - 必须等于 UI_PREVIEW_H (240)
 */
void board_ui_camera_preview_update(const void *rgb565, int width, int height)
{
    if (!s_ready || !s_preview || !rgb565 || width != UI_PREVIEW_W || height != UI_PREVIEW_H) return;
#if UI_PREVIEW_BUFFER_COUNT < 2
    return;
#endif

#if UI_PREVIEW_MAX_FPS > 0
    TickType_t now = xTaskGetTickCount();
    TickType_t min_interval = pdMS_TO_TICKS(1000U / UI_PREVIEW_MAX_FPS);
    if (min_interval < 1) min_interval = 1;

    if (s_preview_last_update_tick != 0 && (now - s_preview_last_update_tick) < min_interval) {
#if UI_PREVIEW_YIELD_AFTER_UPDATE
        vTaskDelay(1);
#endif
        return;
    }
    s_preview_last_update_tick = now;
#endif

#if UI_PREVIEW_SOURCE_DMA_WRITES
    esp_err_t src_sync = esp_cache_msync((void *)rgb565, UI_PREVIEW_BYTES,
                                         ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if (src_sync != ESP_OK && src_sync != ESP_ERR_NOT_SUPPORTED && !s_preview_src_sync_warned) {
        s_preview_src_sync_warned = true;
        ESP_LOGW(TAG, "preview source cache sync failed once: %s", esp_err_to_name(src_sync));
    }
#endif

    /*
     * The camera task must not call LVGL or block on the LVGL mutex. It only
     * writes a non-front, non-pending source buffer and marks it pending; the
     * LVGL timer consumes that pending frame on the LVGL task.
     */
    uint8_t write_index = 0;
    bool found = false;

    portENTER_CRITICAL(&s_preview_mux);
    uint8_t front_index = s_preview_front_index;
    int pending_index = s_preview_pending_index;
    uint8_t candidate = s_preview_write_index;
    for (uint8_t i = 0; i < UI_PREVIEW_BUFFER_COUNT; i++) {
        if (candidate != front_index &&
            (pending_index < 0 || candidate != (uint8_t)pending_index) &&
            s_preview_buf[candidate]) {
            write_index = candidate;
            s_preview_write_index = (uint8_t)((candidate + 1U) % UI_PREVIEW_BUFFER_COUNT);
            found = true;
            break;
        }
        candidate = (uint8_t)((candidate + 1U) % UI_PREVIEW_BUFFER_COUNT);
    }
    portEXIT_CRITICAL(&s_preview_mux);

    if (!found) {
        return;
    }

    uint8_t *dst = s_preview_buf[write_index];
    memcpy(dst, rgb565, UI_PREVIEW_BYTES);

    esp_err_t dst_sync = esp_cache_msync(dst, UI_PREVIEW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (dst_sync != ESP_OK && dst_sync != ESP_ERR_NOT_SUPPORTED && !s_preview_dst_sync_warned) {
        s_preview_dst_sync_warned = true;
        ESP_LOGW(TAG, "preview dst cache sync failed once: %s", esp_err_to_name(dst_sync));
    }

    portENTER_CRITICAL(&s_preview_mux);
    s_preview_pending_index = (int)write_index;
    portEXIT_CRITICAL(&s_preview_mux);

#if UI_PREVIEW_YIELD_AFTER_UPDATE
    vTaskDelay(1);
#endif
}


/*
 * 设置摄像头性能统计（供摄像头任务线程安全更新）。
 * 参数: stats - 统计结构体指针
 */
void board_ui_camera_stats_update(const board_ui_camera_stats_t *stats)
{
    if (stats) s_camera_stats = *stats;
}

/*
 * 配置加载状态回调：设置 provider/model 字段并触发外部字体加载。
 * 参数: loaded   - 配置已加载
 *       provider - 提供商名称
 *       model    - 模型名称
 */
void board_ui_set_config_loaded(bool loaded, const char *provider, const char *model)
{
    s_config_loaded = loaded;
    snprintf(s_provider, sizeof(s_provider), "%s", provider ? provider : "");
    snprintf(s_model, sizeof(s_model), "%s", model ? model : "");
    if (loaded && s_ready) {
        bool external_font_loaded = board_ui_font_load_external();
        ui_lock();
        apply_font_tree(s_root);
        apply_font_tree(s_config_modal);
        apply_font_tree(s_keyboard);
        apply_font_tree(s_ota_layer);
        ui_unlock();
        if (!external_font_loaded) {
            board_ui_boot_log_append(BOARD_CONV_ERROR, "external CJK font not loaded: check configured font path");
        } else {
            board_ui_boot_log_append(BOARD_CONV_STATUS, "external CJK font loaded");
        }
    }
}

/* 设置 USB 挂载状态。 */
void board_ui_set_usb_mounted(bool mounted)
{
    s_usb_mounted = mounted;
}

/* 设置 Wi-Fi 连接状态和详情文本。 */
void board_ui_set_wifi_state(bool connected, const char *detail)
{
    s_wifi_connected = connected;
    snprintf(s_wifi_detail, sizeof(s_wifi_detail), "%s", detail ? detail : "");
}

/* 标记系统需要重启（配置变更后）。 */
void board_ui_set_runtime_restart_required(bool required)
{
    s_restart_required = required;
}

/*
 * OTA 准备阶段：显示 OTA 覆盖层、蓝底进度条、固件名称和预期大小。
 * 参数: name          - 固件名称
 *       expected_size - 预期固件大小（字节）
 *       message       - 附加状态消息
 */
void board_ui_ota_prepare(const char *name, size_t expected_size, const char *message)
{
    if (!s_ready) return;
    s_ota_active = true;
    s_ota_last_update_tick = xTaskGetTickCount();
    ui_lock();
    create_ota_layer_locked();
    if (s_ota_title) {
        char title[128];
        snprintf(title, sizeof(title), "OTA update: %s", name && name[0] ? name : "firmware");
        lv_label_set_text(s_ota_title, title);
    }
    if (s_ota_layer) {
        lv_obj_clear_flag(s_ota_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ota_bar) {
        lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x56a8ff), LV_PART_INDICATOR);
        lv_bar_set_value(s_ota_bar, 0, LV_ANIM_OFF);
    }
    if (s_ota_detail) {
        char expected[16];
        char text[192];
        format_bytes(expected_size, expected, sizeof(expected));
        snprintf(text,
                 sizeof(text),
                 "Preparing OTA\n%s\nImage: %s",
                 message && message[0] ? message : "Waiting for current work to finish",
                 expected);
        lv_label_set_text(s_ota_detail, text);
    }
    if (s_ota_layer) lv_obj_invalidate(s_ota_layer);
    ui_unlock();
}

/*
 * OTA 写入开始：显示覆盖层并初始化进度条为 0%。
 * 参数: name          - 固件名称
 *       expected_size - 预期固件大小（字节）
 */
void board_ui_ota_begin(const char *name, size_t expected_size)
{
    if (!s_ready) return;
    s_ota_active = true;
    s_ota_last_update_tick = xTaskGetTickCount();
    ui_lock();
    create_ota_layer_locked();
    if (s_ota_title) {
        char title[128];
        snprintf(title, sizeof(title), "OTA update: %s", name && name[0] ? name : "firmware");
        lv_label_set_text(s_ota_title, title);
    }
    ota_progress_update_locked(0, expected_size, "Starting");
    ui_unlock();
}

/*
 * OTA 进度更新：限制更新频率（最多每 250ms 一次）以防止 UI 抖动。
 * 参数: bytes_written - 已写入字节数
 *       expected_size - 预期总大小
 */
void board_ui_ota_progress(size_t bytes_written, size_t expected_size)
{
    if (!s_ready) return;
    TickType_t now = xTaskGetTickCount();
    if (bytes_written < expected_size &&
        s_ota_last_update_tick != 0 &&
        now - s_ota_last_update_tick < pdMS_TO_TICKS(250)) {
        return;
    }
    s_ota_last_update_tick = now;
    ui_lock();
    ota_progress_update_locked(bytes_written, expected_size, "Writing");
    ui_unlock();
}

/*
 * OTA 结束：显示成功/失败状态，绿色/红色进度条，延迟退出 OTA 激活标志。
 * 参数: ok      - 是否成功
 *       message - 结果说明消息
 */
void board_ui_ota_end(bool ok, const char *message)
{
    if (!s_ready) return;
    ui_lock();
    create_ota_layer_locked();
    if (s_ota_layer) {
        lv_obj_clear_flag(s_ota_layer, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ota_bar) {
        lv_bar_set_value(s_ota_bar, ok ? 100 : 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_ota_bar,
                                  lv_color_hex(ok ? 0x56c878 : 0xff5a5a),
                                  LV_PART_INDICATOR);
    }
    if (s_ota_detail) {
        char text[160];
        snprintf(text,
                 sizeof(text),
                 "%s\n%s",
                 ok ? "OTA ready" : "OTA failed",
                 message && message[0] ? message : (ok ? "Use Set boot to choose a boot partition, or Reboot to restart the board." : "Check serial log."));
        lv_label_set_text(s_ota_detail, text);
    }
    if (s_ota_layer) lv_obj_invalidate(s_ota_layer);
    ui_unlock();
    s_ota_active = false;
}

/*
 * 为重启做准备：显示黑色全屏覆盖层，强制刷新一帧后延迟 250ms。
 */
void board_ui_prepare_for_reboot(void)
{
    if (!s_ready) {
        return;
    }
    ui_lock();
    create_shutdown_layer_locked();
    if (s_shutdown_layer) {
        lv_obj_clear_flag(s_shutdown_layer, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(s_shutdown_layer, -1);
        lv_obj_invalidate(s_shutdown_layer);
    }
    if (s_disp) {
        lv_refr_now(s_disp);
    }
    ui_unlock();
    vTaskDelay(pdMS_TO_TICKS(250));
}

/*
 * 显示配置编辑模态框：加载 JSON 配置，构建带标签、文本框和虚拟键盘的表单 UI。
 */
void board_ui_show_config_editor(void)
{
    if (!s_ready || s_config_modal) return;
    cc_json_value_t *root = NULL;
    char *json_source = s_callbacks.load_config_json ? s_callbacks.load_config_json(s_callbacks.user_data) : NULL;
    cc_result_t rc = cc_json_parse(json_source ? json_source : "{}", &root);
    free(json_source);
    if (rc.code != CC_OK || !root) {
        ui_appendf(BOARD_CONV_ERROR, "open config failed: %s", rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        return;
    }
    cc_result_free(&rc);

    ui_lock();
    s_field_count = 0;
    s_config_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_config_modal, 760, 430);
    lv_obj_center(s_config_modal);
    lv_obj_set_style_bg_color(s_config_modal, lv_color_hex(0x121a22), 0);
    lv_obj_set_style_border_color(s_config_modal, lv_color_hex(0x405060), 0);
    lv_obj_set_style_radius(s_config_modal, 6, 0);
    lv_obj_set_flex_flow(s_config_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_config_modal, LV_DIR_VER);

    lv_obj_t *title = lv_label_create(s_config_modal);
    apply_font(title);
    lv_label_set_text(title, "Common Config");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);

    char num[32];
    snprintf(num, sizeof(num), "%d", board_ui_json_get_path_bool(root, "network.sta.enabled", true) ? 1 : 0);
    add_field(s_config_modal, "WiFi Enabled 0/1", "network.sta.enabled", num, false);
    add_field(s_config_modal, "WiFi SSID", "network.sta.ssid", board_ui_json_get_path_string(root, "network.sta.ssid"), false);
    add_field(s_config_modal, "WiFi Password Ref", "network.sta.password_ref",
              board_ui_json_get_path_string(root, "network.sta.password_ref"), false);
    add_field(s_config_modal, "Provider", "model.provider", board_ui_json_get_path_string(root, "model.provider"), false);
    add_field(s_config_modal, "Model", "model.name", board_ui_json_get_path_string(root, "model.name"), false);
    add_field(s_config_modal, "Base URL", "model.base_url", board_ui_json_get_path_string(root, "model.base_url"), false);
    add_field(s_config_modal, "API Key Ref", "model.api_key_ref",
              board_ui_json_get_path_string(root, "model.api_key_ref"), false);
    snprintf(num, sizeof(num), "%d", board_ui_json_get_path_int(root, "model.max_tokens", 512));
    add_field(s_config_modal, "Max Tokens", "model.max_tokens", num, false);
    snprintf(num, sizeof(num), "%.2f", cc_json_number_value(board_ui_json_get_path(root, "model.temperature")));
    add_field(s_config_modal, "Temperature", "model.temperature", num, false);
    snprintf(num, sizeof(num), "%d", board_ui_json_get_path_bool(root, "runtime.stream", true) ? 1 : 0);
    add_field(s_config_modal, "Stream 0/1", "runtime.stream", num, false);
    snprintf(num, sizeof(num), "%d", board_ui_json_get_path_bool(root, "runtime.thinking", true) ? 1 : 0);
    add_field(s_config_modal, "Thinking 0/1", "runtime.thinking", num, false);
    add_field(s_config_modal, "Font Path", "display.font_path", board_ui_json_get_path_string(root, "display.font_path"), false);
    add_field(s_config_modal, "Workspace", "workspace.root", board_ui_json_get_path_string(root, "workspace.root"), false);
    add_field(s_config_modal, "Sessions", "storage.sessions", board_ui_json_get_path_string(root, "storage.sessions"), false);
    add_field(s_config_modal, "Memory", "storage.memory", board_ui_json_get_path_string(root, "storage.memory"), false);
    add_field(s_config_modal, "Capture Path", "camera.default_save_path", board_ui_json_get_path_string(root, "camera.default_save_path"), false);
    snprintf(num, sizeof(num), "%d", board_ui_json_get_path_int(root, "camera.default_quality", 70));
    add_field(s_config_modal, "JPEG Quality", "camera.default_quality", num, false);

    lv_obj_t *row = lv_obj_create(s_config_modal);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 720, 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t *save = lv_button_create(row);
    lv_obj_set_size(save, 120, 38);
    lv_obj_add_event_cb(save, save_config_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    apply_font(save_label);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);

    lv_obj_t *close = lv_button_create(row);
    lv_obj_set_size(close, 120, 38);
    lv_obj_add_event_cb(close, close_config_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close);
    apply_font(close_label);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    s_keyboard = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(s_keyboard, 760, 150);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    ui_unlock();
    cc_json_destroy(root);
}
