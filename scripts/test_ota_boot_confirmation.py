"""执行生产 OTA 确认与启动生命周期函数，核对失败清理和配置准入。"""
from pathlib import Path
from test_event_ring_ownership import run_c_test

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
/* Host fixture models the acquire load of immutable post-init transport state. */
#ifndef __GNUC__
#define __ATOMIC_ACQUIRE 2
#define __atomic_load_n(pointer, order) (*(pointer))
#endif
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define pdTRUE 1
#define OTA_SERVICE_STAGE_FLASHING 4
#define OTA_SERVICE_STAGE_REBOOT_PENDING 5
#define OTA_MANAGER_UPLOADING 1
#define OTA_MANAGER_PENDING_REBOOT 2
typedef struct { int state; bool rollback_pending; } ota_manager_status_t;
typedef struct { bool preparing, prepared, reboot_pending, selecting; int stage; } ota_service_state_snapshot_t;
static int s_ota_operation_mutex = 1;
static bool s_ota_recovery_pending;
static bool s_ota_initialized = true;
static ota_manager_status_t manager_value;
static ota_service_state_snapshot_t state_value;
static int locked, allow_lock, fail_step, writes, restores, fail_restore;
static char trace[64];
static void step(char c) { size_t n = strlen(trace); trace[n] = c; trace[n + 1] = 0; }
static int xSemaphoreTakeRecursive(int mutex, int ticks) {
    (void)mutex; (void)ticks;
    if (!allow_lock) return 0;
    assert(!locked); locked = 1; return pdTRUE;
}
static void xSemaphoreGiveRecursive(int mutex) {
    (void)mutex; assert(locked); locked = 0;
}
static void ota_manager_refresh_status(void) {}
static void ota_manager_get_status(ota_manager_status_t *s) { *s = manager_value; }
static ota_service_state_snapshot_t ota_state_snapshot(void) { return state_value; }
static void ota_serial_note(const char *s) { (void)s; }
static void ota_serial_notef(const char *s, ...) { (void)s; }
static const char *esp_err_to_name(int e) { (void)e; return "test"; }
static int ota_enter_maintenance_mode(void) { step('P'); return fail_step == 1 ? 11 : 0; }
static int ota_wait_for_quiescent(void) { step('Q'); return fail_step == 2 ? 12 : 0; }
static int ota_stop_network_for_flash(void) { step('N'); return fail_step == 3 ? 13 : 0; }
static int ota_manager_mark_app_valid(void) {
    assert(strcmp(trace, "PQN") == 0);
    step('W'); ++writes; return fail_step == 4 ? 14 : 0;
}
static void ota_leave_maintenance_mode(void) { step('R'); ++restores; s_ota_recovery_pending = fail_restore != 0; }
'''

TESTS = r'''
static void reset(void) {
    assert(!locked);
    s_ota_initialized = true;
    memset(&state_value, 0, sizeof(state_value));
    manager_value.state = 0; manager_value.rollback_pending = true;
    s_ota_recovery_pending = false;
    allow_lock = 1; fail_step = writes = restores = fail_restore = 0;
    trace[0] = 0;
}
int main(void) {
    reset(); s_ota_initialized = false;
    assert(ota_service_confirm_running_app() == ESP_ERR_INVALID_STATE);
    assert(!writes && !restores && !locked);
    reset();
    assert(ota_service_confirm_running_app() == ESP_OK);
    assert(strcmp(trace, "PQNWR") == 0 && writes == 1 && restores == 1 && !locked);
    reset(); fail_restore = 1;
    assert(ota_service_confirm_running_app() == ESP_ERR_INVALID_STATE);
    assert(writes == 1 && restores == 1 && s_ota_recovery_pending && !locked);
    for (int i = 1; i <= 4; ++i) {
        reset(); fail_step = i;
        assert(ota_service_confirm_running_app() == 10 + i);
        assert(writes == (i == 4 ? 1 : 0));
        assert(restores == 1 && !locked);
    }
    reset(); manager_value.rollback_pending = false;
    assert(ota_service_confirm_running_app() == ESP_OK);
    assert(!writes && !restores && !locked);
    reset(); allow_lock = 0;
    assert(ota_service_confirm_running_app() == ESP_ERR_INVALID_STATE);
    assert(!writes && !restores && !locked);
    for (int i = 0; i < 9; ++i) {
        reset();
        if (i == 0) s_ota_recovery_pending = true;
        if (i == 1) state_value.preparing = true;
        if (i == 2) state_value.prepared = true;
        if (i == 3) state_value.reboot_pending = true;
        if (i == 4) state_value.stage = OTA_SERVICE_STAGE_FLASHING;
        if (i == 5) state_value.stage = OTA_SERVICE_STAGE_REBOOT_PENDING;
        if (i == 6) manager_value.state = OTA_MANAGER_UPLOADING;
        if (i == 7) manager_value.state = OTA_MANAGER_PENDING_REBOOT;
        if (i == 8) state_value.selecting = true;
        assert(ota_service_confirm_running_app() == ESP_ERR_INVALID_STATE);
        assert(!writes && !restores && !locked);
    }
    puts("OTA boot confirmation: offline write ordering, preparation/drain/network/write failures, restore, valid image, busy states: PASS");
    return 0;
}
'''


def source_function(relative_path, signature):
    """读取并提取一个完整生产 C 函数，保留其实际控制流。"""
    text = (ROOT / relative_path).read_text(encoding='utf-8')
    start = text.index(signature)
    return text[start:text.index('\n}\n', start) + 3]


BUILDER_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define CC_OK 0
#define CC_ERR_INVALID_ARGUMENT 1
#define CC_ERR_INVALID_STATE 2
#define CC_ERR_OUT_OF_MEMORY 3
#define CC_ERR_LIMIT_EXCEEDED 4
#define CC_ERR_UNKNOWN 5
#define SB_INITIAL_CAPACITY 256
typedef struct { int code; char *message; } cc_result_t;
typedef struct { char *data; size_t length, capacity; int error_code; } cc_string_builder_t;
static int messages_created, messages_released, fail_builder_init, fail_builder_growth;
/** 构造无拥有消息的成功结果。 */
static cc_result_t cc_result_ok(void) { return (cc_result_t){CC_OK, NULL}; }
/** 复制真实拥有的错误消息，独立于 builder 分配故障。 */
static cc_result_t cc_result_error(int code, const char *message) {
    size_t length = strlen(message) + 1U;
    char *owned = malloc(length);
    assert(owned);
    memcpy(owned, message, length);
    messages_created++;
    return (cc_result_t){code, owned};
}
/** 消费返回结果拥有的消息并清零。 */
static void cc_result_free(cc_result_t *result) {
    if (result->message) { messages_released++; free(result->message); }
    *result = cc_result_ok();
}
/** 只对实际 builder 的初始缓冲注入失败。 */
static void *builder_malloc(size_t size) { return fail_builder_init ? NULL : malloc(size); }
/** 只对实际 builder 的扩容注入失败。 */
static void *builder_realloc(void *buffer, size_t size) {
    return fail_builder_growth ? NULL : realloc(buffer, size);
}
#define malloc builder_malloc
#define realloc builder_realloc
'''


