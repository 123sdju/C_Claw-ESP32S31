#include "ota_service.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_event_bus.h"
#include "app_lifecycle.h"
#include "board_camera.h"
#include "board_ui.h"
#include "cc/util/cc_string_builder.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "interaction_service.h"
#include "network_service.h"
#include "ota_manager.h"

#define OTA_PREPARE_INTERACTION_TIMEOUT_MS 30000U
#define OTA_PREPARE_EVENT_TIMEOUT_MS 5000U
#define OTA_PROGRESS_LCD_STEP_BYTES (256U * 1024U)
#define OTA_FLASH_TASK_STACK_BYTES 8192U
#define OTA_FLASH_START_DELAY_MS 800U
#define OTA_REBOOT_TASK_STACK_BYTES 4096U
#define OTA_REBOOT_DELAY_MS 1200U
#define OTA_IO_CHUNK_FALLBACK 4096U
#define OTA_PSRAM_RESERVE_BYTES (256U * 1024U)
#define OTA_INTERNAL_RESERVE_BYTES (64U * 1024U)
#define OTA_UPLOAD_TOTAL_TIMEOUT_MS 180000ULL
#define OTA_UPLOAD_IDLE_TIMEOUT_MS 30000ULL
#define OTA_UPLOAD_MAX_CONSECUTIVE_TIMEOUTS 5U

static ota_service_transport_t s_transport;

typedef enum {
    OTA_SERVICE_STAGE_IDLE = 0,
    OTA_SERVICE_STAGE_PREPARING,
    OTA_SERVICE_STAGE_PREPARED,
    OTA_SERVICE_STAGE_FLASHING,
    OTA_SERVICE_STAGE_REBOOT_PENDING,
    OTA_SERVICE_STAGE_FAILED,
} ota_service_stage_t;

typedef enum {
    OTA_SERVICE_FAILURE_NONE = 0,
    OTA_SERVICE_FAILURE_BAD_REQUEST,
    OTA_SERVICE_FAILURE_PARTITION_TOO_SMALL,
    OTA_SERVICE_FAILURE_NO_UPDATE_PARTITION,
    OTA_SERVICE_FAILURE_PSRAM_INSUFFICIENT,
    OTA_SERVICE_FAILURE_INTERNAL_RAM_INSUFFICIENT,
    OTA_SERVICE_FAILURE_SIZE_MISMATCH,
    OTA_SERVICE_FAILURE_HASH_MISMATCH,
    OTA_SERVICE_FAILURE_UPLOAD_TIMEOUT,
    OTA_SERVICE_FAILURE_IMAGE_INVALID,
    OTA_SERVICE_FAILURE_UPLOAD_IN_PROGRESS,
    OTA_SERVICE_FAILURE_RECV_FAILED,
    OTA_SERVICE_FAILURE_FLASH_WRITE,
    OTA_SERVICE_FAILURE_FLASH_FINISH,
    OTA_SERVICE_FAILURE_TASK_START,
    OTA_SERVICE_FAILURE_REBOOT_PENDING,
    OTA_SERVICE_FAILURE_CONCURRENT_STATE,
} ota_service_failure_t;

typedef struct {
    size_t expected_size;
    size_t target_size;
    size_t psram_free;
    size_t psram_largest;
    size_t psram_reserve;
    size_t internal_free;
    size_t internal_largest;
    size_t internal_reserve;
    size_t io_chunk;
    char target_label[24];
} ota_service_resource_snapshot_t;

static SemaphoreHandle_t s_ota_mutex;
static bool s_ota_preview_was_running;
static bool s_ota_preparing;
static bool s_ota_prepared;
static size_t s_ota_prepared_size;
static char s_ota_prepared_name[96];
static char s_ota_prepared_time[40];
static char s_ota_prepared_sha256[65];
static bool s_ota_reboot_pending;
static bool s_ota_flash_services_stopped;
static ota_service_stage_t s_ota_stage;
static ota_service_failure_t s_ota_failure_code;
static char s_ota_failure_stage[32];
static char s_ota_failure_message[192];
static ota_service_resource_snapshot_t s_ota_last_resources;

typedef struct {
    uint8_t *image;
    size_t size;
    char name[96];
} ota_flash_job_t;

typedef struct {
    bool preview_was_running;
    bool preparing;
    bool prepared;
    bool reboot_pending;
    size_t prepared_size;
    char prepared_name[96];
    char prepared_time[40];
    char prepared_sha256[65];
    ota_service_stage_t stage;
    ota_service_failure_t failure_code;
    char failure_stage[32];
    char failure_message[192];
    ota_service_resource_snapshot_t resources;
} ota_service_state_snapshot_t;

/*
 * 创建互斥锁（如尚未存在）。
 * 返回: true 成功
 */
/*
 * 获取互斥锁（阻塞）。
 * 返回: true 成功
 */
static bool ota_state_lock(void)
{
    if (!s_ota_mutex) return false;
    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    return true;
}

/*
 * 释放互斥锁。
 */
static void ota_state_unlock(void)
{
    if (s_ota_mutex) {
        xSemaphoreGive(s_ota_mutex);
    }
}

/*
 * 清除已准备的固件元数据（锁定条件下）。
 */
static void ota_state_clear_prepared_locked(void)
{
    s_ota_prepared = false;
    s_ota_prepared_size = 0;
    s_ota_prepared_name[0] = '\0';
    s_ota_prepared_time[0] = '\0';
    s_ota_prepared_sha256[0] = '\0';
}

/*
 * 将 ota_service_stage_t 转换为可读字符串。
 */
static const char *ota_service_stage_string(ota_service_stage_t stage)
{
    switch (stage) {
    case OTA_SERVICE_STAGE_IDLE: return "idle";
    case OTA_SERVICE_STAGE_PREPARING: return "preparing";
    case OTA_SERVICE_STAGE_PREPARED: return "prepared";
    case OTA_SERVICE_STAGE_FLASHING: return "flashing";
    case OTA_SERVICE_STAGE_REBOOT_PENDING: return "reboot_pending";
    case OTA_SERVICE_STAGE_FAILED: return "failed";
    default: return "unknown";
    }
}

/*
 * 将 ota_service_failure_t 转换为可读字符串。
 */
static const char *ota_service_failure_string(ota_service_failure_t code)
{
    switch (code) {
    case OTA_SERVICE_FAILURE_NONE: return "none";
    case OTA_SERVICE_FAILURE_BAD_REQUEST: return "bad_request";
    case OTA_SERVICE_FAILURE_PARTITION_TOO_SMALL: return "partition_too_small";
    case OTA_SERVICE_FAILURE_NO_UPDATE_PARTITION: return "no_update_partition";
    case OTA_SERVICE_FAILURE_PSRAM_INSUFFICIENT: return "psram_insufficient";
    case OTA_SERVICE_FAILURE_INTERNAL_RAM_INSUFFICIENT: return "internal_ram_insufficient";
    case OTA_SERVICE_FAILURE_SIZE_MISMATCH: return "size_mismatch";
    case OTA_SERVICE_FAILURE_HASH_MISMATCH: return "hash_mismatch";
    case OTA_SERVICE_FAILURE_UPLOAD_TIMEOUT: return "upload_timeout";
    case OTA_SERVICE_FAILURE_IMAGE_INVALID: return "image_invalid";
    case OTA_SERVICE_FAILURE_UPLOAD_IN_PROGRESS: return "upload_in_progress";
    case OTA_SERVICE_FAILURE_RECV_FAILED: return "recv_failed";
    case OTA_SERVICE_FAILURE_FLASH_WRITE: return "flash_write";
    case OTA_SERVICE_FAILURE_FLASH_FINISH: return "flash_finish";
    case OTA_SERVICE_FAILURE_TASK_START: return "task_start";
    case OTA_SERVICE_FAILURE_REBOOT_PENDING: return "reboot_pending";
    case OTA_SERVICE_FAILURE_CONCURRENT_STATE: return "concurrent_state";
    default: return "unknown";
    }
}

/*
 * 设置 OTA 阶段（线程安全）。
 */
