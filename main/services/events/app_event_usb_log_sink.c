#include "app_event_bus_internal.h"
#include "app_profile.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define APP_EVENT_USB_TASK_STACK_IN_PSRAM 1
#else
#define APP_EVENT_USB_TASK_STACK_IN_PSRAM 0
#endif

/** @brief 在状态锁内取得当前 USB 配置开关。 @return 配置启用为 true，已接纳队列消息仍由 writer 完整处理。 */
static bool usb_log_enabled(void)
{
    app_event_bus_lock();
    bool enabled = g_app_event_bus.usb_log_enabled;
    app_event_bus_unlock();
    return enabled;
}

/**
 * @brief 逐级尽力创建日志目录，后续 fopen 决定实际可写性。
 * @param[in] path 调用期目录借用，NULL 或空串无操作，内部调用保证可放入 160 字节。
 * @post 尝试 mkdir 中间层及末级，单次失败继续，输入不修改，末尾一个斜杠在局部副本中去除。
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

/**
 * @brief 根据墙钟阈值决定是否使用日期文件名。
 * @return time 值至少为 2024-01-01 UTC 的秒值时为 true，未达到或失败为 false；本函数不证明 NTP 状态。
 */
static bool time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return now >= 1704067200;
}

/**
 * @brief 按本地时间生成 cclaw-YYYYMMDD-HHMMSS.log 完整路径。
 * @param[out] out 输出缓冲，NULL 或零容量无操作。
 * @param[in] out_size 输出字节容量，含 NUL。
 * @param[in] directory 调用期目录借用，须与输出不重叠。
 * @post 有效输出先置空，时间转换、格式化或完整拼接失败保持空串，成功得到拥有在调用方缓冲中的路径。
 */
static void build_time_base_path(char *out, size_t out_size, const char *directory)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    time_t now = 0;
    struct tm tm_info;
    if (time(&now) == (time_t)-1 || !localtime_r(&now, &tm_info)) return;
    char stamp[32];
    if (!strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_info)) return;
    if (!app_event_join_path(out, out_size, directory, "cclaw-") ||
        !app_event_append_text(out, out_size, stamp) ||
        !app_event_append_text(out, out_size, ".log")) {
        out[0] = '\0';
    }
}

/**
 * @brief 用当前 tick 毫秒低 32 位构造启动日志路径。
 * @param[out] out 输出缓冲，NULL 或零容量无操作。
 * @param[in] out_size 输出字节容量，含 NUL。
 * @param[in] directory 调用期目录借用，与输出不重叠。
 * @post 成功生成 cclaw-boot-数值.log，拼接失败为空串；同名文件按追加模式使用，此命名不保证跨启动唯一。
 */
static void build_boot_base_path(char *out, size_t out_size, const char *directory)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!app_event_join_path(out, out_size, directory, "cclaw-boot-") ||
        !app_event_append_uint32(out, out_size, (uint32_t)pdTICKS_TO_MS(xTaskGetTickCount())) ||
        !app_event_append_text(out, out_size, ".log")) {
        out[0] = '\0';
    }
}

/**
 * @brief 将分卷号加入日志基础路径。
 * @param[out] out 与输入不重叠的输出缓冲，NULL 或零容量无操作。
 * @param[in] out_size 输出字节容量，含 NUL。
 * @param[in] base_path 调用期基础路径借用，NULL 输出空串。
 * @param[in] part 不大于 1 时有界复制基础路径，大于 1 时替换末尾 .log 或追加 -partN.log。
 * @post 分卷格式化放不下时输出空串；内部基础路径槽小于输出槽，首卷完整复制。
 */