def builder_source():
    """提取真实 sticky builder 所需函数，并把故障宏限制在这些函数内。"""
    path = 'third_party/cclaw-sdk/cclaw/core/src/util/cc_string_builder.c'
    signatures = (
        'cc_result_t cc_string_builder_init(',
        'static cc_result_t ensure_capacity(',
        'cc_result_t cc_string_builder_append(',
        'cc_result_t cc_string_builder_appendf(',
        'cc_result_t cc_string_builder_append_char(',
        'char *cc_string_builder_take(',
        'void cc_string_builder_deinit(',
    )
    return ''.join(source_function(path, signature) for signature in signatures) + '\n#undef malloc\n#undef realloc\n'


STATUS_PREFIX = r'''
typedef struct { int preview_frames, preview_drops, preview_fps, latest_jpeg_bytes, width, height; } camera_status_t;
typedef struct {
    int enabled, connected, ap_enabled, ap_started, keepalive, retry_count, max_retry;
    int last_disconnect_reason, time_synced, stopping, command_high_water;
    int wifi_event_coalesced, config_coalesced;
    uint64_t owner_heartbeat_ms;
    const char *ssid, *sta_ip, *ap_ssid, *ap_ip, *mdns_hostname;
} network_status_t;
typedef struct {
    uint64_t uptime_ms;
    int usb_mounted, camera_preview_running;
    camera_status_t camera;
    network_status_t network;
} app_status_t;
typedef struct { app_status_t status; } app_context_t;
typedef struct {
    int lifecycle, maintenance_mode, serial_debug_enabled, usb_log_enabled, usb_log_started;
    int usb_log_fallback_serial, dropped, ring_overwrites, sink_dropped, published;
    int maintenance_skipped, oom_dropped, usb_queue_dropped, display_queue_dropped;
    int max_usb_queue_waiting, max_display_queue_waiting, oldest_seq, latest_seq;
    int capacity, retained_count, retained_bytes, usb_bytes_written;
    const char *maintenance_reason, *usb_log_path, *log_route, *chat_route;
    const char *thinking_route, *tool_route, *status_route, *error_route, *profile_route;
} app_event_bus_status_t;
typedef struct {
    int state, started, busy, pending, queue_depth, queue_high_water, late_reply_count;
    int cancel_latency_ms, max_input_bytes, thinking_enabled, stream_supported;
    unsigned generation;
    uint64_t owner_heartbeat_ms;
    const char *session_id;
} interaction_service_status_t;
typedef struct {
    int input_frames, published_frames, source_updates, rate_limited_frames, replaced_pending_frames;
    int no_buffer_frames, cache_sync_errors, input_row_bytes, stride_bytes, buffer_bytes, buffer_count;
    int integrity_checks, integrity_mismatches, last_mismatch_frame, pink_jump_candidates;
    int last_pink_frame, last_pink_before_rgb, last_pink_after_rgb;
} board_ui_preview_stats_t;
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
static void *s_recovery;
static int profile_calls;
/** 各查询输出独立的零值快照，uptime 用于正常输出对照。 */
static void app_status_collect(app_status_t *out) { memset(out, 0, sizeof(*out)); out->uptime_ms = 17; }
static void app_event_bus_get_status(app_event_bus_status_t *out) { memset(out, 0, sizeof(*out)); }
static void interaction_service_get_status(interaction_service_status_t *out) { memset(out, 0, sizeof(*out)); }
static void board_ui_camera_preview_get_stats(board_ui_preview_stats_t *out) { memset(out, 0, sizeof(*out)); }
static size_t heap_caps_get_free_size(int caps) { (void)caps; return 0; }
static size_t heap_caps_get_largest_free_block(int caps) { (void)caps; return 0; }
static size_t heap_caps_get_total_size(int caps) { (void)caps; return 0; }
static const char *app_event_bus_lifecycle_string(int value) { (void)value; return "new"; }
static const char *interaction_service_state_string(int value) { (void)value; return "new"; }
static const char *recovery_service_mode_name(void *service) { (void)service; return "none"; }
/** 外部 profile 追加入口消费自己的返回消息，隔离本文件的清理责任。 */
static void app_profile_append_status_json(cc_string_builder_t *builder) {
    profile_calls++;
    cc_result_t rc = cc_string_builder_append(builder, "\"profile\":{}");
    cc_result_free(&rc);
}
'''


