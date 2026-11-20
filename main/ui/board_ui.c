#include "board_ui.h"
#include "board_lcd.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdatomic.h>
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
#include "esp_cache.h"
#include "board_preview_probe.h"
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
#define UI_PREVIEW_ROW_BYTES (UI_PREVIEW_W * 2U)
#define UI_PREVIEW_BYTES (s_preview_stride_bytes * UI_PREVIEW_H)
#ifndef UI_PREVIEW_BUFFER_COUNT
#define UI_PREVIEW_BUFFER_COUNT 5
#endif
#ifndef UI_PREVIEW_STRICT_PSRAM_FOR_RGB_LCD
#define UI_PREVIEW_STRICT_PSRAM_FOR_RGB_LCD 1
#endif
#define UI_CONFIG_FIELD_MAX 17
#define UI_BOOT_LOG_MAX_LINES 80U

/* 对话滚动历史同时受条数和文本字节限制，超额逐个淘汰最早标签。 */
#ifndef UI_CONV_MAX_MESSAGES
#define UI_CONV_MAX_MESSAGES 160U
#endif
#ifndef UI_CONV_MAX_BYTES
#define UI_CONV_MAX_BYTES (512U * 1024U)
#endif

#ifndef UI_CONV_BG_COLOR
#define UI_CONV_BG_COLOR 0x080c10
#endif

#ifndef UI_ROOT_BG_COLOR
#define UI_ROOT_BG_COLOR 0x0b1014
#endif

/* 流式正文先合并进缓冲，按刷新节奏更新标签并滚动，减少 RGB 局部刷新残影。 */
#ifndef UI_STREAM_SCROLL_INTERVAL_MS
#define UI_STREAM_SCROLL_INTERVAL_MS 50U
#endif

/* 工具独立绘制区位于预览和设置按钮下方，使用命令队列在 LVGL 任务执行。 */
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

/* 工具区使用 CJK 字体，按字形下沿增加留白，并以末项实际底部定位下一行。 */
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


/* 默认允许 RGB 和 SPI 源缓冲使用 PSRAM，RGB 的严格模式在分配失败时降级预览。
 * 面板绘制缓冲由 adapter 单独配置，源缓冲与面板 DMA 缓冲各有自己的生命周期。 */
#ifndef UI_PREVIEW_PSRAM_FOR_RGB_LCD
#define UI_PREVIEW_PSRAM_FOR_RGB_LCD 1
#endif

#ifndef UI_PREVIEW_PSRAM_FOR_SPI_LCD
#define UI_PREVIEW_PSRAM_FOR_SPI_LCD 1
#endif

/* 限制预览控件的帧拷贝频率，降低持续复制 115200 字节帧对摄像头核空闲时间的影响。 */
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

static const char *TAG = "board_ui";

static lv_display_t *s_disp;
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
static size_t s_preview_stride_bytes;
static board_ui_preview_stats_t s_preview_stats;
static board_preview_probe_t s_preview_probe[UI_PREVIEW_BUFFER_COUNT];
static uint32_t s_preview_frame_id[UI_PREVIEW_BUFFER_COUNT];
/* 下列采样历史只由单一摄像头生产者更新。 */
static bool s_preview_probe_valid;
static uint32_t s_preview_previous_rgb;
static uint8_t s_preview_front_index;
static uint8_t s_preview_write_index;
static volatile int s_preview_pending_index = -1;
static int s_preview_presenting_index = -1;
static portMUX_TYPE s_preview_mux = portMUX_INITIALIZER_UNLOCKED;

static lv_obj_t *s_last_msg_label;
static board_conversation_role_t s_last_role;
static lv_obj_t *s_stream_msg_label;
static board_conversation_role_t s_stream_role;
static bool s_stream_active;
static size_t s_conversation_text_bytes;
static char *s_stream_text;
static size_t s_stream_text_len;
static size_t s_stream_text_capacity;
static bool s_stream_text_dirty;
static bool s_conversation_layout_pending;
static board_ui_camera_stats_t s_camera_stats;
static atomic_bool s_ready;
static bool s_usb_mounted;
static bool s_config_loaded;
static bool s_wifi_connected;
static bool s_restart_required;
static bool s_ppa_accel_enabled;
static bool s_lcd_is_rgb;
static char s_wifi_detail[64];
static char s_provider[32];
static char s_model[64];

static bool s_preview_dst_sync_warned;
static TickType_t s_preview_last_update_tick;
static TickType_t s_last_stream_scroll_tick;
static TickType_t s_ota_last_update_tick;
static atomic_bool s_ota_active;
static QueueHandle_t s_tool_cmd_queue;

static board_ui_callbacks_t s_callbacks;

/* 刷新和空闲钩子使用原子模计数，UI 任务通过窗口差值估算 FPS 与各核负载。 */
static atomic_uint_least32_t s_screen_refresh_count;
static uint32_t s_screen_fps;
static uint32_t s_screen_refresh_last_count;
static TickType_t s_perf_last_update_tick;

#if UI_HAVE_ESP_FREERTOS_HOOKS && UI_ENABLE_CPU_LOAD_IDLE_HOOK
static atomic_uint_least32_t s_idle_count[SOC_CPU_CORES_NUM];
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

/**
 * @brief 初始化 owner 创建并取得工具命令队列。
 * @param[out] out_with_caps 必填来源标志槽，供候选失败时选择匹配的删除 API。
 * @return 队列可用为 true，全部分配失败为 false，调用者持 LVGL 锁且尚未发布 ready。
 */
static bool tool_cmd_queue_ensure(bool *out_with_caps);
/**
 * @brief LVGL 任务非阻塞消费一批工具命令。
 * @param[in] timer 同步定时器借用，字段未使用，可 NULL。
 * @post 已消费命令在 LVGL 锁内执行，每批至多处理队列深度条。
 */
static void tool_cmd_timer_cb(lv_timer_t *timer);

/**
 * @brief 阻塞取得 adapter 的递归 LVGL 锁，统一串行化对象树操作。
 * @post 正常 adapter 生命周期内本任务持有一层锁，调用者随后配对 ui_unlock。
 * @pre adapter 已初始化并在本次调用和临界区内持续有效，禁止与 adapter 销毁并发。
 */
static void ui_lock(void)
{
    esp_lv_adapter_lock(-1);
}

/**
 * @brief 归还本任务持有的一层 LVGL 锁。
 * @post 当前层递归锁释放，最外层归还后其他 UI 调用可进入。
 * @pre 本任务此前成功进入对应的 ui_lock 临界区。
 */
static void ui_unlock(void)
{
    esp_lv_adapter_unlock();
}

/**
 * @brief 将 RGB565 分量左移扩展为 LVGL 使用的 RGB888 颜色。
 * @param[in] c RGB565 按值颜色。
 * @return 0x00RRGGBB，低位补零，未作满量程位复制。
 */
