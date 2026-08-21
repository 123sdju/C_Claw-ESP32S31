#include "app_event_bus_internal.h"
#include "app_profile.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define APP_EVENT_USB_TASK_STACK_IN_PSRAM 1
#else
#define APP_EVENT_USB_TASK_STACK_IN_PSRAM 0
#endif

/*
 * 递归创建目录路径中的所有中间目录，类似 mkdir -p。
 * 路径末尾的 '/' 会被去除。
 *
 * 参数: path - 要创建的完整目录路径
 * 返回: 无
 */
static void make_dirs(const char *path)
{
    if (!path || !path[0]) return;
    char tmp[160];
    app_event_copy_text(tmp, sizeof(tmp), path);
    size_t len = strlen(tmp);
    if (len == 0) return;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/*
 * 检查系统时间是否已同步有效（2024年1月1日之后），用于判断
 * 是否可以将启动时的临时日志文件名重命名为基于真实时间戳的名称。
 *
 * 参数: 无
 * 返回: true 表示时间已同步，false 表示时间无效
 */
static bool time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return now >= 1704067200;
}

/*
 * 基于当前系统时间构建以时间戳命名的日志基础路径，格式为 "cclaw-YYYYMMDD-HHMMSS.log"。
 * NTP 同步后用于替换启动时的临时文件名。
 *
 * 参数: out      - 输出缓冲区
 * 参数: out_size - 缓冲区大小
 * 返回: 无
 */
static void build_time_base_path(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    time_t now = 0;
    struct tm tm_info;
    time(&now);
    localtime_r(&now, &tm_info);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_info);
    if (!app_event_join_path(out, out_size, g_app_event_bus.usb_dir, "cclaw-") ||
        !app_event_append_text(out, out_size, stamp) ||
        !app_event_append_text(out, out_size, ".log")) {
        out[0] = '\0';
    }
}

/*
 * 构建启动时使用的临时日志基础路径，格式为 "cclaw-boot-<tick_ms>.log"，
 * 使用 FreeRTOS tick 作为唯一标识，在 NTP 同步后会被重命名。
 *
 * 参数: out      - 输出缓冲区
 * 参数: out_size - 缓冲区大小
 * 返回: 无
 */
static void build_boot_base_path(char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!app_event_join_path(out, out_size, g_app_event_bus.usb_dir, "cclaw-boot-") ||
        !app_event_append_uint32(out, out_size, (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount())) ||
        !app_event_append_text(out, out_size, ".log")) {
        out[0] = '\0';
    }
}

/*
 * 从基础路径和分卷编号构建完整日志文件路径。part <= 1 时直接使用基础路径，
 * part > 1 时插入 "-part{N}" 后缀，如 "cclaw.log" → "cclaw-part2.log"。
 *
 * 参数: out       - 输出缓冲区
 * 参数: out_size  - 缓冲区大小
 * 参数: base_path - 基础路径（不含分卷号）
 * 参数: part      - 分卷编号（从 1 开始）
 * 返回: 无
 */
static void build_part_path(char *out, size_t out_size, const char *base_path, int part)
{
    if (part <= 1) {
        app_event_copy_text(out, out_size, base_path);
        return;
    }
    const char *dot = strrchr(base_path, '.');
    if (dot && strcmp(dot, ".log") == 0) {
        size_t prefix_len = (size_t)(dot - base_path);
        if (prefix_len >= out_size) prefix_len = out_size - 1;
        memcpy(out, base_path, prefix_len);
        out[prefix_len] = '\0';
        snprintf(out + prefix_len, out_size - prefix_len, "-part%d.log", part);
    } else {
        snprintf(out, out_size, "%s-part%d.log", base_path, part);
    }
}

/*
 * 以追加模式打开日志文件，关闭之前的文件句柄，更新 usb_current_path 和
 * usb_current_bytes 计数器。失败时返回 false。
 *
 * 参数: path - 要打开的日志文件完整路径
 * 返回: true 表示成功打开，false 表示打开失败
 */