LOG_PREFIX = r'''
typedef int esp_log_level_t;
typedef int board_conversation_role_t;
typedef int cc_log_level_t;
#define ESP_LOG_ERROR 1
#define ESP_LOG_WARN 2
#define ESP_LOG_INFO 3
#define ESP_LOG_DEBUG 4
#define ESP_LOG_VERBOSE 5
#define BOARD_CONV_ERROR 1
#define BOARD_CONV_STATUS 2
#define CC_LOG_ERROR 1
#define CC_LOG_WARN 2
#define CC_LOG_INFO 3
#define CC_LOG_DEBUG 4
#define APP_EVENT_KIND_ERROR 1
#define APP_EVENT_KIND_STATUS 2
#define APP_EVENT_SOURCE_SYSTEM 1
static int log_dispatches, fail_format;
#define ESP_LOGE(...) ((void)++log_dispatches)
#define ESP_LOGW(...) ((void)++log_dispatches)
#define ESP_LOGI(...) ((void)++log_dispatches)
#define ESP_LOGD(...) ((void)++log_dispatches)
static void board_ui_boot_log_append(int role, const char *text) {
    (void)role; (void)text; log_dispatches++;
}
static void app_event_publish(int kind, int source, int level, const char *text) {
    (void)kind; (void)source; (void)level; (void)text; log_dispatches++;
}
/** 失败时保持输出缓冲未写，核对生产日志不会继续分发它。 */
static int controlled_vsnprintf(char *out, size_t size, const char *format, va_list args) {
    return fail_format ? -1 : vsnprintf(out, size, format, args);
}
#define vsnprintf controlled_vsnprintf
'''


