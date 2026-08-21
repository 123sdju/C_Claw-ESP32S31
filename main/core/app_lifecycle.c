/*
 * C-Claw 固件生命周期编排器。
 *
 * 本文件负责把 config、USB、UI、Camera、Runtime、Interaction、Network、Web
 * 和 OTA 健康确认按固定顺序装配起来。它只做跨模块编排，不接管各子系统内部资源：
 * 各模块仍分别负责自己的 task、queue、锁、buffer 和停止/释放流程。
 *
 * 关键约束：
 *   1. USB 中的 config.json 是当前启动配置来源，配置加载成功前不启动 Runtime。
 *   2. Runtime 必须先于 Interaction 启动，Interaction 持有可用的 runtime 指针。
 *   3. Web/UI 通过回调访问配置和观测接口，Serial 通过 Interaction 的公开提交接口进入业务链。
 *   4. OTA pending-verify 固件只有在关键服务完成启动检查后才标记 valid。
 *   5. app_lifecycle_run() 是应用级常驻入口；正常路径进入 USB 热插拔主循环后不返回。
 */
#include "app_lifecycle.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_config.h"
#include "app_context.h"
#include "app_event_bus.h"
#include "app_profile.h"
#include "app_status.h"
#include "board_camera.h"
#include "board_conversation.h"
#include "board_ui.h"
#include "board_ui_font.h"
#include "cc/util/cc_string_builder.h"
#include "cc/util/cc_redaction.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "interaction_service.h"
#include "network_service.h"
#include "ota_manager.h"
#include "runtime_service.h"
#include "serial_console.h"
#include "usb_storage_service.h"
#include "web_console.h"

static const char *TAG = "CClaw";

#define CAMERA_START_RETRY_COUNT 5
#define CAMERA_START_RETRY_DELAY_MS 1000
#define CONFIG_NETWORK_APPLY_TIMEOUT_MS 30000U

/*
 * 进程生命周期级应用上下文。
 * app_lifecycle_run() 是唯一初始化者，其他模块通过显式参数或回调访问其中的配置、
 * 状态、USB 事件队列和 runtime；不应在其他文件再创建第二份组合根。
 */
static app_context_t s_app;
static bool s_runtime_restart_pending;
static SemaphoreHandle_t s_config_transaction_mutex;

static bool app_config_transaction_init(void)
{
    if (s_config_transaction_mutex) return true;
    s_config_transaction_mutex = xSemaphoreCreateMutex();
    return s_config_transaction_mutex != NULL;
}

static void app_config_transaction_destroy(void)
{
    if (!s_config_transaction_mutex) return;
    vSemaphoreDelete(s_config_transaction_mutex);
    s_config_transaction_mutex = NULL;
}

static void app_startup_rollback(bool interaction_created, bool runtime_created)
{
    if (interaction_created) {
        (void)interaction_service_stop(5000U);
        (void)interaction_service_destroy();
    }
    (void)app_profile_stop(3000U);
    if (runtime_created) {
        cc_result_t stop_rc = runtime_service_stop(5000U);
        cc_result_free(&stop_rc);
    }
    (void)app_event_bus_destroy(3000U);
    (void)app_profile_destroy(3000U);
    app_context_destroy(&s_app);
    app_config_transaction_destroy();
}

