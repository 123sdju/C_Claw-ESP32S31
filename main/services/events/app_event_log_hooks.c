#include "app_event_bus_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cc/ports/cc_logger.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define APP_LOG_HOOK_STACK_BUF 192U

/*
 * 跳过 ANSI 转义序列（\033[...m 格式），返回实际内容起始位置的指针。
 * 用于剥离 ESP-IDF 日志输出中的颜色控制码。
 *
 * 参数: text - 原始日志行指针
 * 返回: 跳过转义序列后的文本起始指针
 */
static const char *skip_ansi_escape_sequences(const char *text)
{
    const char *p = text;
    while (p && p[0] == '\033' && p[1] == '[') {
        p += 2;
        while (*p && ((unsigned char)*p < 0x40 || (unsigned char)*p > 0x7e)) p++;
        if (*p) p++;
    }
    return p;
}

/*
 * 根据 ESP-IDF 日志行的首字母（E/W/I/D/V）推断日志级别，
 * 在跳过 ANSI 转义和前置空白后识别。
 *
 * 参数: line - 日志行指针
 * 返回: 对应的 cc_log_level_t 级别
 */
static cc_log_level_t esp_line_level(const char *line)
{
    const char *p = line;
    while (p && (*p == '\r' || *p == '\n')) p++;
    p = skip_ansi_escape_sequences(p);
    if (!p) return CC_LOG_INFO;
    switch (p[0]) {
    case 'E': return CC_LOG_ERROR;
    case 'W': return CC_LOG_WARN;
    case 'I': return CC_LOG_INFO;
    case 'D': return CC_LOG_DEBUG;
    case 'V': return CC_LOG_TRACE;
    default: return CC_LOG_INFO;
    }
}

/*
 * 判断 ESP-IDF 日志行是否包含指定的标签名称（位于 ")" 和 ":" 之间）。
 * 在 ANSI 转义跳过之后进行匹配。
 *
 * 参数: line - 日志行指针
 * 参数: tag  - 标签名称
 * 返回: true 表示标签匹配
 */
static bool line_has_tag(const char *line, const char *tag)
{
    if (!line || !tag) return false;
    const char *p = skip_ansi_escape_sequences(line);
    p = strchr(p ? p : line, ')');
    if (!p) return false;
    p++;
    while (*p == ' ') p++;
    size_t tag_len = strlen(tag);
    return strncmp(p, tag, tag_len) == 0 && p[tag_len] == ':';
}

/*
 * 提取日志行中指定标签对应的负载（payload），即 "tag: " 之后的内容。
 * 返回负载起始位置的指针，若标签不匹配或不存在则返回 NULL。
 *
 * 参数: line - 日志行指针
 * 参数: tag  - 标签名称
 * 返回: 负载内容起始指针，失败返回 NULL
 */
static const char *line_tag_payload(const char *line, const char *tag)
{
    if (!line || !tag) return NULL;
    const char *p = skip_ansi_escape_sequences(line);
    p = strchr(p ? p : line, ')');
    if (!p) return NULL;
    p++;
    while (*p == ' ') p++;
    size_t tag_len = strlen(tag);
    if (strncmp(p, tag, tag_len) != 0 || p[tag_len] != ':') return NULL;
    p += tag_len + 1;
    while (*p == ' ') p++;
    return p;
}

/*
 * 判断日志行的标签是否匹配给定标签集合中的任意一个。
 *
 * 参数: line      - 日志行指针
 * 参数: tags      - 标签字符串数组指针
 * 参数: tag_count - 标签数组长度
 * 返回: true 表示匹配到至少一个标签
 */
static bool line_has_any_tag(const char *line, const char *const *tags, size_t tag_count)
{
    for (size_t i = 0; i < tag_count; i++) {
        if (line_has_tag(line, tags[i])) {
            return true;
        }
    }
    return false;
}

