#include "ota_service.h"

#include <stdarg.h>
#include <limits.h>
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
#define OTA_PREPARE_WATCH_STACK_BYTES 8192U
#define OTA_REBOOT_TASK_STACK_BYTES 4096U
#define OTA_SELECT_TASK_STACK_BYTES 8192U
#define OTA_REBOOT_DELAY_MS 1200U
#define OTA_IO_CHUNK_FALLBACK 4096U
#define OTA_PSRAM_RESERVE_BYTES (256U * 1024U)
#define OTA_INTERNAL_RESERVE_BYTES (64U * 1024U)
#define OTA_UPLOAD_TOTAL_TIMEOUT_MS 180000ULL
#define OTA_UPLOAD_IDLE_TIMEOUT_MS 30000ULL
#define OTA_UPLOAD_MAX_CONSECUTIVE_TIMEOUTS 5U
#define OTA_PREPARED_TIMEOUT_MS 60000ULL

static ota_service_transport_t s_transport;
static bool s_ota_initialized;

typedef struct {
    char label[24];
} ota_select_job_t;

static void ota_select_task(void *arg);

typedef enum {
    OTA_SERVICE_STAGE_IDLE = 0,
    OTA_SERVICE_STAGE_PREPARING,
    OTA_SERVICE_STAGE_PREPARED,
    OTA_SERVICE_STAGE_SELECTING,
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
/* 准备及恢复与 watchdog 经此锁串行，恢复在普通任务中执行，
 * 慢服务调用期间只持操作锁，先释放短状态锁。 */
static SemaphoreHandle_t s_ota_operation_mutex;
static TaskHandle_t s_ota_watchdog;
static uint64_t s_ota_prepared_deadline_ms;
static bool s_ota_recovery_pending; /* 写入由操作锁串行，状态入口读取原子快照。 */
static uint32_t s_ota_recovery_retry_delay_ms = 1000U;
static uint64_t s_ota_recovery_retry_after_ms;
static bool s_ota_selecting;
static void ota_prepared_watchdog_task(void *arg);
static void ota_leave_maintenance_mode(void);
static uint64_t ota_monotonic_ms(void);
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
    bool selecting;
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

/** @brief 阻塞取得短状态锁，不调用外部服务。
 * @return 未初始化或 take 失败为 false，true 后本调用者必须 unlock。
 * @note 仅保护 RAM 字段，生命周期操作由递归 operation mutex 串行。
 */
static bool ota_state_lock(void)
{
    if (!s_ota_mutex) return false;
    return xSemaphoreTake(s_ota_mutex, portMAX_DELAY) == pdTRUE;
}

/** @brief 释放本任务已经取得的短状态锁。
 * @pre 当前任务持锁一次，锁未初始化时跳过，无资源转移。
 */
static void ota_state_unlock(void)
{
    if (s_ota_mutex) {
        xSemaphoreGive(s_ota_mutex);
    }
}

/** @brief 清空准备租约的标志、截止时间及所有文件元数据。
 * @pre 调用者持有状态锁，字符串为内部数组，无外部资源需释放。
 */
static void ota_state_clear_prepared_locked(void)
{
    s_ota_prepared = false;
    s_ota_prepared_deadline_ms = 0;
    s_ota_prepared_size = 0;
    s_ota_prepared_name[0] = '\0';
    s_ota_prepared_time[0] = '\0';
    s_ota_prepared_sha256[0] = '\0';
}

/** @brief 映射服务阶段为诊断名称。
 * @param[in] stage 任意阶段枚举值。
 * @return 进程期常量借用，未知值为 unknown，可并发调用。
 */
static const char *ota_service_stage_string(ota_service_stage_t stage)
{
    switch (stage) {
    case OTA_SERVICE_STAGE_IDLE: return "idle";
    case OTA_SERVICE_STAGE_PREPARING: return "preparing";
    case OTA_SERVICE_STAGE_PREPARED: return "prepared";
    case OTA_SERVICE_STAGE_SELECTING: return "selecting";
    case OTA_SERVICE_STAGE_FLASHING: return "flashing";
    case OTA_SERVICE_STAGE_REBOOT_PENDING: return "reboot_pending";
    case OTA_SERVICE_STAGE_FAILED: return "failed";
    default: return "unknown";
    }
}

/** @brief 映射服务失败码为 JSON 名称。
 * @param[in] code 任意失败码。
 * @return 进程期常量借用，未知值为 unknown，无释放责任。
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

/** @brief 取得短状态锁后更新阶段。
 * @param[in] stage 新阶段值，无拥有资源。
 * @post 锁不可用时保持旧值，操作之间的排他由外层 operation mutex 保证。
 */
static void ota_set_stage(ota_service_stage_t stage)
{
    if (ota_state_lock()) {
        s_ota_stage = stage;
        ota_state_unlock();
    }
}

/** @brief 清空失败码及内部诊断字符串。
 * @pre 当前任务持有状态锁，不改变阶段或持久分区。
 */
static void ota_clear_failure_locked(void)
{
    s_ota_failure_code = OTA_SERVICE_FAILURE_NONE;
    s_ota_failure_stage[0] = '\0';
    s_ota_failure_message[0] = '\0';
}

/** @brief 在短状态锁内记录 FAILED 阶段和截断的独立诊断。
 * @param[in] code 失败类别。
 * @param[in] stage 可空同步 NUL 文本借用，NULL 存空串。
 * @param[in] message 可空同步 NUL 文本借用，NULL 存空串。
 * @post 锁不可用时保持旧值，调用者保留输入字符串所有权。
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

/** @brief 在短状态锁内按值复制服务 RAM 状态。
 * @return 独立结构及字符串，锁不可用返回全零快照，无需释放。
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
        snapshot.selecting = s_ota_selecting;
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

/** @brief 原子发布进程期操作锁并安装不可变的传输回调表和 watchdog。
 * @param[in] transport 必填同步表借用，复制函数指针及 user_data 借用。
 * @return 缺少回调、chunk 超过 INT_MAX 或非负超时码为 INVALID_ARG，
 * 锁/任务分配失败为 NO_MEM，已安装不同表为 INVALID_STATE，同表重入为成功。
 * @note 回调和 user_data 须保持到进程结束，同表重入支持 Web 恢复。
 * 初始化失败保留已创建的锁供重试，watchdog 栈为 8192 字节。
 */
esp_err_t ota_service_init(const ota_service_transport_t *transport)
{
    if (!transport ||
        !transport->send_text ||
        !transport->send_error ||
        !transport->query_value ||
        !transport->recv ||
        !transport->stop_web ||
        !transport->start_web || transport->io_chunk > INT_MAX ||
        transport->request_timeout_code >= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!__atomic_load_n(&s_ota_operation_mutex, __ATOMIC_ACQUIRE)) {
        SemaphoreHandle_t candidate = xSemaphoreCreateRecursiveMutex();
        if (!candidate) return ESP_ERR_NO_MEM;
        SemaphoreHandle_t expected = NULL;
        if (!__atomic_compare_exchange_n(&s_ota_operation_mutex, &expected, candidate,
                                         false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            vSemaphoreDelete(candidate);
        }
    }
    xSemaphoreTakeRecursive(s_ota_operation_mutex, portMAX_DELAY);
    if (__atomic_load_n(&s_ota_initialized, __ATOMIC_ACQUIRE)) {
        bool same = s_transport.alloc_io == transport->alloc_io &&
            s_transport.send_text == transport->send_text &&
            s_transport.send_error == transport->send_error &&
            s_transport.query_value == transport->query_value &&
            s_transport.recv == transport->recv &&
            s_transport.json_escape_append == transport->json_escape_append &&
            s_transport.stop_web == transport->stop_web &&
            s_transport.start_web == transport->start_web &&
            s_transport.request_timeout_code == transport->request_timeout_code &&
            s_transport.bad_request_code == transport->bad_request_code &&
            s_transport.server_error_code == transport->server_error_code &&
            s_transport.io_chunk == (transport->io_chunk ? transport->io_chunk : OTA_IO_CHUNK_FALLBACK) &&
            s_transport.user_data == transport->user_data;
        xSemaphoreGiveRecursive(s_ota_operation_mutex);
        return same ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    s_transport = *transport;
    if (s_transport.io_chunk == 0) {
        s_transport.io_chunk = OTA_IO_CHUNK_FALLBACK;
    }
    if (!s_ota_mutex) s_ota_mutex = xSemaphoreCreateMutex();
    if (!s_ota_mutex) {
        xSemaphoreGiveRecursive(s_ota_operation_mutex);
        return ESP_ERR_NO_MEM;
    }
    if (!s_ota_watchdog && xTaskCreate(ota_prepared_watchdog_task, "ota_prepare_watch",
            OTA_PREPARE_WATCH_STACK_BYTES, NULL, 3, &s_ota_watchdog) != pdPASS) {
        xSemaphoreGiveRecursive(s_ota_operation_mutex);
        return ESP_ERR_NO_MEM;
    }
    __atomic_store_n(&s_ota_initialized, true, __ATOMIC_RELEASE);
    xSemaphoreGiveRecursive(s_ota_operation_mutex);
    return ESP_OK;
}

/** @brief 使用已安装 allocator 或 INTERNAL|8BIT 分配同步 IO 缓冲。
 * @param[in] size 请求字节数，本模块传非零值。
 * @return 独占缓冲或 NULL，调用者用 heap_caps_free 释放。
 * @pre 已初始化，allocator 返回值须满足内部 RAM 和 heap_caps_free 契约。
 */
static char *ota_alloc_io(size_t size)
{
    if (s_transport.alloc_io) {
        return s_transport.alloc_io(size);
    }
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/** @brief 同步调用已安装文本发送器。
 * @param[in,out] request 传输层请求借用，空值语义由传输层定义。
 * @param[in] text 必填 NUL 响应借用，回调返回后即可释放。
 * @param[in] type 必填 NUL Content-Type 借用。
 * @return 原样传递发送结果，响应提交后的任务状态不会回滚。
 */
static esp_err_t ota_send_text(void *request, const char *text, const char *type)
{
    return s_transport.send_text(request, text, type);
}

/** @brief 同步发送协议错误响应。
 * @param[in,out] request 传输层同步请求借用。
 * @param[in] code 传输层协议错误码。
 * @param[in] message 必填 NUL 诊断借用，覆盖回调返回。
 * @return 发送器结果，ESP_OK 也可表示错误响应已成功发送。
 */
static esp_err_t ota_send_error(void *request, int code, const char *message)
{
    return s_transport.send_error(request, code, message);
}

/** @brief 获取本次请求的独占查询参数副本。
 * @param[in,out] request 同步请求借用。
 * @param[in] key 必填 NUL 参数名借用。
 * @return 可 heap_caps_free 的 NUL 文本或 NULL，调用者负责释放。
 */
static char *ota_query_value(void *request, const char *key)
{
    return s_transport.query_value(request, key);
}

/** @brief 同步接收一块上传体。
 * @param[in,out] request 请求借用，由传输层推进读取位置。
 * @param[out] buffer 必填至少 length 字节可写缓冲借用。
 * @param[in] length 非负容量，调用者保证不超过 INT_MAX。
 * @return 实际字节数、负超时码或失败码，上层验证是否超出容量。
 */
static int ota_recv(void *request, char *buffer, int length)
{
    return s_transport.recv(request, buffer, length);
}

/** @brief 采样堆容量和下一个更新分区。
 * @param[in] expected_size 本次镜像字节数，只记录数值。
 * @return 独立值快照，无目标时容量和标签为空，采样不预留内存。
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

/** @brief 在短锁内复制最近资源采样。
 * @param[in] snapshot 可空同步值借用，NULL 或锁不可用时跳过。
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

/** @brief 消费构造器操作的拥有结果，sticky 错误仍由构造器保留。
 * @param[in] result 按值移动的结果，内部动态诊断在本函数释放。
 */
static void ota_consume_builder_result(cc_result_t result)
{
    cc_result_free(&result);
}

/** @brief 创建本次响应独占构造器并归还初始化诊断。
 * @param[out] builder 必填新对象，成功后由调用者 take 或 deinit。
 * @return 初始化成功为 true，失败为 false 且对象可清理。
 */
static bool ota_builder_init(cc_string_builder_t *builder)
{
    cc_result_t result = cc_string_builder_init(builder);
    bool ready = result.code == CC_OK;
    cc_result_free(&result);
    return ready;
}

/** @brief 追加带双引号的 JSON 字符串并消费每次 append 结果。
 * @param[in,out] sb 必填本调用者独占构造器，错误粘滞至 take。
 * @param[in] text 可空同步 NUL 借用，NULL 编码为空串。
 * @post 文本按字节复制，控制字符转义，构造器仍归调用者 take/deinit。
 */
static void ota_json_escape_append(cc_string_builder_t *sb, const char *text)
{
    ota_consume_builder_result(cc_string_builder_append_char(sb, '"'));
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '\\': ota_consume_builder_result(cc_string_builder_append(sb, "\\\\")); break;
        case '"': ota_consume_builder_result(cc_string_builder_append(sb, "\\\"")); break;
        case '\n': ota_consume_builder_result(cc_string_builder_append(sb, "\\n")); break;
        case '\r': ota_consume_builder_result(cc_string_builder_append(sb, "\\r")); break;
        case '\t': ota_consume_builder_result(cc_string_builder_append(sb, "\\t")); break;
        default:
            if (*p < 0x20) ota_consume_builder_result(cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p));
            else ota_consume_builder_result(cc_string_builder_append_char(sb, (char)*p));
            break;
        }
    }
    ota_consume_builder_result(cc_string_builder_append_char(sb, '"'));
}

/** @brief 输出并 flush 一条带 OTA 前缀的串口消息。
 * @param[in] message 可空同步 NUL 借用，NULL 输出空串。
 * @note stdio 错误不向上传播，多任务日志行可能交错。
 */
static void ota_serial_note(const char *message)
{
    printf("[ota] %s\r\n", message ? message : "");
    fflush(stdout);
}

/** @brief 格式化最多 159 字节诊断后同步输出。
 * @param[in] fmt 可空 printf 格式借用，NULL 跳过。
 * @param[in] ... 与格式匹配的参数借用，字符串覆盖本次返回。
 * @post 超长正文截断，栈缓冲在返回时销毁，无拥有资源外借。
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

/** @brief 根据已知文件名排除常见非 APP 构建产物。
 * @param[in] name 可空稳定 NUL 文本借用，大小写敏感。
 * @return 命中拒绝名为 true，NULL/空为 false，镜像内容还须后续验证。
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

/** @brief 记录资源采样并验证镜像、PSRAM 和内部 IO 缓冲的容量条件。
 * @param[in] expected_size 非零镜像字节数。
 * @param[out] reason 可空诊断槽，非空且容量非零时清空并终止写入。
 * @param[in] reason_size 诊断槽字节容量，含 NUL，零时跳过。
 * @return 成功、INVALID_SIZE、NOT_FOUND 或 NO_MEM，无分配和预留副作用。
 * @note 检查和实际分配之间其他任务仍可消耗内存，分配必须再次判空。
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

/** @brief 分别等待交互队列和事件 sink 排空并发布诊断。
 * @return 成功或 TIMEOUT，等待上限依次为 30 秒和 5 秒。
 * @pre 调用者持 operation mutex，等待本身不关闭交互 admission。
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

/** @brief 暂停交互 admission、进入事件维护模式并停相机预览。
 * @return 原生暂停/停止结果，失败可能已完成部分进入步骤。
 * @pre 持 operation mutex，调用后失败也须 leave 清理部分状态。
 * @post 保存预览恢复责任，不持短状态锁调用外部服务。
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
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    return ESP_OK;
}

/** @brief 追加最多四个 APP 分区详情及实际分区总数。
 * @param[in,out] sb 必填独占构造器，失败粘滞并由调用者 take 处理。
 * @note 分区字段按值采样，无堆分区数组，所有 append 结果在本层释放。
 */
static void append_ota_partitions(cc_string_builder_t *sb)
{
    ota_manager_partition_info_t partitions[4];
    size_t count = ota_manager_get_partitions(partitions, sizeof(partitions) / sizeof(partitions[0]));
    ota_consume_builder_result(cc_string_builder_append(sb, ",\"partitions\":["));
    size_t emitted = count < (sizeof(partitions) / sizeof(partitions[0])) ?
        count : (sizeof(partitions) / sizeof(partitions[0]));
    for (size_t i = 0; i < emitted; i++) {
        if (i > 0) ota_consume_builder_result(cc_string_builder_append_char(sb, ','));
        ota_consume_builder_result(cc_string_builder_append(sb, "{\"label\":"));
        ota_json_escape_append(sb, partitions[i].label);
        ota_consume_builder_result(cc_string_builder_append(sb, ",\"app_name\":"));
        ota_json_escape_append(sb, partitions[i].app_name);
        ota_consume_builder_result(cc_string_builder_append(sb, ",\"version\":"));
        ota_json_escape_append(sb, partitions[i].version);
        ota_consume_builder_result(cc_string_builder_append(sb, ",\"sha256\":"));
        ota_json_escape_append(sb, partitions[i].sha256);
        ota_consume_builder_result(cc_string_builder_append(sb, ",\"uploaded_at\":"));
        ota_json_escape_append(sb, partitions[i].uploaded_at);
        ota_consume_builder_result(cc_string_builder_appendf(sb,
                                  ",\"size\":%u,\"running\":%s,\"boot\":%s,\"selected\":%s}",
                                  (unsigned)partitions[i].size,
                                  partitions[i].running ? "true" : "false",
                                  partitions[i].boot ? "true" : "false",
                                  partitions[i].boot ? "true" : "false"));
    }
    ota_consume_builder_result(cc_string_builder_append_char(sb, ']'));
    ota_consume_builder_result(cc_string_builder_appendf(sb, ",\"partition_count\":%u", (unsigned)count));
}

/** @brief 检查已采样前四个 APP 分区是否包含标签。
 * @param[in] label 可空同步 NUL 标签借用。
 * @return 匹配为 true，NULL/空或未列入前四项为 false。
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

/** @brief 清除准备租约并尽力恢复网络、Web、交互和原有预览。
 * @pre 持 operation mutex，支持部分进入失败后的重复恢复。
 * @post 恢复未全成功设置 recovery_pending，watchdog 后续重试。
 * 网络/Web 恢复责任和预览恢复责任分别保留，外部调用不持短状态锁。
 */
static void ota_leave_maintenance_mode(void)
{
    bool preview_was_running = false;
    bool restore_services = false;
    if (ota_state_lock()) {
        preview_was_running = s_ota_preview_was_running;
        restore_services = s_ota_flash_services_stopped;
        ota_state_clear_prepared_locked();
        s_ota_preparing = false;
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
    __atomic_store_n(&s_ota_recovery_pending,
                     !services_restored || resume_err != ESP_OK, __ATOMIC_RELEASE);
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
            __atomic_store_n(&s_ota_recovery_pending, true, __ATOMIC_RELEASE);
            if (ota_state_lock()) {
                s_ota_preview_was_running = true;
                ota_state_unlock();
            }
        }
    }
    app_event_publish(APP_EVENT_KIND_STATUS,
                      APP_EVENT_SOURCE_SYSTEM,
                      s_ota_recovery_pending ? CC_LOG_WARN : CC_LOG_INFO,
                      s_ota_recovery_pending ? "OTA maintenance exit incomplete; service recovery pending" :
                                               "OTA upload maintenance mode disabled");
}