static bool open_usb_log_file(const char *path)
{
    if (g_app_event_bus.usb_file) {
        fclose(g_app_event_bus.usb_file);
        g_app_event_bus.usb_file = NULL;
    }
    g_app_event_bus.usb_file = fopen(path, "a");
    if (!g_app_event_bus.usb_file) {
        return false;
    }
    app_event_copy_text(g_app_event_bus.usb_current_path,
                        sizeof(g_app_event_bus.usb_current_path),
                        path);
    g_app_event_bus.usb_current_bytes = 0;
    return true;
}

/*
 * 检查当前日志文件是否需要轮转：若写入 next_len 字节后将超过 usb_max_bytes，
 * 递增分卷号并打开新文件。usb_max_bytes 为 0 时不做大小限制。
 *
 * 参数: next_len - 即将写入的字节数
 * 返回: 无
 */
static void rotate_if_needed(size_t next_len)
{
    if (!g_app_event_bus.usb_file || g_app_event_bus.usb_max_bytes == 0 ||
        g_app_event_bus.usb_current_bytes + next_len <= g_app_event_bus.usb_max_bytes) {
        return;
    }
    g_app_event_bus.usb_part++;
    char next_path[sizeof(g_app_event_bus.usb_current_path)];
    build_part_path(next_path, sizeof(next_path), g_app_event_bus.usb_base_path, g_app_event_bus.usb_part);
    open_usb_log_file(next_path);
}

/*
 * 若系统时间已同步且尚未重命名，则将启动时的 boot 临时文件重命名为
 * 基于真实时间戳的文件名。重命名后设置 renamed_with_time 标志防止重复操作。
 *
 * 参数: 无
 * 返回: 无
 */
static void rename_to_time_if_ready(void)
{
    if (g_app_event_bus.renamed_with_time || !g_app_event_bus.rename_pending || !time_is_valid()) {
        return;
    }
    char new_base[sizeof(g_app_event_bus.usb_base_path)];
    char new_path[sizeof(g_app_event_bus.usb_current_path)];
    build_time_base_path(new_base, sizeof(new_base));
    build_part_path(new_path, sizeof(new_path), new_base, g_app_event_bus.usb_part);
    if (g_app_event_bus.usb_file) {
        fclose(g_app_event_bus.usb_file);
        g_app_event_bus.usb_file = NULL;
    }
    if (rename(g_app_event_bus.usb_current_path, new_path) != 0) {
        open_usb_log_file(g_app_event_bus.usb_current_path);
        return;
    }
    app_event_copy_text(g_app_event_bus.usb_base_path,
                        sizeof(g_app_event_bus.usb_base_path),
                        new_base);
    app_event_copy_text(g_app_event_bus.usb_current_path,
                        sizeof(g_app_event_bus.usb_current_path),
                        new_path);
    g_app_event_bus.renamed_with_time = true;
    g_app_event_bus.rename_pending = false;
    open_usb_log_file(g_app_event_bus.usb_current_path);
}

/*
 * 判断文件名是否是本系统生成的日志文件：以 "cclaw-" 开头且包含 ".log" 后缀。
 *
 * 参数: name - 文件名指针
 * 返回: true 表示是日志文件，false 表示不是
 */
static bool is_log_name(const char *name)
{
    return name && strncmp(name, "cclaw-", 6) == 0 && strstr(name, ".log") != NULL;
}

/*
 * 清理过旧的日志文件：扫描日志目录，按修改时间排序，删除超过 usb_max_files
 * 数量限制的最旧文件。当前正在写入的文件不会被删除。
 *
 * 参数: 无
 * 返回: 无
 */
