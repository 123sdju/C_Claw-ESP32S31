#include "app_event_bus_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "cc/ports/cc_logger.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define APP_LOG_HOOK_STACK_BUF 192U /**< ESP 日志首次格式化的栈缓冲字节数，包含 NUL。 */

/**
 * @brief 跳过日志起始处连续的 ANSI CSI 控制序列。
 * @param[in] text 稳定 NUL 文本借用，允许 NULL。
 * @return 原串中的首个正文位置，NULL 仍为 NULL，不复制、不修改、不删除正文中间的控制序列。
 * @note CSI 以 ESC 加左方括号开始，扫描到首个 0x40 至 0x7e 终结字节，残缺序列停在原结尾。
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

/**
 * @brief 跳过起始 CR/LF 和 ANSI CSI 后按首字母推断 ESP 日志级别。
 * @param[in] line 调用期稳定 NUL 日志借用，允许 NULL，不按任意空白归一。
 * @return E/W/I/D/V 分别映射 ERROR/WARN/INFO/DEBUG/TRACE，空值或其他前缀使用 INFO。
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

/**
 * @brief 在首个时间戳右括号后查找精确标签与紧随的冒号。
 * @param[in] line 稳定 NUL 行借用，NULL 不匹配。
 * @param[in] tag 稳定 NUL 标签借用，NULL 不匹配。
 * @return 跳过起始 CSI、右括号后 ASCII 空格并精确匹配为 true，格式不符为 false。
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

/**
 * @brief 按同一时间戳和标签规则借出冒号后正文。
 * @param[in] line 稳定 NUL 行借用，NULL 返回 NULL。
 * @param[in] tag 精确标签借用，NULL 返回 NULL。
 * @return 匹配时返回跳过冒号后 ASCII 空格的原串视图，空正文可指向结尾，未匹配为 NULL。
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

/**
 * @brief 顺序核对固定标签表，遇到首个匹配即结束。
 * @param[in] line 调用期 NUL 日志借用，允许 NULL。
 * @param[in] tags 含 tag_count 项的稳定借用数组，零项时可为 NULL，空元素沿单标签不匹配处理。
 * @param[in] tag_count 数组元素数量。
 * @return 至少一个精确标签命中为 true，其他为 false，不保存输入视图。
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

/**
 * @brief 按固定文本和标签规则抑制已知的驱动重复日志。
 * @param[in] level 本次推断的 SDK 级别，指定噪声标签只过滤 INFO 及以下。
 * @param[in] line 稳定 NUL 日志借用，空串丢弃，NULL 返回 false。
 * @return 命中任一固定规则为 true，其他为 false，不读取总线可变状态。
 * @note 此过滤与事件总线的维护模式、路由、脱敏和输出级别各自执行。
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

/**
 * @brief 同步格式化 ESP 日志片段，去除尾部 CR/LF、过滤后转交事件发布入口。
 * @param[in] fmt 调用期有效的 printf 格式借用，NULL 返回零。
 * @param[in] ap 与格式匹配的可变参数借用，每次格式化都用 va_copy，不消费原游标。
 * @return 首次格式化的预期字符数，过滤和有界截断仍保留该值，任一次格式化失败返回对应负值。
 * @note 短片段使用栈缓冲，长片段最多申请 APP_EVENT_TEXT_MAX 加一字节，PSRAM 失败再试内部 RAM，两者失败保留栈截断。
 * 回调不借用可销毁的总线字段，发布入口自行取得资源租约，关闭后迟到的静态回调不能接触已释放原语。
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
        size_t len = (size_t)n < sizeof(stack) ? (size_t)n : sizeof(stack) - 1;
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
                int written = vsnprintf(heap, heap_len + 1, fmt, copy);
                va_end(copy);
                if (written < 0) {
                    heap_caps_free(heap);
                    return written;
                }
                len = (size_t)written < heap_len ? (size_t)written : heap_len;
                heap[len] = '\0';
                line = heap;
            } else {
                stack[sizeof(stack) - 1] = '\0';
            }
        }
        /* 已初始化范围内的首个 NUL 沿既有 C 文本语义结束正文。 */
        const char *terminator = memchr(line, '\0', len);
        if (terminator) len = (size_t)(terminator - line);
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

/**
 * @brief 将 SDK 日志同步转为 runtime 来源的日志事件。
 * @param[in,out] user_data 注册为 NULL，当前忽略，不保管外部上下文。
 * @param[in] level 原样交事件层的级别值。
 * @param[in] logger_name 调用期名称借用，当前忽略。
 * @param[in] message 调用期 NUL 正文借用，NULL 按空串，跨调用保存由事件层复制。
 * @post 已向公共发布入口提交一次，路由、脱敏和关闭门禁沿总线契约，输入不被接管。
 * @note SDK 在释放 sink 注册锁后调用该静态函数，替换 sink 不等待已经取得快照的回调。
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

/**
 * @brief 在同一总线状态锁内读取当前配置并顺序设置整批 ESP 日志阈值。
 * @pre 调用者持总线资源租约或生命周期独占，状态锁存活，当前 native setter 不记录日志或调用用户回调。
 * @post wildcard 与各固定标签已按锁内最新状态依次调用 setter，旧配置快照不能在解锁后覆盖新状态。
 * @note 当前 IDF setter 只取得独立日志级别锁并更新表，日志路径释放该锁后才进入本组件回调。
 * native void setter 不报告内部标签分配失败，本函数不宣称各标签更新均已确认成功。
 */
void app_event_log_hooks_apply_esp_level(void)
{
    app_event_bus_lock();
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
        app_event_bus_unlock();
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
    app_event_bus_unlock();
}

/**
 * @brief 在总线状态锁内安装全局 ESP hook 并重新发布 SDK sink 配对。
 * @pre 调用者持总线租约或初始化独占，原语存活，本组件是这两个进程期日志入口的配置 owner。
 * @post ESP hook 安装标志与注册保持同锁顺序，SDK sink 设置已尝试，重复调用继续尝试 SDK 注册。
 * @note native ESP 注册为原子指针交换，SDK setter 不在注册锁内调用 sink，安装过程不递归发布事件。
 * SDK void setter 创建注册锁失败时保留原 sink，此失败不由 vprintf_installed 表示。
 */
void app_event_log_hooks_install(void)
{
    app_event_bus_lock();
    if (!g_app_event_bus.vprintf_installed) {
        (void)esp_log_set_vprintf(app_log_vprintf);
        g_app_event_bus.vprintf_installed = true;
    }
    cc_logger_set_sink(sdk_logger_sink, NULL);
    app_event_bus_unlock();
}

/**
 * @brief 在状态锁内恢复 SDK 默认 stderr 与 ESP 默认 vprintf，并清除 ESP 安装标志。
 * @pre 生命周期 owner 已关闭总线入口并排空已接纳资源租约，状态锁尚未销毁，禁止并发重新 install。
 * @post 后续日志读取默认入口，已取得旧函数快照的静态回调仍可结束，公共发布门已关闭且不会再取得动态总线资源。
 * @note 不接管先前第三方 hook，也不恢复其他组件注册，本项目统一拥有该全局日志接线。
 */
void app_event_log_hooks_uninstall(void)
{
    app_event_bus_lock();
    cc_logger_set_sink(NULL, NULL);
    if (g_app_event_bus.vprintf_installed) {
        (void)esp_log_set_vprintf(vprintf);
        g_app_event_bus.vprintf_installed = false;
    }
    app_event_bus_unlock();
}