static void build_part_path(char *out, size_t out_size, const char *base_path, int part)
{
    if (!out || !out_size) return;
    out[0] = '\0';
    if (!base_path) return;
    if (part <= 1) {
        app_event_copy_text(out, out_size, base_path);
        return;
    }
    const char *dot = strrchr(base_path, '.');
    if (dot && strcmp(dot, ".log") == 0) {
        size_t prefix_len = (size_t)(dot - base_path);
        if (prefix_len >= out_size) return;
        memcpy(out, base_path, prefix_len);
        out[prefix_len] = '\0';
        int written = snprintf(out + prefix_len, out_size - prefix_len, "-part%d.log", part);
        if (written < 0 || (size_t)written >= out_size - prefix_len) out[0] = '\0';
    } else {
        int written = snprintf(out, out_size, "%s-part%d.log", base_path, part);
        if (written < 0 || (size_t)written >= out_size) out[0] = '\0';
    }
}

/**
 * @brief 关闭旧 FILE 并追加打开指定日志，记录打开后真实文件大小。
 * @param[in] path 必需非空 NUL 路径借用，可为当前路径槽本身。
 * @return 成功为 true；关闭、打开、fstat 或大小表示失败为 false，FILE 已消费时槽设 NULL，状态路径在新文件验证成功后才更新。
 * @pre 调用者持 file_mutex，写状态路径与计数时内部再取状态锁，调用者不持状态锁等待 file_mutex。
 */
static bool open_usb_log_file(const char *path)
{
    if (!path || !path[0]) return false;
    if (g_app_event_bus.usb_file) {
        int close_result = fclose(g_app_event_bus.usb_file);
        g_app_event_bus.usb_file = NULL;
        if (close_result != 0) return false;
    }
    g_app_event_bus.usb_file = fopen(path, "a");
    if (!g_app_event_bus.usb_file) {
        return false;
    }
    struct stat info;
    if (fstat(fileno(g_app_event_bus.usb_file), &info) != 0 || info.st_size < 0 ||
        (uint64_t)info.st_size > SIZE_MAX) {
        fclose(g_app_event_bus.usb_file);
        g_app_event_bus.usb_file = NULL;
        return false;
    }
    app_event_bus_lock();
    app_event_copy_text(g_app_event_bus.usb_current_path,
                        sizeof(g_app_event_bus.usb_current_path),
                        path);
    g_app_event_bus.usb_current_bytes = (size_t)info.st_size;
    app_event_bus_unlock();
    return true;
}

/**
 * @brief 在下一行超过当前卷阈值时尝试切换一个新分卷。
 * @param[in] next_len 下一行完整字节数。
 * @return 当前文件可写或换卷成功为 true，无 FILE、分卷号到 INT_MAX、路径或文件操作失败为 false。
 * @pre 持 file_mutex，配置阈值通过状态锁取得。
 * @note 零阈值免大小限制，一次仅换一卷，单行可大于阈值，同名既有卷继续追加。
 */
static bool rotate_if_needed(size_t next_len)
{
    app_event_bus_lock();
    size_t maximum = g_app_event_bus.usb_max_bytes;
    app_event_bus_unlock();
    if (!g_app_event_bus.usb_file || maximum == 0 ||
        (g_app_event_bus.usb_current_bytes <= maximum &&
         next_len <= maximum - g_app_event_bus.usb_current_bytes)) {
        return g_app_event_bus.usb_file != NULL;
    }
    if (g_app_event_bus.usb_part == INT_MAX) return false;
    int next_part = g_app_event_bus.usb_part + 1;
    char next_path[sizeof(g_app_event_bus.usb_current_path)];
    build_part_path(next_path, sizeof(next_path), g_app_event_bus.usb_base_path, next_part);
    if (!next_path[0]) return false;
    if (!open_usb_log_file(next_path)) return false;
    g_app_event_bus.usb_part = next_part;
    return true;
}