/*
 * 根据日志级别和内容判断是否应丢弃该日志行，用于过滤 ESP-IDF 中大量
 * 重复的、无意义的底层日志（如 USB、SD卡、WiFi、LVGL 驱动等）。
 * 维护模式或路径特定的噪声日志都会被剔除。
 *
 * 参数: level - 日志级别
 * 参数: line  - 日志行内容
 * 返回: true 表示该行应被丢弃，false 表示保留
 */
static bool app_log_should_drop(cc_log_level_t level, const char *line)
{
    if (!line) return false;
    if (!line[0]) return true;
    if (strstr(line, "esp_lvgl:bridge: Hardware resource ") != NULL) return true;
    if (strstr(line, "dvp_video: size=115200") != NULL) return true;
    if (strstr(line, "LV_USE_GESTURE_RECOGNITION is disabled") != NULL) return true;
    if (strstr(line, "Input device registered") != NULL) return true;
    if (strstr(line, "Touch input device registered successfully") != NULL) return true;
    if (strstr(line, "LVGL task started successfully") != NULL) return true;
    if (strstr(line, "Connected src ") != NULL && line_has_tag(line, "intr_alloc")) return true;
    if (strstr(line, "BLK") != NULL && line_has_tag(line, "efuse")) return true;
    if (line_has_tag(line, "USB_MSC_SCSI") &&
        (strstr(line, "CSW failed") != NULL ||
         strstr(line, "Sense error") != NULL ||
         strstr(line, "Sense error description") != NULL)) {
        return true;
    }

    static const char *const noisy_info_tags[] = {
        "BOARD_DEVICE",
        "BOARD_MANAGER",
        "ENUM",
        "HCD DWC",
        "HUB",
        "USBH",
        "USB_MSC",
        "USB_MSC_SCSI",
        "board_camera",
        "boot_comm",
        "dvp_cam",
        "dvp_gdma",
        "dvp_video",
        "efuse",
        "esp_lvgl:adapter",
        "esp_lvgl:touch",
        "esp_netif_handlers",
        "esp_netif_lwip",
        "esp_ota_ops",
        "esp_video",
        "httpd",
        "httpd_parse",
        "httpd_sess",
        "httpd_txrx",
        "httpd_uri",
        "gpio",
        "gdma-link",
        "lcd.rgb",
        "lcd_hal",
        "lcd_panel.io.i2c",
        "nvs",
        "ov3660",
        "phy_init",
        "sccb_i2c",
        "SD_HOST",
        "sdmmc_common",
        "sdmmc_sd",
        "temperature_sensor_hal",
        "vfs_calls",
        "vfs_fat",
        "vfs_fat_sdmmc",
        "wifi",
        "wifi_init",
        "wifi_init_default",
    };
    if (level <= CC_LOG_INFO &&
        line_has_any_tag(line,
                         noisy_info_tags,
                         sizeof(noisy_info_tags) / sizeof(noisy_info_tags[0]))) {
        return true;
    }
    if (line_has_tag(line, "event") &&
        strstr(line, "ESP_HTTP_SERVER_EVENT") != NULL &&
        strstr(line, "no handlers have been registered") != NULL) {
        return true;
    }
    if ((line_has_tag(line, "httpd_uri") || line_has_tag(line, "httpd_txrx")) &&
        strstr(line, "favicon.ico") != NULL) {
        return true;
    }
    if (line_has_tag(line, "httpd_txrx") &&
        strstr(line, "404 Not Found - Nothing matches") != NULL) {
        return true;
    }
    const char *wifi_payload = line_tag_payload(line, "wifi");
    if (wifi_payload && !wifi_payload[0]) {
        return true;
    }
    if (strstr(line, "[ADDBA]") != NULL ||
        strstr(line, "<ba-add>") != NULL ||
        strstr(line, "(phy)rate:") != NULL ||
        strstr(line, "(trc)band:") != NULL ||
        strstr(line, "Password length matches WPA2 standards") != NULL ||
        strstr(line, "11ax/11ac mode can not work") != NULL ||
        strstr(line, "eb is dhcp or dns") != NULL ||
        strstr(line, "obss scan") != NULL ||
        strstr(line, "start obss scan") != NULL ||
        strstr(line, "clear blacklist") != NULL ||
        strstr(line, "remove all except") != NULL ||
        strstr(line, "pm start") != NULL ||
        strstr(line, "set rx beacon") != NULL ||
        strstr(line, "AP's beacon interval") != NULL ||
        strstr(line, "connected with ") != NULL ||
        strstr(line, "security: WPA") != NULL) {
        return true;
    }
    return false;
}