static void cleanup_old_logs(void)
{
    if (g_app_event_bus.usb_max_files <= 0) return;
    DIR *dir = opendir(g_app_event_bus.usb_dir);
    if (!dir) return;
    typedef struct {
        char path[192];
        time_t mtime;
    } entry_t;
    const size_t entry_capacity = 48;
    entry_t *entries = heap_caps_calloc(entry_capacity, sizeof(entry_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) entries = heap_caps_calloc(entry_capacity, sizeof(entry_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!entries) {
        closedir(dir);
        return;
    }
    size_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!is_log_name(ent->d_name) || count >= entry_capacity) continue;
        if (!app_event_join_path(entries[count].path,
                                 sizeof(entries[count].path),
                                 g_app_event_bus.usb_dir,
                                 ent->d_name)) {
            continue;
        }
        struct stat st;
        entries[count].mtime = stat(entries[count].path, &st) == 0 ? st.st_mtime : 0;
        count++;
    }
    closedir(dir);
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (entries[j].mtime < entries[i].mtime) {
                entry_t tmp = entries[i];
                entries[i] = entries[j];
                entries[j] = tmp;
            }
        }
    }
    size_t keep = (size_t)g_app_event_bus.usb_max_files;
    for (size_t i = 0; count > keep && i < count - keep; i++) {
        if (strcmp(entries[i].path, g_app_event_bus.usb_current_path) != 0) {
            remove(entries[i].path);
        }
    }
    heap_caps_free(entries);
}

/*
 * USB 日志写入任务主循环：从队列消费日志行并写入文件，2 秒间隔 fflush，
 * 30 秒间隔清理旧日志文件。支持时间同步后的文件重命名。
 *
 * 参数: arg - 任务参数（未使用）
 * 返回: 无（永久循环）
 */