/**
 * @brief 按有效时间将当前启动卷改名，再恢复追加 FILE。
 * @return 无请求、时间/命名暂不可用、目标已存在而暂缓或成功恢复 FILE 为 true；目标探测 I/O、关闭或重开失败为 false，rename 失败时尝试重开原路径并保留请求供重试。
 * @pre 持 file_mutex，基于当前文件目录生成目标，避免配置热更新改变旧卷目录。
 * @post rename 成功或目标就是当前路径时提交命名标志；成功安装后重开仍可能失败，后续消息按已安装路径重试。已有其他目标保留并继续写原卷，后续按墙钟重试。
 * @note 目录由此生命周期 owner 排他管理，存在探测和 rename 间的外部并发改名不具备跨 VFS 的原子不覆盖保证。
 */
static bool rename_to_time_if_ready(void)
{
    app_event_bus_lock();
    bool requested = !g_app_event_bus.renamed_with_time && g_app_event_bus.rename_pending;
    app_event_bus_unlock();
    if (!requested || !time_is_valid()) {
        return true;
    }
    char new_base[sizeof(g_app_event_bus.usb_base_path)];
    char new_path[sizeof(g_app_event_bus.usb_current_path)];
    char directory[sizeof(g_app_event_bus.usb_current_path)];
    app_event_copy_text(directory, sizeof(directory), g_app_event_bus.usb_current_path);
    char *slash = strrchr(directory, '/');
    if (!slash) return true;
    if (slash == directory) slash[1] = '\0';
    else *slash = '\0';
    build_time_base_path(new_base, sizeof(new_base), directory);
    if (!new_base[0]) return true;
    build_part_path(new_path, sizeof(new_path), new_base, g_app_event_bus.usb_part);
    if (!new_path[0]) return false;
    if (strcmp(g_app_event_bus.usb_current_path, new_path) == 0) {
        app_event_bus_lock();
        g_app_event_bus.renamed_with_time = true;
        g_app_event_bus.rename_pending = false;
        app_event_bus_unlock();
        return true;
    }
    struct stat existing;
    if (stat(new_path, &existing) == 0) return true;
    if (errno != ENOENT) return false;
    if (g_app_event_bus.usb_file) {
        int closed = fclose(g_app_event_bus.usb_file);
        g_app_event_bus.usb_file = NULL;
        if (closed != 0) return false;
    }
    if (rename(g_app_event_bus.usb_current_path, new_path) != 0) {
        return open_usb_log_file(g_app_event_bus.usb_current_path);
    }
    app_event_copy_text(g_app_event_bus.usb_base_path,
                        sizeof(g_app_event_bus.usb_base_path),
                        new_base);
    app_event_bus_lock();
    app_event_copy_text(g_app_event_bus.usb_current_path,
                        sizeof(g_app_event_bus.usb_current_path),
                        new_path);
    g_app_event_bus.renamed_with_time = true;
    g_app_event_bus.rename_pending = false;
    app_event_bus_unlock();
    return open_usb_log_file(g_app_event_bus.usb_current_path);
}

/**
 * @brief 识别清理扫描可处理的日志名称。
 * @param[in] name 调用期 NUL 名称借用，可 NULL。
 * @return 同时以 cclaw- 开头及 .log 结尾为 true，其他为 false。
 */
static bool is_log_name(const char *name)
{
    if (!name) return false;
    size_t length = strlen(name);
    return length >= 10 && strncmp(name, "cclaw-", 6) == 0 && strcmp(name + length - 4, ".log") == 0;
}

/**
 * @brief 对有界目录候选按 mtime 排序并尽力删除超量旧日志。
 * @pre 持 file_mutex，内部复制目录和保留数时取状态锁。
 * @post 最多缓存扫描到的 48 个匹配常规文件，当前打开路径保留；只将成功删除计入减少量，目录/分配/删除失败不阻断 writer。保留数非正时不清理，缓存外文件不保证本轮受限。
 */