def test_lifecycle_json_and_log_failure_cleanup():
    """编译真实状态/转义/日志函数，检查拥有错误消息释放、sticky 空输出和格式失败出口。"""
    path = 'main/core/app_lifecycle.c'
    code = source_function(path, 'static void app_json_result_discard(')
    code += source_function(path, 'static void app_json_escape_append(')
    code += source_function(path, 'static char *app_status_json(')
    log = LOG_PREFIX + source_function(path, 'static void app_logf(') + '\n#undef vsnprintf\n'
    tests = r'''
int main(void) {
    cc_string_builder_t builder;
    cc_result_t rc = cc_string_builder_init(&builder);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    char long_text[301]; memset(long_text, 'x', sizeof(long_text) - 1U); long_text[300] = 0;
    fail_builder_growth = 1;
    app_json_escape_append(&builder, long_text);
    assert(messages_created == 1 && messages_released == 1);
    assert(cc_string_builder_take(&builder) == NULL);
    cc_string_builder_deinit(&builder);

    messages_created = messages_released = 0;
    fail_builder_growth = 0; fail_builder_init = 1;
    assert(app_status_json(NULL) == NULL);
    assert(messages_created == 1 && messages_released == 1 && profile_calls == 0);

    messages_created = messages_released = 0;
    fail_builder_init = 0; fail_builder_growth = 1;
    assert(app_status_json(NULL) == NULL);
    assert(messages_created > 0 && messages_released == messages_created && profile_calls == 1);

    fail_builder_growth = 0;
    char *json = app_status_json(NULL);
    assert(json && strstr(json, "\"uptime_ms\":17") && strstr(json, "\"profile\":{}"));
    free(json);
    assert(messages_released == messages_created);

    fail_format = 1;
    app_logf(ESP_LOG_INFO, BOARD_CONV_STATUS, "%s", "text");
    assert(log_dispatches == 0);
    fail_format = 0;
    app_logf(ESP_LOG_INFO, BOARD_CONV_STATUS, "%s", "text");
    assert(log_dispatches == 3);
    puts("Lifecycle JSON/log failure cleanup: PASS");
    return 0;
}
'''
    run_c_test(BUILDER_PREFIX + builder_source() + STATUS_PREFIX + code + log + tests)