/** @brief 记录恢复责任并依次停止 Web 和网络，成功再等待 200 毫秒。
 * @return 首个原生失败或成功，失败也需 leave 恢复已停止的服务。
 * @pre 持 operation mutex，调用线程允许阻塞，当前 HTTP handler 已完成交接。
 */
static esp_err_t ota_stop_network_for_flash(void)
{
    ota_serial_note("stopping web and wifi for flash-sensitive OTA metadata update");
    if (ota_state_lock()) {
        s_ota_flash_services_stopped = true;
        ota_state_unlock();
    }
    if (s_transport.stop_web) {
        esp_err_t err = s_transport.stop_web(s_transport.user_data);
        if (err != ESP_OK) {
            ota_serial_notef("web stop failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    esp_err_t err = network_service_shutdown_for_ota();
    if (err != ESP_OK) {
        ota_serial_notef("network shutdown for OTA: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

/** @brief 获取操作租约后在离线窗口确认运行镜像为 VALID。
 * @return 未初始化、忙或恢复未完成为 INVALID_STATE，其余原生结果。
 * @pre startup owner 已做健康判断，服务初始化完成，调用者允许阻塞恢复。
 * @post 成功要求确认和恢复都成功，恢复失败时镜像可能已经 VALID。
 */
esp_err_t ota_service_confirm_running_app(void)
{
    if (!__atomic_load_n(&s_ota_initialized, __ATOMIC_ACQUIRE) ||
        !s_ota_operation_mutex ||
        xSemaphoreTakeRecursive(s_ota_operation_mutex, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    ota_manager_status_t manager;
    ota_manager_refresh_status();
    ota_manager_get_status(&manager);
    ota_service_state_snapshot_t state = ota_state_snapshot();
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_ota_recovery_pending || state.preparing || state.prepared || state.selecting ||
        state.reboot_pending || state.stage == OTA_SERVICE_STAGE_FLASHING ||
        state.stage == OTA_SERVICE_STAGE_REBOOT_PENDING ||
        manager.state == OTA_MANAGER_UPLOADING ||
        manager.state == OTA_MANAGER_PENDING_REBOOT) {
        goto done;
    }
    if (!manager.rollback_pending) {
        err = ESP_OK;
        goto done;
    }

    /* 标记 VALID 同样擦写 otadata，沿用镜像刷写的离线窗口。
     * 调用者在暂停前完成健康判断，此处暂停 DMA/cache 相关活动再提交。 */
    ota_serial_note("boot confirmation: quiescing services before otadata write");
    err = ota_enter_maintenance_mode();
    if (err == ESP_OK) err = ota_wait_for_quiescent();
    if (err == ESP_OK) err = ota_stop_network_for_flash();
    if (err == ESP_OK) err = ota_manager_mark_app_valid();
    ota_leave_maintenance_mode();
    /* otadata 成功提交后保留原生状态，恢复失败由返回值和 recovery_pending 表达，
     * 管理器 rollback_pending 继续反映实际镜像状态。 */
    if (err == ESP_OK && s_ota_recovery_pending) err = ESP_ERR_INVALID_STATE;
    ota_serial_notef("boot confirmation result=%s recovery_pending=%d",
                     esp_err_to_name(err), s_ota_recovery_pending ? 1 : 0);
done:
    xSemaphoreGiveRecursive(s_ota_operation_mutex);
    return err;
}

/** @brief 取得操作租约后离线分块写入，完成 boot 提交并重启。
 * @param[in] arg 移入的独占 ota_flash_job_t，连同 PSRAM image 在本任务释放。
 * @post 失败 abort writer、恢复服务并释放 job，成功提交后准备 reboot。
 * @note IO scratch 用 heap_caps_free 释放，失败消息在 cancel 前消费，
 * 本任务使用 8192 字节内部栈，退出以 vTaskDeleteWithCaps 回收。
 */
static void ota_flash_task(void *arg)
{
    xSemaphoreTakeRecursive(s_ota_operation_mutex, portMAX_DELAY);
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
    err = ota_stop_network_for_flash();
    if (err != ESP_OK) {
        ota_record_failure(OTA_SERVICE_FAILURE_CONCURRENT_STATE, "flash_quiesce",
                           "failed to stop services before flash write");
        failure_message = "OTA services did not stop";
        goto flash_failed;
    }

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
            ota_serial_notef("reboot drain stopped with error: %s",
                             esp_err_to_name(shutdown_err));
        }
        ota_manager_reboot();
        xSemaphoreGiveRecursive(s_ota_operation_mutex);
        vTaskDeleteWithCaps(NULL);
        return;
    }

flash_failed:
    if (scratch) heap_caps_free(scratch);
    if (job && job->image) heap_caps_free(job->image);
    board_ui_ota_end(false, failure_message ? failure_message : ota_manager_last_error());
    ota_manager_cancel();
    ota_leave_maintenance_mode();
    if (job) heap_caps_free(job);
    xSemaphoreGiveRecursive(s_ota_operation_mutex);
    vTaskDeleteWithCaps(NULL);
}

/** @brief 分配内部 job 并启动 flash worker。
 * @param[in] image 必填 size 字节独占 PSRAM 镜像，成功后移动给任务。
 * @param[in] size 非零镜像字节数。
 * @param[in] name 可空同步 NUL 名称借用，截断复制至 job。
 * @return true 后任务拥有 image，false 时调用者仍拥有 image，临时 job 已释放。
 * @pre 调用者持 operation mutex，先置 FLASHING 以阻止交接间隙的新操作。
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

/** @brief 等待响应发送窗口后请求 lifecycle 排空并重启设备。
 * @param[in] arg 未使用，无资源拥有权。
 * @post 排空失败记录日志后仍触发复位，若 reboot 返回则回收本任务内部栈。
 */
static void ota_reboot_task(void *arg)
{
    (void)arg;
    ota_serial_note("reboot board");
    board_ui_ota_end(true, "Rebooting board.");
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_err_t shutdown_err = app_lifecycle_prepare_for_reboot(5000U);
    if (shutdown_err != ESP_OK) {
        ota_serial_notef("reboot drain stopped with error: %s",
                         esp_err_to_name(shutdown_err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    ota_manager_reboot();
    vTaskDeleteWithCaps(NULL);
}

/** @brief 在短状态锁下预占重启标志后创建 4096 字节内部栈任务。
 * @return 已有重启或创建成功为 true，锁/创建失败为 false。
 * @post 创建失败恢复 IDLE 和重启标志，成功后由设备复位终结进程资源。
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

/** @brief 采样管理器、事件和服务状态并同步发送完整 JSON。
 * @param[in,out] request 同步请求借用，空值语义由传输层定义。
 * @return 未初始化为 INVALID_STATE，其余发送器结果，OOM 尝试发送错误响应。
 * @note 每个来源内部加锁，各来源采样时间可不同，构造器和 JSON 本层释放。
 */
esp_err_t ota_service_status(void *request)
{
    if (!__atomic_load_n(&s_ota_initialized, __ATOMIC_ACQUIRE)) return ESP_ERR_INVALID_STATE;
    ota_manager_refresh_status();
    ota_manager_status_t st;
    app_event_bus_status_t events;
    ota_manager_get_status(&st);
    app_event_bus_get_status(&events);
    ota_service_state_snapshot_t state = ota_state_snapshot();
    bool maintenance = state.preparing ||
                       state.prepared ||
                       state.selecting ||
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
    if (!ota_builder_init(&sb)) {
        return ota_send_error(request, s_transport.server_error_code, "OOM");
    }
    ota_consume_builder_result(cc_string_builder_append(&sb, "{\"state\":"));
    ota_json_escape_append(&sb, service_stage);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"manager_state\":"));
    ota_json_escape_append(&sb, manager_state);
    unsigned progress = st.expected_size ?
        (unsigned)(((uint64_t)st.bytes_written * 100U) / st.expected_size) : 0;
    ota_consume_builder_result(cc_string_builder_appendf(&sb,
                              ",\"bytes\":%u,\"expected\":%u,\"progress\":%u,"
                              "\"target_size\":%u,\"maintenance\":%s,\"upload_name\":",
                              (unsigned)st.bytes_written,
                              (unsigned)st.expected_size,
                              progress,
                              (unsigned)st.target_size,
                              maintenance ? "true" : "false"));
    ota_json_escape_append(&sb, st.upload_name);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"maintenance_reason\":"));
    ota_json_escape_append(&sb, maintenance_reason);
    ota_consume_builder_result(cc_string_builder_appendf(&sb,
                              ",\"preparing\":%s,\"prepared\":%s,\"selecting\":%s,\"reboot_pending\":%s,\"prepared_size\":%u",
                              state.preparing ? "true" : "false",
                              state.prepared ? "true" : "false",
                              state.selecting ? "true" : "false",
                              state.reboot_pending ? "true" : "false",
                              (unsigned)state.prepared_size));
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"failure_code\":"));
    ota_json_escape_append(&sb, ota_service_failure_string(state.failure_code));
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"failure_stage\":"));
    ota_json_escape_append(&sb, state.failure_stage);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"failure_message\":"));
    ota_json_escape_append(&sb, state.failure_message);
    ota_consume_builder_result(cc_string_builder_appendf(&sb,
                              ",\"resource_snapshot\":{\"expected_size\":%u,"
                              "\"target_size\":%u,\"target_label\":",
                              (unsigned)state.resources.expected_size,
                              (unsigned)state.resources.target_size));
    ota_json_escape_append(&sb, state.resources.target_label);
    ota_consume_builder_result(cc_string_builder_appendf(&sb,
                              ",\"psram_free\":%u,\"psram_largest\":%u,\"psram_reserve\":%u,"
                              "\"internal_free\":%u,\"internal_largest\":%u,"
                              "\"internal_reserve\":%u,\"io_chunk\":%u}",
                              (unsigned)state.resources.psram_free,
                              (unsigned)state.resources.psram_largest,
                              (unsigned)state.resources.psram_reserve,
                              (unsigned)state.resources.internal_free,
                              (unsigned)state.resources.internal_largest,
                              (unsigned)state.resources.internal_reserve,
                              (unsigned)state.resources.io_chunk));
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"selected\":"));
    ota_json_escape_append(&sb, st.boot_label);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"running\":"));
    ota_json_escape_append(&sb, st.running_label);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"running_app_name\":"));
    ota_json_escape_append(&sb, st.running_app_name);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"running_version\":"));
    ota_json_escape_append(&sb, st.running_version);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"running_sha256\":"));
    ota_json_escape_append(&sb, st.running_sha256);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"running_uploaded_at\":"));
    ota_json_escape_append(&sb, st.running_uploaded_at);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"running_ota_state\":"));
    ota_json_escape_append(&sb, st.running_ota_state);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"boot\":"));
    ota_json_escape_append(&sb, st.boot_label);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"boot_app_name\":"));
    ota_json_escape_append(&sb, st.boot_app_name);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"boot_version\":"));
    ota_json_escape_append(&sb, st.boot_version);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"boot_sha256\":"));
    ota_json_escape_append(&sb, st.boot_sha256);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"boot_uploaded_at\":"));
    ota_json_escape_append(&sb, st.boot_uploaded_at);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"target\":"));
    ota_json_escape_append(&sb, st.target_label);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"target_app_name\":"));
    ota_json_escape_append(&sb, st.target_app_name);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"target_version\":"));
    ota_json_escape_append(&sb, st.target_version);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"target_sha256\":"));
    ota_json_escape_append(&sb, st.target_sha256);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"target_uploaded_at\":"));
    ota_json_escape_append(&sb, st.target_uploaded_at);
    ota_consume_builder_result(cc_string_builder_appendf(&sb, ",\"rollback_pending\":%s,\"recovery_pending\":%s,\"error\":",
        st.rollback_pending ? "true" : "false",
        __atomic_load_n(&s_ota_recovery_pending, __ATOMIC_ACQUIRE) ? "true" : "false"));
    ota_json_escape_append(&sb, st.error);
    append_ota_partitions(&sb);
    ota_consume_builder_result(cc_string_builder_append_char(&sb, '}'));
    char *json = cc_string_builder_take(&sb);
    if (!json) return ota_send_error(request, s_transport.server_error_code, "OOM");
    esp_err_t err = ota_send_text(request, json, "application/json");
    free(json);
    return err;
}