static void usb_log_task(void *arg)
{
    (void)arg;
    TickType_t last_flush = xTaskGetTickCount();
    TickType_t last_cleanup = last_flush;
    app_event_usb_log_msg_t msg;
    while (true) {
        if (xQueueReceive(g_app_event_bus.usb_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (msg.shutdown) {
                if (g_app_event_bus.file_mutex) {
                    xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
                }
                if (g_app_event_bus.usb_file) fflush(g_app_event_bus.usb_file);
                if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
                app_event_bus_lock();
                g_app_event_bus.usb_task = NULL;
                app_event_bus_unlock();
                xEventGroupSetBits(g_app_event_bus.idle_events,
                                   APP_EVENT_USB_TASK_EXITED_BIT | APP_EVENT_BUS_IDLE_BIT);
                vTaskDelete(NULL);
                continue;
            }
            char line[APP_EVENT_TEXT_MAX + 96U];
            const char *text = msg.envelope ? msg.envelope->text : "";
            int rendered = snprintf(line, sizeof(line), "%" PRIu32 " [%s] [%s] %s\n",
                                    msg.seq,
                                    app_event_log_level_string(msg.level),
                                    app_event_kind_string(msg.kind),
                                    text);
            size_t line_len = rendered < 0 ? 0U : (size_t)rendered;
            if (line_len >= sizeof(line)) line_len = sizeof(line) - 1U;
            if (g_app_event_bus.file_mutex) xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
            if (g_app_event_bus.usb_log_enabled) rename_to_time_if_ready();
            if (g_app_event_bus.usb_log_enabled && g_app_event_bus.usb_file && line_len > 0) {
                rotate_if_needed(line_len);
                if (g_app_event_bus.usb_file) {
                    fwrite(line, 1, line_len, g_app_event_bus.usb_file);
                    g_app_event_bus.usb_current_bytes += line_len;
                    g_app_event_bus.usb_bytes_written += line_len;
                }
            }
            if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
            app_event_envelope_release(msg.envelope);
            app_event_bus_lock();
            if (g_app_event_bus.usb_pending > 0) g_app_event_bus.usb_pending--;
            app_event_bus_unlock();
            app_event_bus_notify_idle_if_drained();
        } else {
            app_event_bus_notify_idle_if_drained();
        }

        TickType_t now = xTaskGetTickCount();
        if (g_app_event_bus.usb_log_enabled && g_app_event_bus.usb_file &&
            now - last_flush >= pdMS_TO_TICKS(2000)) {
            if (g_app_event_bus.file_mutex) xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
            if (g_app_event_bus.usb_file) fflush(g_app_event_bus.usb_file);
            if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
            last_flush = now;
        }
        if (g_app_event_bus.usb_log_enabled && now - last_cleanup >= pdMS_TO_TICKS(30000)) {
            if (g_app_event_bus.file_mutex) xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
            cleanup_old_logs();
            if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
            last_cleanup = now;
        }
    }
}

/*
 * 创建 USB 日志写入任务，优先使用 PSRAM 栈，回退到内部 RAM 栈。
 * 任务栈大小 APP_EVENT_USB_TASK_STACK，优先级 APP_EVENT_USB_TASK_PRIORITY。
 *
 * 参数: 无
 * 返回: pdPASS 表示成功，其他值表示失败
 */
static BaseType_t create_usb_log_task(void)
{
#if APP_EVENT_USB_TASK_STACK_IN_PSRAM
    BaseType_t ok = xTaskCreateWithCaps(usb_log_task,
                                        "app_usb_log",
                                        APP_EVENT_USB_TASK_STACK,
                                        NULL,
                                        APP_EVENT_USB_TASK_PRIORITY,
                                        &g_app_event_bus.usb_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) return ok;
    return xTaskCreateWithCaps(usb_log_task,
                               "app_usb_log",
                               APP_EVENT_USB_TASK_STACK,
                               NULL,
                               APP_EVENT_USB_TASK_PRIORITY,
                               &g_app_event_bus.usb_task,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(usb_log_task,
                       "app_usb_log",
                       APP_EVENT_USB_TASK_STACK,
                       NULL,
                       APP_EVENT_USB_TASK_PRIORITY,
                       &g_app_event_bus.usb_task);
#endif
}

/*
 * 初始化 USB 文件输出：创建文件互斥锁，关闭旧文件，创建目录结构，
 * 生成启动时的临时文件路径并打开，重置分卷号和重命名标志。
 *
 * 参数: 无
 * 返回: true 表示文件成功打开，false 表示失败
 */
static bool start_usb_file_output(void)
{
    if (!g_app_event_bus.file_mutex) return false;
    if (g_app_event_bus.file_mutex) xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
    if (g_app_event_bus.usb_file) {
        fclose(g_app_event_bus.usb_file);
        g_app_event_bus.usb_file = NULL;
    }
    make_dirs(g_app_event_bus.usb_dir);
    build_boot_base_path(g_app_event_bus.usb_base_path, sizeof(g_app_event_bus.usb_base_path));
    g_app_event_bus.usb_part = 1;
    g_app_event_bus.renamed_with_time = false;
    bool ok = open_usb_log_file(g_app_event_bus.usb_base_path);
    if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
    return ok;
}

/*
 * 启动 USB 日志服务：创建队列、文件互斥锁，初始化日志文件和启动写入任务。
 * 启动后将 rename_pending 标记为 true 以等待 NTP 同步后重命名。
 *
 * 参数: 无
 * 返回: ESP_OK 成功，ESP_ERR_NO_MEM 内存不足，ESP_FAIL 文件/任务创建失败
 */
esp_err_t app_event_usb_init(void)
{
    if (!g_app_event_bus.file_mutex) {
        g_app_event_bus.file_mutex = xSemaphoreCreateMutex();
        if (!g_app_event_bus.file_mutex) return ESP_ERR_NO_MEM;
    }
    if (!g_app_event_bus.usb_queue) {
#if configSUPPORT_STATIC_ALLOCATION
        g_app_event_bus.usb_queue = xQueueCreateWithCaps(APP_EVENT_USB_QUEUE_DEPTH,
                                                         sizeof(app_event_usb_log_msg_t),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!g_app_event_bus.usb_queue) {
            g_app_event_bus.usb_queue = xQueueCreate(APP_EVENT_USB_QUEUE_DEPTH,
                                                     sizeof(app_event_usb_log_msg_t));
        }
        if (!g_app_event_bus.usb_queue) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_event_usb_start(void)
{
    if (!g_app_event_bus.file_mutex || !g_app_event_bus.usb_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!start_usb_file_output()) {
        return ESP_FAIL;
    }
    if (!g_app_event_bus.usb_task) {
        xEventGroupClearBits(g_app_event_bus.idle_events, APP_EVENT_USB_TASK_EXITED_BIT);
        BaseType_t ok = create_usb_log_task();
        if (ok != pdPASS) {
            if (g_app_event_bus.file_mutex) {
                xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
            }
            if (g_app_event_bus.usb_file) {
                fclose(g_app_event_bus.usb_file);
                g_app_event_bus.usb_file = NULL;
            }
            if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
            return ESP_FAIL;
        }
        app_profile_register_task("app_usb_log", g_app_event_bus.usb_task);
    }
    app_event_bus_lock();
    g_app_event_bus.usb_log_started = true;
    g_app_event_bus.rename_pending = true;
    g_app_event_bus.lifecycle = APP_EVENT_BUS_USB_LOG_READY;
    app_event_bus_unlock();
    return ESP_OK;
}

/*
 * 停止 USB 日志服务：关闭日志文件，将 usb_log_started 置为 false，
 * 等待已排队日志处理完毕，停止 writer task，关闭文件并恢复生命周期状态。
 *
 * 参数: 无
 * 返回: 无
 */
esp_err_t app_event_usb_stop(uint32_t timeout_ms)
{
    if (g_app_event_bus.usb_task) {
        app_event_usb_log_msg_t shutdown_msg;
        memset(&shutdown_msg, 0, sizeof(shutdown_msg));
        shutdown_msg.shutdown = true;
        TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
        xEventGroupClearBits(g_app_event_bus.idle_events, APP_EVENT_USB_TASK_EXITED_BIT);
        if (xQueueSend(g_app_event_bus.usb_queue, &shutdown_msg, ticks) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        EventBits_t bits = xEventGroupWaitBits(g_app_event_bus.idle_events,
                                               APP_EVENT_USB_TASK_EXITED_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               ticks);
        if ((bits & APP_EVENT_USB_TASK_EXITED_BIT) == 0) return ESP_ERR_TIMEOUT;
    }
    if (g_app_event_bus.file_mutex) xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
    if (g_app_event_bus.usb_file) {
        fclose(g_app_event_bus.usb_file);
        g_app_event_bus.usb_file = NULL;
    }
    if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
    app_event_bus_lock();
    g_app_event_bus.usb_log_started = false;
    g_app_event_bus.usb_current_path[0] = '\0';
    if (g_app_event_bus.lifecycle == APP_EVENT_BUS_USB_LOG_READY) {
        g_app_event_bus.lifecycle = APP_EVENT_BUS_CONFIGURED;
    }
    app_event_bus_unlock();
    return ESP_OK;
}

/*
 * 将一行已格式化的日志文本放入 USB 写入队列，由 usb_log_task 异步消费。
 * 队列满时记录丢弃。文本通过 app_event_strdup_len 复制后入队。
 *
 * 参数: text - 已格式化的日志行（以 \n 结尾）
 * 返回: 无
 */
void app_event_usb_enqueue(app_event_envelope_t *envelope,
                           uint32_t seq,
                           app_event_kind_t kind,
                           cc_log_level_t level)
{
    if (!g_app_event_bus.usb_queue || !envelope) return;
    app_event_envelope_retain(envelope);
    app_event_usb_log_msg_t msg = {
        .envelope = envelope,
        .seq = seq,
        .kind = kind,
        .level = level,
    };
    UBaseType_t waiting = uxQueueMessagesWaiting(g_app_event_bus.usb_queue);
    app_event_bus_lock();
    if ((uint32_t)waiting > g_app_event_bus.max_usb_queue_waiting) {
        g_app_event_bus.max_usb_queue_waiting = (uint32_t)waiting;
    }
    g_app_event_bus.usb_pending++;
    app_event_bus_unlock();
    app_event_bus_mark_sink_busy();
    if (xQueueSend(g_app_event_bus.usb_queue, &msg, 0) != pdTRUE) {
        app_event_envelope_release(envelope);
        app_event_bus_lock();
        if (g_app_event_bus.usb_pending > 0) g_app_event_bus.usb_pending--;
        g_app_event_bus.dropped++;
        g_app_event_bus.sink_dropped++;
        g_app_event_bus.usb_queue_dropped++;
        app_event_bus_unlock();
        app_event_bus_notify_idle_if_drained();
    }
}