static void cleanup_old_logs(void)
{
    char directory[sizeof(g_app_event_bus.usb_dir)];
    app_event_bus_lock();
    int max_files = g_app_event_bus.usb_max_files;
    app_event_copy_text(directory, sizeof(directory), g_app_event_bus.usb_dir);
    app_event_bus_unlock();
    if (max_files <= 0) return;
    DIR *dir = opendir(directory);
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
                                 directory,
                                 ent->d_name)) {
            continue;
        }
        struct stat st;
        if (stat(entries[count].path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        entries[count].mtime = st.st_mtime;
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
    size_t keep = (size_t)max_files;
    size_t remaining = count;
    for (size_t i = 0; remaining > keep && i < count; i++) {
        if (strcmp(entries[i].path, g_app_event_bus.usb_current_path) != 0) {
            if (remove(entries[i].path) == 0) remaining--;
        }
    }
    heap_caps_free(entries);
}

/**
 * @brief 消费拥有正文引用的 USB 队列，串行写文件并处理停止哨兵。
 * @param[in] arg 未使用的任务参数，创建时为 NULL。
 * @post 每条正文归还引用与 pending，失败增加饱和统计；已接纳消息覆盖配置关闭继续处理。停止哨兵前已排空正文，最终 flush、注销 profile、SetBits 返回后 release 发布完成位，此后仅原生自删并返回。
 * @note FILE/base/part 由 file_mutex 保护，状态及统计写入另持状态锁；启用时约每 2 秒 flush、30 秒有界清理，原生 I/O 受后端阻塞影响。
 */
static void usb_log_task(void *arg)
{
    (void)arg;
    app_profile_register_task("app_usb_log", xTaskGetCurrentTaskHandle());
    TickType_t last_flush = xTaskGetTickCount();
    TickType_t last_cleanup = last_flush;
    app_event_usb_log_msg_t msg;
    while (true) {
        if (xQueueReceive(g_app_event_bus.usb_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (msg.shutdown) {
                bool flush_failed = false;
                int flush_error = 0;
                if (g_app_event_bus.file_mutex) {
                    xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
                }
                if (g_app_event_bus.usb_file && fflush(g_app_event_bus.usb_file) != 0) {
                    flush_failed = true;
                    flush_error = errno;
                }
                if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
                if (flush_failed) {
                    app_event_bus_lock();
                    app_event_counter_increment(&g_app_event_bus.usb_queue_dropped);
                    app_event_counter_increment(&g_app_event_bus.sink_dropped);
                    app_event_bus_unlock();
                    printf("[usb-log] final flush failed errno=%d\r\n", flush_error);
                }
                app_profile_unregister_task(xTaskGetCurrentTaskHandle());
                app_event_bus_lock();
                g_app_event_bus.usb_log_started = false;
                EventGroupHandle_t idle_events = g_app_event_bus.idle_events;
                app_event_bus_unlock();
                xEventGroupSetBits(idle_events,
                                   APP_EVENT_USB_TASK_EXITED_BIT | APP_EVENT_BUS_IDLE_BIT);
                /* 这是 writer 最后一次访问总线状态，owner 必须 acquire 观察此值后再释放原语。 */
                __atomic_store_n(&g_app_event_bus.usb_task_completed, true, __ATOMIC_RELEASE);
#if APP_EVENT_USB_TASK_STACK_IN_PSRAM
                vTaskDeleteWithCaps(NULL);
#else
                vTaskDelete(NULL);
#endif
                return;
            }
            char line[APP_EVENT_TEXT_MAX + 96U];
            const char *text = msg.envelope ? msg.envelope->text : "";
            int rendered = snprintf(line, sizeof(line), "%" PRIu32 " [%s] [%s] %s\n",
                                    msg.seq,
                                    app_event_log_level_string(msg.level),
                                    app_event_kind_string(msg.kind),
                                    text);
            bool write_failed = rendered < 0 || (size_t)rendered >= sizeof(line);
            size_t line_len = write_failed ? 0U : (size_t)rendered;
            int write_error = 0;
            if (g_app_event_bus.file_mutex) xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
            if (usb_log_enabled() && !rename_to_time_if_ready()) {
                write_failed = true;
                write_error = errno;
            }
            /* 关闭配置只终止新接纳，已入队正文继续完成写入。 */
            if (!write_failed && line_len > 0) {
                if (!g_app_event_bus.usb_file &&
                    (!g_app_event_bus.usb_current_path[0] ||
                     !open_usb_log_file(g_app_event_bus.usb_current_path))) {
                    write_failed = true;
                    write_error = errno;
                }
                if (!write_failed && !rotate_if_needed(line_len)) {
                    write_failed = true;
                    write_error = errno;
                }
                if (!write_failed && g_app_event_bus.usb_file) {
                    size_t written = fwrite(line, 1, line_len, g_app_event_bus.usb_file);
                    bool failed = written != line_len || ferror(g_app_event_bus.usb_file);
                    app_event_bus_lock();
                    g_app_event_bus.usb_current_bytes = written > SIZE_MAX - g_app_event_bus.usb_current_bytes ?
                        SIZE_MAX : g_app_event_bus.usb_current_bytes + written;
                    g_app_event_bus.usb_bytes_written = written > SIZE_MAX - g_app_event_bus.usb_bytes_written ?
                        SIZE_MAX : g_app_event_bus.usb_bytes_written + written;
                    app_event_bus_unlock();
                    if (failed) {
                        write_failed = true;
                        write_error = errno;
                        fclose(g_app_event_bus.usb_file);
                        g_app_event_bus.usb_file = NULL;
                    }
                }
            }
            if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
            if (write_failed) {
                app_event_bus_lock();
                app_event_counter_increment(&g_app_event_bus.usb_queue_dropped);
                app_event_counter_increment(&g_app_event_bus.sink_dropped);
                app_event_bus_unlock();
                printf("[usb-log] write/rotation failed errno=%d; log entry dropped\r\n", write_error);
            }
            app_event_envelope_release(msg.envelope);
            app_event_bus_lock();
            if (g_app_event_bus.usb_pending > 0) g_app_event_bus.usb_pending--;
            app_event_bus_unlock();
            app_event_bus_notify_idle_if_drained();
        } else {
            app_event_bus_notify_idle_if_drained();
        }

        TickType_t now = xTaskGetTickCount();
        if (usb_log_enabled() &&
            now - last_flush >= pdMS_TO_TICKS(2000)) {
            bool flush_failed = false;
            int flush_error = 0;
            if (g_app_event_bus.file_mutex) xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
            if (g_app_event_bus.usb_file && fflush(g_app_event_bus.usb_file) != 0) {
                flush_failed = true;
                flush_error = errno;
                fclose(g_app_event_bus.usb_file);
                g_app_event_bus.usb_file = NULL;
            }
            if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
            if (flush_failed) {
                app_event_bus_lock();
                app_event_counter_increment(&g_app_event_bus.usb_queue_dropped);
                app_event_counter_increment(&g_app_event_bus.sink_dropped);
                app_event_bus_unlock();
                printf("[usb-log] flush failed errno=%d\r\n", flush_error);
            }
            last_flush = now;
        }
        if (usb_log_enabled() && now - last_cleanup >= pdMS_TO_TICKS(30000)) {
            if (g_app_event_bus.file_mutex) xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
            cleanup_old_logs();
            if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
            last_cleanup = now;
        }
    }
}

/**
 * @brief 创建独立运行的 USB writer，配置允许时优先 PSRAM 栈再内部 RAM。
 * @param[out] out_task 必需句柄槽，先清 NULL，成功由 owner 发布并管理退场。
 * @return 原生创建结果，pdPASS 成功，失败不发布任务。
 * @pre owner 持状态锁覆盖原生创建和句柄发布，真实创建将任务调度到独立上下文，禁止替身在此同步重入任务函数。
 */
static BaseType_t create_usb_log_task(TaskHandle_t *out_task)
{
    *out_task = NULL;
#if APP_EVENT_USB_TASK_STACK_IN_PSRAM
    BaseType_t ok = xTaskCreateWithCaps(usb_log_task,
                                        "app_usb_log",
                                        APP_EVENT_USB_TASK_STACK,
                                        NULL,
                                        APP_EVENT_USB_TASK_PRIORITY,
                                        out_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) return ok;
    *out_task = NULL;
    return xTaskCreateWithCaps(usb_log_task,
                               "app_usb_log",
                               APP_EVENT_USB_TASK_STACK,
                               NULL,
                               APP_EVENT_USB_TASK_PRIORITY,
                               out_task,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(usb_log_task,
                       "app_usb_log",
                       APP_EVENT_USB_TASK_STACK,
                       NULL,
                       APP_EVENT_USB_TASK_PRIORITY,
                       out_task);
#endif
}

/**
 * @brief 在 file_mutex 内重建启动卷并重置分卷信息。
 * @return 必需 mutex 存在且新文件验证成功为 true，其他为 false。
 * @pre 生命周期 owner 串行启停，目录通过状态锁复制，writer 文件操作由 file_mutex 排斥。
 * @note 旧 FILE 在开始时消费，目录创建尽力执行，后续打开决定成功，失败仍可能留下已创建目录或空日志文件。
 */
static bool start_usb_file_output(void)
{
    if (!g_app_event_bus.file_mutex) return false;
    if (g_app_event_bus.file_mutex) xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
    if (g_app_event_bus.usb_file) {
        int closed = fclose(g_app_event_bus.usb_file);
        g_app_event_bus.usb_file = NULL;
        if (closed != 0) {
            xSemaphoreGive(g_app_event_bus.file_mutex);
            return false;
        }
    }
    char directory[sizeof(g_app_event_bus.usb_dir)];
    app_event_bus_lock();
    app_event_copy_text(directory, sizeof(directory), g_app_event_bus.usb_dir);
    app_event_bus_unlock();
    make_dirs(directory);
    build_boot_base_path(g_app_event_bus.usb_base_path, sizeof(g_app_event_bus.usb_base_path), directory);
    g_app_event_bus.usb_part = 1;
    app_event_bus_lock();
    g_app_event_bus.renamed_with_time = false;
    app_event_bus_unlock();
    bool ok = open_usb_log_file(g_app_event_bus.usb_base_path);
    if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
    return ok;
}

/**
 * @brief 为 USB sink 补齐文件 mutex 和固定深度队列，不启动 writer。
 * @return 全部可用为 ESP_OK，分配失败为 NO_MEM；已创建原语仍归总线拥有，后续 init 或 destroy 可接续。
 * @pre 生命周期 owner 独占初始化，能力队列失败回退普通队列并记录实际释放方式。
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
        g_app_event_bus.usb_queue_with_caps = g_app_event_bus.usb_queue != NULL;
#endif
        if (!g_app_event_bus.usb_queue) {
            g_app_event_bus.usb_queue = xQueueCreate(APP_EVENT_USB_QUEUE_DEPTH,
                                                     sizeof(app_event_usb_log_msg_t));
        }
        if (!g_app_event_bus.usb_queue) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/** @brief 为生命周期 owner 打开新启动卷并保证 writer 存在。 @return 原语不足为 INVALID_STATE，旧任务停止错误透传，文件或任务失败为 ESP_FAIL，成功为 ESP_OK。 @pre 与 init/stop 串行，正在停止或已完成的旧句柄先退场再重用，任务完成位重置与新句柄发布都在状态锁内。 @post 成功打开接纳并登记时间命名请求，失败文件句柄按实际阶段清理。 */
esp_err_t app_event_usb_start(void)
{
    if (!g_app_event_bus.file_mutex || !g_app_event_bus.usb_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    app_event_bus_lock();
    bool retire_previous = g_app_event_bus.usb_task &&
        (g_app_event_bus.usb_stop_requested ||
         __atomic_load_n(&g_app_event_bus.usb_task_completed, __ATOMIC_ACQUIRE));
    app_event_bus_unlock();
    if (retire_previous) {
        esp_err_t retired = app_event_usb_stop(2000U);
        if (retired != ESP_OK) return retired;
    }
    if (!start_usb_file_output()) {
        return ESP_FAIL;
    }
    BaseType_t created = pdPASS;
    app_event_bus_lock();
    if (!g_app_event_bus.usb_task) {
        __atomic_store_n(&g_app_event_bus.usb_task_completed, false, __ATOMIC_RELAXED);
        xEventGroupClearBits(g_app_event_bus.idle_events, APP_EVENT_USB_TASK_EXITED_BIT);
        TaskHandle_t task = NULL;
        created = create_usb_log_task(&task);
        if (created == pdPASS) g_app_event_bus.usb_task = task;
    }
    if (created == pdPASS) {
        g_app_event_bus.usb_log_started = true;
        g_app_event_bus.rename_pending = true;
        g_app_event_bus.lifecycle = APP_EVENT_BUS_USB_LOG_READY;
    }
    app_event_bus_unlock();
    if (created != pdPASS) {
        xSemaphoreTake(g_app_event_bus.file_mutex, portMAX_DELAY);
        if (g_app_event_bus.usb_file) {
            fclose(g_app_event_bus.usb_file);
            g_app_event_bus.usb_file = NULL;
        }
        xSemaphoreGive(g_app_event_bus.file_mutex);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief 在外层共享预算内关闭接纳、排空正文并等待 writer 完成发布。
 * @param[in] start 同一等待链初始 tick。
 * @param[in] budget 有限 tick 预算，零只作即时尝试。
 * @return 完整停止为 ESP_OK，自停为 INVALID_STATE，等待到期为 TIMEOUT，fclose 失败为 ESP_FAIL；错误保留句柄与原语供重试，已消费的 FILE 槽清 NULL。
 * @pre 生命周期 owner 串行启停并维持总线原语存活，不能从活跃 writer 自停。
 * @post acquire 观察 writer 的最终完成位后才清任务句柄，避免 SetBits 唤醒尾部与 TCB 地址复用误判，内核回收可随后完成。
 */
esp_err_t app_event_usb_stop_until(TickType_t start, TickType_t budget)
{
    bool close_failed = false;
    /* 先关闭接纳，再等已预留 pending 的发布者入队及消费，停止哨兵不能越过已接纳正文。 */
    app_event_bus_lock();
    TaskHandle_t current_task = g_app_event_bus.usb_task;
    if (current_task == xTaskGetCurrentTaskHandle() && current_task &&
        !__atomic_load_n(&g_app_event_bus.usb_task_completed, __ATOMIC_ACQUIRE)) {
        app_event_bus_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    g_app_event_bus.usb_log_started = false;
    app_event_bus_unlock();
    for (;;) {
        app_event_bus_lock();
        uint32_t pending = g_app_event_bus.usb_pending;
        app_event_bus_unlock();
        if (!pending) break;
        if (!app_event_remaining_ticks(start, budget))
            return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    app_event_bus_lock();
    TaskHandle_t task = g_app_event_bus.usb_task;
    bool send_shutdown = task && !g_app_event_bus.usb_stop_requested;
    if (send_shutdown) g_app_event_bus.usb_stop_requested = true;
    app_event_bus_unlock();
    if (send_shutdown) {
        app_event_usb_log_msg_t shutdown_msg;
        memset(&shutdown_msg, 0, sizeof(shutdown_msg));
        shutdown_msg.shutdown = true;
        TickType_t ticks = app_event_remaining_ticks(start, budget);
        xEventGroupClearBits(g_app_event_bus.idle_events, APP_EVENT_USB_TASK_EXITED_BIT);
        if (xQueueSend(g_app_event_bus.usb_queue, &shutdown_msg, ticks) != pdTRUE) {
            app_event_bus_lock();
            g_app_event_bus.usb_stop_requested = false;
            app_event_bus_unlock();
            return ESP_ERR_TIMEOUT;
        }
    }
    if (task) {
        TickType_t ticks = app_event_remaining_ticks(start, budget);
        EventBits_t bits = xEventGroupWaitBits(g_app_event_bus.idle_events,
                                               APP_EVENT_USB_TASK_EXITED_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               ticks);
        if ((bits & APP_EVENT_USB_TASK_EXITED_BIT) == 0) return ESP_ERR_TIMEOUT;
        while (!__atomic_load_n(&g_app_event_bus.usb_task_completed, __ATOMIC_ACQUIRE)) {
            if (!app_event_remaining_ticks(start, budget)) return ESP_ERR_TIMEOUT;
            vTaskDelay(1);
        }
    }
    if (g_app_event_bus.file_mutex &&
        xSemaphoreTake(g_app_event_bus.file_mutex, app_event_remaining_ticks(start, budget)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (g_app_event_bus.usb_file) {
        close_failed = fclose(g_app_event_bus.usb_file) != 0;
        g_app_event_bus.usb_file = NULL;
    }
    if (g_app_event_bus.file_mutex) xSemaphoreGive(g_app_event_bus.file_mutex);
    if (close_failed) return ESP_FAIL;
    app_event_bus_lock();
    g_app_event_bus.usb_task = NULL;
    g_app_event_bus.usb_log_started = false;
    g_app_event_bus.usb_stop_requested = false;
    g_app_event_bus.usb_current_path[0] = '\0';
    if (g_app_event_bus.lifecycle == APP_EVENT_BUS_USB_LOG_READY) {
        g_app_event_bus.lifecycle = APP_EVENT_BUS_CONFIGURED;
    }
    app_event_bus_unlock();
    return ESP_OK;
}

/** @brief 在单一有限毫秒预算内停止 USB writer。 @param[in] timeout_ms 毫秒数，零为不等待尝试。 @return 停止结果，超时保留已接纳工作、任务与原语供 owner 重试。 */
esp_err_t app_event_usb_stop(uint32_t timeout_ms)
{
    return app_event_usb_stop_until(xTaskGetTickCount(), app_event_timeout_ticks(timeout_ms));
}

/**
 * @brief 为已格式化元数据和正文借用取得独立引用并零等待入队。
 * @param[in,out] envelope 调用者保留的有效正文引用，NULL 无操作，成功入队时新增队列引用。
 * @param[in] seq 已分配事件序号。
 * @param[in] kind 按值事件种类。
 * @param[in] level 按值日志级别。
 * @post 接纳时先登记 pending 再尝试入队，队列满回滚 pending 和引用并饱和计数；关闭或维护状态不接纳，调用者原引用始终保留。
 */
void app_event_usb_enqueue(app_event_envelope_t *envelope,
                           uint32_t seq,
                           app_event_kind_t kind,
                           cc_log_level_t level)
{
    if (!envelope || !app_event_bus_acquire()) return;
    app_event_bus_lock();
    bool accepting = g_app_event_bus.usb_queue && g_app_event_bus.usb_log_started &&
        g_app_event_bus.usb_log_enabled && !g_app_event_bus.maintenance_mode &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_DRAINING &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_STOPPED &&
        g_app_event_bus.lifecycle != APP_EVENT_BUS_FAILED;
    if (accepting) g_app_event_bus.usb_pending++;
    app_event_bus_unlock();
    if (!accepting) { app_event_bus_release(); return; }
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
    app_event_bus_unlock();
    app_event_bus_mark_sink_busy();
    if (xQueueSend(g_app_event_bus.usb_queue, &msg, 0) != pdTRUE) {
        app_event_envelope_release(envelope);
        app_event_bus_lock();
        if (g_app_event_bus.usb_pending > 0) g_app_event_bus.usb_pending--;
        app_event_counter_increment(&g_app_event_bus.dropped);
        app_event_counter_increment(&g_app_event_bus.sink_dropped);
        app_event_counter_increment(&g_app_event_bus.usb_queue_dropped);
        app_event_bus_unlock();
        app_event_bus_notify_idle_if_drained();
    }
    app_event_bus_release();
}