/** @brief 获取启动以来的单调毫秒计数。
 * @return 原生微秒时间除千，非正值归零，无拥有资源，可并发调用。
 */
static uint64_t ota_monotonic_ms(void)
{
    int64_t us = esp_timer_get_time();
    return us > 0 ? (uint64_t)us / 1000ULL : 0;
}

/** @brief 逐位解析非零无符号十进制字节数并检查 size_t 溢出。
 * @param[in] text 可空同步 NUL 借用，仅接受数字，不接受空白或符号。
 * @param[out] out_size 必填独占值槽，有效槽失败时写零。
 * @return 解析完整且非零为 true，其他为 false，无分配。
 */
static bool ota_parse_size(const char *text, size_t *out_size)
{
    if (!out_size) return false;
    *out_size = 0;
    if (!text || !text[0]) return false;
    size_t parsed = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        unsigned digit = *p - '0';
        if (parsed > (SIZE_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    if (!parsed) return false;
    *out_size = parsed;
    return true;
}

/** @brief 验证六十四位十六进制 SHA-256 并转小写。
 * @param[in] text 可空同步 NUL 借用，必须恰好六十四字节。
 * @param[out] out 必填至少 65 字节槽，成功终止，失败可能已有部分写入。
 * @return 格式合法为 true，失败为 false，失败输出不得作为摘要使用。
 */
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

/** @brief 将二进制 SHA-256 编码为六十四字节小写文本。
 * @param[in] digest 必填可读三十二字节同步借用。
 * @param[out] out 必填至少 65 字节独占输出，末尾写 NUL。
 */
static void ota_sha256_hex(const unsigned char digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out[i * 2] = hex[(digest[i] >> 4) & 0x0fU];
        out[i * 2 + 1] = hex[digest[i] & 0x0fU];
    }
    out[64] = '\0';
}

/** @brief 在 flash 前检查 ESP 头、段数、各段边界和尾部至少一字节。
 * @param[in] image 可空 size 字节稳定借用，按字节读取以避免未对齐访问。
 * @param[in] size 完整 BIN 字节数。
 * @return 初步布局合法为 true，无分配，最终签名/校验由 esp_ota_end 完成。
 */
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
    /* 所有已声明段之后至少还要容纳 ESP 校验和字节。 */
    return offset < size;
}