static uint32_t rgb565_to_rgb888(uint16_t c)
{
    uint8_t r = (uint8_t)(((c >> 11) & 0x1f) << 3);
    uint8_t g = (uint8_t)(((c >> 5) & 0x3f) << 2);
    uint8_t b = (uint8_t)((c & 0x1f) << 3);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/**
 * @brief 按二进制千进位格式化 B、KB 或 MB，较大单位保留两位小数。
 * @param[in] bytes 原始字节数。
 * @param[out] out 输出区，非零容量时须有效。
 * @param[in] out_size 输出区字节容量，包含 NUL，可为零。
 * @post 遵循 snprintf 截断规则，非零容量有效输出保持 NUL 终止。
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

/**
 * @brief 分别读取指定能力堆的总量及空闲量，格式化近似使用情况。
 * @param[in] caps ESP heap_caps 能力位组合。
 * @param[out] out 输出区，非零容量时须有效。
 * @param[in] out_size 字节容量，包含 NUL，可为零。
 * @post 写入已用、总量及四舍五入百分比，分次读取产生的瞬时 free 大于 total 时将已用钳为零。
 */
static void format_heap_usage(uint32_t caps, char *out, size_t out_size)
{
    size_t total = heap_caps_get_total_size(caps);
    size_t free_size = heap_caps_get_free_size(caps);
    size_t used = total > free_size ? total - free_size : 0;
    unsigned used_pct = total ? (unsigned)((double)used * 100.0 / (double)total + 0.5) : 0;
    char used_text[16];
    char total_text[16];

    format_bytes(used, used_text, sizeof(used_text));
    format_bytes(total, total_text, sizeof(total_text));
    snprintf(out, out_size, "%s/%s %u%%", used_text, total_text, used_pct);
}

/**
 * @brief 选择角色对应的固定文字颜色。
 * @param[in] role 按值角色，未知值沿系统状态颜色。
 * @return LVGL RGB 颜色值，不涉及资源所有权。
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

/**
 * @brief 选择消息正文之前显示的角色标识。
 * @param[in] role 按值角色，未知值使用 STATUS。
 * @return 进程期只读字符串借用，无需释放。
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

/**
 * @brief 在对象默认样式中安装当前字体借用。
 * @param[in,out] obj LVGL 对象借用，NULL 跳过。
 * @post 对象样式借用当前字体，旧字体由外层整树重绑事务统一退休。
 * @pre 本任务持 LVGL 锁，对象在调用期有效。
 */
static void apply_font(lv_obj_t *obj)
{
    if (obj) {
        lv_obj_set_style_text_font(obj, board_ui_font_current(), 0);
    }
}

/**
 * @brief 深度优先遍历对象及全部孩子，重绑当前字体。
 * @param[in,out] obj 子树根借用，NULL 跳过。
 * @post 子树所有对象默认样式已更新，递归期间不转移对象所有权。
 * @pre 持 LVGL 锁排斥对象树变更，树深度由本模块创建的界面层级决定。
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

/**
 * @brief 为对话标签配置字体、自动换行、角色色及不透明背景。
 * @param[in,out] label 标签借用，NULL 跳过。
 * @param[in] role 角色颜色分类。
 * @post 标签占满父容器可用宽度并带固定内边距。
 * @pre 持 LVGL 锁且标签持续有效。
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

    /* 不透明背景覆盖旧字形，降低流式 RGB 局部刷新产生残影的机会。 */
    lv_obj_set_style_bg_color(label, lv_color_hex(UI_CONV_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(label, 2, 0);
    lv_obj_set_style_pad_bottom(label, 2, 0);
    lv_obj_set_style_pad_left(label, 2, 0);
    lv_obj_set_style_pad_right(label, 2, 0);
}

/**
 * @brief 删除对话覆盖层的全部绘制对象并请求重绘。
 * @post 覆盖层存在时孩子及其 LVGL 资源释放，未创建时跳过。
 * @pre 持 LVGL 锁，对象树由 UI owner 管理。
 */
static void conversation_overlay_clear_locked(void)
{
    if (!s_overlay) {
        return;
    }

    lv_obj_clean(s_overlay);
    lv_obj_invalidate(s_overlay);
}

/**
 * @brief 将整数钳位到包含端点的区间。
 * @param[in] v 待限制值。
 * @param[in] min_v 下界，调用者保证不大于 max_v。
 * @param[in] max_v 上界。
 * @return 区间内原值或相应边界值。
 */
static int clamp_int(int v, int min_v, int max_v)
{
    if (v < min_v) return min_v;
    if (v > max_v) return max_v;
    return v;
}

/**
 * @brief 删除工具显示区全部孩子并标记失效。
 * @post 已启用且已创建的工具区清空，未启用或未创建时跳过。
 * @pre 持 LVGL 锁。
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

/**
 * @brief 逐个淘汰工具区最早对象，直到满足对象数限制。
 * @post 工具区孩子数不超过配置上限，删除同时释放 LVGL 拥有资源。
 * @pre 持 LVGL 锁，工具区对象由本模块管理。
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

/**
 * @brief 为工具标签设置当前字体、角色色、换行及 CJK 留白。
 * @param[in,out] label 标签借用，NULL 跳过。
 * @param[in] role 决定文字颜色的角色。
 * @post 更新对象样式，字体借用随全树字体事务管理。
 * @pre 持 LVGL 锁，标签在调用期有效。
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

    /* 为 CJK 字形下缘留出上下边距和最小高度。 */
    lv_obj_set_style_pad_top(label, UI_TOOL_LABEL_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(label, UI_TOOL_LABEL_PAD_BOTTOM, 0);
    lv_obj_set_style_pad_left(label, UI_TOOL_LABEL_PAD_X, 0);
    lv_obj_set_style_pad_right(label, UI_TOOL_LABEL_PAD_X, 0);
    lv_obj_set_style_min_height(label, UI_TOOL_LABEL_MIN_H, 0);
    lv_obj_set_style_text_line_space(label, UI_TOOL_LABEL_LINE_SPACE, 0);
}

/**
 * @brief 完成末项布局后计算下一工具项的纵向位置。
 * @return 已有末项的底部加间距，空区为 2，工具区缺失或裁剪禁用为零。
 * @pre 持 LVGL 锁，工具容器及孩子由本模块独占修改。
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

    /* 保留滚动容器的完整纵向位置，随后由 scroll_to_view 显示新行。 */
    lv_obj_update_layout(last);
    return lv_obj_get_y(last) + lv_obj_get_height(last) + UI_TOOL_LABEL_ROW_GAP;
#else
    return 0;
#endif
}

/**
 * @brief 在对话标签销毁前归还本模块通过 static 文本接口交给标签借用的副本。
 * @param[in] event LV_EVENT_DELETE 的同步事件借用，target 为本模块创建的对话标签。
 * @post 标签 user_data 清空且文本副本归还，LVGL 随后按 static 标志跳过重复释放。
 * @pre LVGL 锁覆盖删除，标签不存在未完成的绘制借用。
 */
static void conversation_label_delete_cb(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_target(event);
    void *text = lv_obj_get_user_data(label);
    lv_obj_set_user_data(label, NULL);
    heap_caps_free(text);
}

/**
 * @brief 创建带角色样式和文本释放回调的空对话标签。
 * @param[in] role 角色颜色及后续正文前缀的分类值。
 * @return 由对话容器拥有的标签借用，原生创建或回调登记失败为 NULL。
 * @pre 持 LVGL 锁且对话容器有效，新标签空串的 NUL 纳入累计字节。
 */
static lv_obj_t *conversation_create_label_locked(board_conversation_role_t role)
{
    lv_obj_t *label = lv_label_create(s_conversation);
    if (label) {
        lv_obj_set_user_data(label, NULL);
        lv_label_set_text_static(label, "");
        if (!lv_obj_add_event_cb(label, conversation_label_delete_cb, LV_EVENT_DELETE, NULL)) {
            lv_obj_delete(label);
            return NULL;
        }
        s_conversation_text_bytes++;
        apply_message_label_style(label, role);
    }
    return label;
}

/**
 * @brief 先分配完整候选文本，再安装标签借用并退休旧副本。
 * @param[in,out] label 本模块创建且已计入累计字节的标签借用，不可 NULL。
 * @param[in] text 调用期 NUL 文本借用，可引用标签旧正文，NULL 作为空串。
 * @return 完整安装为 true，长度溢出或分配失败为 false，失败保持原文本和累计量。
 * @pre 持 LVGL 锁，只有本函数及 DELETE 回调管理标签 user_data 中的文本所有权。
 */
static bool conversation_set_text_locked(lv_obj_t *label, const char *text)
{
    const char *source = text ? text : "";
    size_t length = strlen(source);
    if (length == SIZE_MAX) return false;
    char *candidate = heap_caps_malloc(length + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!candidate) candidate = malloc(length + 1U);
    if (!candidate) return false;
    memcpy(candidate, source, length + 1U);
    void *previous = lv_obj_get_user_data(label);
    s_conversation_text_bytes -= strlen(lv_label_get_text(label)) + 1U;
    lv_label_set_text_static(label, candidate);
    lv_obj_set_user_data(label, candidate);
    s_conversation_text_bytes += length + 1U;
    heap_caps_free(previous);
    return true;
}

/**
 * @brief 按条数和文本字节预算逐个淘汰最早对话标签。
 * @post 删除前清除 last/stream 借用，文本及 NUL 的累计量同步扣除，字节裁剪至少保留一个标签。
 * @pre 持 LVGL 锁，所有对话孩子均由本模块创建且文本已计入累计量。
 */
static void conversation_prune_old_messages_locked(void)
{
    if (!s_conversation) {
        return;
    }

    while (lv_obj_get_child_count(s_conversation) > UI_CONV_MAX_MESSAGES ||
           (lv_obj_get_child_count(s_conversation) > 1U &&
            s_conversation_text_bytes > UI_CONV_MAX_BYTES)) {
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

        s_conversation_text_bytes -= strlen(lv_label_get_text(oldest)) + 1U;
        lv_obj_delete(oldest);
    }
}

/**
 * @brief 将片段复制进优先 PSRAM 的流式增长缓冲，最多保留预算内的字符前缀。
 * @param[in] text 调用期有效 NUL 文本借用，不可引用可能被本次 realloc 搬迁的拼接缓冲。
 * @return 分配和追加成功为 true，扩容失败为 false 并保留旧正文及容量。
 * @post 成功后标记 dirty，总长度含角色前缀和 NUL，超预算的有效 UTF-8 输入保持完整字符前缀。
 * @pre 持 LVGL 锁，当前长度小于 UI_CONV_MAX_BYTES，配置预算为正数。
 */
static bool conversation_buffer_append_locked(const char *text)
{
    size_t length = strlen(text);
    size_t available = UI_CONV_MAX_BYTES - 1U - s_stream_text_len;
    if (length > available) {
        length = available;
        while (length && ((unsigned char)text[length] & 0xc0U) == 0x80U) length--;
    }
    size_t needed = s_stream_text_len + length + 1U;
    if (needed > s_stream_text_capacity) {
        size_t capacity = s_stream_text_capacity ? s_stream_text_capacity : 256U;
        while (capacity < needed) {
            capacity = capacity > UI_CONV_MAX_BYTES / 2U ? UI_CONV_MAX_BYTES : capacity * 2U;
        }
        char *buffer = heap_caps_realloc(s_stream_text, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buffer) buffer = realloc(s_stream_text, capacity);
        if (!buffer) return false;
        s_stream_text = buffer;
        s_stream_text_capacity = capacity;
    }
    memcpy(s_stream_text + s_stream_text_len, text, length);
    s_stream_text_len += length;
    s_stream_text[s_stream_text_len] = '\0';
    s_stream_text_dirty = true;
    return true;
}

/**
 * @brief 将 dirty 正文事务安装到当前标签，再尽力更新显示快照。
 * @post 安装成功后清 dirty 并请求布局和历史裁剪，标签分配失败保留 dirty 和已显示正文。
 * @pre 持 LVGL 锁，stream 标签借用及拼接缓冲在调用期有效。
 */
static void conversation_flush_fragment_locked(void)
{
    if (!s_stream_text_dirty || !s_stream_msg_label) return;
    if (!conversation_set_text_locked(s_stream_msg_label, s_stream_text)) return;
    board_ui_snapshot_replace_last_conversation(s_stream_text);
    s_stream_text_dirty = false;
    s_conversation_layout_pending = true;
    s_last_stream_scroll_tick = xTaskGetTickCount();
    conversation_prune_old_messages_locked();
}

/**
 * @brief 尝试最后一次正文安装后结束合并流并归还拼接缓冲。
 * @post stream 借用及状态复位，标签仍归容器，最终安装失败时保留此前成功显示的正文。
 * @pre 持 LVGL 锁，调用者接受显示的最佳努力分配失败规则。
 */
static void conversation_finish_fragment_locked(void)
{
    conversation_flush_fragment_locked();
    free(s_stream_text);
    s_stream_text = NULL;
    s_stream_text_len = 0;
    s_stream_text_capacity = 0;
    s_stream_text_dirty = false;
    s_stream_msg_label = NULL;
    s_stream_active = false;
}

/**
 * @brief 消费一次待处理对话布局请求并滚动到末项。
 * @post 已创建的容器完成布局且 pending 清空，空容器不执行滚动。
 * @pre LVGL 任务持锁调用，通常位于同批显示事件处理末尾。
 */
static void conversation_apply_pending_layout_locked(void)
{
    if (!s_conversation_layout_pending || !s_conversation) return;
    s_conversation_layout_pending = false;
    lv_obj_update_layout(s_conversation);
    uint32_t count = lv_obj_get_child_count(s_conversation);
    if (count > 0) {
        lv_obj_t *last = lv_obj_get_child(s_conversation, (int32_t)(count - 1U));
        if (last) lv_obj_scroll_to_view(last, LV_ANIM_OFF);
    }
}


/**
 * @brief 对 LVGL 刷新就绪事件执行一次原子计数。
 * @param[in] event 同步事件借用，本回调不读取其字段。
 * @post 刷新模计数增加，用于窗口 FPS 估算，不代表面板像素的独立校验。
 */
static void display_refresh_event_cb(lv_event_t *event)
{
    (void)event;
    s_screen_refresh_count++;
}

#if UI_HAVE_ESP_FREERTOS_HOOKS && UI_ENABLE_CPU_LOAD_IDLE_HOOK

/**
 * @brief 在 CPU0 空闲钩子中增加轻量原子模计数。
 * @return true，允许当前空闲钩子链继续执行。
 * @post 计数供 UI 任务读取差值，不分配、不等待 UI 锁。
 */
static bool ui_idle_hook_cpu0(void)
{
    s_idle_count[0]++;
    return true;
}

#if SOC_CPU_CORES_NUM > 1
/**
 * @brief 在 CPU1 空闲钩子中增加轻量原子模计数。
 * @return true，允许当前空闲钩子链继续执行。
 * @post 计数供 UI 任务读取差值，不分配、不等待 UI 锁。
 */
static bool ui_idle_hook_cpu1(void)
{
    s_idle_count[1]++;
    return true;
}
#endif

/**
 * @brief 启动时按编译核数尽力注册空闲钩子，每进程尝试一次。
 * @post 注册失败记录日志，已安装钩子保留到进程结束，后续调用跳过重复注册。
 * @pre 单一 UI 初始化 owner 调用，不能与独立的钩子注销或重复初始化并发。
 */
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

/**
 * @brief 到达采样窗口后计算屏幕 FPS 和相对最高空闲基线的近似核负载。
 * @post 首次调用建立基线，后续按 tick 差更新 UI 私有指标，原子计数允许窗口间回绕。
 * @pre LVGL 任务串行调用，窗口间隔短于 tick 与 uint32_t 计数的完整回绕周期。
 */
static void ui_perf_update_metrics(void)
{
    /* 状态栏以 FreeRTOS tick 的毫秒窗口采样，避免引入额外定时器组件依赖。 */
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

        /* 用本次启动观察到的最高空闲循环数作为当前核的零负载基线。 */
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

/**
 * @brief 格式化最近采样的各核近似负载，未编译钩子时写入 n/a。
 * @param[out] out 输出区，非零容量时须有效。
 * @param[in] out_size 字节容量，含 NUL，可为零。
 * @post 按 snprintf 规则写入并截断，读取与指标更新由 LVGL 任务串行化。
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

/**
 * @brief 在 384 字节栈缓冲中格式化一条 UI 诊断消息。
 * @param[in] role 显示角色。
 * @param[in] fmt 调用期 printf 格式借用，NULL 忽略。
 * @param[in] ... 与格式匹配的参数及调用期字符串借用。
 * @post 负格式化结果丢弃，非负结果最多 383 字节后由对话接口复制。
 */
static void ui_appendf(board_conversation_role_t role, const char *fmt, ...)
{
    if (!fmt) return;
    char text[384];
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (written >= 0) board_ui_conversation_append(role, text);
}

/**
 * @brief 从短临界区取得一致的应用状态副本后更新状态栏与快照。
 * @param[in] timer LVGL 定时器借用，首次同步刷新可为 NULL，函数不读取该参数。
 * @post OTA 活跃时跳过，否则更新近似性能及状态显示，快照复制失败保持旧快照。
 * @pre LVGL 任务持锁调用，状态 setter 使用 preview_mux，不持配置事务等待 LVGL 锁。
 */
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
    board_ui_camera_stats_t camera;
    bool usb_mounted, config_loaded, wifi_connected, restart_required;
    char wifi_detail[sizeof(s_wifi_detail)];
    char provider[sizeof(s_provider)];
    char model[sizeof(s_model)];
    portENTER_CRITICAL(&s_preview_mux);
    camera = s_camera_stats;
    usb_mounted = s_usb_mounted;
    config_loaded = s_config_loaded;
    wifi_connected = s_wifi_connected;
    restart_required = s_restart_required;
    memcpy(wifi_detail, s_wifi_detail, sizeof(wifi_detail));
    memcpy(provider, s_provider, sizeof(provider));
    memcpy(model, s_model, sizeof(model));
    portEXIT_CRITICAL(&s_preview_mux);
    format_heap_usage(MALLOC_CAP_8BIT, heap_text, sizeof(heap_text));
    format_heap_usage(MALLOC_CAP_INTERNAL, int_text, sizeof(int_text));
    format_heap_usage(MALLOC_CAP_SPIRAM, psram_text, sizeof(psram_text));
    format_bytes(esp_get_minimum_free_heap_size(), min_text, sizeof(min_text));
    format_cpu_usage(cpu_text, sizeof(cpu_text));

    web_text[0] = '\0';
    snprintf(wifi_text,
             sizeof(wifi_text),
             "%s",
             wifi_connected ? (wifi_detail[0] ? wifi_detail : "connected") : "disabled");

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
             s_screen_fps, cpu_text, camera.preview_fps, camera.preview_drops,
             usb_mounted ? "mounted" : "unmounted",
             wifi_text,
             web_text[0] ? web_text : "n/a",
             config_loaded ? "loaded" : "waiting",
             restart_required ? "yes" : "no",
             provider, model,
             s_lcd_is_rgb ? 1 : 0, SOC_GDMA_SUPPORTED, SOC_DMA2D_SUPPORTED,
             s_ppa_accel_enabled ? 1 : 0, SOC_JPEG_CODEC_SUPPORTED);
    lv_label_set_text(s_status_label, text);
    board_ui_snapshot_set_status(text);
}