esp_err_t app_lifecycle_prepare_for_reboot(uint32_t timeout_ms)
{
    if (timeout_ms == 0) timeout_ms = 5000U;
    esp_err_t first_error = ESP_OK;
    if (web_console_destroy() != ESP_OK) first_error = ESP_FAIL;
    if (board_camera_preview_running() && board_camera_stop_preview() != ESP_OK &&
        first_error == ESP_OK) {
        first_error = ESP_ERR_TIMEOUT;
    }
    esp_err_t interaction_err = interaction_service_stop(timeout_ms);
    if (interaction_err != ESP_OK && first_error == ESP_OK) first_error = interaction_err;
    if (interaction_err == ESP_OK) {
        esp_err_t destroy_err = interaction_service_destroy();
        if (destroy_err != ESP_OK && first_error == ESP_OK) first_error = destroy_err;
    }
    esp_err_t profile_err = app_profile_stop(timeout_ms);
    if (profile_err != ESP_OK && first_error == ESP_OK) first_error = profile_err;
    cc_result_t runtime_err = runtime_service_stop(timeout_ms);
    if (runtime_err.code != CC_OK && first_error == ESP_OK) {
        first_error = runtime_err.code == CC_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    cc_result_free(&runtime_err);
    network_service_prepare_for_reboot();
    esp_err_t network_destroy_err = network_service_destroy();
    if (network_destroy_err != ESP_OK && first_error == ESP_OK) {
        first_error = network_destroy_err;
    }
    board_ui_prepare_for_reboot();
    esp_err_t event_err = app_event_bus_destroy(timeout_ms);
    if (event_err != ESP_OK && first_error == ESP_OK) first_error = event_err;
    if (profile_err == ESP_OK) {
        esp_err_t destroy_err = app_profile_destroy(timeout_ms);
        if (destroy_err != ESP_OK && first_error == ESP_OK) first_error = destroy_err;
    }
    return first_error;
}

/*
 * 统一日志输出函数，格式化字符串后同时输出到 ESP_LOG、LCD boot_log 区域和事件总线。
 *
 * 参数:
 *   level - ESP 日志级别，映射到对应的 ESP_LOG* 宏和事件总线日志级别
 *   role  - 对话角色，决定事件种类（ERROR 或 STATUS）
 *   fmt   - printf 格式字符串
 *   ...   - 可变参数
 *
 * ESP_LOG_ERROR/WARN → 事件总线 CC_LOG_ERROR/CC_LOG_WARN；
 * ESP_LOG_DEBUG/VERBOSE → CC_LOG_DEBUG；其余 → CC_LOG_INFO。
 */
static void app_logf(esp_log_level_t level, board_conversation_role_t role, const char *fmt, ...)
{
    if (!fmt) {
        return;
    }

    /* 启动日志使用固定栈缓冲，过长文本由 vsnprintf 截断，避免日志路径再触发堆分配。 */
    char text[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    switch (level) {
    case ESP_LOG_ERROR: ESP_LOGE(TAG, "%s", text); break;
    case ESP_LOG_WARN: ESP_LOGW(TAG, "%s", text); break;
    case ESP_LOG_INFO: ESP_LOGI(TAG, "%s", text); break;
    default: ESP_LOGD(TAG, "%s", text); break;
    }

    /* 同一条事实同时进入串口日志、启动 UI 和事件总线，便于三种入口交叉定位。 */
    board_ui_boot_log_append(role, text);
    cc_log_level_t event_level = CC_LOG_INFO;
    if (level == ESP_LOG_ERROR) event_level = CC_LOG_ERROR;
    else if (level == ESP_LOG_WARN) event_level = CC_LOG_WARN;
    else if (level == ESP_LOG_DEBUG || level == ESP_LOG_VERBOSE) event_level = CC_LOG_DEBUG;
    app_event_publish(role == BOARD_CONV_ERROR ? APP_EVENT_KIND_ERROR : APP_EVENT_KIND_STATUS,
                      APP_EVENT_SOURCE_SYSTEM,
                      event_level,
                      text);
}

#define APP_LOGI(...) app_logf(ESP_LOG_INFO, BOARD_CONV_STATUS, __VA_ARGS__)
#define APP_LOGW(...) app_logf(ESP_LOG_WARN, BOARD_CONV_STATUS, __VA_ARGS__)
#define APP_LOGE(...) app_logf(ESP_LOG_ERROR, BOARD_CONV_ERROR, __VA_ARGS__)

/*
 * 读取并输出 ESP32 芯片信息，包括型号、核心数、WiFi/BT/BLE 特性、硅版本和 Flash 大小。
 */
static void log_chip_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    APP_LOGI("Target: %s, cores: %d, features: %s%s%s%s",
             CONFIG_IDF_TARGET,
             chip_info.cores,
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi " : "",
             (chip_info.features & CHIP_FEATURE_BT) ? "BT " : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? "BLE " : "",
             (chip_info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4 " : "");

    APP_LOGI("Silicon revision: v%u.%u", chip_info.revision / 100, chip_info.revision % 100);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        APP_LOGI("Flash: %" PRIu32 " MB %s",
                 flash_size / (uint32_t)(1024 * 1024),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    } else {
        APP_LOGW("Failed to read flash size");
    }
}

/*
 * 检查指定路径的字体文件是否存在，存在则输出文件大小；不存在则输出错误日志。
 *
 * 参数:
 *   path - 字体文件路径，为空或 NULL 时直接返回
 */
static void check_font_file(const char *path)
{
    if (!path || !path[0]) {
        return;
    }

    /* 这里只验证路径和大小；字体的实际解析、加载与生命周期仍由 board_ui_font 持有。 */
    struct stat st;
    if (stat(path, &st) != 0) {
        APP_LOGE("font stat failed: %s, errno=%d", path, errno);
        return;
    }

    APP_LOGI("font exists: %s, size=%ld", path, (long)st.st_size);
}

/*
 * 带重试的摄像头启动，最多重试 CAMERA_START_RETRY_COUNT 次，每次间隔 1 秒。
 * 依次调用 board_camera_init() 和 board_camera_start_preview()，
 * 任一步骤失败则重试；全部失败后输出警告日志。
 */
static void start_camera_with_retry(void)
{
    esp_err_t camera_err = ESP_FAIL;
    esp_err_t preview_err = ESP_FAIL;

    for (int attempt = 1; attempt <= CAMERA_START_RETRY_COUNT; attempt++) {
        camera_err = board_camera_init();
        if (camera_err != ESP_OK) {
            APP_LOGW("Camera init failed attempt %d/%d: %s",
                     attempt,
                     CAMERA_START_RETRY_COUNT,
                     esp_err_to_name(camera_err));
        } else {
            preview_err = board_camera_start_preview();
            if (preview_err == ESP_OK) {
                APP_LOGI("Camera preview started");
                return;
            }
            APP_LOGW("Camera preview start failed attempt %d/%d: %s",
                     attempt,
                     CAMERA_START_RETRY_COUNT,
                     esp_err_to_name(preview_err));
        }

        /* 让出 CPU 并给底层设备节点/驱动状态恢复时间，避免无间隔忙重试。 */
        vTaskDelay(pdMS_TO_TICKS(CAMERA_START_RETRY_DELAY_MS));
    }

    APP_LOGW("Camera preview unavailable after retries: %s",
             esp_err_to_name(camera_err != ESP_OK ? camera_err : preview_err));
}

/*
 * 摄像头预览帧回调，将 RGB565 数据转发到 board_ui_camera_preview_update() 用于 LCD 实时预览。
 *
 * 参数:
 *   rgb565    - 预览帧 RGB565 像素数据
 *   width     - 帧宽度
 *   height    - 帧高度
 *   user_data - 用户上下文（未使用）
 */
static void app_camera_preview_update(const void *rgb565, int width, int height, void *user_data)
{
    (void)user_data;
    board_ui_camera_preview_update(rgb565, width, height);
}

/*
 * 摄像头统计回调，将 board_camera_stats_t 转换为 board_ui_camera_stats_t 后更新 UI 统计信息。
 *
 * 参数:
 *   stats     - 摄像头统计信息，为 NULL 时直接返回
 *   user_data - 用户上下文（未使用）
 */
static void app_camera_stats_update(const board_camera_stats_t *stats, void *user_data)
{
    (void)user_data;
    if (!stats) {
        return;
    }
    const board_ui_camera_stats_t ui_stats = {
        .preview_frames = stats->preview_frames,
        .preview_drops = stats->preview_drops,
        .preview_fps = stats->preview_fps,
        .latest_jpeg_bytes = stats->latest_jpeg_bytes,
        .width = stats->width,
        .height = stats->height,
    };
    board_ui_camera_stats_update(&ui_stats);
}

/*
 * OTA 健康检查：启动完成后验证关键组件（配置加载、USB 挂载、runtime、
 * 交互服务、Web 控制台、摄像头）是否就绪，全部通过则标记 OTA 固件有效。
 *
 * 参数:
 *   web_ready        - Web 控制台是否启动成功
 *   camera_attempted - 是否已尝试启动摄像头
 *
 * 仅当 ota_status.rollback_pending 为 true 时才执行健康检查。
 */
static void app_mark_ota_valid_after_health_check(bool web_ready, bool camera_attempted)
{
    /* 只处理 bootloader 标记为 pending verify 的新镜像；普通冷启动无需重复写 OTA 状态。 */
    ota_manager_refresh_status();
    ota_manager_status_t ota_status;
    ota_manager_get_status(&ota_status);
    if (!ota_status.rollback_pending) {
        return;
    }

    interaction_service_status_t interaction;
    interaction_service_get_status(&interaction);

    /*
     * 当前健康门槛验证“关键启动链已经走通”，不是完整业务验收：
     * camera_attempted 只表示已执行摄像头启动阶段，不表示预览一定成功；
     * web_ready 则要求 Web Console 确实启动成功。该边界必须和 OTA 专项测试区分。
     */
    bool healthy = s_app.config.loaded &&
                   usb_storage_service_is_mounted() &&
                   s_app.runtime_builder != NULL &&
                   interaction.started &&
                   web_ready &&
                   camera_attempted;
    if (!healthy) {
        APP_LOGW("OTA app still pending verification: config=%d usb=%d runtime=%d interaction=%d web=%d camera=%d",
                 s_app.config.loaded ? 1 : 0,
                 usb_storage_service_is_mounted() ? 1 : 0,
                 s_app.runtime_builder ? 1 : 0,
                 interaction.started ? 1 : 0,
                 web_ready ? 1 : 0,
                 camera_attempted ? 1 : 0);
        return;
    }

    APP_LOGI("OTA health check passed; marking running app valid");
    ota_manager_mark_app_valid();
}

/*
 * UI 回调：获取当前配置的 JSON 字符串，供 LCD 界面显示。
 *
 * 返回: 调用者负责释放的 JSON 字符串副本；user_data 为 app_context_t 时读取其 config。
 */
static char *app_ui_load_config(void *user_data)
{
    app_context_t *ctx = (app_context_t *)user_data;
    if (!s_config_transaction_mutex ||
        xSemaphoreTake(s_config_transaction_mutex, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    char *snapshot = cc_redact_secrets(app_config_json(ctx ? &ctx->config : NULL));
    xSemaphoreGive(s_config_transaction_mutex);
    return snapshot;
}

/*
 * UI/Web 配置保存回调：校验并保存 JSON 配置到文件，随后触发各子系统应用新配置。
 *
 * 参数:
 *   json      - 待保存的 JSON 配置字符串
 *   user_data - 指向 app_context_t 的指针，为 NULL 时返回 false
 * 返回: 保存成功返回 true
 *
 * 保存成功后依次：应用 thinking 设置 → 事件路由配置 → profile 配置 →
 * 交互服务配置 → WiFi 配置 → 字体路径 → 标记需要重启 runtime。
 */
static void app_config_result_fail(app_config_apply_result_t *result,
                                   const char *subsystem,
                                   esp_err_t error)
{
    if (!result) return;
    result->error = error != ESP_OK ? error : ESP_FAIL;
    snprintf(result->failed_subsystem, sizeof(result->failed_subsystem), "%s",
             subsystem ? subsystem : "unknown");
}

static esp_err_t app_apply_config(const char *json,
                                  app_config_apply_result_t *out_result,
                                  void *user_data)
{
    app_context_t *ctx = (app_context_t *)user_data;
    app_config_apply_result_t local_result;
    app_config_apply_result_t *result = out_result ? out_result : &local_result;
    memset(result, 0, sizeof(*result));
    result->error = ESP_OK;
    if (!ctx || !json) {
        app_config_result_fail(result, "arguments", ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_config_transaction_mutex ||
        xSemaphoreTake(s_config_transaction_mutex, pdMS_TO_TICKS(60000U)) != pdTRUE) {
        app_config_result_fail(result, "transaction_busy", ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }

    app_config_t prepared = {0};
    esp_err_t err = app_config_prepare_update(&prepared, &ctx->config, json);
    if (err != ESP_OK) {
        app_config_result_fail(result, "parse_validate", err);
        xSemaphoreGive(s_config_transaction_mutex);
        return err;
    }
    result->parsed = true;
    result->restart_required = app_config_requires_restart(&ctx->config, &prepared);

    /*
     * IO route、profile、interaction、WiFi 和字体路径支持当前进程内重新应用。
     * provider/model 等 Runtime 组合依赖不在此处原地重建，只设置 restart-required，
     * 由 UI/上层流程明确提示或执行重启，避免运行中的 Agent 对象被并发替换。
     */
    err = app_event_bus_apply_config(app_config_io(&prepared));
    result->event_bus_applied = err == ESP_OK;
    if (err == ESP_OK) {
        err = interaction_service_apply_config(app_config_io(&prepared));
        result->interaction_applied = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = network_service_apply_wifi_config(app_config_network(&prepared),
                                                CONFIG_NETWORK_APPLY_TIMEOUT_MS);
        result->network_applied = err == ESP_OK;
    }
    if (err == ESP_OK) {
        err = app_profile_apply_config(&prepared.profile);
        result->profile_applied = err == ESP_OK;
    }
    if (err == ESP_OK) {
        board_ui_font_set_path(prepared.display_font_path);
        result->font_applied = true;
        runtime_service_set_thinking(app_config_runtime_thinking(&prepared));
        result->runtime_controls_applied = true;
    }
    if (err == ESP_OK) {
        err = app_config_commit_prepared(&ctx->config, &prepared, APP_CONFIG_FILE_PATH);
        result->durable_committed = err == ESP_OK;
    }
    if (err != ESP_OK) {
        if (!result->event_bus_applied) app_config_result_fail(result, "event_bus", err);
        else if (!result->interaction_applied) app_config_result_fail(result, "interaction", err);
        else if (!result->network_applied) app_config_result_fail(result, "network", err);
        else if (!result->profile_applied) app_config_result_fail(result, "profile", err);
        else app_config_result_fail(result, "durable_commit", err);

        /* Restore every hot-applied subsystem from the still-live old config. */
        result->rollback_attempted = true;
        bool rollback_ok = app_event_bus_apply_config(app_config_io(&ctx->config)) == ESP_OK;
        rollback_ok = interaction_service_apply_config(app_config_io(&ctx->config)) == ESP_OK &&
                      rollback_ok;
        rollback_ok = network_service_apply_wifi_config(app_config_network(&ctx->config),
                        CONFIG_NETWORK_APPLY_TIMEOUT_MS) == ESP_OK && rollback_ok;
        rollback_ok = app_profile_apply_config(&ctx->config.profile) == ESP_OK && rollback_ok;
        board_ui_font_set_path(ctx->config.display_font_path);
        runtime_service_set_thinking(app_config_runtime_thinking(&ctx->config));
        result->rollback_succeeded = rollback_ok;
        app_config_destroy(&prepared);
        xSemaphoreGive(s_config_transaction_mutex);
        return err;
    }
    app_config_destroy(&prepared);
    s_runtime_restart_pending = s_runtime_restart_pending || result->restart_required;
    board_ui_set_runtime_restart_required(s_runtime_restart_pending);
    xSemaphoreGive(s_config_transaction_mutex);
    return ESP_OK;
}

static bool app_save_config(const char *json, void *user_data)
{
    app_config_apply_result_t result;
    return app_apply_config(json, &result, user_data) == ESP_OK;
}

/*
 * Web 控制台回调：获取当前配置的 JSON 字符串。
 *
 * 返回: 配置 JSON 独立快照；调用者负责释放。
 */
static char *app_web_config_json(void *user_data)
{
    app_context_t *ctx = (app_context_t *)user_data;
    if (!s_config_transaction_mutex ||
        xSemaphoreTake(s_config_transaction_mutex, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    char *snapshot = cc_redact_secrets(app_config_json(ctx ? &ctx->config : NULL));
    xSemaphoreGive(s_config_transaction_mutex);
    return snapshot;
}

/*
 * JSON 字符串转义辅助：将 text 用双引号包裹后追加到 cc_string_builder，
 * 对 \"、\\、\\n、\\r、\\t 和控制字符（< 0x20）进行转义。
 *
 * 参数:
 *   sb   - 目标字符串构建器
 *   text - 待转义的原始字符串，为 NULL 时视为空字符串
 */
static void app_json_escape_append(cc_string_builder_t *sb, const char *text)
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
 * 构建完整状态 JSON，聚合 uptime、USB、堆内存、摄像头、WiFi、事件总线统计、
 * 交互服务状态、profile 等所有子系统状态。
 *
 * 返回: 调用者负责释放的 JSON 字符串；内存分配失败时返回 NULL。
 */
static char *app_status_json(void *user_data)
{
    app_context_t *ctx = (app_context_t *)user_data;
    /* 回调通常携带 s_app；保留 local_status 使空上下文诊断调用仍能生成基础快照。 */
    app_status_t local_status;
    app_status_t *status = ctx ? &ctx->status : &local_status;
    app_event_bus_status_t events;
    interaction_service_status_t interaction;
    /*
     * 三个模块分别提供只读快照。它们不是同一把全局锁下的事务快照，
     * 因此 JSON 适合状态观测和趋势诊断，不应被控制逻辑当作强一致输入。
     */
    app_status_collect(status);
    app_event_bus_get_status(&events);
    interaction_service_get_status(&interaction);

    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) {
        return NULL;
    }
    /* 第一段：运行时间、USB、Internal/PSRAM 堆和 Camera 数据面状态。 */
    cc_string_builder_appendf(&sb,
        "{\"uptime_ms\":%lld,\"usb\":{\"mounted\":%s},"
        "\"heap\":{\"internal_free\":%u,\"internal_largest\":%u,"
        "\"psram_free\":%u,\"psram_total\":%u,\"psram_largest\":%u},"
        "\"camera\":{\"preview_running\":%s,\"width\":%d,\"height\":%d,"
        "\"frames\":%u,\"fps\":%u,\"drops\":%u,\"latest_jpeg_bytes\":%u},",
        (long long)status->uptime_ms,
        status->usb_mounted ? "true" : "false",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        status->camera_preview_running ? "true" : "false",
        status->camera.width,
        status->camera.height,
        (unsigned)status->camera.preview_frames,
        (unsigned)status->camera.preview_fps,
        (unsigned)status->camera.preview_drops,
        (unsigned)status->camera.latest_jpeg_bytes);
    /* 第二段：STA/SoftAP、重连、SNTP 与 mDNS 名称；字符串统一走 JSON 转义。 */
    cc_string_builder_appendf(&sb,
        "\"wifi\":{\"enabled\":%s,\"connected\":%s,\"ap_enabled\":%s,\"ap_started\":%s,"
        "\"keepalive\":%s,\"retry\":%d,\"max_retry\":%d,\"last_reason\":%u,\"time_synced\":%s,\"ssid\":",
        status->network.enabled ? "true" : "false",
        status->network.connected ? "true" : "false",
        status->network.ap_enabled ? "true" : "false",
        status->network.ap_started ? "true" : "false",
        status->network.keepalive ? "true" : "false",
        status->network.retry_count,
        status->network.max_retry,
        (unsigned)status->network.last_disconnect_reason,
        status->network.time_synced ? "true" : "false");
    app_json_escape_append(&sb, status->network.ssid);
    cc_string_builder_append(&sb, ",\"sta_ip\":");
    app_json_escape_append(&sb, status->network.sta_ip);
    cc_string_builder_append(&sb, ",\"ap_ssid\":");
    app_json_escape_append(&sb, status->network.ap_ssid);
    cc_string_builder_append(&sb, ",\"ap_ip\":");
    app_json_escape_append(&sb, status->network.ap_ip);
    cc_string_builder_append(&sb, ",\"mdns_hostname\":");
    app_json_escape_append(&sb, status->network.mdns_hostname);
    cc_string_builder_appendf(&sb,
        ",\"command_high_water\":%u,\"wifi_event_coalesced\":%u,"
        "\"config_coalesced\":%u,\"owner_heartbeat_ms\":%llu,\"stopping\":%s",
        (unsigned)status->network.command_high_water,
        (unsigned)status->network.wifi_event_coalesced,
        (unsigned)status->network.config_coalesced,
        (unsigned long long)status->network.owner_heartbeat_ms,
        status->network.stopping ? "true" : "false");
    /* 第三段：EventBus 生命周期、维护模式、丢弃计数、队列高水位和路由。 */
    cc_string_builder_append(&sb, "},\"events\":{\"lifecycle\":");
    app_json_escape_append(&sb, app_event_bus_lifecycle_string(events.lifecycle));
    cc_string_builder_appendf(&sb,
        ",\"maintenance\":%s,\"maintenance_reason\":",
        events.maintenance_mode ? "true" : "false");
    app_json_escape_append(&sb, events.maintenance_reason);
    cc_string_builder_appendf(&sb,
        ",\"serial_debug\":%s,\"usb_log_enabled\":%s,\"usb_log_started\":%s,"
        "\"usb_fallback_serial\":%s,\"dropped\":%u,\"ring_overwrites\":%u,"
        "\"sink_dropped\":%u,\"published\":%u,\"maintenance_skipped\":%u,"
        "\"oom_dropped\":%u,\"usb_queue_dropped\":%u,\"display_queue_dropped\":%u,"
        "\"max_usb_queue_waiting\":%u,\"max_display_queue_waiting\":%u,"
        "\"oldest_seq\":%u,\"latest_seq\":%u,\"capacity\":%u,"
        "\"usb_bytes\":%u,\"usb_path\":",
        events.serial_debug_enabled ? "true" : "false",
        events.usb_log_enabled ? "true" : "false",
        events.usb_log_started ? "true" : "false",
        events.usb_log_fallback_serial ? "true" : "false",
        (unsigned)events.dropped,
        (unsigned)events.ring_overwrites,
        (unsigned)events.sink_dropped,
        (unsigned)events.published,
        (unsigned)events.maintenance_skipped,
        (unsigned)events.oom_dropped,
        (unsigned)events.usb_queue_dropped,
        (unsigned)events.display_queue_dropped,
        (unsigned)events.max_usb_queue_waiting,
        (unsigned)events.max_display_queue_waiting,
        (unsigned)events.oldest_seq,
        (unsigned)events.latest_seq,
        (unsigned)events.capacity,
        (unsigned)events.usb_bytes_written);
    app_json_escape_append(&sb, events.usb_log_path);
    cc_string_builder_append(&sb, ",\"routes\":{\"log\":");
    app_json_escape_append(&sb, events.log_route);
    cc_string_builder_append(&sb, ",\"chat\":");
    app_json_escape_append(&sb, events.chat_route);
    cc_string_builder_append(&sb, ",\"thinking\":");
    app_json_escape_append(&sb, events.thinking_route);
    cc_string_builder_append(&sb, ",\"tool\":");
    app_json_escape_append(&sb, events.tool_route);
    cc_string_builder_append(&sb, ",\"status\":");
    app_json_escape_append(&sb, events.status_route);
    cc_string_builder_append(&sb, ",\"error\":");
    app_json_escape_append(&sb, events.error_route);
    cc_string_builder_append(&sb, ",\"profile\":");
    app_json_escape_append(&sb, events.profile_route);
    cc_string_builder_append(&sb, "}");
    /* 第四段：Interaction owner task 的状态、generation、队列和会话快照。 */
    cc_string_builder_append(&sb, "},\"interaction\":{\"state\":");
    app_json_escape_append(&sb, interaction_service_state_string(interaction.state));
    cc_string_builder_appendf(&sb,
        ",\"started\":%s,\"busy\":%s,\"generation\":%u,\"pending\":%d,"
        "\"queue_depth\":%d,\"queue_high_water\":%u,\"late_reply_count\":%u,"
        "\"cancel_latency_ms\":%u,\"owner_heartbeat_ms\":%llu,"
        "\"max_input_bytes\":%d,\"thinking\":%s,\"stream_supported\":%s,\"session_id\":",
        interaction.started ? "true" : "false",
        interaction.busy ? "true" : "false",
        interaction.generation,
        interaction.pending,
        interaction.queue_depth,
        (unsigned)interaction.queue_high_water,
        (unsigned)interaction.late_reply_count,
        (unsigned)interaction.cancel_latency_ms,
        (unsigned long long)interaction.owner_heartbeat_ms,
        interaction.max_input_bytes,
        interaction.thinking_enabled ? "true" : "false",
        interaction.stream_supported ? "true" : "false");
    app_json_escape_append(&sb, interaction.session_id);
    cc_string_builder_append(&sb, "},");
    /* profile 自行追加带键名的 JSON 成员，最后由本函数闭合最外层对象。 */
    app_profile_append_status_json(&sb);
    cc_string_builder_append(&sb, "}");
    return cc_string_builder_take(&sb);
}

/*
 * Web 事件查询回调：获取增量事件 JSON 列表，支持分页和类型过滤。
 *
 * 参数:
 *   since     - 起始序列号（不含）
 *   limit     - 最大返回条数
 *   kind_mask - 事件类型掩码，用于过滤
 * 返回: 调用者负责释放的事件 JSON 字符串。
 */
static char *app_events_json(uint32_t since, int limit, uint32_t kind_mask, void *user_data)
{
    (void)user_data;
    /* since 是排他游标；分页、过滤和 ring overwrite 语义由 EventBus 统一实现。 */
    return app_event_bus_json(since, limit, kind_mask);
}

/*
 * Web 提交消息回调：将用户文本提交到交互服务的消息队列。
 *
 * 参数:
 *   text      - 用户输入的文本消息
 * 返回: 提交结果，ESP_OK 表示成功。
 */
static esp_err_t app_submit_message(const char *text, void *user_data)
{
    (void)user_data;
    /* Web handler 不直接运行 Agent，只向 Interaction owner task 提交有来源标记的命令。 */
    return interaction_service_submit_text(APP_EVENT_SOURCE_WEB, text);
}

/*
 * 等待 USB 设备插入并加载 config.json 的阻塞循环。
 * 监听 USB 热插拔事件：设备插入时挂载并尝试加载配置，成功则返回；
 * 设备拔出时卸载并启用串口回退调试。加载成功前永不返回。
 *
 * 参数:
 *   usb_events - USB 事件队列句柄
 */
static void wait_for_config(QueueHandle_t usb_events)
{
    /*
     * 这是启动屏障而不是后台轮询：只有 U 盘挂载且配置解析成功后才返回。
     * 这样后续 Runtime/Network/事件路由看到的是同一份已确认配置；代价是没有配置介质时
     * 设备停留在引导界面和串口诊断状态，不会以隐式默认配置启动完整业务。
     */
    while (true) {
        usb_storage_event_t event;

        /* 5 秒超时只用于周期输出 heap 诊断，不改变“等待配置成功”的状态。 */
        if (xQueueReceive(usb_events, &event, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (event.id == USB_STORAGE_DEVICE_CONNECTED) {
                APP_LOGI("USB device connected, mounting...");

                esp_err_t mount_err = usb_storage_service_mount(event.data.address);
                if (mount_err != ESP_OK) {
                    APP_LOGE("USB mount failed: %s", esp_err_to_name(mount_err));
                    continue;
                }
                board_ui_set_usb_mounted(true);

                /* mount 成功不等于配置有效；只有解析成功才解除启动屏障。 */
                if (app_config_load(&s_app.config, APP_CONFIG_FILE_PATH) == ESP_OK) {
                    APP_LOGI("Config loaded from %s", APP_CONFIG_FILE_PATH);
                    return;
                }
                APP_LOGE("Failed to load config from %s", APP_CONFIG_FILE_PATH);
            } else if (event.id == USB_STORAGE_DEVICE_DISCONNECTED) {
                APP_LOGW("USB device disconnected while waiting for config");
                app_event_bus_start_serial_fallback_debug();
                usb_storage_service_unmount(event.data.handle);
                board_ui_set_usb_mounted(false);
            }
        } else {
            APP_LOGI("Waiting for USB config, minimum free heap: %" PRIu32 " bytes",
                     esp_get_minimum_free_heap_size());
        }
    }
}

/*
 * 应用生命周期主入口，永不返回。
 *
 * 启动序列：
 *   1. 初始化上下文/事件总线/UI 回调/摄像头回调
 *   2. 初始化 Board UI 并输出芯片信息
 *   3. 启动 USB host，等待 config.json（wait_for_config）
 *   4. 配置字体路径、事件路由、profile，采样启动阶段
 *   5. 启动 runtime_service（含自检）和 interaction_service
 *   6. 启动串口控制台、WiFi、Web 控制台
 *   7. 带重试启动摄像头
 *   8. OTA 健康检查，通过则标记固件有效
 *   9. 进入主循环监听 USB 热插拔事件
 */
void app_lifecycle_run(void)
{
    /* 阶段 1：建立组合根和 EventBus，使后续启动日志已有统一观测出口。 */
    app_context_init(&s_app);
    if (!app_config_transaction_init()) {
        ESP_LOGE(TAG, "Config transaction mutex init failed");
        app_context_destroy(&s_app);
        return;
    }
    if (app_profile_init() != ESP_OK) {
        ESP_LOGE(TAG, "Profile service init failed");
        (void)app_profile_destroy(0);
        app_config_transaction_destroy();
        app_context_destroy(&s_app);
        return;
    }
    if (app_event_bus_init(NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Application EventBus init failed");
        (void)app_event_bus_destroy(1000U);
        (void)app_profile_destroy(1000U);
        app_config_transaction_destroy();
        app_context_destroy(&s_app);
        return;
    }

    /* UI 只持有配置读写回调，不直接依赖 app_config 的内部表示。 */
    const board_ui_callbacks_t board_callbacks = {
        .load_config_json = app_ui_load_config,
        .save_config_json = app_save_config,
        .user_data = &s_app,
    };
    board_ui_set_callbacks(&board_callbacks);

    /* Camera 通过回调交付稳定预览帧和统计，生命周期仍由 board_camera 持有。 */
    const board_camera_callbacks_t camera_callbacks = {
        .preview_update = app_camera_preview_update,
        .stats_update = app_camera_stats_update,
        .user_data = &s_app,
    };
    board_camera_set_callbacks(&camera_callbacks);

    /* 阶段 2：UI 失败允许降级启动；串口和事件路径仍可提供诊断。 */
    esp_err_t display_err = board_ui_init();
    if (display_err != ESP_OK) {
        ESP_LOGW(TAG, "Board UI init failed: %s", esp_err_to_name(display_err));
    }

    APP_LOGI("Booting C-Claw on ESP32S31");
    log_chip_info();

    /* 阶段 3：USB host 和事件队列是配置加载前置条件，失败时无法继续完整启动。 */
    if (usb_storage_service_init(&s_app.usb_events) != ESP_OK ||
        usb_storage_service_start_host(s_app.usb_events) != ESP_OK) {
        APP_LOGE("Failed to start USB host");
        app_startup_rollback(false, false);
        return;
    }

    APP_LOGI("Insert a USB flash drive with %s", APP_CONFIG_FILE_PATH);
    wait_for_config(s_app.usb_events);

    /* 阶段 4：配置已加载，先应用不依赖 Runtime 的字体、事件路由和 profile。 */
    board_ui_font_set_path(s_app.config.display_font_path);
    (void)app_event_bus_apply_config(app_config_io(&s_app.config));
    if (app_profile_apply_config(&s_app.config.profile) != ESP_OK) {
        APP_LOGW("Profile config apply failed");
    }
    app_profile_sample("startup", "config_loaded", false);
    check_font_file(s_app.config.display_font_path);

    /* UI ready 时展示实际 provider/model；UI 不可用不阻塞 Runtime 启动。 */
    if (board_ui_ready()) {
        const cc_config_t *loaded_config = app_config_runtime(&s_app.config);
        board_ui_set_config_loaded(true,
                                   loaded_config ? loaded_config->provider : NULL,
                                   loaded_config ? loaded_config->model : NULL);
        APP_LOGI("Display config applied");
    }

    /* 阶段 5：先做 Runtime 配置自检，再计时创建 SDK runtime。失败属于硬门槛。 */
    runtime_service_run_self_test(app_config_runtime(&s_app.config));
    app_profile_span_t runtime_span = app_profile_span_begin("startup", "runtime_start", NULL, NULL);
    if (!runtime_service_start(app_config_runtime(&s_app.config))) {
        app_profile_span_end(&runtime_span, "error");
        APP_LOGE("Runtime start failed");
        app_startup_rollback(false, false);
        return;
    }
    app_profile_span_end(&runtime_span, "ok");
    s_app.runtime_builder = runtime_service_builder();

    /* Interaction 依赖已创建的 runtime，并成为用户命令和 Agent run 的唯一业务 owner。 */
    app_profile_span_t interaction_span = app_profile_span_begin("startup", "interaction_start", NULL, NULL);
    esp_err_t interaction_err = interaction_service_start(app_config_io(&s_app.config),
                                                          s_app.runtime_builder);
    if (interaction_err != ESP_OK) {
        app_profile_span_end(&interaction_span, "error");
        APP_LOGE("Interaction start failed: %s", esp_err_to_name(interaction_err));
        app_startup_rollback(true, true);
        return;
    }
    app_profile_span_end(&interaction_span, "ok");
    /* Serial 是可降级入口：启动失败记录告警，但不撤销已就绪的 Runtime/Interaction。 */
    if (!serial_console_start()) {
        APP_LOGW("Serial console start failed");
    }
    APP_LOGI("Runtime and interaction started");

    /* 阶段 6：Network 内部负责异步重连；首次未连上不等于必须终止本地业务。 */
    esp_err_t network_err = network_service_init();
    if (network_err == ESP_OK) {
        network_err = network_service_start_from_config(app_config_network(&s_app.config),
                                                        CONFIG_NETWORK_APPLY_TIMEOUT_MS);
    }
    bool wifi_ok = network_err == ESP_OK;
    board_ui_set_wifi_state(wifi_ok && s_app.config.network.enabled,
                            wifi_ok ? "ok" : "reconnecting");

    /* Web Console 通过窄回调面访问配置、状态、事件和消息，不拥有这些模块。 */
    const web_console_context_t web_context = {
        .io = app_config_io(&s_app.config),
        .config_json = app_web_config_json,
        .config_applied = app_apply_config,
        .status_json = app_status_json,
        .events_json = app_events_json,
        .message_submitted = app_submit_message,
        .user_data = &s_app,
    };
    app_profile_span_t web_span = app_profile_span_begin("startup", "web_console_start", NULL, NULL);
    esp_err_t web_err = web_console_start(&web_context);
    app_profile_span_end(&web_span, web_err == ESP_OK ? "ok" : "error");
    if (web_err != ESP_OK) {
        APP_LOGW("Web console start failed: %s", esp_err_to_name(web_err));
    }

    if (!wifi_ok) {
        APP_LOGW("WiFi is not connected yet; keepalive reconnect is running");
    }

    /* 阶段 7：Camera 放在交互和网络之后启动，失败时保留文本 Agent 与诊断能力。 */
    app_profile_span_t camera_span = app_profile_span_begin("startup", "camera_start", NULL, NULL);
    start_camera_with_retry();
    app_profile_span_end(&camera_span, "done");
    /* 阶段 8：所有关键启动阶段执行后，再决定 pending-verify 镜像是否可标记 valid。 */
    app_mark_ota_valid_after_health_check(web_err == ESP_OK, true);

    APP_LOGI("Initialization complete; switching to main console");
    board_ui_boot_log_finish();

    /*
     * 阶段 9：常驻主循环只处理 USB 生命周期。
     * 启动后重新插入 U 盘目前只记录事件，不自动重载配置；配置变更仍走 UI/Web 保存接口，
     * 避免介质抖动在运行中隐式替换 Runtime 配置。
     */
    while (true) {
        usb_storage_event_t event;

        if (xQueueReceive(s_app.usb_events, &event, pdMS_TO_TICKS(5000)) == pdTRUE) {
            if (event.id == USB_STORAGE_DEVICE_DISCONNECTED) {
                APP_LOGW("USB device disconnected");
                app_event_bus_start_serial_fallback_debug();
                usb_storage_service_unmount(event.data.handle);
                board_ui_set_usb_mounted(false);
            } else if (event.id == USB_STORAGE_DEVICE_CONNECTED) {
                APP_LOGI("USB device connected after startup");
            }
        }
    }
}