CONFIG_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_TIMEOUT 3
#define ESP_ERR_NOT_FINISHED 4
#define CC_OK 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define __atomic_load_n(pointer, order) (*(pointer))
#ifndef __ATOMIC_ACQUIRE
#define __ATOMIC_ACQUIRE 2
#endif
#define STARTUP_GATE_RUNNING 1
#define CONFIG_NETWORK_APPLY_TIMEOUT_MS 30000
#define APP_CONFIG_FILE_PATH "/config.json"
#define APP_EVENT_KIND_ERROR 1
#define APP_EVENT_SOURCE_SYSTEM 1
#define CC_LOG_WARN 1
#define ESP_LOGW(...) ((void)0)
typedef struct { int port, event_limit; } web_config_t;
typedef struct { web_config_t web; int max_input_bytes; } io_config_t;
typedef struct {
    io_config_t io;
    int runtime, network, profile;
    bool updates_blocked;
    const char *display_font_path;
} app_config_t;
typedef struct { app_config_t config; } app_context_t;
typedef struct {
    bool parsed, event_bus_applied, interaction_applied, network_applied;
    bool profile_applied, font_applied, runtime_controls_applied, durable_committed;
    bool rollback_attempted, rollback_succeeded, restart_required;
    esp_err_t error;
    char failed_subsystem[24];
} app_config_apply_result_t;
typedef struct { int code; const char *message; } cc_result_t;
static int s_config_transaction_mutex = 1, s_boot_complete, s_runtime_restart_pending;
static void *s_recovery;
static app_context_t context;
static int phase, locked, lock_calls, parse_calls, hot_calls, warning_calls;
static int change_at_lock, mutate_at_unlock, commit_error, limit_publications;
static int published_event_limit, published_input_limit;
/** 以确定交错模拟取得事务锁期间发生关闭。 */
static int xSemaphoreTake(int mutex, unsigned ticks) {
    (void)mutex; (void)ticks; assert(!locked); locked = 1; lock_calls++;
    if (change_at_lock == 1) s_boot_complete = 0;
    if (change_at_lock == 2) phase = 0;
    return pdTRUE;
}
/** 解锁后模拟下一事务改变阻断位，检查当前报告只用锁内快照。 */
static void xSemaphoreGive(int mutex) {
    (void)mutex; assert(locked); locked = 0;
    if (mutate_at_unlock) context.config.updates_blocked = true;
}
static int startup_gate_phase(void) { return phase; }
static bool usb_storage_service_is_mounted(void) { return true; }
static bool recovery_service_is_current(void *service) { (void)service; return true; }
static cc_result_t recovery_service_validate_configuration(const int *runtime, const io_config_t *io) {
    (void)runtime; (void)io; return (cc_result_t){CC_OK, NULL};
}
static void cc_result_free(cc_result_t *result) { (void)result; }
static const char *cc_error_string(int code) { (void)code; return "test"; }
static esp_err_t app_config_prepare_update(app_config_t *out, const app_config_t *old, const char *json) {
    (void)json; assert(locked); parse_calls++; *out = *old;
    out->io.web.event_limit = 23; out->io.max_input_bytes = 345;
    return ESP_OK;
}
static bool app_config_requires_restart(const app_config_t *old, const app_config_t *next) {
    (void)old; (void)next; return false;
}
static const io_config_t *app_config_io(const app_config_t *config) { return &config->io; }
static const int *app_config_network(const app_config_t *config) { return &config->network; }
static bool app_config_runtime_thinking(const app_config_t *config) { (void)config; return false; }
static esp_err_t app_event_bus_apply_config(const io_config_t *io) { (void)io; hot_calls++; return ESP_OK; }
static esp_err_t interaction_service_apply_config(const io_config_t *io) { (void)io; hot_calls++; return ESP_OK; }
static esp_err_t network_service_set_web_port(uint16_t port) { (void)port; hot_calls++; return ESP_OK; }
static esp_err_t network_service_apply_wifi_config(const int *network, int timeout) {
    (void)network; (void)timeout; hot_calls++; return ESP_OK;
}
static esp_err_t app_profile_apply_config(const int *profile) { (void)profile; hot_calls++; return ESP_OK; }
static void board_ui_font_set_path(const char *path) { (void)path; hot_calls++; }
static void runtime_service_set_thinking(bool enabled) { (void)enabled; hot_calls++; }
/** 仅安装成功或目录同步未知时移交新配置，其他错误保留旧快照。 */
static esp_err_t app_config_commit_prepared(app_config_t *out, app_config_t *prepared, const char *path) {
    (void)path; assert(locked);
    if (commit_error == ESP_OK || commit_error == ESP_ERR_NOT_FINISHED) {
        *out = *prepared; memset(prepared, 0, sizeof(*prepared));
        out->updates_blocked = commit_error == ESP_ERR_NOT_FINISHED;
    }
    return commit_error;
}
static void app_config_destroy(app_config_t *config) { memset(config, 0, sizeof(*config)); }
static void board_ui_set_runtime_restart_required(bool required) { (void)required; }
/** 在锁内记录已采用配置的两项请求限制。 */
static void web_console_publish_limits(int event_limit, int input_limit) {
    assert(locked); limit_publications++;
    published_event_limit = event_limit; published_input_limit = input_limit;
}
static void app_event_publish(int kind, int source, int level, const char *text) {
    (void)kind; (void)source; (void)level; (void)text; assert(!locked); warning_calls++;
}
'''


def test_lifecycle_config_admission_and_warning_snapshot():
    """执行完整配置事务，核对启动双重准入、提交后限制发布和锁外警告快照。"""
    path = 'main/core/app_lifecycle.c'
    code = source_function(path, 'static void app_config_result_fail(')
    code += source_function(path, 'static esp_err_t app_apply_config(')
    tests = r'''