/**
 * @brief 创建全宽单行滚动状态栏并登记三秒刷新定时器。
 * @param[in,out] parent 有效 UI 父容器借用，拥有新建孩子。
 * @param[out] out_timer 必填输出槽，入口清空，成功后交付新定时器供初始化 owner 管理失败回收。
 * @return 所有可检查的对象和定时器创建成功为 true，否则 false，已建孩子留给父树回收。
 * @pre 初始化 owner 持 LVGL 锁。
 */
static bool create_status_bar(lv_obj_t *parent, lv_timer_t **out_timer)
{
    *out_timer = NULL;
    lv_obj_t *bar = lv_obj_create(parent);
    if (!bar) return false;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, UI_LCD_WIDTH, UI_STATUS_H);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_ROOT_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    s_status_label = lv_label_create(bar);
    if (!s_status_label) return false;
    apply_font(s_status_label);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_duration(s_status_label, UI_STATUS_SCROLL_MS, 0);
    lv_obj_set_size(s_status_label, UI_LCD_WIDTH - 2 * UI_PANEL_PAD, UI_STATUS_H - 4);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, UI_PANEL_PAD, 2);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xc8d0d8), 0);
    lv_obj_set_style_bg_color(s_status_label, lv_color_hex(UI_ROOT_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_status_label, LV_OPA_COVER, 0);
    status_timer_cb(NULL);
    *out_timer = lv_timer_create(status_timer_cb, UI_STATUS_REFRESH_MS, NULL);
    return *out_timer != NULL;
}

/**
 * @brief 分配当前行跨度对应的对齐预览缓冲，RGB 严格 PSRAM 模式禁用内部回退。
 * @param[in] allow_psram 是否先尝试 PSRAM。
 * @param[in] name 非 NULL 调用期诊断名称借用。
 * @return 拥有的 UI_PREVIEW_BYTES 字节缓冲，heap_caps_free 归还，所有允许的分配路径失败为 NULL。
 * @pre 初始化阶段已确定面板类型及行跨度，尚未向摄像头生产者发布 ready。
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

        /* SPI 或允许回退的配置使用内部能力堆，RGB 严格 PSRAM 配置已在上方返回。 */
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

/**
 * @brief 将一项预览累计统计饱和增加，避免长时间运行后回绕。
 * @param[in,out] counter 有效 uint32_t 统计槽，UINT32_MAX 时保持上限。
 * @post 未达上限时增加一。
 * @pre 调用任务持 preview_mux，窗口 FPS 与空闲模计数使用独立原子字段。
 */
static void preview_counter_increment(uint32_t *counter)
{
    if (*counter != UINT32_MAX) (*counter)++;
}

/**
 * @brief 保留 pending 与旧 front 缓冲，采样后将新描述符交给 LVGL。
 * @param[in] timer 同步 LVGL 定时器借用，参数字段未使用，可 NULL。
 * @post 更新 front 后清 presenting，生产者可复用退休源，统计由 preview_mux 保护。
 * @pre LVGL 锁覆盖调用，前一轮绘制任务已由 LVGL 刷新流程收尾，面板 DMA 使用显示绘制缓冲。
 */
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
        /* 切换描述符时同时保留旧 front 与当前源，生产者仍可提交其他 pending。 */
        s_preview_presenting_index = pending;
    } else {
        pending = -1;
    }
    portEXIT_CRITICAL(&s_preview_mux);

    if (pending < 0) {
        return;
    }

    /* presenting_index 保留源，使采样可在短临界区之外完成。 */
    board_preview_probe_t observed = board_preview_probe(s_preview_buf[pending], s_preview_stride_bytes);
    portENTER_CRITICAL(&s_preview_mux);
    preview_counter_increment(&s_preview_stats.integrity_checks);
    if (observed.hash != s_preview_probe[pending].hash) {
        preview_counter_increment(&s_preview_stats.integrity_mismatches);
        s_preview_stats.last_mismatch_frame = s_preview_frame_id[pending];
    }
    portEXIT_CRITICAL(&s_preview_mux);
    lv_image_set_src(s_preview, &s_preview_dsc[pending]);
    portENTER_CRITICAL(&s_preview_mux);
    s_preview_front_index = (uint8_t)pending;
    preview_counter_increment(&s_preview_stats.source_updates);
    if (s_preview_pending_index == pending) s_preview_pending_index = -1;
    s_preview_presenting_index = -1;
    portEXIT_CRITICAL(&s_preview_mux);
    lv_obj_invalidate(s_preview);
}

/**
 * @brief 泵送一批应用显示事件，再完成到期流式刷新和合并布局。
 * @param[in] timer 同步 LVGL 定时器借用，参数字段未使用，可 NULL。
 * @post 已消费事件按显示接收策略释放，布局最多在批末提交一次。
 * @pre LVGL 任务持锁，递归调用的 board_ui 接口使用同一递归锁。
 */
static void event_display_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    app_event_display_pump();
    if (s_stream_text_dirty &&
        (xTaskGetTickCount() - s_last_stream_scroll_tick) >= pdMS_TO_TICKS(UI_STREAM_SCROLL_INTERVAL_MS)) {
        conversation_flush_fragment_locked();
    }
    conversation_apply_pending_layout_locked();
}

/**
 * @brief 创建 240×240 预览及配置数量的 CPU 源缓冲，缓冲不足时降级为提示标签。
 * @param[in,out] parent 有效父容器，拥有创建的图像或降级标签。
 * @param[out] out_timer 必填输出槽，入口清空，仅可用图像分支交付预览定时器。
 * @return 图像及定时器完整创建或降级提示可用为 true，可检查的控件或定时器分配失败为 false。
 * @pre 初始化 owner 持 LVGL 锁，ready 尚未发布，失败时外层清理已建立缓冲和父树。
 */