/** @brief 校验查询参数和容量、等待空闲、进入维护并建立六十秒准备租约。
 * @param[in,out] request 同步请求借用，读取 name/size/sha256/time。
 * @return 发送器结果，协议拒绝也可返回 ESP_OK，失败详情写入服务状态。
 * @pre 持 operation mutex，查询参数副本由本层 heap_caps_free。
 * @post 部分进入失败和发送失败由外层 wrapper 执行维护恢复。
 */
static esp_err_t ota_service_prepare_impl(void *request)
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
        s_ota_prepared_deadline_ms = ota_monotonic_ms() + OTA_PREPARED_TIMEOUT_MS;
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

/** @brief 接收整个上传体至 PSRAM，校验 SHA-256 和布局后交接 flash 任务。
 * @param[in,out] request 同步请求借用，用于查询和 recv，绝不交给 worker。
 * @param[in] content_len 正镜像字节数，须与准备元数据一致。
 * @param[out] handed_off 必填 bool 槽，任务创建成功写 true，其他路径保持原值。
 * @return 发送器结果，错误响应可成功发送，任务接收所有权后发送失败仍继续。
 * @pre 持 operation mutex，外层将 handed_off 初始化为 false。
 * @post 各失败路径释放参数、IO、镜像和 hash 状态，成功仅移动镜像给任务。
 */