static void ota_set_stage(ota_service_stage_t stage)
{
    if (ota_state_lock()) {
        s_ota_stage = stage;
        ota_state_unlock();
    }
}

/*
 * 清除失败记录字段。
 */
static void ota_clear_failure_locked(void)
{
    s_ota_failure_code = OTA_SERVICE_FAILURE_NONE;
    s_ota_failure_stage[0] = '\0';
    s_ota_failure_message[0] = '\0';
}

/*
 * 记录失败信息（阶段码、阶段名、消息）。
 */
static void ota_record_failure(ota_service_failure_t code, const char *stage, const char *message)
{
    if (ota_state_lock()) {
        s_ota_stage = OTA_SERVICE_STAGE_FAILED;
        s_ota_failure_code = code;
        snprintf(s_ota_failure_stage, sizeof(s_ota_failure_stage), "%s", stage ? stage : "");
        snprintf(s_ota_failure_message, sizeof(s_ota_failure_message), "%s", message ? message : "");
        ota_state_unlock();
    }
}

/*
 * 创建 OTA 状态快照（线程安全）。
 */
static ota_service_state_snapshot_t ota_state_snapshot(void)
{
    ota_service_state_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (ota_state_lock()) {
        snapshot.preview_was_running = s_ota_preview_was_running;
        snapshot.preparing = s_ota_preparing;
        snapshot.prepared = s_ota_prepared;
        snapshot.reboot_pending = s_ota_reboot_pending;
        snapshot.prepared_size = s_ota_prepared_size;
        snprintf(snapshot.prepared_name, sizeof(snapshot.prepared_name), "%s", s_ota_prepared_name);
        snprintf(snapshot.prepared_time, sizeof(snapshot.prepared_time), "%s", s_ota_prepared_time);
        snprintf(snapshot.prepared_sha256, sizeof(snapshot.prepared_sha256), "%s",
                 s_ota_prepared_sha256);
        snapshot.stage = s_ota_stage;
        snapshot.failure_code = s_ota_failure_code;
        snprintf(snapshot.failure_stage, sizeof(snapshot.failure_stage), "%s", s_ota_failure_stage);
        snprintf(snapshot.failure_message, sizeof(snapshot.failure_message), "%s", s_ota_failure_message);
        snapshot.resources = s_ota_last_resources;
        ota_state_unlock();
    }
    return snapshot;
}

/*
 * 初始化 OTA 服务的传输层接口和互斥锁。
 */
esp_err_t ota_service_init(const ota_service_transport_t *transport)
{
    if (!transport ||
        !transport->send_text ||
        !transport->send_error ||
        !transport->query_value ||
        !transport->recv ||
        !transport->stop_web ||
        !transport->start_web) {
        return ESP_ERR_INVALID_ARG;
    }
    s_transport = *transport;
    if (s_transport.io_chunk == 0) {
        s_transport.io_chunk = OTA_IO_CHUNK_FALLBACK;
    }
    if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();
    if (!s_ota_mutex) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

/*
 * 分配 IO 缓冲区（优先使用传输层 alloc_io，否则用内部 RAM）。
 */
static char *ota_alloc_io(size_t size)
{
    if (s_transport.alloc_io) {
        return s_transport.alloc_io(size);
    }
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/*
 * 通过传输层发送文本响应。
 */
static esp_err_t ota_send_text(void *request, const char *text, const char *type)
{
    return s_transport.send_text(request, text, type);
}

/*
 * 通过传输层发送错误响应。
 */
static esp_err_t ota_send_error(void *request, int code, const char *message)
{
    return s_transport.send_error(request, code, message);
}

/*
 * 通过传输层获取查询参数。
 */
static char *ota_query_value(void *request, const char *key)
{
    return s_transport.query_value(request, key);
}

/*
 * 通过传输层接收数据。
 */
static int ota_recv(void *request, char *buffer, int length)
{
    return s_transport.recv(request, buffer, length);
}

/*
 * 创建可用资源快照（PSRAM/内部 RAM 剩余、目标分区大小等）。
 */
static ota_service_resource_snapshot_t ota_resource_snapshot(size_t expected_size)
{
    ota_service_resource_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.expected_size = expected_size;
    snapshot.psram_reserve = OTA_PSRAM_RESERVE_BYTES;
    snapshot.internal_reserve = OTA_INTERNAL_RESERVE_BYTES;
    snapshot.io_chunk = s_transport.io_chunk;
    snapshot.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snapshot.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    snapshot.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    snapshot.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    (void)ota_manager_get_next_update_partition_info(&snapshot.target_size,
                                                     snapshot.target_label,
                                                     sizeof(snapshot.target_label));
    return snapshot;
}

/*
 * 将资源快照存入全局状态（线程安全）。
 * 参数: snapshot - 资源快照指针，为 NULL 时无操作
 */
static void ota_store_resource_snapshot(const ota_service_resource_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    if (ota_state_lock()) {
        s_ota_last_resources = *snapshot;
        ota_state_unlock();
    }
}

/*
 * 将字符串以 JSON 转义格式追加到字符串构建器（双引号包裹，特殊字符转义）。
 * 参数: sb - 字符串构建器指针
 *       text - 原始文本，为 NULL 时按空字符串处理
 */
static void ota_json_escape_append(cc_string_builder_t *sb, const char *text)
{
    cc_string_builder_append_char(sb, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '\\': cc_string_builder_append(sb, "\\\\"); break;
        case '"': cc_string_builder_append(sb, "\\\""); break;
        case '\n': cc_string_builder_append(sb, "\\n"); break;
        case '\r': cc_string_builder_append(sb, "\\r"); break;
        case '\t': cc_string_builder_append(sb, "\\t"); break;
        default:
            if (*p < 0x20) cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p);
            else cc_string_builder_append_char(sb, (char)*p);
            break;
        }
    }
    cc_string_builder_append_char(sb, '"');
}

/*
 * 通过串口输出 OTA 状态消息（带 [ota] 前缀）。
 * 参数: message - 要输出的消息，为 NULL 时输出空字符串
 */
static void ota_serial_note(const char *message)
{
    printf("[ota] %s\r\n", message ? message : "");
    fflush(stdout);
}

/*
 * 格式化版本：通过串口输出带格式的 OTA 状态消息。
 * 参数: fmt - printf 格式字符串，为 NULL 时直接返回
 *       ... - 格式参数
 */
static void ota_serial_notef(const char *fmt, ...)
{
    if (!fmt) return;
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ota_serial_note(text);
}

/*
 * 检查上传文件名是否为已知的非 App 固件文件（bootloader、partition-table 等）。
 * 参数: name - 文件名，为 NULL 或空字符串返回 false
 * 返回: true 表示是非 App 固件，应拒绝
 */
static bool ota_upload_name_is_known_non_app(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    return strcmp(name, "bootloader.bin") == 0 ||
           strcmp(name, "partition-table.bin") == 0 ||
           strcmp(name, "ota_data_initial.bin") == 0 ||
           strstr(name, "CMakeDetermineCompilerABI") != NULL;
}

/*
 * 校验上传固件的资源条件：目标分区大小、PSRAM 容量、内部 RAM IO 缓冲。
 * 参数: expected_size - 期望固件大小
 *       reason - 失败原因输出缓冲区
 *       reason_size - 缓冲区大小
 * 返回: ESP_OK 成功；ESP_ERR_INVALID_SIZE（分区太小）、ESP_ERR_NOT_FOUND（无分区）、
 *       ESP_ERR_NO_MEM（内存不足）
 */