static bool create_preview(lv_obj_t *parent, lv_timer_t **out_timer)
{
    *out_timer = NULL;
    bool allow_psram = (!s_lcd_is_rgb && UI_PREVIEW_PSRAM_FOR_SPI_LCD) ||
                       (s_lcd_is_rgb && UI_PREVIEW_PSRAM_FOR_RGB_LCD);

    /* 使用原生 stride 规则，当前 480 字节有效行按 64 字节对齐为 512 字节，避免绘制时反复重排。 */
    s_preview_stride_bytes = lv_draw_buf_width_to_stride(UI_PREVIEW_W, LV_COLOR_FORMAT_RGB565);
    if (s_preview_stride_bytes < UI_PREVIEW_ROW_BYTES || s_preview_stride_bytes > UINT16_MAX) {
        ESP_LOGE(TAG, "invalid preview stride: %u", (unsigned)s_preview_stride_bytes);
        s_preview = lv_label_create(parent);
        if (!s_preview) return false;
        apply_font(s_preview);
        lv_label_set_text(s_preview, "Preview disabled: invalid stride");
        return true;
    }

    s_preview_front_index = 0;
    s_preview_write_index = UI_PREVIEW_BUFFER_COUNT > 1 ? 1 : 0;
    s_preview_pending_index = -1;
    s_preview_presenting_index = -1;

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
            if (!s_preview) return false;
            apply_font(s_preview);
            lv_label_set_text(s_preview, "Preview disabled");
            lv_obj_align(s_preview, LV_ALIGN_TOP_LEFT, UI_PANEL_PAD, UI_STATUS_H + UI_PANEL_PAD);
            return true;
        }

        memset(&s_preview_dsc[i], 0, sizeof(s_preview_dsc[i]));
        s_preview_dsc[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        s_preview_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_preview_dsc[i].header.w = UI_PREVIEW_W;
        s_preview_dsc[i].header.h = UI_PREVIEW_H;
        s_preview_dsc[i].header.stride = (uint16_t)s_preview_stride_bytes;
        s_preview_dsc[i].data_size = UI_PREVIEW_BYTES;
        s_preview_dsc[i].data = s_preview_buf[i];
    }

    ESP_LOGI(TAG, "preview layout-v2 input_row=%u stride=%u bytes=%u buffers=%u",
             (unsigned)UI_PREVIEW_ROW_BYTES, (unsigned)s_preview_stride_bytes,
             (unsigned)UI_PREVIEW_BYTES, (unsigned)UI_PREVIEW_BUFFER_COUNT);

    s_preview = lv_image_create(parent);
    if (!s_preview) return false;
    lv_image_set_src(s_preview, &s_preview_dsc[s_preview_front_index]);
    lv_obj_set_size(s_preview, UI_PREVIEW_W, UI_PREVIEW_H);
    lv_obj_align(s_preview, LV_ALIGN_TOP_LEFT, UI_PANEL_PAD, UI_STATUS_H + UI_PANEL_PAD);
    *out_timer = lv_timer_create(preview_timer_cb, UI_PREVIEW_TIMER_MS, NULL);
    return *out_timer != NULL;
}

/**
 * @brief 创建带纵向 flex 布局和滚动条的对话容器。
 * @param[in,out] parent 有效父对象，拥有新建容器。
 * @return 容器创建成功为 true，原生创建返回 NULL 为 false。
 * @pre 初始化 owner 持 LVGL 锁，失败由外层回收整棵候选树。
 */
static bool create_conversation(lv_obj_t *parent)
{
    s_conversation = lv_obj_create(parent);
    if (!s_conversation) return false;
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
    return true;
}

/**
 * @brief 创建位于对话面板上方的透明绘制覆盖层。
 * @param[in,out] parent 有效父对象，拥有新建覆盖层。
 * @return 原生创建成功为 true，NULL 为 false。
 * @pre 初始化 owner 持 LVGL 锁，失败由外层回收候选树。
 */
static bool create_overlay(lv_obj_t *parent)
{
    s_overlay = lv_obj_create(parent);
    if (!s_overlay) return false;
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, UI_LCD_WIDTH - UI_CONV_X - UI_PANEL_PAD,
                    UI_LCD_HEIGHT - UI_STATUS_H - UI_PANEL_PAD * 2);
    lv_obj_align(s_overlay, LV_ALIGN_TOP_LEFT, UI_CONV_X, UI_STATUS_H + UI_PANEL_PAD);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    return true;
}

/**
 * @brief 创建预览下方的工具面板、标题和独立滚动画布。
 * @param[in,out] parent 有效父对象，拥有全部新建孩子。
 * @return 可检查的创建均成功或工具区编译禁用为 true，原生 NULL 返回为 false。
 * @pre 初始化 owner 持 LVGL 锁，部分孩子由外层父树统一回收。
 */
static bool create_tool_area(lv_obj_t *parent)
{
#if UI_TOOL_AREA_ENABLED
    s_tool_panel = lv_obj_create(parent);
    if (!s_tool_panel) return false;
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
    if (!title) return false;
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
    if (!s_tool_area) return false;
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
    return true;
}

/**
 * @brief 在顶层创建启动覆盖层及可滚动日志容器。
 * @return 所有可检查对象创建成功为 true，NULL 返回为 false。
 * @pre 初始化 owner 持 LVGL 锁，失败时外层单独删除这棵顶层候选树。
 */
static bool create_boot_log_layer(void)
{
    s_boot_layer = lv_obj_create(lv_layer_top());
    if (!s_boot_layer) return false;
    lv_obj_remove_style_all(s_boot_layer);
    lv_obj_set_size(s_boot_layer, UI_LCD_WIDTH, UI_LCD_HEIGHT);
    lv_obj_set_style_bg_color(s_boot_layer, lv_color_hex(0x05080c), 0);
    lv_obj_set_style_bg_opa(s_boot_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_boot_layer, 12, 0);
    lv_obj_set_style_pad_row(s_boot_layer, 8, 0);
    lv_obj_set_flex_flow(s_boot_layer, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *title = lv_label_create(s_boot_layer);
    if (!title) return false;
    apply_font(title);
    lv_label_set_text(title, "C-Claw Boot Log");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe8eef4), 0);

    s_boot_log = lv_obj_create(s_boot_layer);
    if (!s_boot_log) return false;
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
    return true;
}

/**
 * @brief 尝试创建默认隐藏的 OTA 顶层树，存在完整树时复用。
 * @post 创建失败删除本轮候选并清空所有 OTA 控件借用，允许后续调用重试。
 * @pre 持 LVGL 锁，顶层对象由 UI 进程期生命周期管理。
 */
static void create_ota_layer_locked(void)
{
    if (s_ota_layer) {
        return;
    }

    s_ota_layer = lv_obj_create(lv_layer_top());
    if (!s_ota_layer) return;
    lv_obj_remove_style_all(s_ota_layer);
    lv_obj_set_size(s_ota_layer, UI_LCD_WIDTH, UI_LCD_HEIGHT);
    lv_obj_set_style_bg_color(s_ota_layer, lv_color_hex(0x05080c), 0);
    lv_obj_set_style_bg_opa(s_ota_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_ota_layer, 32, 0);
    lv_obj_set_flex_flow(s_ota_layer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ota_layer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(s_ota_layer, LV_OBJ_FLAG_HIDDEN);

    s_ota_title = lv_label_create(s_ota_layer);
    if (!s_ota_title) goto fail;
    apply_font(s_ota_title);
    lv_label_set_text(s_ota_title, "OTA update");
    lv_obj_set_style_text_color(s_ota_title, lv_color_hex(0xe8eef4), 0);

    s_ota_bar = lv_bar_create(s_ota_layer);
    if (!s_ota_bar) goto fail;
    lv_obj_set_size(s_ota_bar, UI_LCD_WIDTH - 160, 28);
    lv_bar_set_range(s_ota_bar, 0, 100);
    lv_bar_set_value(s_ota_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x203040), 0);
    lv_obj_set_style_bg_color(s_ota_bar, lv_color_hex(0x56c878), LV_PART_INDICATOR);

    s_ota_detail = lv_label_create(s_ota_layer);
    if (!s_ota_detail) goto fail;
    apply_font(s_ota_detail);
    lv_label_set_long_mode(s_ota_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ota_detail, UI_LCD_WIDTH - 120);
    lv_obj_set_style_text_align(s_ota_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ota_detail, lv_color_hex(0xaab6c0), 0);
    lv_label_set_text(s_ota_detail, "Waiting");
    return;
fail:
    lv_obj_delete(s_ota_layer);
    s_ota_layer = NULL;
    s_ota_title = NULL;
    s_ota_bar = NULL;
    s_ota_detail = NULL;
}

/**
 * @brief 尽力创建默认隐藏的全屏黑色关机覆盖层，已存在时复用。
 * @post 创建成功后由顶层拥有对象，原生返回 NULL 时保留空借用供后续重试。
 * @pre 持 LVGL 锁。
 */