static esp_err_t ota_service_upload_impl(void *request, int content_len, bool *handed_off)
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
        size_t capacity = (size_t)remaining;
        if (capacity > s_transport.io_chunk) capacity = s_transport.io_chunk;
        int n = ota_recv(request, buf, (int)capacity);
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
        if (n <= 0 || (size_t)n > capacity) {
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
    *handed_off = true;
    return ota_send_text(request,
                                 "{\"ok\":true,\"flashing\":true,\"offline\":true}",
                                 "application/json");
}

/** @brief 查询目标标签，已有 boot 同步响应，其他标签交给离线选择任务。
 * @param[in,out] request 同步请求借用，读取 label 后本层释放其副本。
 * @return 发送器结果，异步接受后响应 OOM 使用固定 JSON，worker 继续运行。
 * @pre 持 operation mutex，任务创建成功后 job 所有权转移。
 */
static esp_err_t ota_service_select_impl(void *request)
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
    if (boot_already_set) {
        ota_manager_refresh_status();
        ota_manager_get_status(&st);
    } else {
        if (strlen(label) >= sizeof(((ota_select_job_t *)0)->label)) {
            heap_caps_free(label);
            return ota_send_error(request, s_transport.bad_request_code,
                                  "partition label is too long");
        }
        ota_select_job_t *job = calloc(1, sizeof(*job));
        if (!job) {
            heap_caps_free(label);
            return ota_send_error(request, s_transport.server_error_code, "OOM");
        }
        snprintf(job->label, sizeof(job->label), "%s", label);
        if (!ota_state_lock()) {
            free(job);
            heap_caps_free(label);
            return ota_send_error(request, s_transport.server_error_code,
                                  "OTA state lock failed");
        }
        s_ota_selecting = true;
        s_ota_stage = OTA_SERVICE_STAGE_SELECTING;
        ota_state_unlock();

        BaseType_t task_result = xTaskCreateWithCaps(
            ota_select_task, "ota_select", OTA_SELECT_TASK_STACK_BYTES, job,
            5, NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (task_result != pdPASS) {
            if (ota_state_lock()) {
                s_ota_selecting = false;
                s_ota_stage = OTA_SERVICE_STAGE_IDLE;
                ota_state_unlock();
            }
            free(job);
            heap_caps_free(label);
            return ota_send_error(request, s_transport.server_error_code,
                                  "OTA select task start failed");
        }

        cc_string_builder_t accepted;
        if (!ota_builder_init(&accepted)) {
            /* 任务已经承接操作，用固定响应报告已接受，继续保留任务所有权。 */
            heap_caps_free(label);
            return ota_send_text(request,
                "{\"ok\":true,\"selecting\":true,\"reboot_required\":true}",
                "application/json");
        }
        ota_consume_builder_result(cc_string_builder_append(&accepted,
            "{\"ok\":true,\"selecting\":true,\"reboot_required\":true,\"selected\":"));
        ota_json_escape_append(&accepted, label);
        ota_consume_builder_result(cc_string_builder_append_char(&accepted, '}'));
        char *accepted_json = cc_string_builder_take(&accepted);
        heap_caps_free(label);
        if (!accepted_json) {
            return ota_send_text(request,
                "{\"ok\":true,\"selecting\":true,\"reboot_required\":true}",
                "application/json");
        }
        esp_err_t send_err = ota_send_text(request, accepted_json, "application/json");
        free(accepted_json);
        return send_err;
    }
    bool reboot_required = st.running_label[0] &&
                           st.boot_label[0] &&
                           strcmp(st.running_label, st.boot_label) != 0;
    cc_string_builder_t sb;
    if (!ota_builder_init(&sb)) {
        heap_caps_free(label);
        return ota_send_error(request, s_transport.server_error_code, "OOM");
    }
    ota_consume_builder_result(cc_string_builder_appendf(&sb,
                              "{\"ok\":true,\"pending_reboot\":%s,\"reboot_required\":%s,\"running\":",
                              reboot_required ? "true" : "false",
                              reboot_required ? "true" : "false"));
    ota_json_escape_append(&sb, st.running_label);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"boot\":"));
    ota_json_escape_append(&sb, st.boot_label);
    ota_consume_builder_result(cc_string_builder_append(&sb, ",\"selected\":"));
    ota_json_escape_append(&sb, st.boot_label);
    ota_consume_builder_result(cc_string_builder_append_char(&sb, '}'));
    char *json = cc_string_builder_take(&sb);
    heap_caps_free(label);
    if (!json) return ota_send_error(request, s_transport.server_error_code, "OOM");
    esp_err_t err = ota_send_text(request, json, "application/json");
    free(json);
    return err;
}