static esp_err_t ota_validate_upload_resources(size_t expected_size, char *reason, size_t reason_size)
{
    ota_service_resource_snapshot_t resources = ota_resource_snapshot(expected_size);
    ota_store_resource_snapshot(&resources);
    if (reason && reason_size > 0) {
        reason[0] = '\0';
    }
    if (expected_size == 0) {
        if (reason && reason_size > 0) {
            snprintf(reason, reason_size, "bad OTA image size");
        }
        return ESP_ERR_INVALID_SIZE;
    }

    if (resources.target_size == 0) {
        if (reason && reason_size > 0) {
            snprintf(reason, reason_size, "no OTA update partition");
        }
        return ESP_ERR_NOT_FOUND;
    }
    if (expected_size > resources.target_size) {
        if (reason && reason_size > 0) {
            snprintf(reason,
                     reason_size,
                     "OTA image size %u exceeds target %s size %u",
                     (unsigned)expected_size,
                     resources.target_label[0] ? resources.target_label : "?",
                     (unsigned)resources.target_size);
        }
        return ESP_ERR_INVALID_SIZE;
    }

    if (expected_size > SIZE_MAX - OTA_PSRAM_RESERVE_BYTES ||
        resources.psram_largest < expected_size ||
        resources.psram_free < expected_size + OTA_PSRAM_RESERVE_BYTES) {
        if (reason && reason_size > 0) {
            snprintf(reason,
                     reason_size,
                     "insufficient PSRAM for OTA image: need=%u free=%u largest=%u reserve=%u",
                     (unsigned)expected_size,
                     (unsigned)resources.psram_free,
                     (unsigned)resources.psram_largest,
                     (unsigned)OTA_PSRAM_RESERVE_BYTES);
        }
        return ESP_ERR_NO_MEM;
    }

    if (s_transport.io_chunk > SIZE_MAX - OTA_INTERNAL_RESERVE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t internal_need = s_transport.io_chunk + OTA_INTERNAL_RESERVE_BYTES;
    if (resources.internal_free < internal_need || resources.internal_largest < s_transport.io_chunk) {
        if (reason && reason_size > 0) {
            snprintf(reason,
                     reason_size,
                     "insufficient internal RAM for OTA IO: chunk=%u free=%u largest=%u reserve=%u",
                     (unsigned)s_transport.io_chunk,
                     (unsigned)resources.internal_free,
                     (unsigned)resources.internal_largest,
                     (unsigned)OTA_INTERNAL_RESERVE_BYTES);
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * 等待交互队列和事件总线空闲，为 OTA 上传做准备。
 * 返回: ESP_OK 成功；ESP_ERR_TIMEOUT 超时
 */
static esp_err_t ota_wait_for_quiescent(void)
{
    app_event_publish(APP_EVENT_KIND_STATUS,
                      APP_EVENT_SOURCE_SYSTEM,
                      CC_LOG_INFO,
                      "Preparing OTA upload; waiting for current interactions and events to finish");
    if (!interaction_service_wait_idle(OTA_PREPARE_INTERACTION_TIMEOUT_MS)) {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          APP_EVENT_SOURCE_SYSTEM,
                          CC_LOG_ERROR,
                          "OTA prepare timeout: interaction queue still busy");
        return ESP_ERR_TIMEOUT;
    }
    if (!app_event_bus_wait_idle(OTA_PREPARE_EVENT_TIMEOUT_MS)) {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          APP_EVENT_SOURCE_SYSTEM,
                          CC_LOG_ERROR,
                          "OTA prepare timeout: event sinks still busy");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/*
 * 进入维护模式：停止相机预览，启用事件总线维护模式。
 * 记录并保存预览运行状态以便退出时恢复。
 */
static esp_err_t ota_enter_maintenance_mode(void)
{
    if (ota_state_lock()) {
        s_ota_preparing = false;
        ota_state_unlock();
    }
    esp_err_t pause_err = interaction_service_set_maintenance_paused(true, "ota_upload");
    if (pause_err != ESP_OK) {
        app_event_publishf(APP_EVENT_KIND_ERROR,
                           APP_EVENT_SOURCE_SYSTEM,
                           CC_LOG_ERROR,
                           "interaction maintenance pause failed: %s",
                           esp_err_to_name(pause_err));
        return pause_err;
    }
    if (app_event_bus_maintenance_mode_enabled()) {
        return ESP_OK;
    }
    ota_serial_note("maintenance enter");
    app_event_bus_set_maintenance_mode(true, "ota_upload");
    app_event_publish(APP_EVENT_KIND_STATUS,
                      APP_EVENT_SOURCE_SYSTEM,
                      CC_LOG_INFO,
                      "OTA upload maintenance mode enabled");
    bool preview_was_running = board_camera_preview_running();
    if (ota_state_lock()) {
        s_ota_preview_was_running = preview_was_running;
        ota_state_unlock();
    }
    if (preview_was_running) {
        ota_serial_note("camera preview stop");
        esp_err_t err = board_camera_stop_preview();
        if (err != ESP_OK) {
            ota_serial_notef("camera preview stop failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return ESP_OK;
}

/*
 * 将 OTA 分区列表以 JSON 数组格式追加到字符串构建器。
 * 参数: sb - 字符串构建器指针
 */
static void append_ota_partitions(cc_string_builder_t *sb)
{
    ota_manager_partition_info_t partitions[4];
    size_t count = ota_manager_get_partitions(partitions, sizeof(partitions) / sizeof(partitions[0]));
    cc_string_builder_append(sb, ",\"partitions\":[");
    size_t emitted = count < (sizeof(partitions) / sizeof(partitions[0])) ?
        count : (sizeof(partitions) / sizeof(partitions[0]));
    for (size_t i = 0; i < emitted; i++) {
        if (i > 0) cc_string_builder_append_char(sb, ',');
        cc_string_builder_append(sb, "{\"label\":");
        ota_json_escape_append(sb, partitions[i].label);
        cc_string_builder_append(sb, ",\"app_name\":");
        ota_json_escape_append(sb, partitions[i].app_name);
        cc_string_builder_append(sb, ",\"version\":");
        ota_json_escape_append(sb, partitions[i].version);
        cc_string_builder_append(sb, ",\"sha256\":");
        ota_json_escape_append(sb, partitions[i].sha256);
        cc_string_builder_append(sb, ",\"uploaded_at\":");
        ota_json_escape_append(sb, partitions[i].uploaded_at);
        cc_string_builder_appendf(sb,
                                  ",\"size\":%u,\"running\":%s,\"boot\":%s,\"selected\":%s}",
                                  (unsigned)partitions[i].size,
                                  partitions[i].running ? "true" : "false",
                                  partitions[i].boot ? "true" : "false",
                                  partitions[i].boot ? "true" : "false");
    }
    cc_string_builder_append_char(sb, ']');
    cc_string_builder_appendf(sb, ",\"partition_count\":%u", (unsigned)count);
}

/*
 * 检查指定 label 的分区是否存在。
 * 参数: label - 分区标签，为 NULL 或空返回 false
 * 返回: true 存在
 */
static bool ota_partition_label_exists(const char *label)
{
    if (!label || !label[0]) {
        return false;
    }
    ota_manager_partition_info_t partitions[4];
    size_t count = ota_manager_get_partitions(partitions, sizeof(partitions) / sizeof(partitions[0]));
    size_t emitted = count < (sizeof(partitions) / sizeof(partitions[0])) ?
        count : (sizeof(partitions) / sizeof(partitions[0]));
    for (size_t i = 0; i < emitted; i++) {
        if (strcmp(partitions[i].label, label) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * 退出维护模式：退出事件总线维护模式，清除准备状态，恢复相机预览。
 */
static void ota_leave_maintenance_mode(void)
{
    bool preview_was_running = false;
    bool restore_services = false;
    if (ota_state_lock()) {
        preview_was_running = s_ota_preview_was_running;
        restore_services = s_ota_flash_services_stopped;
        ota_state_clear_prepared_locked();
        s_ota_preview_was_running = false;
        ota_state_unlock();
    }
    bool services_restored = true;
    if (restore_services) {
        esp_err_t network_err = network_service_resume(30000U);
        esp_err_t web_err = s_transport.start_web ?
            s_transport.start_web(s_transport.user_data) : ESP_ERR_INVALID_STATE;
        services_restored = network_err == ESP_OK && web_err == ESP_OK;
        if (!services_restored) {
            ota_serial_notef("service restore failed: network=%s web=%s",
                             esp_err_to_name(network_err), esp_err_to_name(web_err));
        }
        if (ota_state_lock()) {
            s_ota_flash_services_stopped = !services_restored;
            ota_state_unlock();
        }
    }
    app_event_bus_set_maintenance_mode(false, NULL);
    esp_err_t resume_err = interaction_service_set_maintenance_paused(false, NULL);
    if (resume_err != ESP_OK) {
        app_event_publishf(APP_EVENT_KIND_ERROR,
                           APP_EVENT_SOURCE_SYSTEM,
                           CC_LOG_ERROR,
                           "interaction maintenance resume failed: %s",
                           esp_err_to_name(resume_err));
    }
    if (preview_was_running) {
        ota_serial_note("camera preview restart");
        esp_err_t err = board_camera_start_preview();
        if (err != ESP_OK) {
            ota_serial_notef("camera preview restart failed: %s", esp_err_to_name(err));
        }
    }
    app_event_publish(APP_EVENT_KIND_STATUS,
                      APP_EVENT_SOURCE_SYSTEM,
                      CC_LOG_INFO,
                      "OTA upload maintenance mode disabled");
}

/*
 * 停止 Web 服务和网络（WiFi），为闪存写入腾出系统资源。
 */
static void ota_stop_network_for_flash(void)
{
    ota_serial_note("firmware received; stopping web and wifi for flash write");
    if (ota_state_lock()) {
        s_ota_flash_services_stopped = true;
        ota_state_unlock();
    }
    if (s_transport.stop_web) {
        esp_err_t err = s_transport.stop_web(s_transport.user_data);
        if (err != ESP_OK) {
            ota_serial_notef("web stop failed: %s", esp_err_to_name(err));
        }
    }

    esp_err_t err = network_service_shutdown_for_ota();
    if (err != ESP_OK) {
        ota_serial_notef("network shutdown for OTA: %s", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(200));
}

/*
 * FreeRTOS 闪存写入任务：分块写入固件到 OTA 分区，更新 LCD 进度条。
 * 写入完成后调用 ota_manager_finish，成功则自动重启，失败则取消并退出维护模式。
 * 参数: arg - ota_flash_job_t 指针，包含固件数据、大小和名称
 */
static void ota_flash_task(void *arg)
{
    ota_flash_job_t *job = (ota_flash_job_t *)arg;
    char *scratch = NULL;
    esp_err_t err = ESP_OK;
    const char *failure_message = NULL;
    if (!job || !job->image || job->size == 0) {
        ota_serial_note("flash task missing image");
        ota_record_failure(OTA_SERVICE_FAILURE_FLASH_WRITE, "flash_input",
                           "OTA flash task received an invalid image job");
        err = ESP_ERR_INVALID_ARG;
        failure_message = "Invalid OTA flash job";
        goto flash_failed;
    }

    vTaskDelay(pdMS_TO_TICKS(OTA_FLASH_START_DELAY_MS));
    ota_stop_network_for_flash();

    scratch = ota_alloc_io(s_transport.io_chunk);
    if (!scratch) {
        ota_record_failure(OTA_SERVICE_FAILURE_INTERNAL_RAM_INSUFFICIENT, "flash_alloc", "OTA flash scratch OOM");
        err = ESP_ERR_NO_MEM;
        failure_message = "OTA flash scratch OOM";
        goto flash_failed;
    }

    ota_serial_note("flash write begin");
    size_t offset = 0;
    size_t last_yield_bytes = 0;
    size_t last_lcd_bytes = 0;
    while (offset < job->size) {
        size_t chunk = job->size - offset;
        if (chunk > s_transport.io_chunk) {
            chunk = s_transport.io_chunk;
        }
        memcpy(scratch, job->image + offset, chunk);
        err = ota_manager_write(scratch, chunk);
        if (err != ESP_OK) {
            ota_serial_notef("flash write failed at %u: %s", (unsigned)offset, esp_err_to_name(err));
            ota_record_failure(OTA_SERVICE_FAILURE_FLASH_WRITE, "flash_write", ota_manager_last_error());
            failure_message = ota_manager_last_error();
            break;
        }

        offset += chunk;
        if (offset == job->size || offset - last_lcd_bytes >= OTA_PROGRESS_LCD_STEP_BYTES) {
            last_lcd_bytes = offset;
            board_ui_ota_progress(offset, job->size);
        }
        if (offset - last_yield_bytes >= 64U * 1024U) {
            last_yield_bytes = offset;
            vTaskDelay(1);
        }
    }

    if (err == ESP_OK) {
        ota_serial_note("ota finish");
        err = ota_manager_finish();
        ota_serial_notef("ota finish result=%s", esp_err_to_name(err));
        if (err != ESP_OK) {
            ota_record_failure(OTA_SERVICE_FAILURE_FLASH_FINISH, "flash_finish", ota_manager_last_error());
            failure_message = ota_manager_last_error();
        }
    }

    heap_caps_free(scratch);
    scratch = NULL;
    heap_caps_free(job->image);
    job->image = NULL;

    if (err == ESP_OK) {
        ota_set_stage(OTA_SERVICE_STAGE_REBOOT_PENDING);
        board_ui_ota_end(true, "Upload complete. Rebooting board.");
        heap_caps_free(job);
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_err_t shutdown_err = app_lifecycle_prepare_for_reboot(5000U);
        if (shutdown_err != ESP_OK) {
            ota_serial_notef("reboot drain completed with error: %s",
                             esp_err_to_name(shutdown_err));
        }
        ota_manager_reboot();
        vTaskDelete(NULL);
        return;
    }

flash_failed:
    if (scratch) heap_caps_free(scratch);
    if (job && job->image) heap_caps_free(job->image);
    board_ui_ota_end(false, failure_message ? failure_message : ota_manager_last_error());
    ota_manager_cancel();
    ota_leave_maintenance_mode();
    if (job) heap_caps_free(job);
    vTaskDelete(NULL);
}

/*
 * 创建 ota_flash_task 任务。image 所有权转移给任务，任务结束后释放。
 * 参数: image - PSRAM 分配的固件镜像
 *       size - 镜像大小
 *       name - 固件文件名
 * 返回: true 任务创建成功
 */
static bool ota_start_flash_task(uint8_t *image, size_t size, const char *name)
{
    ota_flash_job_t *job = (ota_flash_job_t *)ota_alloc_io(sizeof(*job));
    if (!job) {
        return false;
    }
    memset(job, 0, sizeof(*job));
    job->image = image;
    job->size = size;
    snprintf(job->name, sizeof(job->name), "%s", name ? name : "");

    BaseType_t ok = xTaskCreateWithCaps(ota_flash_task,
                                        "ota_flash",
                                        OTA_FLASH_TASK_STACK_BYTES,
                                        job,
                                        10,
                                        NULL,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        heap_caps_free(job);
        return false;
    }
    return true;
}

/*
 * FreeRTOS 重启任务：延迟后停止相机、准备重启并调用 ota_manager_reboot。
 */
static void ota_reboot_task(void *arg)
{
    (void)arg;
    ota_serial_note("reboot board");
    board_ui_ota_end(true, "Rebooting board.");
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_err_t shutdown_err = app_lifecycle_prepare_for_reboot(5000U);
    if (shutdown_err != ESP_OK) {
        ota_serial_notef("reboot drain completed with error: %s",
                         esp_err_to_name(shutdown_err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ota_manager_reboot();
    vTaskDelete(NULL);
}

/*
 * 创建 ota_reboot_task 任务（防重入）。
 * 返回: true 任务启动或已在进行中
 */
static bool ota_start_reboot_task(void)
{
    if (!ota_state_lock()) {
        return false;
    }
    if (s_ota_reboot_pending) {
        ota_state_unlock();
        return true;
    }
    s_ota_reboot_pending = true;
    s_ota_stage = OTA_SERVICE_STAGE_REBOOT_PENDING;
    ota_state_unlock();
    BaseType_t ok = xTaskCreateWithCaps(ota_reboot_task,
                                        "ota_reboot",
                                        OTA_REBOOT_TASK_STACK_BYTES,
                                        NULL,
                                        10,
                                        NULL,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        if (ota_state_lock()) {
            s_ota_reboot_pending = false;
            s_ota_stage = OTA_SERVICE_STAGE_IDLE;
            ota_state_unlock();
        }
        return false;
    }
    return true;
}

/*
 * HTTP 接口：返回 OTA 完整状态 JSON（阶段、进度、分区信息、资源快照、失败详情）。
 * 参数: request - HTTP 请求上下文
 * 返回: ESP_OK 成功
 */
esp_err_t ota_service_status(void *request)
{
    ota_manager_refresh_status();
    ota_manager_status_t st;
    app_event_bus_status_t events;
    ota_manager_get_status(&st);
    app_event_bus_get_status(&events);
    ota_service_state_snapshot_t state = ota_state_snapshot();
    bool maintenance = state.preparing ||
                       state.prepared ||
                       state.preview_was_running ||
                       events.maintenance_mode ||
                       st.state == OTA_MANAGER_UPLOADING;
    const char *manager_state = ota_manager_state_string(st.state);
    const char *service_stage = ota_service_stage_string(state.stage);
    if (state.stage == OTA_SERVICE_STAGE_IDLE && st.state == OTA_MANAGER_UPLOADING) {
        service_stage = "flashing";
    } else if (state.stage == OTA_SERVICE_STAGE_IDLE &&
               st.state == OTA_MANAGER_PENDING_REBOOT) {
        service_stage = "reboot_pending";
    } else if (state.stage == OTA_SERVICE_STAGE_IDLE &&
               st.state == OTA_MANAGER_ERROR) {
        service_stage = "failed";
    }
    const char *maintenance_reason = events.maintenance_reason[0] ?
        events.maintenance_reason : (state.preparing ? "ota_prepare" : "");
    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) {
        return ota_send_error(request, s_transport.server_error_code, "OOM");
    }
    cc_string_builder_append(&sb, "{\"state\":");
    ota_json_escape_append(&sb, service_stage);
    cc_string_builder_append(&sb, ",\"manager_state\":");
    ota_json_escape_append(&sb, manager_state);
    unsigned progress = st.expected_size ?
        (unsigned)((st.bytes_written * 100U) / st.expected_size) : 0;
    cc_string_builder_appendf(&sb,
                              ",\"bytes\":%u,\"expected\":%u,\"progress\":%u,"
                              "\"target_size\":%u,\"maintenance\":%s,\"upload_name\":",
                              (unsigned)st.bytes_written,
                              (unsigned)st.expected_size,
                              progress,
                              (unsigned)st.target_size,
                              maintenance ? "true" : "false");
    ota_json_escape_append(&sb, st.upload_name);
    cc_string_builder_append(&sb, ",\"maintenance_reason\":");
    ota_json_escape_append(&sb, maintenance_reason);
    cc_string_builder_appendf(&sb,
                              ",\"preparing\":%s,\"prepared\":%s,\"reboot_pending\":%s,\"prepared_size\":%u",
                              state.preparing ? "true" : "false",
                              state.prepared ? "true" : "false",
                              state.reboot_pending ? "true" : "false",
                              (unsigned)state.prepared_size);
    cc_string_builder_append(&sb, ",\"failure_code\":");
    ota_json_escape_append(&sb, ota_service_failure_string(state.failure_code));
    cc_string_builder_append(&sb, ",\"failure_stage\":");
    ota_json_escape_append(&sb, state.failure_stage);
    cc_string_builder_append(&sb, ",\"failure_message\":");
    ota_json_escape_append(&sb, state.failure_message);
    cc_string_builder_appendf(&sb,
                              ",\"resource_snapshot\":{\"expected_size\":%u,"
                              "\"target_size\":%u,\"target_label\":",
                              (unsigned)state.resources.expected_size,
                              (unsigned)state.resources.target_size);
    ota_json_escape_append(&sb, state.resources.target_label);
    cc_string_builder_appendf(&sb,
                              ",\"psram_free\":%u,\"psram_largest\":%u,\"psram_reserve\":%u,"
                              "\"internal_free\":%u,\"internal_largest\":%u,"
                              "\"internal_reserve\":%u,\"io_chunk\":%u}",
                              (unsigned)state.resources.psram_free,
                              (unsigned)state.resources.psram_largest,
                              (unsigned)state.resources.psram_reserve,
                              (unsigned)state.resources.internal_free,
                              (unsigned)state.resources.internal_largest,
                              (unsigned)state.resources.internal_reserve,
                              (unsigned)state.resources.io_chunk);
    cc_string_builder_append(&sb, ",\"selected\":");
    ota_json_escape_append(&sb, st.boot_label);
    cc_string_builder_append(&sb, ",\"running\":");
    ota_json_escape_append(&sb, st.running_label);
    cc_string_builder_append(&sb, ",\"running_app_name\":");
    ota_json_escape_append(&sb, st.running_app_name);
    cc_string_builder_append(&sb, ",\"running_version\":");
    ota_json_escape_append(&sb, st.running_version);
    cc_string_builder_append(&sb, ",\"running_sha256\":");
    ota_json_escape_append(&sb, st.running_sha256);
    cc_string_builder_append(&sb, ",\"running_uploaded_at\":");
    ota_json_escape_append(&sb, st.running_uploaded_at);
    cc_string_builder_append(&sb, ",\"running_ota_state\":");
    ota_json_escape_append(&sb, st.running_ota_state);
    cc_string_builder_append(&sb, ",\"boot\":");
    ota_json_escape_append(&sb, st.boot_label);
    cc_string_builder_append(&sb, ",\"boot_app_name\":");
    ota_json_escape_append(&sb, st.boot_app_name);
    cc_string_builder_append(&sb, ",\"boot_version\":");
    ota_json_escape_append(&sb, st.boot_version);
    cc_string_builder_append(&sb, ",\"boot_sha256\":");
    ota_json_escape_append(&sb, st.boot_sha256);
    cc_string_builder_append(&sb, ",\"boot_uploaded_at\":");
    ota_json_escape_append(&sb, st.boot_uploaded_at);
    cc_string_builder_append(&sb, ",\"target\":");
    ota_json_escape_append(&sb, st.target_label);
    cc_string_builder_append(&sb, ",\"target_app_name\":");
    ota_json_escape_append(&sb, st.target_app_name);
    cc_string_builder_append(&sb, ",\"target_version\":");
    ota_json_escape_append(&sb, st.target_version);
    cc_string_builder_append(&sb, ",\"target_sha256\":");
    ota_json_escape_append(&sb, st.target_sha256);
    cc_string_builder_append(&sb, ",\"target_uploaded_at\":");
    ota_json_escape_append(&sb, st.target_uploaded_at);
    cc_string_builder_appendf(&sb, ",\"rollback_pending\":%s,\"error\":", st.rollback_pending ? "true" : "false");
    ota_json_escape_append(&sb, st.error);
    append_ota_partitions(&sb);
    cc_string_builder_append_char(&sb, '}');
    char *json = cc_string_builder_take(&sb);
    esp_err_t err = ota_send_text(request, json, "application/json");
    free(json);
    return err;
}

/*
 * HTTP 接口：OTA 准备阶段——校验固件名和大小，等待系统空闲，进入维护模式。
 * 参数: request - HTTP 请求上下文（需提供 name/size/time 查询参数）
 * 返回: ESP_OK 成功
 */
static uint64_t ota_monotonic_ms(void)
{
    int64_t us = esp_timer_get_time();
    return us > 0 ? (uint64_t)us / 1000ULL : 0;
}

static bool ota_parse_size(const char *text, size_t *out_size)
{
    if (!text || !text[0] || !out_size || text[0] == '-') return false;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > (unsigned long long)SIZE_MAX) {
        return false;
    }
    *out_size = (size_t)parsed;
    return true;
}

static bool ota_normalize_sha256(const char *text, char out[65])
{
    if (!text || strlen(text) != 64 || !out) return false;
    static const char lower[] = "0123456789abcdef";
    for (size_t i = 0; i < 64; i++) {
        unsigned char c = (unsigned char)text[i];
        unsigned value;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10U;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10U;
        else return false;
        out[i] = lower[value];
    }
    out[64] = '\0';
    return true;
}

static void ota_sha256_hex(const unsigned char digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hex[(digest[i] >> 4) & 0x0fU];
        out[i * 2 + 1] = hex[digest[i] & 0x0fU];
    }
    out[64] = '\0';
}

/* Validate the ESP image header and every declared segment before flash starts. */
static bool ota_validate_image_layout(const uint8_t *image, size_t size)
{
    const size_t image_header_size = 24U;
    const size_t segment_header_size = 8U;
    if (!image || size < image_header_size || image[0] != 0xe9U) return false;
    unsigned segment_count = image[1];
    if (segment_count == 0 || segment_count > 16U) return false;
    size_t offset = image_header_size;
    for (unsigned i = 0; i < segment_count; i++) {
        if (offset > size || size - offset < segment_header_size) return false;
        uint32_t data_len = (uint32_t)image[offset + 4] |
            ((uint32_t)image[offset + 5] << 8) |
            ((uint32_t)image[offset + 6] << 16) |
            ((uint32_t)image[offset + 7] << 24);
        offset += segment_header_size;
        if ((size_t)data_len > size - offset) return false;
        offset += (size_t)data_len;
    }
    /* At least the ESP checksum byte must follow the declared segments. */
    return offset < size;
}

esp_err_t ota_service_prepare(void *request)
{
    char *upload_name = ota_query_value(request, "name");
    char *size_text = ota_query_value(request, "size");
    char *sha_text = ota_query_value(request, "sha256");
    char *browser_time = ota_query_value(request, "time");
    size_t expected_size = 0;
    char prepared_sha256[65];
    bool size_valid = ota_parse_size(size_text, &expected_size);
    bool sha_valid = ota_normalize_sha256(sha_text, prepared_sha256);
    ota_serial_notef("prepare request name=%s size=%u",
                     upload_name && upload_name[0] ? upload_name : "(none)",
                     (unsigned)expected_size);
    heap_caps_free(size_text);
    if (sha_text) {
        memset(sha_text, 0, strlen(sha_text));
        heap_caps_free(sha_text);
    }
    if (!upload_name || !upload_name[0] || !size_valid || !sha_valid) {
        ota_record_failure(OTA_SERVICE_FAILURE_BAD_REQUEST, "prepare",
                           "name, size and SHA-256 are required");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request, s_transport.bad_request_code,
                              "name, size and valid sha256 are required");
    }
    if (ota_upload_name_is_known_non_app(upload_name)) {
        ota_record_failure(OTA_SERVICE_FAILURE_BAD_REQUEST, "prepare", "OTA requires the app image");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request,
                                      s_transport.bad_request_code,
                                      "OTA requires the app image, use build/CClaw.bin");
    }
    if (expected_size == 0) {
        ota_record_failure(OTA_SERVICE_FAILURE_BAD_REQUEST, "prepare", "bad OTA image size");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request, s_transport.bad_request_code, "bad OTA image size");
    }
    char reject_reason[192];
    esp_err_t resource_err = ota_validate_upload_resources(expected_size,
                                                           reject_reason,
                                                           sizeof(reject_reason));
    if (resource_err != ESP_OK) {
        ota_service_failure_t failure = OTA_SERVICE_FAILURE_INTERNAL_RAM_INSUFFICIENT;
        if (resource_err == ESP_ERR_NOT_FOUND) {
            failure = OTA_SERVICE_FAILURE_NO_UPDATE_PARTITION;
        } else if (resource_err == ESP_ERR_INVALID_SIZE) {
            failure = OTA_SERVICE_FAILURE_PARTITION_TOO_SMALL;
        } else if (strstr(reject_reason, "PSRAM")) {
            failure = OTA_SERVICE_FAILURE_PSRAM_INSUFFICIENT;
        }
        ota_record_failure(failure, "prepare_resources", reject_reason);
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        int code = resource_err == ESP_ERR_NO_MEM ?
            s_transport.server_error_code : s_transport.bad_request_code;
        return ota_send_error(request,
                              code,
                              reject_reason[0] ? reject_reason : esp_err_to_name(resource_err));
    }
    ota_manager_status_t st;
    ota_manager_get_status(&st);
    if (st.state == OTA_MANAGER_UPLOADING) {
        ota_record_failure(OTA_SERVICE_FAILURE_UPLOAD_IN_PROGRESS, "prepare", "OTA upload in progress");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request, s_transport.bad_request_code, "OTA upload in progress");
    }

    if (!ota_state_lock()) {
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request, s_transport.server_error_code, "OTA state lock failed");
    }
    ota_state_clear_prepared_locked();
    ota_clear_failure_locked();
    s_ota_preparing = true;
    s_ota_stage = OTA_SERVICE_STAGE_PREPARING;
    ota_state_unlock();
    board_ui_ota_prepare(upload_name,
                         expected_size,
                         "Waiting for current events to finish");
    esp_err_t err = ota_wait_for_quiescent();
    if (err != ESP_OK) {
        if (ota_state_lock()) {
            s_ota_preparing = false;
            s_ota_stage = OTA_SERVICE_STAGE_FAILED;
            ota_state_unlock();
        }
        ota_record_failure(OTA_SERVICE_FAILURE_CONCURRENT_STATE, "prepare_wait", "OTA postponed: current work still active");
        board_ui_ota_end(false, "OTA postponed: current work still active");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request,
                                      s_transport.bad_request_code,
                                      "OTA postponed: current work still active");
    }

    esp_err_t maintenance_err = ota_enter_maintenance_mode();
    if (maintenance_err != ESP_OK) {
        ota_record_failure(OTA_SERVICE_FAILURE_CONCURRENT_STATE, "maintenance_enter", "failed to pause interaction service");
        board_ui_ota_end(false, "OTA postponed: interaction service did not pause");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request,
                              s_transport.bad_request_code,
                              "OTA postponed: interaction service did not pause");
    }
    if (ota_state_lock()) {
        s_ota_prepared = true;
        s_ota_prepared_size = expected_size;
        s_ota_stage = OTA_SERVICE_STAGE_PREPARED;
        snprintf(s_ota_prepared_name, sizeof(s_ota_prepared_name), "%s", upload_name ? upload_name : "");
        snprintf(s_ota_prepared_time, sizeof(s_ota_prepared_time), "%s", browser_time ? browser_time : "");
        snprintf(s_ota_prepared_sha256, sizeof(s_ota_prepared_sha256), "%s",
                 prepared_sha256);
        ota_state_unlock();
    }
    board_ui_ota_prepare(upload_name, expected_size, "Ready to receive firmware");
    ota_serial_note("prepare done");
    heap_caps_free(upload_name);
    heap_caps_free(browser_time);
    return ota_send_text(request, "{\"ok\":true,\"prepared\":true}", "application/json");
}

/*
 * HTTP 接口：接收固件上传——分块接收固件到 PSRAM，校验后启动闪存任务。
 * 参数: request - HTTP 请求上下文
 *       content_len - 固件内容长度
 * 返回: ESP_OK 成功
 */
esp_err_t ota_service_upload(void *request, int content_len)
{
    char *upload_name = ota_query_value(request, "name");
    char *sha_text = ota_query_value(request, "sha256");
    char *browser_time = ota_query_value(request, "time");
    char upload_sha256[65];
    bool sha_valid = ota_normalize_sha256(sha_text, upload_sha256);
    if (sha_text) {
        memset(sha_text, 0, strlen(sha_text));
        heap_caps_free(sha_text);
    }
    ota_serial_notef("upload request name=%s len=%u",
                     upload_name && upload_name[0] ? upload_name : "(none)",
                     (unsigned)content_len);
    if (!upload_name || !upload_name[0] || !sha_valid) {
        ota_record_failure(OTA_SERVICE_FAILURE_BAD_REQUEST, "upload",
                           "name and SHA-256 are required");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request, s_transport.bad_request_code,
                              "name and valid sha256 are required");
    }
    if (ota_upload_name_is_known_non_app(upload_name)) {
        ota_record_failure(OTA_SERVICE_FAILURE_BAD_REQUEST, "upload", "OTA requires the app image");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request,
                                      s_transport.bad_request_code,
                                      "OTA requires the app image, use build/CClaw.bin");
    }
    if (content_len <= 0) {
        ota_record_failure(OTA_SERVICE_FAILURE_BAD_REQUEST, "upload", "bad OTA image size");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request, s_transport.bad_request_code, "bad OTA image size");
    }
    char reject_reason[192];
    esp_err_t resource_err = ota_validate_upload_resources((size_t)content_len,
                                                           reject_reason,
                                                           sizeof(reject_reason));
    if (resource_err != ESP_OK) {
        ota_service_failure_t failure = OTA_SERVICE_FAILURE_INTERNAL_RAM_INSUFFICIENT;
        if (resource_err == ESP_ERR_NOT_FOUND) {
            failure = OTA_SERVICE_FAILURE_NO_UPDATE_PARTITION;
        } else if (resource_err == ESP_ERR_INVALID_SIZE) {
            failure = OTA_SERVICE_FAILURE_PARTITION_TOO_SMALL;
        } else if (strstr(reject_reason, "PSRAM")) {
            failure = OTA_SERVICE_FAILURE_PSRAM_INSUFFICIENT;
        }
        ota_record_failure(failure, "upload_resources", reject_reason);
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        int code = resource_err == ESP_ERR_NO_MEM ?
            s_transport.server_error_code : s_transport.bad_request_code;
        return ota_send_error(request,
                              code,
                              reject_reason[0] ? reject_reason : esp_err_to_name(resource_err));
    }

    ota_service_state_snapshot_t state = ota_state_snapshot();
    if (state.prepared) {
        bool mismatch = state.prepared_size != (size_t)content_len ||
                        (state.prepared_name[0] &&
                         strcmp(state.prepared_name, upload_name) != 0) ||
                        !state.prepared_sha256[0] ||
                        strcmp(state.prepared_sha256, upload_sha256) != 0;
        if (mismatch) {
            ota_service_failure_t mismatch_code =
                (state.prepared_sha256[0] &&
                 strcmp(state.prepared_sha256, upload_sha256) != 0) ?
                OTA_SERVICE_FAILURE_HASH_MISMATCH : OTA_SERVICE_FAILURE_SIZE_MISMATCH;
            ota_record_failure(mismatch_code, "upload_match",
                               "OTA upload does not match prepared file");
            board_ui_ota_end(false, "OTA upload does not match prepared file");
            ota_leave_maintenance_mode();
            heap_caps_free(upload_name);
            heap_caps_free(browser_time);
            return ota_send_error(request,
                                          s_transport.bad_request_code,
                                          "OTA upload does not match prepared file");
        }
        if (!state.prepared_time[0] && browser_time) {
            if (ota_state_lock()) {
                if (!s_ota_prepared_time[0]) {
                    snprintf(s_ota_prepared_time, sizeof(s_ota_prepared_time), "%s", browser_time);
                }
                ota_state_unlock();
            }
            snprintf(state.prepared_time, sizeof(state.prepared_time), "%s", browser_time);
        }
    } else {
        ota_record_failure(OTA_SERVICE_FAILURE_BAD_REQUEST, "upload_match",
                           "OTA upload was not prepared");
        heap_caps_free(upload_name);
        heap_caps_free(browser_time);
        return ota_send_error(request, s_transport.bad_request_code,
                              "call /api/ota/prepare before upload");
    }
    board_ui_ota_begin(upload_name, (size_t)content_len);
    ota_serial_note("ota begin");
    ota_set_stage(OTA_SERVICE_STAGE_FLASHING);
    const char *upload_time = state.prepared_time[0] ? state.prepared_time : browser_time;
    esp_err_t err = ota_manager_begin((size_t)content_len, upload_name, upload_time);
    heap_caps_free(upload_name);
    heap_caps_free(browser_time);
    if (err != ESP_OK) {
        ota_record_failure(OTA_SERVICE_FAILURE_FLASH_WRITE, "begin", ota_manager_last_error());
        board_ui_ota_end(false, ota_manager_last_error());
        ota_leave_maintenance_mode();
        return ota_send_error(request, s_transport.bad_request_code, ota_manager_last_error());
    }
    char *buf = ota_alloc_io(s_transport.io_chunk);
    if (!buf) {
        ota_record_failure(OTA_SERVICE_FAILURE_INTERNAL_RAM_INSUFFICIENT, "upload_alloc_io", "OOM");
        ota_manager_cancel();
        board_ui_ota_end(false, "OOM");
        ota_leave_maintenance_mode();
        return ota_send_error(request, s_transport.server_error_code, "OOM");
    }
    uint8_t *image = (uint8_t *)heap_caps_malloc((size_t)content_len,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!image) {
        ota_record_failure(OTA_SERVICE_FAILURE_PSRAM_INSUFFICIENT, "upload_alloc_image", "OOM");
        heap_caps_free(buf);
        ota_manager_cancel();
        board_ui_ota_end(false, "OOM");
        ota_leave_maintenance_mode();
        return ota_send_error(request, s_transport.server_error_code, "OOM");
    }
    int remaining = content_len;
    size_t last_lcd_bytes = 0;
    size_t received = 0;
    bool first_recv = true;
    unsigned consecutive_timeouts = 0;
    uint64_t upload_start_ms = ota_monotonic_ms();
    uint64_t last_progress_ms = upload_start_ms;
    psa_hash_operation_t sha_op = PSA_HASH_OPERATION_INIT;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_setup(&sha_op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        (void)psa_hash_abort(&sha_op);
        ota_record_failure(OTA_SERVICE_FAILURE_HASH_MISMATCH, "upload_hash_init",
                           "SHA-256 initialization failed");
        heap_caps_free(buf);
        heap_caps_free(image);
        ota_manager_cancel();
        board_ui_ota_end(false, "SHA-256 initialization failed");
        ota_leave_maintenance_mode();
        return ota_send_error(request, s_transport.server_error_code,
                              "SHA-256 initialization failed");
    }
    while (remaining > 0) {
        int n = ota_recv(request, buf, remaining > s_transport.io_chunk ? s_transport.io_chunk : remaining);
        uint64_t now_ms = ota_monotonic_ms();
        if (n == s_transport.request_timeout_code) {
            consecutive_timeouts++;
            bool total_timeout = now_ms >= upload_start_ms &&
                now_ms - upload_start_ms >= OTA_UPLOAD_TOTAL_TIMEOUT_MS;
            bool idle_timeout = now_ms >= last_progress_ms &&
                now_ms - last_progress_ms >= OTA_UPLOAD_IDLE_TIMEOUT_MS;
            if (!total_timeout && !idle_timeout &&
                consecutive_timeouts < OTA_UPLOAD_MAX_CONSECUTIVE_TIMEOUTS) {
                continue;
            }
            (void)psa_hash_abort(&sha_op);
            ota_record_failure(OTA_SERVICE_FAILURE_UPLOAD_TIMEOUT, "upload_recv",
                               total_timeout ? "OTA upload total deadline exceeded" :
                               (idle_timeout ? "OTA upload made no progress" :
                                "too many consecutive OTA request timeouts"));
            heap_caps_free(buf);
            heap_caps_free(image);
            ota_manager_cancel();
            board_ui_ota_end(false, "OTA upload timed out");
            ota_leave_maintenance_mode();
            return ota_send_error(request, s_transport.server_error_code,
                                  "OTA upload timed out");
        }
        if (n <= 0) {
            (void)psa_hash_abort(&sha_op);
            ota_record_failure(OTA_SERVICE_FAILURE_RECV_FAILED, "upload_recv", "recv failed");
            heap_caps_free(buf);
            heap_caps_free(image);
            ota_manager_cancel();
            board_ui_ota_end(false, "recv failed");
            ota_leave_maintenance_mode();
            return ota_send_error(request, s_transport.server_error_code, "recv failed");
        }
        consecutive_timeouts = 0;
        last_progress_ms = now_ms;
        if ((now_ms >= upload_start_ms &&
             now_ms - upload_start_ms >= OTA_UPLOAD_TOTAL_TIMEOUT_MS) ||
            psa_hash_update(&sha_op, (const uint8_t *)buf, (size_t)n) != PSA_SUCCESS) {
            (void)psa_hash_abort(&sha_op);
            ota_record_failure(OTA_SERVICE_FAILURE_UPLOAD_TIMEOUT, "upload_hash",
                               "OTA upload deadline or SHA-256 update failed");
            heap_caps_free(buf);
            heap_caps_free(image);
            ota_manager_cancel();
            board_ui_ota_end(false, "OTA upload validation failed");
            ota_leave_maintenance_mode();
            return ota_send_error(request, s_transport.server_error_code,
                                  "OTA upload validation failed");
        }
        if (first_recv) {
            ota_serial_notef("first recv chunk=%d", n);
            first_recv = false;
        }
        memcpy(image + received, buf, (size_t)n);
        received += (size_t)n;
        if (received == (size_t)content_len ||
            received - last_lcd_bytes >= OTA_PROGRESS_LCD_STEP_BYTES) {
            last_lcd_bytes = received;
            board_ui_ota_progress(received, (size_t)content_len);
        }
        remaining -= n;
    }
    uint8_t digest[32] = {0};
    size_t digest_length = 0;
    psa_status_t hash_finish_status = psa_hash_finish(
        &sha_op, digest, sizeof(digest), &digest_length);
    if (hash_finish_status != PSA_SUCCESS) {
        (void)psa_hash_abort(&sha_op);
    }
    heap_caps_free(buf);
    char actual_sha256[65];
    ota_sha256_hex(digest, actual_sha256);
    memset(digest, 0, sizeof(digest));
    if (hash_finish_status != PSA_SUCCESS || digest_length != sizeof(digest) ||
        strcmp(actual_sha256, upload_sha256) != 0) {
        memset(actual_sha256, 0, sizeof(actual_sha256));
        ota_record_failure(OTA_SERVICE_FAILURE_HASH_MISMATCH, "upload_hash",
                           "OTA SHA-256 does not match prepare");
        heap_caps_free(image);
        ota_manager_cancel();
        board_ui_ota_end(false, "OTA SHA-256 mismatch");
        ota_leave_maintenance_mode();
        return ota_send_error(request, s_transport.bad_request_code,
                              "OTA SHA-256 mismatch");
    }
    memset(actual_sha256, 0, sizeof(actual_sha256));
    if (!ota_validate_image_layout(image, (size_t)content_len)) {
        ota_record_failure(OTA_SERVICE_FAILURE_IMAGE_INVALID, "upload_image",
                           "invalid ESP image header or segment layout");
        heap_caps_free(image);
        ota_manager_cancel();
        board_ui_ota_end(false, "Invalid ESP firmware image");
        ota_leave_maintenance_mode();
        return ota_send_error(request, s_transport.bad_request_code,
                              "invalid ESP firmware image");
    }
    ota_serial_note("upload verified in PSRAM; flash will continue offline");
    state = ota_state_snapshot();
    board_ui_ota_prepare(state.prepared_name[0] ? state.prepared_name : "firmware",
                         (size_t)content_len,
                         "Upload received. Network pauses while flashing.");
    if (!ota_start_flash_task(image, (size_t)content_len, state.prepared_name)) {
        ota_record_failure(OTA_SERVICE_FAILURE_TASK_START, "flash_task", "OTA flash task start failed");
        heap_caps_free(image);
        ota_manager_cancel();
        board_ui_ota_end(false, "OTA flash task start failed");
        ota_leave_maintenance_mode();
        return ota_send_error(request,
                                      s_transport.server_error_code,
                                      "OTA flash task start failed");
    }
    return ota_send_text(request,
                                 "{\"ok\":true,\"flashing\":true,\"offline\":true}",
                                 "application/json");
}

/*
 * HTTP 接口：选择 OTA 启动分区——设置指定分区为下次启动分区。
 * 参数: request - HTTP 请求上下文（需提供 label 查询参数）
 * 返回: ESP_OK 成功
 */
esp_err_t ota_service_select(void *request)
{
    char *label = ota_query_value(request, "label");
    ota_manager_status_t st;
    ota_manager_refresh_status();
    ota_manager_get_status(&st);
    if (st.state == OTA_MANAGER_UPLOADING) {
        ota_record_failure(OTA_SERVICE_FAILURE_UPLOAD_IN_PROGRESS, "select", "OTA upload in progress");
        heap_caps_free(label);
        return ota_send_error(request, s_transport.bad_request_code, "OTA upload in progress");
    }
    if (!ota_partition_label_exists(label)) {
        heap_caps_free(label);
        return ota_send_error(request, s_transport.bad_request_code, "partition not found");
    }
    bool boot_already_set = st.boot_label[0] && strcmp(st.boot_label, label) == 0;
    if (!boot_already_set) {
        esp_err_t set_err = ota_manager_set_boot_partition(label);
        if (set_err != ESP_OK) {
            const char *error = ota_manager_last_error();
            heap_caps_free(label);
            return ota_send_error(request,
                                          s_transport.bad_request_code,
                                          error && error[0] ? error : esp_err_to_name(set_err));
        }
    }
    ota_manager_refresh_status();
    ota_manager_get_status(&st);
    bool reboot_required = st.running_label[0] &&
                           st.boot_label[0] &&
                           strcmp(st.running_label, st.boot_label) != 0;
    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) {
        heap_caps_free(label);
        return ota_send_error(request, s_transport.server_error_code, "OOM");
    }
    cc_string_builder_appendf(&sb,
                              "{\"ok\":true,\"pending_reboot\":%s,\"reboot_required\":%s,\"running\":",
                              reboot_required ? "true" : "false",
                              reboot_required ? "true" : "false");
    ota_json_escape_append(&sb, st.running_label);
    cc_string_builder_append(&sb, ",\"boot\":");
    ota_json_escape_append(&sb, st.boot_label);
    cc_string_builder_append(&sb, ",\"selected\":");
    ota_json_escape_append(&sb, st.boot_label);
    cc_string_builder_append_char(&sb, '}');
    char *json = cc_string_builder_take(&sb);
    heap_caps_free(label);
    esp_err_t err = ota_send_text(request, json, "application/json");
    free(json);
    return err;
}