/*
 * 注册到 ESP-IDF 的 vprintf 钩子：拦截所有 ESP_LOG* 输出，
 * 去除行尾换行回车，跳过空行，过滤噪声日志，将保留的日志发布为事件。
 * 优先使用栈缓冲区，超长时回退到堆分配。
 *
 * 参数: fmt - printf 格式字符串
 * 参数: ap  - 可变参数列表
 * 返回: 格式化后的字符数
 */
static int app_log_vprintf(const char *fmt, va_list ap)
{
    if (!fmt) {
        return 0;
    }
    char stack[APP_LOG_HOOK_STACK_BUF];
    char *heap = NULL;
    char *line = stack;
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(stack, sizeof(stack), fmt, copy);
    va_end(copy);

    if (n > 0) {
        if ((size_t)n >= sizeof(stack)) {
            size_t heap_len = (size_t)n;
            if (heap_len > APP_EVENT_TEXT_MAX) {
                heap_len = APP_EVENT_TEXT_MAX;
            }
            heap = heap_caps_malloc(heap_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!heap) {
                heap = heap_caps_malloc(heap_len + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            if (heap) {
                va_copy(copy, ap);
                vsnprintf(heap, heap_len + 1, fmt, copy);
                va_end(copy);
                line = heap;
            } else {
                stack[sizeof(stack) - 1] = '\0';
            }
        }
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            heap_caps_free(heap);
            return n;
        }
        cc_log_level_t level = esp_line_level(line);
        if (!app_log_should_drop(level, line)) {
            app_event_publish(APP_EVENT_KIND_LOG, APP_EVENT_SOURCE_SYSTEM, level, line);
        }
    }
    heap_caps_free(heap);
    return n;
}

/*
 * cc_logger 的自定义 sink 回调：将应用层 SDK 日志转换为 APP_EVENT_KIND_LOG 事件发布。
 * user_data 和 logger_name 参数当前未使用。
 *
 * 参数: user_data   - 用户数据（未使用）
 * 参数: level       - 日志级别
 * 参数: logger_name - 日志器名称（未使用）
 * 参数: message     - 日志消息内容
 * 返回: 无
 */
static void sdk_logger_sink(void *user_data,
                            cc_log_level_t level,
                            const char *logger_name,
                            const char *message)
{
    (void)user_data;
    (void)logger_name;
    app_event_publish(APP_EVENT_KIND_LOG, APP_EVENT_SOURCE_RUNTIME, level, message ? message : "");
}

/*
 * 根据当前事件总线的配置（串口调试开关、USB日志级别、维护模式）动态调整
 * ESP-IDF 所有模块的日志级别。维护模式下大量模块被设为 WARN 以抑制噪声。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_log_hooks_apply_esp_level(void)
{
    if (g_app_event_bus.maintenance_mode) {
        esp_log_level_set("*", ESP_LOG_WARN);
        esp_log_level_set("ota_manager", ESP_LOG_INFO);
        esp_log_level_set("esp_lvgl:bridge", ESP_LOG_WARN);
        esp_log_level_set("httpd", ESP_LOG_WARN);
        esp_log_level_set("httpd_parse", ESP_LOG_WARN);
        esp_log_level_set("httpd_txrx", ESP_LOG_WARN);
        esp_log_level_set("httpd_uri", ESP_LOG_WARN);
        esp_log_level_set("httpd_sess", ESP_LOG_WARN);
        esp_log_level_set("vfs_calls", ESP_LOG_WARN);
        esp_log_level_set("event", ESP_LOG_WARN);
        esp_log_level_set("wifi", ESP_LOG_WARN);
        esp_log_level_set("wifi_init_default", ESP_LOG_WARN);
        esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
        esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
        esp_log_level_set("ov3660", ESP_LOG_WARN);
        esp_log_level_set("esp_video", ESP_LOG_WARN);
        esp_log_level_set("dvp_video", ESP_LOG_WARN);
        esp_log_level_set("dvp_cam", ESP_LOG_WARN);
        esp_log_level_set("dvp_gdma", ESP_LOG_WARN);
        esp_log_level_set("SD_HOST", ESP_LOG_WARN);
        esp_log_level_set("sdmmc_common", ESP_LOG_WARN);
        esp_log_level_set("sdmmc_sd", ESP_LOG_WARN);
        esp_log_level_set("vfs_fat", ESP_LOG_WARN);
        esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_WARN);
        esp_log_level_set("boot_comm", ESP_LOG_WARN);
        esp_log_level_set("esp_ota_ops", ESP_LOG_WARN);
        esp_log_level_set("gpio", ESP_LOG_WARN);
        esp_log_level_set("gdma-link", ESP_LOG_WARN);
        esp_log_level_set("lcd.rgb", ESP_LOG_WARN);
        esp_log_level_set("lcd_hal", ESP_LOG_WARN);
        esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_WARN);
        esp_log_level_set("sccb_i2c", ESP_LOG_WARN);
        esp_log_level_set("BOARD_DEVICE", ESP_LOG_WARN);
        esp_log_level_set("BOARD_MANAGER", ESP_LOG_WARN);
        esp_log_level_set("board_camera", ESP_LOG_WARN);
        esp_log_level_set("nvs", ESP_LOG_WARN);
        esp_log_level_set("wifi_init", ESP_LOG_WARN);
        esp_log_level_set("phy_init", ESP_LOG_WARN);
        esp_log_level_set("intr_alloc", ESP_LOG_WARN);
        esp_log_level_set("efuse", ESP_LOG_WARN);
        esp_log_level_set("temperature_sensor_hal", ESP_LOG_WARN);
        esp_log_level_set("USBH", ESP_LOG_WARN);
        esp_log_level_set("ENUM", ESP_LOG_WARN);
        esp_log_level_set("HUB", ESP_LOG_WARN);
        esp_log_level_set("HCD DWC", ESP_LOG_WARN);
        esp_log_level_set("USB_MSC", ESP_LOG_WARN);
        esp_log_level_set("USB_MSC_SCSI", ESP_LOG_WARN);
        esp_log_level_set("esp_lvgl:adapter", ESP_LOG_WARN);
        esp_log_level_set("esp_lvgl:touch", ESP_LOG_WARN);
        return;
    }

    bool capture_debug = g_app_event_bus.serial_debug_enabled ||
                         g_app_event_bus.serial_fallback_debug_enabled ||
                         (g_app_event_bus.usb_log_enabled &&
                          g_app_event_bus.usb_log_level <= CC_LOG_DEBUG);
    esp_log_level_set("*",
                      capture_debug ? ESP_LOG_DEBUG : ESP_LOG_INFO);
    esp_log_level_set("esp_lvgl:bridge", ESP_LOG_WARN);
    esp_log_level_set("httpd", ESP_LOG_INFO);
    esp_log_level_set("httpd_parse", ESP_LOG_INFO);
    esp_log_level_set("httpd_txrx", ESP_LOG_INFO);
    esp_log_level_set("httpd_uri", ESP_LOG_INFO);
    esp_log_level_set("httpd_sess", ESP_LOG_INFO);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_INFO);
    esp_log_level_set("SSL TLS", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_INFO);
    esp_log_level_set("esp-tls-mbedtls", ESP_LOG_INFO);
    esp_log_level_set("transport_base", ESP_LOG_INFO);
    esp_log_level_set("vfs_calls", ESP_LOG_INFO);
    esp_log_level_set("event", ESP_LOG_INFO);
    esp_log_level_set("wifi", ESP_LOG_WARN);
    esp_log_level_set("wifi_init_default", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_WARN);
    esp_log_level_set("esp_netif_lwip", ESP_LOG_WARN);
    esp_log_level_set("ov3660", ESP_LOG_WARN);
    esp_log_level_set("esp_video", ESP_LOG_WARN);
    esp_log_level_set("dvp_video", ESP_LOG_WARN);
    esp_log_level_set("dvp_cam", ESP_LOG_WARN);
    esp_log_level_set("dvp_gdma", ESP_LOG_WARN);
    esp_log_level_set("SD_HOST", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_common", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_sd", ESP_LOG_WARN);
    esp_log_level_set("vfs_fat", ESP_LOG_WARN);
    esp_log_level_set("vfs_fat_sdmmc", ESP_LOG_WARN);
    esp_log_level_set("boot_comm", ESP_LOG_WARN);
    esp_log_level_set("esp_ota_ops", ESP_LOG_WARN);
    esp_log_level_set("gpio", ESP_LOG_WARN);
    esp_log_level_set("gdma-link", ESP_LOG_WARN);
    esp_log_level_set("lcd.rgb", ESP_LOG_WARN);
    esp_log_level_set("lcd_hal", ESP_LOG_WARN);
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_WARN);
    esp_log_level_set("sccb_i2c", ESP_LOG_WARN);
    esp_log_level_set("BOARD_DEVICE", ESP_LOG_WARN);
    esp_log_level_set("BOARD_MANAGER", ESP_LOG_WARN);
    esp_log_level_set("board_camera", ESP_LOG_WARN);
    esp_log_level_set("nvs", ESP_LOG_WARN);
    esp_log_level_set("wifi_init", ESP_LOG_WARN);
    esp_log_level_set("phy_init", ESP_LOG_WARN);
    esp_log_level_set("intr_alloc", ESP_LOG_WARN);
    esp_log_level_set("efuse", ESP_LOG_WARN);
    esp_log_level_set("temperature_sensor_hal", ESP_LOG_WARN);
    esp_log_level_set("USBH", ESP_LOG_WARN);
    esp_log_level_set("ENUM", ESP_LOG_WARN);
    esp_log_level_set("HUB", ESP_LOG_WARN);
    esp_log_level_set("HCD DWC", ESP_LOG_WARN);
    esp_log_level_set("USB_MSC", ESP_LOG_WARN);
    esp_log_level_set("USB_MSC_SCSI", ESP_LOG_WARN);
    esp_log_level_set("esp_lvgl:adapter", ESP_LOG_WARN);
    esp_log_level_set("esp_lvgl:touch", ESP_LOG_WARN);
}

/*
 * 安装 ESP-IDF 和 SDK 日志钩子：将 app_log_vprintf 注册为 ESP 的 vprintf 钩子，
 * 将 sdk_logger_sink 注册为 cc_logger 的 sink。已安装时跳过重复注册。
 *
 * 参数: 无
 * 返回: 无
 */
void app_event_log_hooks_install(void)
{
    if (!g_app_event_bus.vprintf_installed) {
        (void)esp_log_set_vprintf(app_log_vprintf);
        g_app_event_bus.vprintf_installed = true;
    }
    cc_logger_set_sink(sdk_logger_sink, NULL);
}

void app_event_log_hooks_uninstall(void)
{
    cc_logger_set_sink(NULL, NULL);
    if (g_app_event_bus.vprintf_installed) {
        (void)esp_log_set_vprintf(vprintf);
        g_app_event_bus.vprintf_installed = false;
    }
}