/** @brief 等待 HTTP 响应窗口后离线提交 boot 分区并恢复服务。
 * @param[in] arg 移入的独占 calloc job，正常任务结束 free，NULL 直接退任务。
 * @post 操作锁覆盖暂停、写入及恢复，失败记录诊断，退出回收内部任务栈。
 * 恢复失败由 watchdog 按 recovery_pending 重试。
 */
static void ota_select_task(void *arg)
{
    ota_select_job_t *job = (ota_select_job_t *)arg;
    if (!job) {
        vTaskDeleteWithCaps(NULL);
        return;
    }
    /* 等待 HTTP handler 发送接受响应的窗口，再执行停止 httpd 的后续步骤。 */
    vTaskDelay(pdMS_TO_TICKS(250U));
    xSemaphoreTakeRecursive(s_ota_operation_mutex, portMAX_DELAY);

    esp_err_t err = ota_enter_maintenance_mode();
    if (err == ESP_OK) err = ota_wait_for_quiescent();
    if (err == ESP_OK) err = ota_stop_network_for_flash();
    if (err == ESP_OK) err = ota_manager_set_boot_partition(job->label);

    /* 维护命令失败前可能已经暂停交互 admission，统一执行恢复，
     * 同时处理事件总线尚未进入维护的部分完成路径。 */
    ota_leave_maintenance_mode();
    if (err != ESP_OK) {
        ota_record_failure(OTA_SERVICE_FAILURE_CONCURRENT_STATE, "select_offline",
                           esp_err_to_name(err));
        app_event_publishf(APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_SYSTEM, CC_LOG_ERROR,
                           "OTA partition selection failed: %s", esp_err_to_name(err));
    } else {
        ota_manager_refresh_status();
        app_event_publishf(APP_EVENT_KIND_STATUS, APP_EVENT_SOURCE_SYSTEM, CC_LOG_INFO,
                           "OTA boot partition selected: %s", job->label);
    }
    if (ota_state_lock()) {
        s_ota_selecting = false;
        if (err != ESP_OK) s_ota_stage = OTA_SERVICE_STAGE_FAILED;
        else if (s_ota_stage == OTA_SERVICE_STAGE_SELECTING)
            s_ota_stage = OTA_SERVICE_STAGE_IDLE;
        ota_state_unlock();
    }
    xSemaphoreGiveRecursive(s_ota_operation_mutex);
    free(job);
    vTaskDeleteWithCaps(NULL);
}