/** 重置无拥有指针的事务 fixture，保留锁未泄漏断言。 */
static void reset(void) {
    assert(!locked); memset(&context, 0, sizeof(context));
    context.config.io.web.port = 80;
    context.config.io.web.event_limit = 7; context.config.io.max_input_bytes = 128;
    context.config.display_font_path = "font";
    s_boot_complete = 1; phase = STARTUP_GATE_RUNNING;
    lock_calls = parse_calls = hot_calls = warning_calls = 0;
    change_at_lock = mutate_at_unlock = commit_error = limit_publications = 0;
}
int main(void) {
    app_config_apply_result_t result;
    reset(); s_boot_complete = 0;
    assert(app_apply_config("{}", &result, &context) == ESP_ERR_INVALID_STATE);
    assert(strcmp(result.failed_subsystem, "startup_config_pending") == 0);
    assert(!lock_calls && !parse_calls && !hot_calls);
    reset(); phase = 0;
    assert(app_apply_config("{}", &result, &context) == ESP_ERR_INVALID_STATE);
    assert(strcmp(result.failed_subsystem, "startup_gate") == 0);
    assert(!lock_calls && !parse_calls && !hot_calls);
    for (int changed = 1; changed <= 2; changed++) {
        reset(); change_at_lock = changed;
        assert(app_apply_config("{}", &result, &context) == ESP_ERR_INVALID_STATE);
        assert(strcmp(result.failed_subsystem, changed == 1 ? "startup_config_pending" : "startup_gate") == 0);
        assert(lock_calls == 1 && !parse_calls && !hot_calls && !locked);
    }
    reset();
    assert(app_apply_config("{}", &result, &context) == ESP_OK);
    assert(result.parsed && result.durable_committed && !result.rollback_attempted);
    assert(parse_calls == 1 && hot_calls > 0 && !locked && !warning_calls);
    assert(limit_publications == 1 && published_event_limit == 23 && published_input_limit == 345);
    reset(); commit_error = ESP_ERR_NOT_FINISHED;
    assert(app_apply_config("{}", &result, &context) == ESP_ERR_NOT_FINISHED);
    assert(result.parsed && !result.durable_committed && !result.rollback_attempted);
    assert(context.config.updates_blocked && warning_calls == 1 && !locked);
    assert(limit_publications == 1 && published_event_limit == 23 && published_input_limit == 345);
    reset(); commit_error = ESP_FAIL; mutate_at_unlock = 1;
    assert(app_apply_config("{}", &result, &context) == ESP_FAIL);
    assert(result.rollback_attempted && result.rollback_succeeded && !locked);
    assert(context.config.updates_blocked && warning_calls == 0 && limit_publications == 0);
    puts("Lifecycle configuration admission, limits and warning snapshot: PASS");
    return 0;
}
'''
    run_c_test(CONFIG_PREFIX + code + tests)


if __name__ == '__main__':
    source = (ROOT / 'main/services/ota_service.c').read_text(encoding='utf-8')
    start = source.index('esp_err_t ota_service_confirm_running_app(void)')
    end = source.index('\n}\n', start) + 3
    run_c_test(PREFIX + source[start:end] + TESTS)
    test_lifecycle_json_and_log_failure_cleanup()
    test_lifecycle_config_admission_and_warning_snapshot()