/*
 * HTTP 接口：触发设备重启（创建延迟重启任务）。
 * 参数: request - HTTP 请求上下文
 * 返回: ESP_OK 成功
 */
esp_err_t ota_service_reboot(void *request)
{
    ota_manager_refresh_status();
    ota_manager_status_t st;
    ota_manager_get_status(&st);
    if (st.state == OTA_MANAGER_UPLOADING) {
        ota_record_failure(OTA_SERVICE_FAILURE_UPLOAD_IN_PROGRESS, "reboot", "OTA upload in progress");
        return ota_send_error(request, s_transport.bad_request_code, "OTA upload in progress");
    }
    if (!ota_start_reboot_task()) {
        ota_record_failure(OTA_SERVICE_FAILURE_REBOOT_PENDING, "reboot", "reboot task start failed");
        return ota_send_error(request, s_transport.server_error_code, "reboot task start failed");
    }
    return ota_send_text(request, "{\"ok\":true,\"rebooting\":true}", "application/json");
}

/*
 * HTTP 接口：取消 OTA——重置状态，退出维护模式，更新 UI。
 * 参数: request - HTTP 请求上下文
 * 返回: ESP_OK 成功
 */
esp_err_t ota_service_cancel(void *request)
{
    ota_manager_cancel();
    if (ota_state_lock()) {
        s_ota_preparing = false;
        s_ota_stage = OTA_SERVICE_STAGE_IDLE;
        ota_clear_failure_locked();
        ota_state_unlock();
    }
    ota_leave_maintenance_mode();
    board_ui_ota_end(false, "OTA cancelled");
    return ota_send_text(request, "{\"ok\":true}", "application/json");
}