/** @brief 拒绝活动 writer 后启动延迟 reboot 并发送接受响应。
 * @param[in,out] request 同步传输请求借用。
 * @return 发送器结果，发送失败时已启动任务仍会复位。
 * @pre 持 operation mutex，无请求或消息拥有权传给重启任务。
 */
static esp_err_t ota_service_reboot_impl(void *request)
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

/** @brief abort 管理器、清空失败状态并尝试恢复维护资源后响应。
 * @param[in,out] request 同步请求借用。
 * @return 发送器结果，恢复责任通过 recovery_pending 继续保留。
 * @pre 持 operation mutex，外层已排除异步 flash/select/reboot。
 */
static esp_err_t ota_service_cancel_impl(void *request)
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

/** @brief 在当前操作锁内重读准备截止时间并恢复已到期租约。
 * @return 到期并执行恢复为 true，否则 false，无资源转移。
 * @pre 持 operation mutex，上传已认领的 deadline 为零，另受 recv 超时限制。
 */
static bool ota_expire_prepared(void)
{
    ota_state_lock();
    bool expired = s_ota_prepared && s_ota_prepared_deadline_ms != 0 &&
        ota_monotonic_ms() >= s_ota_prepared_deadline_ms;
    ota_state_unlock();
    if (!expired) return false;
    ota_record_failure(OTA_SERVICE_FAILURE_UPLOAD_TIMEOUT, "prepare_expired",
                       "prepared upload expired before upload started");
    ota_leave_maintenance_mode();
    board_ui_ota_end(false, "OTA preparation expired");
    return true;
}

/** @brief 每秒尝试获取操作锁，处理准备到期和指数退避恢复。
 * @param[in] arg 未使用，无拥有资源。
 * @note 任务进程期运行，恢复退避上限六十秒，取得锁后重新读取恢复状态，
 * 避免旧的观察值取消新准备租约，慢服务调用只持 operation mutex。
 */
static void ota_prepared_watchdog_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        bool recovering = __atomic_load_n(&s_ota_recovery_pending, __ATOMIC_ACQUIRE);
        if (recovering) {
            uint64_t now_ms = ota_monotonic_ms();
            if (s_ota_recovery_retry_after_ms != 0 &&
                now_ms < s_ota_recovery_retry_after_ms) continue;
        }
        if (xSemaphoreTakeRecursive(s_ota_operation_mutex, 0) != pdTRUE) continue;
        recovering = __atomic_load_n(&s_ota_recovery_pending, __ATOMIC_ACQUIRE);
        if (recovering) {
            ota_leave_maintenance_mode();
            if (__atomic_load_n(&s_ota_recovery_pending, __ATOMIC_ACQUIRE)) {
                if (s_ota_recovery_retry_delay_ms < 60000U) {
                    s_ota_recovery_retry_delay_ms *= 2U;
                    if (s_ota_recovery_retry_delay_ms > 60000U)
                        s_ota_recovery_retry_delay_ms = 60000U;
                }
                s_ota_recovery_retry_after_ms =
                    ota_monotonic_ms() + s_ota_recovery_retry_delay_ms;
            } else {
                s_ota_recovery_retry_delay_ms = 1000U;
                s_ota_recovery_retry_after_ms = 0;
            }
        } else {
            (void)ota_expire_prepared();
            if (!__atomic_load_n(&s_ota_recovery_pending, __ATOMIC_ACQUIRE)) {
                s_ota_recovery_retry_delay_ms = 1000U;
                s_ota_recovery_retry_after_ms = 0;
            }
        }
        xSemaphoreGiveRecursive(s_ota_operation_mutex);
    }
}