static void create_shutdown_layer_locked(void)
{
    if (s_shutdown_layer) {
        return;
    }

    s_shutdown_layer = lv_obj_create(lv_layer_top());
    if (!s_shutdown_layer) return;
    lv_obj_remove_style_all(s_shutdown_layer);
    lv_obj_set_size(s_shutdown_layer, UI_LCD_WIDTH, UI_LCD_HEIGHT);
    lv_obj_set_style_bg_color(s_shutdown_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_shutdown_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_shutdown_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_shutdown_layer, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 尽力创建 OTA 树并更新进度值及格式化大小信息。
 * @param[in] bytes_written 已写入字节数。
 * @param[in] expected_size 预期字节数，零表示未知并显示零百分比。
 * @param[in] prefix 调用期进度前缀借用，NULL 使用 Writing。
 * @post 进度钳至 0 到 100，创建失败时跳过对象访问。
 * @pre 持 LVGL 锁，所有对象借用归本模块管理。
 */
static void ota_progress_update_locked(size_t bytes_written, size_t expected_size, const char *prefix)
{
    create_ota_layer_locked();
    if (!s_ota_layer) {
        return;
    }

    unsigned pct = 0;
    if (expected_size && bytes_written >= expected_size) {
        pct = 100;
    } else if (expected_size) {
        /* 逐次累加模余数，保持 floor(100 * written / expected) 且避免 size_t 乘法溢出。 */
        size_t remainder = 0;
        size_t gap = expected_size - bytes_written;
        for (unsigned i = 0; i < 100; i++) {
            if (remainder >= gap) {
                remainder -= gap;
                pct++;
            } else {
                remainder += bytes_written;
            }
        }
    }
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

/**
 * @brief 将已创建键盘绑定到获得焦点的文本框并显示键盘。
 * @param[in] event 同步焦点事件借用，target 为本模块表单文本框。
 * @post 键盘不存在时跳过，否则更新目标借用，目标随模态框一起退场。
 * @pre LVGL 任务持锁执行事件回调。
 */
static void textarea_focus_cb(lv_event_t *event)
{
    if (!s_keyboard) return;
    lv_keyboard_set_textarea(s_keyboard, lv_event_get_target(event));
    lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 创建一个标题及文本框，完整登记后才增加表单字段数。
 * @param[in,out] parent 候选模态框借用，拥有创建的孩子。
 * @param[in] title 调用期非 NULL 标题借用，由 LVGL 复制。
 * @param[in] path 进程期 JSON 路径常量借用，字段表保留到模态框关闭。
 * @param[in] value 调用期初始文本借用，NULL 作为空串。
 * @param[in] secret 是否启用密码显示模式。
 * @post 可检查创建或回调登记失败时不登记字段，外层检查总数并删除整个不完整候选表单。
 * @pre 持 LVGL 锁，表单总字段数与 UI_CONFIG_FIELD_MAX 一致。
 */
static void add_field(lv_obj_t *parent, const char *title, const char *path, const char *value, bool secret)
{
    if (s_field_count >= UI_CONFIG_FIELD_MAX) return;
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return;
    apply_font(label);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0xaab4be), 0);

    lv_obj_t *ta = lv_textarea_create(parent);
    if (!ta) return;
    apply_font(ta);
    lv_obj_set_width(ta, 360);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_text(ta, value ? value : "");
    if (secret) lv_textarea_set_password_mode(ta, true);
    if (!lv_obj_add_event_cb(ta, textarea_focus_cb, LV_EVENT_FOCUSED, NULL)) return;

    if (s_field_count < UI_CONFIG_FIELD_MAX) {
        s_fields[s_field_count++] = (config_field_t){path, ta};
    }
}

/**
 * @brief 匹配表单支持的布尔配置路径。
 * @param[in] path 有效 NUL 路径借用。
 * @return 命中既有布尔字段列表为 true，否则 false，比较区分大小写。
 */
static bool is_bool_path(const char *path)
{
    return strcmp(path, "network.sta.enabled") == 0 ||
           strcmp(path, "network.ap.enabled") == 0 ||
           strcmp(path, "network.mdns.enabled") == 0 ||
           strcmp(path, "runtime.stream") == 0 ||
           strcmp(path, "runtime.thinking") == 0 ||
           strcmp(path, "runtime.web_enabled") == 0;
}

/**
 * @brief 匹配表单支持的整数配置路径。
 * @param[in] path 有效 NUL 路径借用。
 * @return 命中既有整数字段列表为 true，否则 false，比较区分大小写。
 */
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

/**
 * @brief 按表单既有约定识别四种真值字符串。
 * @param[in] text 调用期 NUL 借用，可 NULL。
 * @return 精确等于 1、true、on、yes 为 true，其余为 false。
 */
static bool text_is_true(const char *text)
{
    return text && (strcmp(text, "1") == 0 || strcmp(text, "true") == 0 || strcmp(text, "on") == 0 || strcmp(text, "yes") == 0);
}

/**
 * @brief 在独立配置树上收集完整表单，全部字段安装及序列化成功后同步保存。
 * @param[in] event 同步点击事件借用，函数不读取字段，可 NULL。
 * @post 任一解析、数值范围、路径安装或分配错误撤销候选，不调用保存；结果和树及字符串所有分支回收。
 * @pre LVGL 锁覆盖回调组读取及同步调用，外部回调上下文由 detach 退场屏障保护。
 * @note 数字保留既有前缀解析语义，整数显式校验 int 范围，温度拒绝非有限及转换范围错误。
 */
static void save_config_cb(lv_event_t *event)
{
    (void)event;
    cc_json_value_t *root = NULL;
    char *json_source = s_callbacks.load_config_json ? s_callbacks.load_config_json(s_callbacks.user_data) : NULL;
    cc_result_t rc = cc_json_parse(json_source ? json_source : "{}", &root);
    free(json_source);
    if (rc.code != CC_OK || !root || !cc_json_is_object(root)) {
        ui_appendf(BOARD_CONV_ERROR, "config open failed: %s", rc.code == CC_OK ? "object expected" :
                   (rc.message ? rc.message : cc_error_string(rc.code)));
        cc_result_free(&rc);
        cc_json_destroy(root);
        return;
    }
    cc_result_free(&rc);

    for (size_t i = 0; i < s_field_count; i++) {
        const char *path = s_fields[i].path;
        const char *text = lv_textarea_get_text(s_fields[i].textarea);
        bool installed;
        if (is_int_path(path)) {
            errno = 0;
            long value = strtol(text ? text : "0", NULL, 10);
            installed = errno != ERANGE && value >= INT_MIN && value <= INT_MAX &&
                        board_ui_json_set_path_number(root, path, (double)value);
        } else if (strcmp(path, "model.temperature") == 0) {
            errno = 0;
            double value = strtod(text ? text : "0", NULL);
            installed = errno != ERANGE && isfinite(value) &&
                        board_ui_json_set_path_number(root, path, value);
        } else if (is_bool_path(path)) {
            installed = board_ui_json_set_path_bool(root, path, text_is_true(text));
        } else {
            installed = board_ui_json_set_path_string(root, path, text);
        }
        if (!installed) {
            cc_json_destroy(root);
            board_ui_conversation_append(BOARD_CONV_ERROR, "config field update failed: invalid value, path or OOM");
            return;
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
/**
 * @brief 关闭配置编辑器并归还键盘和模态框子树。
 * @param[in] event 同步点击事件借用，未读取，可 NULL 以回收构造失败的候选。
 * @post 键盘、模态框及字段计数清空，表单路径为常量借用无需释放。
 * @pre 持 LVGL 锁，不能与另一条表单构造或关闭路径并发。
 */
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

/**
 * @brief 从 Settings 点击事件进入配置编辑器。
 * @param[in] event 同步点击事件借用，字段未使用，可 NULL。
 * @post 配置编辑器自行处理已打开、解析和分配失败，递归使用同一 LVGL 锁。
 */
static void settings_btn_cb(lv_event_t *event)
{
    (void)event;
    board_ui_show_config_editor();
}

/**
 * @brief 在预览下方创建 Settings 按钮并登记点击回调。
 * @param[in,out] parent 候选 UI 父容器借用，拥有按钮和标签。
 * @return 按钮、标签及回调登记均成功为 true，可检查的分配失败为 false。
 * @pre 初始化 owner 持 LVGL 锁，部分创建由外层父树统一回收。
 */
static bool create_settings_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);
    if (!btn) return false;
    lv_obj_set_size(btn, 96, 38);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, UI_PANEL_PAD, UI_STATUS_H + UI_PANEL_PAD + UI_PREVIEW_H + 8);
    if (!lv_obj_add_event_cb(btn, settings_btn_cb, LV_EVENT_CLICKED, NULL)) return false;

    lv_obj_t *label = lv_label_create(btn);
    if (!label) return false;
    apply_font(label);
    lv_label_set_text(label, "Settings");
    lv_obj_center(label);
    return true;
}

/**
 * @brief 启动前按值复制配置回调组及其借用上下文。
 * @param[in] callbacks 调用期结构借用，NULL 清空回调组。
 * @post 不保留结构地址，user_data 继续由外层拥有并覆盖回调使用期。
 * @pre 初始化 owner 在 UI 启动前串行调用，运行后通过 detach 结束借用。
 */
void board_ui_set_callbacks(const board_ui_callbacks_t *callbacks)
{
    if (callbacks) {
        s_callbacks = *callbacks;
    } else {
        memset(&s_callbacks, 0, sizeof(s_callbacks));
    }
}

/**
 * @brief 取得 LVGL 锁等待所有已进入的配置回调返回，再清空回调组。
 * @param[in] timeout_ms 锁等待毫秒数，零立即尝试，超过 INT32_MAX 按该上限等待。
 * @return 清空成功为 ESP_OK，锁失败透传并保持回调及上下文借用。
 * @pre owner 关闭新入口并排斥初始化，回调内不得提前释放其仍在使用的 user_data。
 * @post 成功后旧上下文可由 owner 释放，已有 UI 对象继续存在。
 */
esp_err_t board_ui_detach_callbacks(uint32_t timeout_ms)
{
    if (!s_ready) {
        memset(&s_callbacks, 0, sizeof(s_callbacks));
        return ESP_OK;
    }
    int32_t wait_ms = timeout_ms > INT32_MAX ? INT32_MAX : (int32_t)timeout_ms;
    esp_err_t err = esp_lv_adapter_lock(wait_ms);
    if (err != ESP_OK) return err;
    memset(&s_callbacks, 0, sizeof(s_callbacks));
    esp_lv_adapter_unlock();
    return ESP_OK;
}

/**
 * @brief 建立板级显示及完整候选 UI，成功后原子发布 ready。
 * @return 完整可用为 ESP_OK，快照锁或可检查的控件、队列、定时器 OOM 为 NO_MEM，板级错误透传。
 * @pre 单一启动 owner 串行调用，BSP 与 adapter 在 UI 进程期内持续有效。
 * @note 可检查的 UI 构造失败会回收树、源缓冲、队列及定时器，已成功初始化的板级 display 保留供重试。
 * @post 成功后队列、对象及预览布局只通过各自锁域访问，预览缓冲 OOM 允许提示式降级。
 */
esp_err_t board_ui_init(void)
{
    if (!board_ui_snapshot_init()) return ESP_ERR_NO_MEM;

    if (s_ready) return ESP_OK;

    if (!s_disp) {
        board_lcd_t lcd;
        esp_err_t ret = board_lcd_init(&lcd);
        if (ret != ESP_OK) return ret;
        s_disp = lcd.display;
        s_lcd_is_rgb = lcd.is_rgb;
        s_ppa_accel_enabled = lcd.ppa_accel_enabled;
    }

    lv_timer_t *status_timer = NULL;
    lv_timer_t *preview_timer = NULL;
    lv_timer_t *tool_timer = NULL;
    lv_timer_t *event_timer = NULL;
    bool queue_with_caps = false;
    ui_lock();
    s_root = lv_obj_create(lv_screen_active());
    if (!s_root) goto fail;
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, UI_LCD_WIDTH, UI_LCD_HEIGHT);
    lv_obj_set_style_bg_color(s_root, lv_color_hex(UI_ROOT_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    if (!create_status_bar(s_root, &status_timer) ||
        !create_preview(s_root, &preview_timer) ||
        !create_settings_button(s_root) || !create_tool_area(s_root) ||
        !create_conversation(s_root) || !create_overlay(s_root) ||
        !create_boot_log_layer() || !tool_cmd_queue_ensure(&queue_with_caps)) goto fail;
    tool_timer = lv_timer_create(tool_cmd_timer_cb, UI_TOOL_CMD_TIMER_MS, NULL);
    if (!tool_timer) goto fail;
    event_timer = lv_timer_create(event_display_timer_cb, UI_EVENT_DISPLAY_TIMER_MS, NULL);
    if (!event_timer) goto fail;
    lv_display_add_event_cb(s_disp, display_refresh_event_cb, LV_EVENT_REFR_READY, NULL);
#if UI_HAVE_ESP_FREERTOS_HOOKS && UI_ENABLE_CPU_LOAD_IDLE_HOOK
    ui_perf_register_idle_hooks();
#endif

    s_ready = true;
    ui_unlock();
    return ESP_OK;
fail:
    if (event_timer) lv_timer_delete(event_timer);
    if (tool_timer) lv_timer_delete(tool_timer);
    if (preview_timer) lv_timer_delete(preview_timer);
    if (status_timer) lv_timer_delete(status_timer);
    if (s_tool_cmd_queue) {
#if configSUPPORT_STATIC_ALLOCATION
        if (queue_with_caps) vQueueDeleteWithCaps(s_tool_cmd_queue);
        else
#endif
        vQueueDelete(s_tool_cmd_queue);
        s_tool_cmd_queue = NULL;
    }
    if (s_boot_layer) lv_obj_delete(s_boot_layer);
    if (s_root) lv_obj_delete(s_root);
    s_root = s_status_label = s_preview = s_conversation = s_overlay = NULL;
    s_tool_panel = s_tool_area = s_boot_layer = s_boot_log = NULL;
    for (size_t i = 0; i < UI_PREVIEW_BUFFER_COUNT; i++) {
        heap_caps_free(s_preview_buf[i]);
        s_preview_buf[i] = NULL;
    }
    ui_unlock();
    return ESP_ERR_NO_MEM;
}

/**
 * @brief 原子查询完整 UI 的发布状态。
 * @return true 表示初始化已提交，false 表示尚未完成或最近构造失败。
 */
bool board_ui_ready(void)
{
    return s_ready;
}

/**
 * @brief 持 LVGL 锁结束流并清空对话、覆盖层和工具显示及其快照。
 * @post 对话拥有文本及相关对象释放，索引和字节统计复位，未就绪时跳过。
 */
void board_ui_conversation_clear(void)
{
    if (!s_ready) return;

    ui_lock();

    conversation_finish_fragment_locked();
    board_ui_snapshot_clear_conversation();
    board_ui_snapshot_clear_tool();
    board_ui_snapshot_clear_draw();
    lv_obj_clean(s_conversation);
    s_conversation_text_bytes = 0;
    conversation_overlay_clear_locked();
    tool_area_clear_locked();
    s_last_msg_label = NULL;
    s_stream_msg_label = NULL;
    s_stream_active = false;
    s_last_stream_scroll_tick = 0;

    lv_obj_invalidate(s_conversation);
    s_conversation_layout_pending = false;

    ui_unlock();
}

/**
 * @brief 结束旧流后创建一条独立角色消息并按预算淘汰旧标签。
 * @param[in] role 按值角色，决定颜色和前缀。
 * @param[in] text 调用期 NUL 正文借用，NULL 作为空串。
 * @post 最多保留 511 字节的显示行，候选文本失败会删除新空标签，快照尽力复制。
 * @note LVGL 锁覆盖完整更新，调用结束不持有输入字符串借用。
 */
void board_ui_conversation_append(board_conversation_role_t role, const char *text)
{
    if (!s_ready) return;

    ui_lock();
    conversation_finish_fragment_locked();

    lv_obj_t *label = conversation_create_label_locked(role);
    if (!label) { ui_unlock(); return; }

    char line[512];
    snprintf(line, sizeof(line), "%s: %s", role_prefix(role), text ? text : "");
    if (!conversation_set_text_locked(label, line)) {
        s_conversation_text_bytes--;
        lv_obj_delete(label);
        ui_unlock();
        return;
    }
    board_ui_snapshot_append_conversation(line);

    s_last_role = role;
    s_last_msg_label = label;
    s_last_stream_scroll_tick = xTaskGetTickCount();

    conversation_prune_old_messages_locked();

    s_conversation_layout_pending = true;

    ui_unlock();
}

/**
 * @brief 将同角色片段复制进合并缓冲，到刷新窗口才安装标签正文。
 * @param[in] role 按值角色，变化时结束旧流。
 * @param[in] text 调用期非别名 NUL 文本借用，NULL 或空串跳过。
 * @post 建立流前缀失败撤销候选，追加 OOM 保留此前正文，超预算保留 UTF-8 字符前缀。
 * @note 内部取得 LVGL 锁，标签与拼接缓冲独立拥有文本，安装成功才推进显示快照。
 */
void board_ui_conversation_append_fragment(board_conversation_role_t role, const char *text)
{
    if (!s_ready || !text || !text[0]) return;

    ui_lock();

    if (!s_stream_active || !s_stream_msg_label || s_stream_role != role) {
        conversation_finish_fragment_locked();
        lv_obj_t *label = conversation_create_label_locked(role);
        if (!label) { ui_unlock(); return; }

        s_last_role = role;
        s_last_msg_label = label;
        s_stream_role = role;
        s_stream_msg_label = label;
        s_stream_active = true;
        if (!conversation_buffer_append_locked(role_prefix(role)) ||
            !conversation_buffer_append_locked(": ")) {
            free(s_stream_text);
            s_stream_text = NULL;
            s_stream_text_len = 0;
            s_stream_text_capacity = 0;
            s_stream_text_dirty = false;
            s_stream_msg_label = NULL;
            s_stream_active = false;
            s_last_msg_label = NULL;
            s_conversation_text_bytes -= strlen(lv_label_get_text(label)) + 1U;
            lv_obj_delete(label);
            ui_unlock();
            return;
        }
        board_ui_snapshot_append_conversation("");
        s_last_stream_scroll_tick = 0;
    }

    if (!conversation_buffer_append_locked(text)) {
        ui_unlock();
        return;
    }
    TickType_t now = xTaskGetTickCount();
    if (s_last_stream_scroll_tick == 0 ||
        (now - s_last_stream_scroll_tick) >= pdMS_TO_TICKS(UI_STREAM_SCROLL_INTERVAL_MS)) {
        conversation_flush_fragment_locked();
    }

    ui_unlock();
}

/**
 * @brief 取得 LVGL 锁后结束当前合并流，并请求批末布局。
 * @post 拼接缓冲归还，标签继续归对话容器持有，未就绪时跳过。
 */
void board_ui_conversation_finish_fragment(void)
{
    if (!s_ready) return;

    ui_lock();
    if (s_stream_active && s_stream_msg_label) {
        lv_obj_set_height(s_stream_msg_label, LV_SIZE_CONTENT);
        s_conversation_layout_pending = true;
    }
    conversation_finish_fragment_locked();
    ui_unlock();
}

/**
 * @brief 在启动覆盖层追加带角色前缀的日志，覆盖层已退场时改投对话。
 * @param[in] role 按值显示角色。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串。
 * @post 内部持锁，完整显示行最多保留 511 字节，启动标签最多保留 80 条，创建失败跳过。
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
    if (!label) {
        ui_unlock();
        return;
    }
    apply_message_label_style(label, role);

    char line[512];
    snprintf(line, sizeof(line), "%s: %s", role_prefix(role), text ? text : "");
    lv_label_set_text(label, line);
    board_ui_snapshot_append_boot(line);
    lv_obj_scroll_to_view(label, LV_ANIM_OFF);
    ui_unlock();
}

/**
 * @brief 锁内删除启动日志孩子并清空启动显示快照。
 * @post 启动行资源释放，覆盖层本身保持，未就绪时跳过。
 */
void board_ui_boot_log_clear(void)
{
    if (!s_ready) return;
    ui_lock();
    if (s_boot_log) lv_obj_clean(s_boot_log);
    board_ui_snapshot_clear_boot();
    ui_unlock();
}

/**
 * @brief 锁内删除启动覆盖层并使主 UI 失效重绘。
 * @post boot 对象借用清空，显示快照保留至显式清空，未就绪时跳过。
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

/**
 * @brief 在对话覆盖层创建填充矩形并记录绘制诊断。
 * @param[in] x 相对覆盖层水平像素坐标。
 * @param[in] y 相对覆盖层垂直像素坐标。
 * @param[in] width 正像素宽度。
 * @param[in] height 正像素高度。
 * @param[in] color RGB565 颜色值。
 * @post 内部持锁创建对象，原生父层负责裁剪，未就绪、非正尺寸或创建失败跳过。
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
    if (!obj) {
        ui_unlock();
        return;
    }
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(rgb565_to_rgb888(color)), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    ui_unlock();
}

/**
 * @brief 在对话覆盖层创建不透明背景的角色文本标签。
 * @param[in] x 相对水平像素坐标，负值钳为零。
 * @param[in] y 相对垂直像素坐标，负值钳为零。
 * @param[in] role 文字颜色分类。
 * @param[in] text 调用期 NUL 正文借用，NULL 作为空串。
 * @post 内部持锁由 LVGL 复制正文，宽度至少一像素，创建失败跳过，对象归覆盖层所有。
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
    if (!label) {
        ui_unlock();
        return;
    }
    apply_message_label_style(label, role);
    lv_obj_set_pos(label, x, y);

    int width = UI_LCD_WIDTH - UI_CONV_X - x - UI_PANEL_PAD;
    if (width < 1) width = 1;
    lv_obj_set_width(label, width);

    lv_label_set_text(label, text ? text : "");

    ui_unlock();
}

/**
 * @brief 锁内标记主树、对话、覆盖层及工具区需要重绘。
 * @post LVGL 后续刷新这些失效区域，未就绪时跳过，返回不等待面板完成。
 */
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

/**
 * @brief 初始化阶段创建工具队列，优先能力堆再回退普通队列。
 * @param[out] out_with_caps 必填标志槽，入口置 false，本轮能力堆创建成功时置 true。
 * @return 队列可用为 true，两种分配均失败为 false。
 * @pre 单一 UI 初始化 owner 持 LVGL 锁，尚未发布 ready，已有队列只能来自本次初始化。
 * @post 外层按本轮来源选择失败回收 API，成功队列保留至进程结束。
 */
static bool tool_cmd_queue_ensure(bool *out_with_caps)
{
    *out_with_caps = false;
    if (s_tool_cmd_queue) {
        return true;
    }
#if configSUPPORT_STATIC_ALLOCATION
    s_tool_cmd_queue = xQueueCreateWithCaps(UI_TOOL_CMD_QUEUE_DEPTH,
                                            sizeof(tool_cmd_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    *out_with_caps = s_tool_cmd_queue != NULL;
#endif
    if (!s_tool_cmd_queue) {
        s_tool_cmd_queue = xQueueCreate(UI_TOOL_CMD_QUEUE_DEPTH, sizeof(tool_cmd_t));
    }
    return s_tool_cmd_queue != NULL;
}

/**
 * @brief 将工具命令按值复制进已发布队列，不阻塞等待空位。
 * @param[in] cmd 调用期完整命令借用，NULL 跳过，内嵌文本必须 NUL 终止。
 * @post 接纳后队列独立拥有副本，未就绪、缺队列或队列满时跳过，输入借用结束。
 */
static void tool_cmd_enqueue(const tool_cmd_t *cmd)
{
    if (!cmd || !s_ready || !s_tool_cmd_queue) {
        return;
    }
    (void)xQueueSend(s_tool_cmd_queue, cmd, 0);
}

/**
 * @brief 清空工具快照及工具显示对象。
 * @post 工具区孩子和对应文本资源归还。
 * @pre LVGL 消费任务持锁。
 */
static void tool_area_clear_cmd_locked(void)
{
    board_ui_snapshot_clear_tool();
    tool_area_clear_locked();
}

/**
 * @brief 在滚动工具区末尾追加一条带角色前缀的标签。
 * @param[in] role 按值角色颜色及前缀分类。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串。
 * @post 完整显示行最多保留 383 字节，标签归工具区，超过条数预算淘汰最早对象，分配失败跳过。
 * @pre LVGL 消费任务持锁，工具区启用时方创建对象。
 */
static void tool_area_append_cmd_locked(board_conversation_role_t role, const char *text)
{
#if UI_TOOL_AREA_ENABLED
    if (!s_tool_area) {
        return;
    }

    int y = tool_area_next_y_locked();
    lv_obj_t *label = lv_label_create(s_tool_area);
    if (!label) return;
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

/**
 * @brief 在工具画布可见范围创建填充矩形。
 * @param[in] x 水平像素坐标，钳入画布。
 * @param[in] y 垂直像素坐标，钳入画布。
 * @param[in] width 正像素宽度，裁至剩余空间。
 * @param[in] height 正像素高度，裁至剩余空间。
 * @param[in] color RGB565 颜色值。
 * @post 非正尺寸跳过，其他调用尽力记录诊断，成功创建后执行工具条数裁剪。
 * @pre LVGL 消费任务持锁。
 */
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
    if (!obj) return;
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, width, height);
    lv_obj_set_style_bg_color(obj, lv_color_hex(rgb565_to_rgb888(color)), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);

    tool_area_prune_locked();
    lv_obj_invalidate(s_tool_area);
#endif
}

/**
 * @brief 在工具画布指定位置创建角色文本并滚动到该对象。
 * @param[in] x 水平像素坐标，钳入画布。
 * @param[in] y 垂直像素坐标，钳入画布。
 * @param[in] role 按值文字颜色分类。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串，由 LVGL 复制。
 * @post 成功对象归工具区，创建失败跳过，诊断快照为最佳努力。
 * @pre LVGL 消费任务持锁。
 */
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
    if (!label) return;
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

/**
 * @brief 标记工具面板及画布失效。
 * @post 已创建对象在后续 LVGL 刷新时重绘，未创建对象跳过。
 * @pre LVGL 消费任务持锁。
 */
static void tool_area_flush_cmd_locked(void)
{
    if (s_tool_panel) lv_obj_invalidate(s_tool_panel);
    if (s_tool_area) lv_obj_invalidate(s_tool_area);
}

/**
 * @brief 每次非阻塞消费至多队列深度条命令并在 LVGL 任务执行。
 * @param[in] timer 同步 LVGL 定时器借用，参数字段未使用，可 NULL。
 * @post 清空、追加、绘制和刷新命令按队列顺序执行，未知类型忽略，队列缺失直接返回。
 * @pre LVGL 任务持锁，队列在 ready 发布后保留至进程结束。
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

/** @brief 非阻塞提交工具清空命令。 @post 接纳后异步执行，未就绪或队列满跳过。 */
void board_ui_tool_area_clear(void)
{
    if (!s_ready) return;
    const tool_cmd_t cmd = {
        .type = UI_TOOL_CMD_CLEAR,
    };
    tool_cmd_enqueue(&cmd);
}

/**
 * @brief 非阻塞提交工具追加文本命令。
 * @param[in] role 按值角色。
 * @param[in] text 调用期 NUL 文本借用，NULL 为空，最多复制 383 字节。
 * @post 命令副本交给队列，返回不保留输入借用，队列满或未就绪跳过。
 */
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

/**
 * @brief 非阻塞提交工具矩形命令。
 * @param[in] x 水平像素坐标。
 * @param[in] y 垂直像素坐标。
 * @param[in] width 正像素宽度。
 * @param[in] height 正像素高度。
 * @param[in] color RGB565 颜色值。
 * @post 非正尺寸、未就绪或队列满跳过，否则消费任务按画布边界裁剪执行。
 */
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

/**
 * @brief 非阻塞提交工具指定位置文本命令。
 * @param[in] x 水平像素坐标。
 * @param[in] y 垂直像素坐标。
 * @param[in] role 按值角色颜色分类。
 * @param[in] text 调用期 NUL 文本借用，NULL 为空，最多复制 383 字节。
 * @post 接纳后由消费任务裁剪位置并创建标签，无跨调用字符串借用，队列满时跳过。
 */
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

/** @brief 非阻塞提交工具刷新命令。 @post 接纳后设置失效区域，队列满或未就绪跳过。 */
void board_ui_tool_area_request_flush(void)
{
    if (!s_ready) return;
    const tool_cmd_t cmd = {
        .type = UI_TOOL_CMD_FLUSH,
    };
    tool_cmd_enqueue(&cmd);
}

/**
 * @brief 由单一摄像头生产者复制 CPU 可读源帧，完成目标同步后发布 pending。
 * @param[in] rgb565 调用期稳定的紧密排列 240×240 RGB565 借用，NULL 忽略。
 * @param[in] width 像素宽度，必须为 240。
 * @param[in] height 像素高度，必须为 240。
 * @post 不保留输入借用，限频、无空槽或同步失败时跳过，配置允许时主动 yield 一 tick。
 * @pre 仅一个摄像头任务调用，调用者保证输入 CPU 可见且未被 DMA 或其他生产者修改，不持 LVGL 锁。
 * @note preview_mux 仅保护索引与统计，完整帧复制在临界区之外，front/pending/presenting 均不可作为写槽。
 */
void board_ui_camera_preview_update(const void *rgb565, int width, int height)
{
    if (!s_ready || !s_preview || !rgb565 || width != UI_PREVIEW_W || height != UI_PREVIEW_H) return;
#if UI_PREVIEW_BUFFER_COUNT < 2
    return;
#endif
    portENTER_CRITICAL(&s_preview_mux);
#if UI_PREVIEW_MAX_FPS > 0
    bool first_input = s_preview_stats.input_frames == 0;
#endif
    preview_counter_increment(&s_preview_stats.input_frames);
    portEXIT_CRITICAL(&s_preview_mux);

#if UI_PREVIEW_MAX_FPS > 0
    TickType_t now = xTaskGetTickCount();
    uint64_t interval = ((uint64_t)configTICK_RATE_HZ + UI_PREVIEW_MAX_FPS - 1U) / UI_PREVIEW_MAX_FPS;
    if (interval >= portMAX_DELAY) interval = (uint64_t)portMAX_DELAY - 1U;
    TickType_t min_interval = interval ? (TickType_t)interval : 1;

    if (!first_input && (now - s_preview_last_update_tick) < min_interval) {
        portENTER_CRITICAL(&s_preview_mux);
        preview_counter_increment(&s_preview_stats.rate_limited_frames);
        portEXIT_CRITICAL(&s_preview_mux);
#if UI_PREVIEW_YIELD_AFTER_UPDATE
        vTaskDelay(1);
#endif
        return;
    }
    s_preview_last_update_tick = now;
#endif

    /* 摄像头调用者已完成 DMA 源到 CPU 副本的同步，此处读取副本并保持已写入的像素。 */
    /* 只选择未被 front、pending 或 presenting 保留的槽，消费交接在 LVGL 定时器完成。 */
    uint8_t write_index = 0;
    bool found = false;

    portENTER_CRITICAL(&s_preview_mux);
    uint8_t front_index = s_preview_front_index;
    int pending_index = s_preview_pending_index;
    int presenting_index = s_preview_presenting_index;
    uint8_t candidate = s_preview_write_index;
    for (uint8_t i = 0; i < UI_PREVIEW_BUFFER_COUNT; i++) {
        if (candidate != front_index &&
            (pending_index < 0 || candidate != (uint8_t)pending_index) &&
            (presenting_index < 0 || candidate != (uint8_t)presenting_index) &&
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
        portENTER_CRITICAL(&s_preview_mux);
        preview_counter_increment(&s_preview_stats.no_buffer_frames);
        portEXIT_CRITICAL(&s_preview_mux);
        return;
    }

    uint8_t *dst = s_preview_buf[write_index];
    const uint8_t *src = rgb565;
    board_preview_probe_t probe = board_preview_probe(src, UI_PREVIEW_ROW_BYTES);
    bool pink_jump = s_preview_probe_valid && board_preview_pink_jump(s_preview_previous_rgb, probe.rgb);
    uint32_t previous_rgb = s_preview_previous_rgb;
    s_preview_previous_rgb = probe.rgb;
    s_preview_probe_valid = true;
    for (size_t y = 0; y < UI_PREVIEW_H; y++) {
        memcpy(dst + y * s_preview_stride_bytes, src + y * UI_PREVIEW_ROW_BYTES, UI_PREVIEW_ROW_BYTES);
    }

    esp_err_t dst_sync = esp_cache_msync(dst, UI_PREVIEW_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    if (dst_sync != ESP_OK && dst_sync != ESP_ERR_NOT_SUPPORTED) {
        portENTER_CRITICAL(&s_preview_mux);
        preview_counter_increment(&s_preview_stats.cache_sync_errors);
        portEXIT_CRITICAL(&s_preview_mux);
        if (!s_preview_dst_sync_warned) {
            s_preview_dst_sync_warned = true;
            ESP_LOGW(TAG, "preview dst cache sync failed once: %s", esp_err_to_name(dst_sync));
        }
        return;
    }

    portENTER_CRITICAL(&s_preview_mux);
    if (s_preview_pending_index >= 0 && s_preview_pending_index != s_preview_presenting_index) {
        preview_counter_increment(&s_preview_stats.replaced_pending_frames);
    }
    preview_counter_increment(&s_preview_stats.published_frames);
    s_preview_probe[write_index] = probe;
    s_preview_frame_id[write_index] = s_preview_stats.input_frames;
    if (pink_jump) {
        preview_counter_increment(&s_preview_stats.pink_jump_candidates);
        s_preview_stats.last_pink_frame = s_preview_stats.input_frames;
        s_preview_stats.last_pink_before_rgb = previous_rgb;
        s_preview_stats.last_pink_after_rgb = probe.rgb;
    }
    s_preview_pending_index = (int)write_index;
    portEXIT_CRITICAL(&s_preview_mux);

#if UI_PREVIEW_YIELD_AFTER_UPDATE
    vTaskDelay(1);
#endif
}

/**
 * @brief 在短临界区复制预览交接统计及初始化后固定的布局参数。
 * @param[out] out_stats 有效可写输出，NULL 跳过，ready 发布前清零。
 * @post 输出独立按值快照，不借用任何帧缓冲。
 */
void board_ui_camera_preview_get_stats(board_ui_preview_stats_t *out_stats)
{
    if (!out_stats) return;
    if (!s_ready) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    portENTER_CRITICAL(&s_preview_mux);
    *out_stats = s_preview_stats;
    out_stats->input_row_bytes = UI_PREVIEW_ROW_BYTES;
    out_stats->stride_bytes = (uint32_t)s_preview_stride_bytes;
    out_stats->buffer_bytes = (uint32_t)UI_PREVIEW_BYTES;
    out_stats->buffer_count = UI_PREVIEW_BUFFER_COUNT;
    portEXIT_CRITICAL(&s_preview_mux);
}


/**
 * @brief 在 preview_mux 短临界区发布摄像头性能统计副本。
 * @param[in] stats 调用期稳定结构借用，NULL 跳过。
 * @post 状态栏读取同一临界区保护的整组值，输入结构仍归调用者。
 */
void board_ui_camera_stats_update(const board_ui_camera_stats_t *stats)
{
    if (!stats) return;
    portENTER_CRITICAL(&s_preview_mux);
    s_camera_stats = *stats;
    portEXIT_CRITICAL(&s_preview_mux);
}

/**
 * @brief 发布配置状态，加载时在同一 LVGL 临界区完成字体构造、六树重绑和旧字体退休。
 * @param[in] loaded 按值配置加载标志。
 * @param[in] provider 调用期 NUL 名称借用，NULL 为空，最多复制 31 字节。
 * @param[in] model 调用期 NUL 名称借用，NULL 为空，最多复制 63 字节。
 * @post 状态经短临界区成组发布，字体失败则重绑内置回退字体并记录启动诊断。
 * @pre 启动 owner 串行维护字体路径和挂载期，已就绪加载不与字体路径更新并发。
 */
void board_ui_set_config_loaded(bool loaded, const char *provider, const char *model)
{
    char provider_copy[sizeof(s_provider)];
    char model_copy[sizeof(s_model)];
    snprintf(provider_copy, sizeof(provider_copy), "%s", provider ? provider : "");
    snprintf(model_copy, sizeof(model_copy), "%s", model ? model : "");
    portENTER_CRITICAL(&s_preview_mux);
    s_config_loaded = loaded;
    memcpy(s_provider, provider_copy, sizeof(s_provider));
    memcpy(s_model, model_copy, sizeof(s_model));
    portEXIT_CRITICAL(&s_preview_mux);
    if (loaded && s_ready) {
        ui_lock();
        lv_font_t *retired_font = NULL;
        bool external_font_loaded = board_ui_font_load_external(&retired_font);
        apply_font_tree(s_root);
        apply_font_tree(s_config_modal);
        apply_font_tree(s_keyboard);
        apply_font_tree(s_ota_layer);
        apply_font_tree(s_boot_layer);
        apply_font_tree(s_shutdown_layer);
        /* adapter 在同一互斥锁内运行完整 timer handler；原生刷新在返回前
         * dispatch 到各层 draw_task_head 清空，故此处已无上一轮绘制字体借用。 */
        board_ui_font_release_retired(&retired_font);
        ui_unlock();
        if (!external_font_loaded) {
            board_ui_boot_log_append(BOARD_CONV_ERROR, "external CJK font not loaded: check configured font path");
        } else {
            board_ui_boot_log_append(BOARD_CONV_STATUS, "external CJK font loaded");
        }
    }
}

/**
 * @brief 在短临界区发布状态栏的 USB 挂载标志。
 * @param[in] mounted 按值挂载状态。
 * @post 状态栏随后读取新副本，不等待 LVGL 锁。
 */
void board_ui_set_usb_mounted(bool mounted)
{
    portENTER_CRITICAL(&s_preview_mux);
    s_usb_mounted = mounted;
    portEXIT_CRITICAL(&s_preview_mux);
}

/**
 * @brief 先复制 Wi-Fi 详情，再在短临界区成组发布连接状态和详情。
 * @param[in] connected 按值连接标志。
 * @param[in] detail 调用期 NUL 借用，NULL 为空，最多复制 63 字节。
 * @post 返回不保留输入借用，不等待 LVGL 锁，状态栏读取完整副本。
 */
void board_ui_set_wifi_state(bool connected, const char *detail)
{
    char copy[sizeof(s_wifi_detail)];
    snprintf(copy, sizeof(copy), "%s", detail ? detail : "");
    portENTER_CRITICAL(&s_preview_mux);
    s_wifi_connected = connected;
    memcpy(s_wifi_detail, copy, sizeof(s_wifi_detail));
    portEXIT_CRITICAL(&s_preview_mux);
}

/**
 * @brief 在短临界区发布配置变更后的重启提示。
 * @param[in] required 按值提示标志。
 * @post 状态栏下次刷新读取该值，不等待 LVGL 锁，可从配置事务内调用。
 */
void board_ui_set_runtime_restart_required(bool required)
{
    portENTER_CRITICAL(&s_preview_mux);
    s_restart_required = required;
    portEXIT_CRITICAL(&s_preview_mux);
}

/**
 * @brief 标记 OTA 活跃并尽力显示准备覆盖层及零进度。
 * @param[in] name 调用期固件名借用，NULL 或空串使用 firmware，标题按 128 字节栈区截断。
 * @param[in] expected_size 预期固件字节数，零表示未知。
 * @param[in] message 调用期状态借用，NULL 或空串使用默认等待说明。
 * @post 内部持 LVGL 锁创建和更新对象，创建失败跳过显示，状态栏在活跃期暂停刷新。
 * @pre OTA owner 串行调用 prepare、begin、progress 和 end。
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

/**
 * @brief 标记 OTA 活跃并显示写入开始状态。
 * @param[in] name 调用期固件名借用，NULL 或空串使用 firmware。
 * @param[in] expected_size 预期字节数，零表示未知。
 * @post 内部持锁尽力创建覆盖层并将写入进度置零，输入文本不跨调用借用。
 * @pre OTA owner 串行调用该阶段接口。
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

/**
 * @brief 按节奏更新 OTA 写入进度，达到预期大小时立即提交末次状态。
 * @param[in] bytes_written 已写入字节数。
 * @param[in] expected_size 预期总字节数，零时显示零百分比。
 * @post 未完成阶段距上次不足 250ms 时跳过，否则持锁更新，百分比限制为 0 到 100。
 * @pre OTA owner 串行调用，tick 采样间隔短于完整回绕周期。
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

/**
 * @brief 尽力显示 OTA 结果后结束状态栏暂停标志。
 * @param[in] ok 成功为绿色满进度，失败为红色零进度。
 * @param[in] message 调用期 NUL 结果借用，NULL 或空串使用对应默认说明。
 * @post 内部持锁更新覆盖层，随后原子清 active，覆盖层继续显示结果。
 * @pre OTA owner 串行调用，内部文本由 LVGL 在调用期复制。
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

/**
 * @brief 尽力显示黑色顶层覆盖并强制 LVGL 刷新后延迟 250ms。
 * @post 内部 UI 锁在延迟前释放，UI 未就绪直接跳过，显示创建失败仍尝试刷新。
 * @note 该延迟给显示驱动完成机会，物理面板传输状态由 BSP 和 adapter 负责。
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

/**
 * @brief 持 LVGL 锁调用配置读取回调并构造完整表单和独立顶层键盘。
 * @post 已打开时复用现有模态框，解析或可检查的构造失败回收候选树、键盘和配置副本。
 * @note 回调读取及调用均在 detach 同一锁域，借用上下文覆盖整个同步调用。
 */
void board_ui_show_config_editor(void)
{
    if (!s_ready) return;
    ui_lock();
    if (s_config_modal) {
        ui_unlock();
        return;
    }
    cc_json_value_t *root = NULL;
    char *json_source = s_callbacks.load_config_json ? s_callbacks.load_config_json(s_callbacks.user_data) : NULL;
    cc_result_t rc = cc_json_parse(json_source ? json_source : "{}", &root);
    free(json_source);
    if (rc.code != CC_OK || !root || !cc_json_is_object(root)) {
        ui_appendf(BOARD_CONV_ERROR, "open config failed: %s", rc.code == CC_OK ? "object expected" :
                   (rc.message ? rc.message : cc_error_string(rc.code)));
        cc_result_free(&rc);
        cc_json_destroy(root);
        ui_unlock();
        return;
    }
    cc_result_free(&rc);

    s_field_count = 0;
    s_config_modal = lv_obj_create(lv_layer_top());
    if (!s_config_modal) goto fail;
    lv_obj_set_size(s_config_modal, 760, 430);
    lv_obj_center(s_config_modal);
    lv_obj_set_style_bg_color(s_config_modal, lv_color_hex(0x121a22), 0);
    lv_obj_set_style_border_color(s_config_modal, lv_color_hex(0x405060), 0);
    lv_obj_set_style_radius(s_config_modal, 6, 0);
    lv_obj_set_flex_flow(s_config_modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_config_modal, LV_DIR_VER);

    lv_obj_t *title = lv_label_create(s_config_modal);
    if (!title) goto fail;
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
    if (s_field_count != UI_CONFIG_FIELD_MAX) goto fail;

    lv_obj_t *row = lv_obj_create(s_config_modal);
    if (!row) goto fail;
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 720, 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_t *save = lv_button_create(row);
    if (!save) goto fail;
    lv_obj_set_size(save, 120, 38);
    if (!lv_obj_add_event_cb(save, save_config_cb, LV_EVENT_CLICKED, NULL)) goto fail;
    lv_obj_t *save_label = lv_label_create(save);
    if (!save_label) goto fail;
    apply_font(save_label);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);

    lv_obj_t *close = lv_button_create(row);
    if (!close) goto fail;
    lv_obj_set_size(close, 120, 38);
    if (!lv_obj_add_event_cb(close, close_config_cb, LV_EVENT_CLICKED, NULL)) goto fail;
    lv_obj_t *close_label = lv_label_create(close);
    if (!close_label) goto fail;
    apply_font(close_label);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);

    s_keyboard = lv_keyboard_create(lv_layer_top());
    if (!s_keyboard) goto fail;
    lv_obj_set_size(s_keyboard, 760, 150);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    ui_unlock();
    cc_json_destroy(root);
    return;
fail:
    close_config_cb(NULL);
    cc_json_destroy(root);
    ui_unlock();
}