/** @brief 非阻塞取得操作租约并拒绝恢复、异步任务或重启冲突。
 * @param[in,out] request 同步请求借用，冲突时尝试发送拒绝响应。
 * @return true 后本任务持有递归 operation mutex，false 时无锁须释放。
 * @note 初始化前返回 false，任务交接的 FLASHING 标志覆盖 worker 尚未取锁窗口。
 */
static bool ota_begin_operation(void *request)
{
    if (!__atomic_load_n(&s_ota_initialized, __ATOMIC_ACQUIRE)) return false;
    if (!s_ota_operation_mutex ||
        xSemaphoreTakeRecursive(s_ota_operation_mutex, 0) != pdTRUE) {
        ota_send_error(request, s_transport.bad_request_code, "OTA recovery in progress; retry");
        return false;
    }
    ota_manager_status_t manager;
    ota_manager_get_status(&manager);
    ota_service_state_snapshot_t state = ota_state_snapshot();
    if (s_ota_recovery_pending || manager.state == OTA_MANAGER_UPLOADING ||
        state.selecting || state.stage == OTA_SERVICE_STAGE_FLASHING || state.reboot_pending ||
        state.stage == OTA_SERVICE_STAGE_REBOOT_PENDING) {
        xSemaphoreGiveRecursive(s_ota_operation_mutex);
        ota_send_error(request, s_transport.bad_request_code, "OTA recovery, flash or reboot in progress");
        return false;
    }
    return true;
}

/** @brief 串行释放旧准备租约并准备新上传，失败后恢复部分维护状态。
 * @param[in,out] request 同步请求借用，不保存至返回后。
 * @return 入场失败为 ESP_FAIL，其余为发送器结果，详情见服务状态。
 */
esp_err_t ota_service_prepare(void *request)
{
    if (!ota_begin_operation(request)) return ESP_FAIL;
    /* 重新准备先释放旧租约并恢复服务，再校验新请求。 */
    if (ota_state_snapshot().prepared) ota_leave_maintenance_mode();
    esp_err_t err = s_ota_recovery_pending ?
        ota_send_error(request, s_transport.bad_request_code, "OTA recovery in progress; retry") :
        ota_service_prepare_impl(request);
    if (err != ESP_OK || ota_state_snapshot().stage == OTA_SERVICE_STAGE_FAILED) {
        ota_leave_maintenance_mode();
    }
    xSemaphoreGiveRecursive(s_ota_operation_mutex);
    return err;
}

/** @brief 串行认领有效准备租约并执行受限接收和异步 flash 交接。
 * @param[in,out] request 同步请求借用，worker 不保留此指针。
 * @param[in] content_len 镜像正字节数，非正值发送参数错误。
 * @return 入场失败为 ESP_FAIL，其余为发送器结果，任务交接后继续异步写入。
 * @post 未交接的失败清理维护，交接成功由 worker 承接全部镜像和恢复责任。
 */
esp_err_t ota_service_upload(void *request, int content_len)
{
    if (!ota_begin_operation(request)) return ESP_FAIL;
    if (ota_expire_prepared()) {
        xSemaphoreGiveRecursive(s_ota_operation_mutex);
        return ota_send_error(request, s_transport.bad_request_code, "OTA preparation expired; prepare again");
    }
    ota_state_lock();
    s_ota_prepared_deadline_ms = 0;
    ota_state_unlock();
    bool handed_off = false;
    esp_err_t err = ota_service_upload_impl(request, content_len, &handed_off);
    /* send_error 成功发送 HTTP 错误时同样可返回 ESP_OK，须同时检查业务阶段。 */
    if (!handed_off && (err != ESP_OK || ota_state_snapshot().stage == OTA_SERVICE_STAGE_FAILED)) {
        ota_leave_maintenance_mode();
    }
    xSemaphoreGiveRecursive(s_ota_operation_mutex);
    return err;
}

/** @brief 在操作租约内取消当前准备和管理器事务并恢复服务。
 * @param[in,out] request 同步传输请求借用。
 * @return 冲突为 ESP_FAIL，其余发送器结果，恢复未完成交由 watchdog。
 */
esp_err_t ota_service_cancel(void *request)
{
    if (!ota_begin_operation(request)) return ESP_FAIL;
    esp_err_t err = ota_service_cancel_impl(request);
    xSemaphoreGiveRecursive(s_ota_operation_mutex);
    return err;
}

/** @brief 串行释放准备租约并请求异步选择 boot 分区。
 * @param[in,out] request 同步请求借用，读取 label。
 * @return 入场冲突为 ESP_FAIL，其余发送器结果，接受后的状态通过 status 查询。
 */
esp_err_t ota_service_select(void *request)
{
    if (!ota_begin_operation(request)) return ESP_FAIL;
    if (ota_state_snapshot().prepared) ota_leave_maintenance_mode();
    esp_err_t err = s_ota_recovery_pending ?
        ota_send_error(request, s_transport.bad_request_code, "OTA recovery in progress; retry") :
        ota_service_select_impl(request);
    xSemaphoreGiveRecursive(s_ota_operation_mutex);
    return err;
}

/** @brief 串行退出准备租约并安排延迟设备复位。
 * @param[in,out] request 同步请求借用。
 * @return 冲突为 ESP_FAIL，其余发送器结果，接受后复位由任务负责。
 */
esp_err_t ota_service_reboot(void *request)
{
    if (!ota_begin_operation(request)) return ESP_FAIL;
    if (ota_state_snapshot().prepared) ota_leave_maintenance_mode();
    esp_err_t err = s_ota_recovery_pending ?
        ota_send_error(request, s_transport.bad_request_code, "OTA recovery in progress; retry") :
        ota_service_reboot_impl(request);
    xSemaphoreGiveRecursive(s_ota_operation_mutex);
    return err;
}
