/**
 * @file
 * @brief 固件启动、配置事务、恢复装配、USB 生命周期和重启关闭的应用组合根。
 * 本文件保管配置、恢复与记忆组合句柄，服务各自拥有线程、队列和设备资源。
 * 启动 owner 串行装配，配置回调使用事务锁，USB 操作使用 owner guard，重启请求使用 shutdown mutex。
 * 成功启动后常驻，初始化失败按路径返回或保留诊断，关闭失败保留后续依赖供重试。
 */
#include "app_lifecycle.h"
#include "app_shutdown_sequence.h"
#include "app_usb_owner_guard.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#include "cc/ports/cc_path.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
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
#include "ota_service.h"
#include "runtime_service.h"
#include "serial_console.h"
#include "startup_gate.h"
#include "startup_recovery.h"
#include "startup_managed_memory.h"
#include "usb_storage_service.h"
#include "web_console.h"

static const char *TAG = "CClaw";

#define CAMERA_START_RETRY_COUNT 5
#define CAMERA_START_RETRY_DELAY_MS 1000
#define CONFIG_NETWORK_APPLY_TIMEOUT_MS 30000U
#define OTA_CONFIG_WAIT_TIMEOUT_MS 60000U

/*
 * 进程生命周期级应用上下文。
 * app_lifecycle_run() 是唯一初始化者，其他模块通过显式参数或回调访问其中的配置、
 * 状态、USB 事件队列和 runtime；不应在其他文件再创建第二份组合根。
 */
static app_context_t s_app;
/* 配置事务锁内更新，Web 路径策略在同一锁内读取。 */
static bool s_runtime_restart_pending;
static SemaphoreHandle_t s_config_transaction_mutex;
/* builder 借用恢复账本与检查点，恢复 owner 和暂存配置持续保管至各自借用屏障结束。 */
static recovery_service_t *s_recovery;
/* 拥有易失记忆任务，在交互结束后、builder 销毁前释放。 */
static startup_managed_memory_t *s_memory_owner;
static app_config_stage_t *s_boot_stage;
/* 由唯一启动 owner 保管的前检进度，呈现给 Web 的降级标记另以原子操作交接。 */
static bool s_boot_web_started;
static bool s_boot_wifi_ok;
static bool s_boot_config_attempted;
static int s_boot_web_degraded;
static int s_boot_complete;
static app_usb_owner_guard_t s_usb_owner_guard;
static SemaphoreHandle_t s_shutdown_mutex;
/**
 * @brief 为受限启动 Web 提供阶段或提升失败提示的拥有 JSON。
 * @param[in] user_data 预留的回调借用上下文，当前忽略，允许 NULL。
 * @return 需要 free 的状态正文，分配失败为 NULL，具体状态在定义处按原子阶段读取。
 */
static char *app_startup_status_json(void *user_data);

/**
 * @brief 结束启动恢复呈现，释放恢复服务和配置 stage，并复位本轮启动标记。
 * @pre owner 已停止或尚未创建交互、Web、网络、记忆 owner 和附加 builder，相关借用已结束。
 * @post gate 设为 OFFLINE，恢复服务和 stage 释放置空，Web/WiFi/配置尝试标记复位。
 */
static void app_release_startup_recovery(void)
{
    startup_recovery_reset();
    recovery_service_destroy(s_recovery);
    s_recovery=NULL;
    app_config_stage_destroy(s_boot_stage);
    s_boot_stage=NULL;
    s_boot_web_started=false;
    s_boot_wifi_ok=false;
    s_boot_config_attempted=false;
    __atomic_store_n(&s_boot_web_degraded,0,__ATOMIC_RELEASE);
}

/**
 * @brief 仅对 pending-verify 镜像请求 bootloader 标记无效并回滚重启。
 * @param[in] reason 同步借用的日志原因，NULL 使用启动健康失败提示。
 * @return 无待验证镜像为 ESP_OK，调用回滚 API 后若返回则原样传回结果，失败同时记录日志。
 * @note 成功回滚重启路径可不返回，owner 在调用前已保管或安全收尾相关依赖。
 */
static esp_err_t app_reject_pending_ota(const char *reason)
{
    ota_manager_refresh_status();
    ota_manager_status_t status;
    ota_manager_get_status(&status);
    if (!status.rollback_pending) return ESP_OK;
    ESP_LOGE(TAG, "rejecting unverified OTA image: %s", reason ? reason : "startup health check failed");
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to mark OTA image invalid: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief 由启动 owner 幂等创建配置读写事务锁。
 * @return 已存在或创建成功为真，分配失败为假。
 * @pre 初始化期间排除并发创建、销毁及配置回调。
 */
static bool app_config_transaction_init(void)
{
    if (s_config_transaction_mutex) return true;
    s_config_transaction_mutex = xSemaphoreCreateMutex();
    return s_config_transaction_mutex != NULL;
}

/**
 * @brief 释放配置事务锁并清空全局句柄。
 * @pre owner 已排空 UI/Web 配置回调及所有锁持有者、等待者。
 * @post 非空 mutex 已删除，重复调用为空操作。
 */
static void app_config_transaction_destroy(void)
{
    if (!s_config_transaction_mutex) return;
    vSemaphoreDelete(s_config_transaction_mutex);
    s_config_transaction_mutex = NULL;
}

/**
 * @brief 等待已经进入的配置事务完成一次临界区。
 * @param[in] timeout_ms 取得事务锁的毫秒预算，按 FreeRTOS tick 转换。
 * @return 无锁或成功取得并归还锁为 ESP_OK，等待失败为 ESP_ERR_TIMEOUT。
 * @pre 调用者先关闭 startup gate，使后续配置更新在取得锁后拒绝执行，锁寿命覆盖本次等待。
 */
static esp_err_t app_config_transaction_drain(uint32_t timeout_ms)
{
    if (!s_config_transaction_mutex) return ESP_OK;
    if (xSemaphoreTake(s_config_transaction_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    xSemaphoreGive(s_config_transaction_mutex);
    return ESP_OK;
}

/**
 * @brief 关闭启动输入并按借用依赖回滚，首个屏障失败即保留后续资源。
 * @param[in] interaction_created 是否尝试交互 stop/destroy，交互启动部分失败时也传真。
 * @param[in] runtime_created 是否尝试停止 runtime，实际存在 builder 时同样进入停止。
 * @post 成功收尾时记忆先于 builder、恢复后于 builder 释放，最终清理配置与锁；失败保留后续依赖并处理待验证 OTA。
 * @pre 由启动 owner 调用，USB 常驻循环尚未接管 guard，其他启动动作已经停止推进。
 */
static void app_startup_rollback(bool interaction_created, bool runtime_created)
{
    startup_gate_set_phase(STARTUP_GATE_OFFLINE);
    esp_err_t serial_err=serial_console_stop(5000U);
    if (serial_err!=ESP_OK) {
        ESP_LOGE(TAG,"Startup rollback stopped at serial: %s; dependencies remain alive",esp_err_to_name(serial_err));
        (void)app_reject_pending_ota("serial did not stop during startup rollback");
        return;
    }
    esp_err_t config_err=app_config_transaction_drain(5000U);
    if (config_err!=ESP_OK) {
        ESP_LOGE(TAG,"Startup rollback stopped at config transaction: %s; dependencies remain alive",esp_err_to_name(config_err));
        (void)app_reject_pending_ota("config transaction did not stop during startup rollback");
        return;
    }
    esp_err_t ui_err=board_ui_detach_callbacks(5000U);
    if (ui_err!=ESP_OK) {
        ESP_LOGE(TAG,"Startup rollback stopped at UI callbacks: %s; dependencies remain alive",esp_err_to_name(ui_err));
        (void)app_reject_pending_ota("UI callbacks did not stop during startup rollback");
        return;
    }
    esp_err_t web_err=web_console_destroy();
    if (web_err!=ESP_OK) {
        ESP_LOGE(TAG,"Startup rollback stopped at Web: %s; dependencies remain alive",esp_err_to_name(web_err));
        (void)app_reject_pending_ota("Web did not stop during startup rollback");
        return;
    }
    if (interaction_created) {
        esp_err_t err=interaction_service_stop(5000U);
        if (err==ESP_OK) err=interaction_service_destroy();
        if (err!=ESP_OK) {
            ESP_LOGE(TAG,"Startup rollback stopped at interaction: %s; dependencies remain alive",esp_err_to_name(err));
            (void)app_reject_pending_ota("interaction did not stop during startup rollback");
            return;
        }
    }
    esp_err_t profile_err=app_profile_stop(3000U);
    if (profile_err!=ESP_OK) {
        ESP_LOGE(TAG,"Startup rollback stopped at profile: %s; dependencies remain alive",esp_err_to_name(profile_err));
        (void)app_reject_pending_ota("profile did not stop during startup rollback");
        return;
    }
    if (!network_service_prepare_for_reboot(5000U)) {
        ESP_LOGE(TAG,"Startup rollback stopped at network; dependencies remain alive");
        (void)app_reject_pending_ota("network did not stop during startup rollback");
        return;
    }
    esp_err_t network_err=network_service_destroy(5000U);
    if (network_err!=ESP_OK) {
        ESP_LOGE(TAG,"Startup rollback stopped at network cleanup: %s",esp_err_to_name(network_err));
        (void)app_reject_pending_ota("network cleanup failed during startup rollback");
        return;
    }
    cc_result_t memory_stop=startup_managed_memory_destroy(&s_memory_owner);
    if (memory_stop.code!=CC_OK) {
        ESP_LOGE(TAG,"Startup rollback stopped at memory: %s; dependencies remain alive",
                 memory_stop.message ? memory_stop.message : cc_error_string(memory_stop.code));
        cc_result_free(&memory_stop);
        (void)app_reject_pending_ota("memory did not stop during startup rollback");
        return;
    }
    cc_result_free(&memory_stop);
    if (runtime_created || runtime_service_builder()) {
        cc_result_t stop_rc = runtime_service_stop(5000U);
        bool stopped=stop_rc.code==CC_OK;
        if (!stopped) ESP_LOGE(TAG,"Startup rollback stopped at runtime: %s; dependencies remain alive",
                               stop_rc.message ? stop_rc.message : cc_error_string(stop_rc.code));
        cc_result_free(&stop_rc);
        if (!stopped) { (void)app_reject_pending_ota("runtime did not stop during startup rollback"); return; }
    }
    s_app.runtime_builder=NULL;
    app_release_startup_recovery();
    esp_err_t event_err=app_event_bus_destroy(3000U);
    if (event_err!=ESP_OK) {
        ESP_LOGE(TAG,"Startup rollback stopped at event bus: %s; dependencies remain alive",esp_err_to_name(event_err));
        (void)app_reject_pending_ota("event bus did not stop during startup rollback");
        return;
    }
    profile_err=app_profile_destroy(3000U);
    if (profile_err!=ESP_OK) {
        ESP_LOGE(TAG,"Startup rollback stopped at profile destruction: %s",esp_err_to_name(profile_err));
        (void)app_reject_pending_ota("profile did not finish startup rollback");
        return;
    }
    app_context_destroy(&s_app);
    app_usb_owner_guard_destroy(&s_usb_owner_guard);
    if (s_shutdown_mutex) { vSemaphoreDelete(s_shutdown_mutex); s_shutdown_mutex=NULL; }
    app_config_transaction_destroy();
    (void)app_reject_pending_ota("startup service failed");
}

/**
 * @brief 为旧介质卸载依次暂停交互、排空 Web/配置事务并关闭 USB 日志 writer。
 * @param[in] handle 同步借用的待卸载设备身份，传给 begin_unmount 核对当前盘。
 * @return 屏障均成功为 ESP_OK，maintenance 为 INVALID_STATE，交互超时为 TIMEOUT，其余返回对应服务错误或 NOT_FOUND。
 * @pre 当前生命周期 owner 持有 USB guard，startup gate 已转为 USB_DISCONNECTED。
 * @note 此处设置卸载状态，实际 VFS/MSC 注销由后续 unmount 执行，失败由常驻循环保管 pending handle 重试。
 */
static esp_err_t app_quiesce_usb_storage_users(msc_host_device_handle_t handle)
{
    if (app_event_bus_maintenance_mode_enabled()) return ESP_ERR_INVALID_STATE;
    interaction_service_status_t interaction;
    interaction_service_get_status(&interaction);
    if (interaction.started && interaction.state != INTERACTION_SERVICE_PAUSED_MAINTENANCE) {
        esp_err_t pause_err = interaction_service_set_maintenance_paused(true, "usb_storage_disconnect");
        if (pause_err != ESP_OK) return pause_err;
    }
    if (!interaction_service_wait_idle(30000U)) return ESP_ERR_TIMEOUT;

    /* VFS 注销前先排空 HTTP handler，使其结束 USB FILE 的借用。 */
    esp_err_t web_err = web_console_destroy();
    if (web_err != ESP_OK) return web_err;

    esp_err_t gate_err = usb_storage_service_begin_unmount(handle);
    if (gate_err != ESP_OK) return gate_err;

    /* 配置锁同时排空写 config.json 的 UI 保存路径。 */
    if (s_config_transaction_mutex) {
        if (xSemaphoreTake(s_config_transaction_mutex, pdMS_TO_TICKS(5000U)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreGive(s_config_transaction_mutex);
    }

    /* 切换出口同时等待 USB writer 结束并关闭其 FILE。 */
    return app_event_bus_start_serial_fallback_debug();
}

/**
 * @brief 在关闭序列中停止串口 owner。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 原样传给 serial_console_stop 的毫秒预算。
 * @return 串口停止结果原样返回。
 */
static esp_err_t app_shutdown_serial(void *ctx,uint32_t timeout_ms)
{ (void)ctx; return serial_console_stop(timeout_ms); }
/**
 * @brief 先关闭 USB guard 接纳，再等待当前介质操作区间结束。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 传给 guard_close 的等锁毫秒预算。
 * @return guard 关闭结果，超时继续保留关闭状态及 mutex。
 */
static esp_err_t app_shutdown_usb_owner(void *ctx,uint32_t timeout_ms)
{ (void)ctx; return app_usb_owner_guard_close(&s_usb_owner_guard,timeout_ms); }
/**
 * @brief 在 gate 关闭后等待配置事务临界区结束。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 配置事务等锁毫秒预算。
 * @return 原样返回事务排空结果。
 */
static esp_err_t app_shutdown_config_drain(void *ctx,uint32_t timeout_ms)
{ (void)ctx; return app_config_transaction_drain(timeout_ms); }
/**
 * @brief 撤销 UI 回调并等待其上下文借用结束。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 传给 board_ui_detach_callbacks 的毫秒预算。
 * @return UI 回调撤销及排空结果原样返回。
 */
static esp_err_t app_shutdown_ui_callbacks(void *ctx,uint32_t timeout_ms)
{ (void)ctx; return board_ui_detach_callbacks(timeout_ms); }
/**
 * @brief 关闭并销毁 Web，结束其配置、状态和 USB 文件借用。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 操作表传入预算，当前由 web_console_destroy 自身的停止契约处理，此参数忽略。
 * @return Web 销毁结果原样返回，失败时后续关闭步骤保留依赖。
 */
static esp_err_t app_shutdown_web(void *ctx,uint32_t timeout_ms)
{ (void)ctx; (void)timeout_ms; return web_console_destroy(); }
/**
 * @brief 在预览实际运行时停止 Camera preview。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 操作表预算，Camera 停止 API 自身处理等待，当前忽略此值。
 * @return 未运行预览为 ESP_OK，否则返回 board_camera_stop_preview 结果。
 */
static esp_err_t app_shutdown_camera(void *ctx,uint32_t timeout_ms)
{ (void)ctx; (void)timeout_ms; return board_camera_preview_running() ? board_camera_stop_preview() : ESP_OK; }
/**
 * @brief 先停止交互 owner，成功后销毁控制器和服务资源。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 交互停止的毫秒预算。
 * @return stop 失败直接返回并保留服务，成功后返回 destroy 结果。
 */
static esp_err_t app_shutdown_interaction(void *ctx,uint32_t timeout_ms)
{
    (void)ctx; esp_err_t err=interaction_service_stop(timeout_ms);
    return err==ESP_OK ? interaction_service_destroy() : err;
}
/**
 * @brief 停止 profile sampler，阻止后续获取 runtime generation。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 传给 profile_stop 的毫秒预算。
 * @return sampler 停止结果原样返回。
 */
static esp_err_t app_shutdown_profile_stop(void *ctx,uint32_t timeout_ms)
{ (void)ctx; return app_profile_stop(timeout_ms); }
/**
 * @brief 先销毁记忆组合 owner，再停止 builder，成功后释放恢复及 stage。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 传给 runtime_service_stop 的毫秒预算，记忆 owner 使用自身销毁契约。
 * @return 记忆失败为 ESP_FAIL，runtime 超时映射 ESP_ERR_TIMEOUT，其他错误映射 ESP_FAIL，完整成功为 ESP_OK。
 * @pre 交互、profile 与网络借用已经结束，当前位于这些服务的回调之外。
 */
static esp_err_t app_shutdown_runtime(void *ctx,uint32_t timeout_ms)
{
    (void)ctx;
    cc_result_t rc=startup_managed_memory_destroy(&s_memory_owner);
    if (rc.code!=CC_OK) {
        cc_result_free(&rc);
        return ESP_FAIL;
    }
    cc_result_free(&rc);
    rc=runtime_service_stop(timeout_ms);
    esp_err_t err=rc.code==CC_OK ? ESP_OK : rc.code==CC_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
    cc_result_free(&rc);
    if (err==ESP_OK) { s_app.runtime_builder=NULL; app_release_startup_recovery(); }
    return err;
}
/**
 * @brief 先等待网络停止，再释放网络 owner 资源。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms prepare 和 destroy 各自取得的毫秒预算。
 * @return prepare 未完成为 ESP_ERR_TIMEOUT，成功后返回 network_service_destroy 结果。
 */
static esp_err_t app_shutdown_network(void *ctx,uint32_t timeout_ms)
{
    (void)ctx;
    if (!network_service_prepare_for_reboot(timeout_ms)) return ESP_ERR_TIMEOUT;
    return network_service_destroy(timeout_ms);
}
/**
 * @brief 在服务排空后更新 UI 的重启收尾状态。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 操作表预算，本同步显示调用忽略此值。
 * @return 调用 board_ui_prepare_for_reboot 后返回 ESP_OK。
 */
static esp_err_t app_shutdown_ui(void *ctx,uint32_t timeout_ms)
{ (void)ctx; (void)timeout_ms; board_ui_prepare_for_reboot(); return ESP_OK; }
/**
 * @brief 排空并释放固件 App EventBus 的事件及 sink 依赖。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms 传给事件总线 destroy 的毫秒预算。
 * @return 总线收尾结果原样返回，失败保留其可重试状态。
 */
static esp_err_t app_shutdown_events(void *ctx,uint32_t timeout_ms)
{ (void)ctx; return app_event_bus_destroy(timeout_ms); }
/**
 * @brief 在 runtime 和事件总线结束后释放 profile owner。
 * @param[in] ctx 预留借用上下文，当前忽略，允许 NULL。
 * @param[in] timeout_ms profile_destroy 的毫秒预算。
 * @return profile 资源释放结果原样返回。
 */
static esp_err_t app_shutdown_profile_destroy(void *ctx,uint32_t timeout_ms)
{ (void)ctx; return app_profile_destroy(timeout_ms); }

/**
 * @brief 在启动移交后串行执行十三个关闭步骤，首个失败保留后续依赖。
 * @param[in] timeout_ms shutdown 锁及每个步骤各自使用的毫秒预算，零采用 5000。
 * @return boot_complete 尚未发布为 INVALID_STATE，shutdown 锁缺失/超时为 TIMEOUT，其余原样返回关闭序列结果。
 * @pre 调用者未持 USB owner guard，当前任务位于将被排空的回调之外。
 * @note 成功或失败后 gate 均保持 OFFLINE，USB guard 一旦关闭保持关闭，实际重启由调用者决定。
 */
esp_err_t app_lifecycle_prepare_for_reboot(uint32_t timeout_ms)
{
    if (timeout_ms == 0) timeout_ms = 5000U;
    /* 启动移交到受 guard 保护的 USB 循环前，重启请求不能释放仍在构建的依赖。 */
    if (!__atomic_load_n(&s_boot_complete,__ATOMIC_ACQUIRE)) return ESP_ERR_INVALID_STATE;
    if (!s_shutdown_mutex || xSemaphoreTake(s_shutdown_mutex,pdMS_TO_TICKS(timeout_ms))!=pdTRUE)
        return ESP_ERR_TIMEOUT;
    startup_gate_set_phase(STARTUP_GATE_OFFLINE);
    const app_shutdown_ops_t ops={.usb_owner=app_shutdown_usb_owner,.serial=app_shutdown_serial,.config_drain=app_shutdown_config_drain,
        .ui_callbacks=app_shutdown_ui_callbacks,.web=app_shutdown_web,
        .camera=app_shutdown_camera,.interaction=app_shutdown_interaction,
        .profile_stop=app_shutdown_profile_stop,.runtime=app_shutdown_runtime,
        .network=app_shutdown_network,.ui=app_shutdown_ui,.events=app_shutdown_events,
        .profile_destroy=app_shutdown_profile_destroy};
    app_shutdown_stage_t failed=APP_SHUTDOWN_NONE;
    esp_err_t err=app_shutdown_sequence_run(&ops,NULL,timeout_ms,&failed);
    if (err!=ESP_OK) ESP_LOGW(TAG,"Reboot drain stopped at stage %d: %s; later dependencies remain alive",
                              (int)failed,esp_err_to_name(err));
    xSemaphoreGive(s_shutdown_mutex);
    return err;
}

/**
 * @brief 用固定栈缓冲格式化一条启动日志，并交给 ESP 日志、启动 UI 和事件总线。
 * @param[in] level ESP 日志级别，ERROR/WARN/DEBUG/VERBOSE 分别映射对应事件级别，其余使用 INFO。
 * @param[in] role 启动对话角色，ERROR 选择错误事件，其余选择状态事件。
 * @param[in] fmt 同步借用的 printf 格式串，NULL 直接结束。
 * @param[in] ... 与格式串匹配的可变参数，字符串借用覆盖本次调用。
 * @post 正常格式化最多保留 383 字节正文，后续 UI/事件层按各自同步及复制规则接纳，格式化失败结束本次分发。
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
    int written = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (written < 0) return;

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

/**
 * @brief 查询并记录芯片核心数、无线能力、硅版本及 flash 容量。
 * @post 查询结果通过 APP_LOG 发布，flash 容量查询失败只记录警告，启动继续。
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

/**
 * @brief 用 stat 检查配置字体路径并记录大小，实际字体装载交给 UI 字体 owner。
 * @param[in] path 同步借用的 NUL 结尾路径，NULL 或空串跳过。
 * @post stat 失败记录路径及 errno，成功按 long 格式记录大小，本函数不取得文件句柄。
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

/**
 * @brief 最多五次初始化 Camera 并启动 preview，后续尝试先恢复底层启动状态。
 * @return 预览启动成功为 ESP_OK，reset 失败立即返回，重试耗尽返回末次 init 或 preview 错误。
 * @note 每次失败后延时一秒，包括最后一次失败。由启动 owner 串行调用，Camera 资源继续归 board owner。
 */
static esp_err_t start_camera_with_retry(void)
{
    esp_err_t camera_err = ESP_FAIL;
    esp_err_t preview_err = ESP_FAIL;

    for (int attempt = 1; attempt <= CAMERA_START_RETRY_COUNT; attempt++) {
        if (attempt > 1) {
            esp_err_t reset_err = board_camera_reset_startup();
            if (reset_err != ESP_OK) {
                APP_LOGE("Camera startup recovery failed: %s", esp_err_to_name(reset_err));
                return reset_err;
            }
        }
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
                return ESP_OK;
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
    return camera_err != ESP_OK ? camera_err : preview_err;
}

/**
 * @brief 将 Camera 借用的 RGB565 帧同步转交 UI 预览接纳入口。
 * @param[in] rgb565 回调期借用像素缓冲，NULL 交 UI 入口忽略，有效帧由 UI 复制到自身缓冲。
 * @param[in] width 像素宽度，UI 入口核对固定预览尺寸。
 * @param[in] height 像素高度，UI 入口核对固定预览尺寸。
 * @param[in] user_data 预留借用上下文，当前忽略，允许 NULL。
 * @post 本函数不保留输入指针，UI 可按就绪、速率和缓冲状态丢弃本帧。
 */
static void app_camera_preview_update(const void *rgb565, int width, int height, void *user_data)
{
    (void)user_data;
    board_ui_camera_preview_update(rgb565, width, height);
}

/**
 * @brief 按值转换 Camera 统计字段并同步交给 UI 保存。
 * @param[in] stats 回调期借用统计，NULL 直接结束。
 * @param[in] user_data 预留借用上下文，当前忽略，允许 NULL。
 * @post UI 接纳栈上临时结构的值副本，帧数、丢帧、帧率、JPEG 字节数和尺寸已映射。
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

/**
 * @brief 对待验证 OTA 镜像核对关键启动依赖，再委托 OTA 服务确认或请求回滚。
 * @param[in] web_ready 本次完整 Web 启动/提升结果是否成功，参与健康门槛。
 * @param[in] camera_ready 本次 Camera init/preview 启动是否成功，仅参与失败诊断日志。
 * @post 普通镜像直接结束，关键依赖不全或确认失败请求拒绝 pending 镜像，确认成功由 OTA 服务完成标记和恢复。
 * @pre 启动 owner 已创建恢复、builder 和交互，状态查询期间各依赖保持存活。
 */
static void app_mark_ota_valid_after_health_check(bool web_ready, bool camera_ready)
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

    /* 健康门槛核对启动依赖，Camera 启动结果单独保留为诊断，文本业务可继续。 */
    bool healthy = s_app.config.loaded && recovery_service_is_current(s_recovery) &&
                   usb_storage_service_is_mounted() &&
                   s_app.runtime_builder != NULL &&
                   interaction.started &&
                   web_ready;
    if (!healthy) {
        APP_LOGW("OTA app still pending verification: config=%d usb=%d runtime=%d interaction=%d web=%d camera=%d",
                 s_app.config.loaded ? 1 : 0,
                 usb_storage_service_is_mounted() ? 1 : 0,
                 s_app.runtime_builder ? 1 : 0,
                 interaction.started ? 1 : 0,
                 web_ready ? 1 : 0,
                 camera_ready ? 1 : 0);
        (void)app_reject_pending_ota("startup health check failed");
        return;
    }

    APP_LOGI("OTA health check passed; marking running app valid");
    esp_err_t confirm_err = ota_service_confirm_running_app();
    if (confirm_err != ESP_OK) {
        APP_LOGW("OTA confirmation or service recovery failed; inspect OTA status: %s",
                 esp_err_to_name(confirm_err));
        (void)app_reject_pending_ota("OTA confirmation failed");
    }
}

/**
 * @brief 在配置事务锁内生成 UI 使用的脱敏配置副本。
 * @param[in] user_data 借用 app_context_t，NULL 使用 app_config_json 的空配置语义。
 * @return 调用方需要 free 的脱敏正文，锁缺失、取得失败或分配失败为 NULL。
 * @note 等锁使用 portMAX_DELAY，owner 撤销并排空该回调后才释放上下文和事务锁。
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

/**
 * @brief 在配置应用报告中保存本次失败领域及错误。
 * @param[in,out] result 可选报告，NULL 直接结束，其他字段保持调用方已有进度。
 * @param[in] subsystem 同步借用领域名，NULL 使用 unknown，复制到固定数组并按容量截断。
 * @param[in] error 本次错误，ESP_OK 归一为 ESP_FAIL。
 * @post error 与 failed_subsystem 已填写，无动态分配。
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

/**
 * @brief 在事务锁内准备配置、热应用并提交文件，失败按旧配置回滚可热应用的服务。
 * @param[in] json 必填同步借用 JSON，解析和深拷贝由 app_config_prepare_update 完成。
 * @param[out] out_result 可选独立报告，进入时清零，记录解析、各应用进度、提交、回滚及重启要求。
 * @param[in,out] user_data 必填借用 app_context_t，配置在事务内移交，owner 排空回调后销毁。
 * @return 完整提交为 ESP_OK，参数、事务超时、启动/存储门禁或服务/提交错误原样返回。
 * @pre boot_complete 已发布且 gate 为 RUNNING，两项条件在锁前和取得事务锁后再次检查。
 * @note 目标已安装但目录同步未知时采用新 RAM 配置并保留更新阻断，返回 NOT_FINISHED；其他失败尝试恢复旧服务配置。
 * 新 RAM 配置采用后在锁内发布 Web 的事件和输入限制。runtime 组合依赖通过 restart_required 提示重启，
 * 当前 builder 保持其在用配置寿命。锁外只发布本次锁内捕获的状态。
 */
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
    if (!__atomic_load_n(&s_boot_complete, __ATOMIC_ACQUIRE)) {
        app_config_result_fail(result, "startup_config_pending", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (startup_gate_phase() != STARTUP_GATE_RUNNING) {
        app_config_result_fail(result, "startup_gate", ESP_ERR_INVALID_STATE);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_config_transaction_mutex ||
        xSemaphoreTake(s_config_transaction_mutex, pdMS_TO_TICKS(60000U)) != pdTRUE) {
        app_config_result_fail(result, "transaction_busy", ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    /* UI 和 Web 共用此回调，先核对启动、恢复及介质状态，再修改路由或配置文件。 */
    if (ctx->config.updates_blocked) {
        app_config_result_fail(result, "configuration_recovery_required", ESP_ERR_INVALID_STATE);
        xSemaphoreGive(s_config_transaction_mutex);
        app_event_publish(APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_SYSTEM, CC_LOG_WARN,
            "配置保存结果仍待核对，已暂停后续配置保存。请检查 U 盘并重启，重新加载配置。当前可用对话可继续。");
        return ESP_ERR_INVALID_STATE;
    }
    if (!__atomic_load_n(&s_boot_complete, __ATOMIC_ACQUIRE)) {
        app_config_result_fail(result, "startup_config_pending", ESP_ERR_INVALID_STATE);
        xSemaphoreGive(s_config_transaction_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (startup_gate_phase() != STARTUP_GATE_RUNNING) {
        app_config_result_fail(result, "startup_gate", ESP_ERR_INVALID_STATE);
        xSemaphoreGive(s_config_transaction_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (!usb_storage_service_is_mounted() || !recovery_service_is_current(s_recovery)) {
        app_config_result_fail(result, "usb_storage_unavailable", ESP_ERR_INVALID_STATE);
        xSemaphoreGive(s_config_transaction_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    app_config_t prepared = {0};
    esp_err_t err = app_config_prepare_update(&prepared, &ctx->config, json);
    if (err != ESP_OK) {
        app_config_result_fail(result, "parse_validate", err);
        xSemaphoreGive(s_config_transaction_mutex);
        return err;
    }
    result->parsed = true;
    /* USB 日志路由即时生效，分别对在用和拟采用的 runtime 路径检查保护区交叠。 */
    cc_result_t path_check=recovery_service_validate_configuration(&prepared.runtime,&prepared.io);
    if (path_check.code==CC_OK)
        path_check=recovery_service_validate_configuration(&ctx->config.runtime,&prepared.io);
    bool storage_current=recovery_service_is_current(s_recovery);
    if (path_check.code!=CC_OK || !storage_current) {
        char detail[192];
        snprintf(detail,sizeof(detail),"%s",!storage_current ? "USB storage changed" :
                 path_check.message ? path_check.message : cc_error_string(path_check.code));
        cc_result_free(&path_check);
        app_config_result_fail(result,"storage_paths",ESP_ERR_INVALID_STATE);
        app_config_destroy(&prepared);
        xSemaphoreGive(s_config_transaction_mutex);
        ESP_LOGW(TAG,"Configuration storage paths are unavailable or overlap protected data: %s",detail);
        return ESP_ERR_INVALID_STATE;
    }
    cc_result_free(&path_check);
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
        err = network_service_set_web_port((uint16_t)prepared.io.web.port);
        if (err == ESP_OK) {
            err = network_service_apply_wifi_config(app_config_network(&prepared),
                                                    CONFIG_NETWORK_APPLY_TIMEOUT_MS);
        }
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
        if (err == ESP_ERR_NOT_FINISHED) {
            /* 目标已安装并采用新 RAM 配置，保管持久化未知状态，继续使用这一已核对版本。 */
            web_console_publish_limits(ctx->config.io.web.event_limit,
                                       ctx->config.io.max_input_bytes);
            app_config_result_fail(result, "directory_sync", err);
            s_runtime_restart_pending = s_runtime_restart_pending || result->restart_required || ctx->config.updates_blocked;
            board_ui_set_runtime_restart_required(s_runtime_restart_pending);
            xSemaphoreGive(s_config_transaction_mutex);
            app_event_publish(APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_SYSTEM, CC_LOG_WARN,
                "已核对并采用新配置，但持久化结果待确认。已暂停后续配置保存，请检查 U 盘并重启重新加载。");
            return err;
        }
    }
    if (err != ESP_OK) {
        if (!result->event_bus_applied) app_config_result_fail(result, "event_bus", err);
        else if (!result->interaction_applied) app_config_result_fail(result, "interaction", err);
        else if (!result->network_applied) app_config_result_fail(result, "network", err);
        else if (!result->profile_applied) app_config_result_fail(result, "profile", err);
        else app_config_result_fail(result, "durable_commit", err);

        /* 从仍由上下文保管的旧配置恢复各热应用服务，并记录全部恢复是否成功。 */
        result->rollback_attempted = true;
        bool rollback_ok = app_event_bus_apply_config(app_config_io(&ctx->config)) == ESP_OK;
        rollback_ok = interaction_service_apply_config(app_config_io(&ctx->config)) == ESP_OK &&
                      rollback_ok;
        rollback_ok = network_service_set_web_port(
                          (uint16_t)app_config_io(&ctx->config)->web.port) == ESP_OK && rollback_ok;
        rollback_ok = network_service_apply_wifi_config(app_config_network(&ctx->config),
                        CONFIG_NETWORK_APPLY_TIMEOUT_MS) == ESP_OK && rollback_ok;
        rollback_ok = app_profile_apply_config(&ctx->config.profile) == ESP_OK && rollback_ok;
        board_ui_font_set_path(ctx->config.display_font_path);
        runtime_service_set_thinking(app_config_runtime_thinking(&ctx->config));
        result->rollback_succeeded = rollback_ok;
        app_config_destroy(&prepared);
        bool updates_blocked = ctx->config.updates_blocked;
        xSemaphoreGive(s_config_transaction_mutex);
        if (updates_blocked)
            app_event_publish(APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_SYSTEM, CC_LOG_WARN,
                "配置保存结果待核对，已暂停后续配置保存。请检查 U 盘并重启，重新加载配置。当前可用对话可继续。");
        return err;
    }
    app_config_destroy(&prepared);
    web_console_publish_limits(ctx->config.io.web.event_limit,
                               ctx->config.io.max_input_bytes);
    s_runtime_restart_pending = s_runtime_restart_pending || result->restart_required;
    board_ui_set_runtime_restart_required(s_runtime_restart_pending);
    xSemaphoreGive(s_config_transaction_mutex);
    return ESP_OK;
}

/**
 * @brief 将 UI 的布尔保存回调适配到带完整报告的配置事务入口。
 * @param[in] json 同步借用的必填配置 JSON。
 * @param[in,out] user_data 借用 app_context_t，NULL 在下游返回参数错误。
 * @return app_apply_config 返回 ESP_OK 时为真，其余为假，NOT_FINISHED 可已采用新配置并保留更新阻断。
 */
static bool app_save_config(const char *json, void *user_data)
{
    app_config_apply_result_t result;
    return app_apply_config(json, &result, user_data) == ESP_OK;
}

/**
 * @brief 在事务锁内为 Web 生成独立的脱敏配置正文。
 * @param[in] user_data 借用 app_context_t，NULL 时读取空对象配置。
 * @return 调用者需要 free 的拥有字符串，锁缺失/取得失败或脱敏分配失败为 NULL。
 * @pre Web owner 保持上下文和事务锁存活，调用方当前未持有同一事务锁。
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

/**
 * @brief 判断规范请求路径是否落入一个配置资源及其目录/点号旁路文件保护区。
 * @param[in] path 非 NULL 的规范长文件名请求路径，transport 已完成输入校验，调用期间借用。
 * @param[in] owned 同步借用的配置资源路径，NULL/空串表示无该保护区。
 * @return 匹配为真，超长、短名或规范化分配失败也保守返回真，其他为假。
 * @note 按忽略大小写前缀及 NUL、斜杠、点号边界匹配，规范根路径覆盖全部请求。
 */
static bool app_web_path_owned(const char *path, const char *owned)
{
    if (!owned || !owned[0]) return false;
    /* 超长/短名无法可靠映射时拒绝通用访问，避免截断或 FAT 别名绕过。 */
    if (strlen(owned) >= 256 || strchr(owned, '~')) return true;
    char *canonical = cc_path_canonical(owned);
    if (!canonical) return true;
    size_t len = strlen(canonical);
    bool matches = strcmp(canonical, "/") == 0 ||
        (strncasecmp(path, canonical, len) == 0 &&
         (path[len] == '\0' || path[len] == '/' || path[len] == '.'));
    free(canonical);
    return matches;
}

/**
 * @brief 在配置事务快照下为 Web 通用文件读写核对恢复状态及在用资源保护区。
 * @param[in] path 非 NULL 的规范请求路径，借用到返回，路径语法由 transport 先行验证。
 * @param[in] user_data 必填借用 app_context_t，Web owner 保管至 handler 排空。
 * @return gate/恢复/配置可用且不属于保护区为真，参数、100 毫秒等锁失败或重启待处理等状态为假。
 * @note 同时保护恢复根、runtime 数据/会话/记忆/媒体、USB 日志和字体路径，事务结束前归还锁。
 */
static bool app_web_file_path_allowed(const char *path, void *user_data)
{
    app_context_t *ctx = (app_context_t *)user_data;
    if (!path || !ctx || !s_config_transaction_mutex ||
        xSemaphoreTake(s_config_transaction_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    const app_config_t *config = &ctx->config;
    /* 待重启配置可能与 active Runtime 仍在使用的路径不同，先停止通用读写。 */
    bool protected = startup_gate_phase()!=STARTUP_GATE_RUNNING || !recovery_service_is_current(s_recovery) ||
        !config->loaded || s_runtime_restart_pending ||
        app_web_path_owned(path, APP_RECOVERY_ROOT) ||
        app_web_path_owned(path, config->runtime.data_dir) ||
        app_web_path_owned(path, config->runtime.storage_path) ||
        app_web_path_owned(path, config->runtime.memory_path) ||
        app_web_path_owned(path, config->runtime.session_media_dir) ||
        app_web_path_owned(path, config->io.usb_log.dir) ||
        app_web_path_owned(path, config->display_font_path);
    xSemaphoreGive(s_config_transaction_mutex);
    return !protected;
}

/**
 * @brief 消费一次状态 JSON 追加结果的拥有消息，builder 保留其 sticky 错误状态。
 * @param[in,out] result 按值转交的追加结果及消息拥有权，调用者不再保管该消息。
 * @post 动态错误消息已释放，状态生成最终由 builder_take 判断是否完整。
 */
static void app_json_result_discard(cc_result_t result)
{
    cc_result_free(&result);
}

/**
 * @brief 以双引号包裹并逐字节转义状态字符串，追加失败时结束当前字段构造。
 * @param[in,out] sb 调用者拥有的 builder，错误沿 sticky 状态保留，NULL 时消费参数错误后结束。
 * @param[in] text 同步借用的 NUL 结尾正文，NULL 按空串，非控制字节原样追加。
 * @post 引号、反斜杠及控制字节已转义或 builder 处于失败状态，当前追加结果消息已清理。
 */
static void app_json_escape_append(cc_string_builder_t *sb, const char *text)
{
    cc_result_t rc = cc_string_builder_append_char(sb, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : "");
         rc.code == CC_OK && *p; p++) {
        switch (*p) {
        case '\\': rc = cc_string_builder_append(sb, "\\\\"); break;
        case '"': rc = cc_string_builder_append(sb, "\\\""); break;
        case '\n': rc = cc_string_builder_append(sb, "\\n"); break;
        case '\r': rc = cc_string_builder_append(sb, "\\r"); break;
        case '\t': rc = cc_string_builder_append(sb, "\\t"); break;
        default:
            if (*p < 0x20) rc = cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p);
            else rc = cc_string_builder_append_char(sb, (char)*p);
            break;
        }
    }
    if (rc.code == CC_OK) rc = cc_string_builder_append_char(sb, '"');
    cc_result_free(&rc);
}

/**
 * @brief 采集各模块观测值，构造 Web 的运行、资源、交互与恢复模式 JSON。
 * @param[in,out] user_data 借用 app_context_t，非 NULL 时复用其 status 值存储，NULL 使用本次栈快照。
 * @return 调用者需要 free 的拥有正文，初始化或追加失败由 sticky builder 返回 NULL。
 * @pre 同一上下文 status 存储由 Web owner 串行调用，回调及依赖持续到 Web handler 排空。
 * @note 子系统分别同步查询，字段可来自相邻时刻。本函数消费各次 builder 返回消息，profile 追加由其入口自行管理。
 */
static char *app_status_json(void *user_data)
{
    app_context_t *ctx = (app_context_t *)user_data;
    /* 回调通常携带 s_app；保留 local_status 使空上下文诊断调用仍能生成基础快照。 */
    app_status_t local_status;
    app_status_t *status = ctx ? &ctx->status : &local_status;
    app_event_bus_status_t events;
    interaction_service_status_t interaction;
    /* 各模块分别提供只读值快照，字段可能来自相邻时刻，用于状态观测和趋势诊断。 */
    app_status_collect(status);
    app_event_bus_get_status(&events);
    interaction_service_get_status(&interaction);
    board_ui_preview_stats_t ui_preview;
    board_ui_camera_preview_get_stats(&ui_preview);

    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        return NULL;
    }
    cc_result_free(&rc);
    /* 第一段：运行时间、USB、Internal/PSRAM 堆和 Camera 数据面状态。 */
    app_json_result_discard(cc_string_builder_appendf(&sb,
        "{\"uptime_ms\":%lld,\"usb\":{\"mounted\":%s},"
        "\"heap\":{\"internal_free\":%u,\"internal_largest\":%u,"
        "\"psram_free\":%u,\"psram_total\":%u,\"psram_largest\":%u},"
        "\"camera\":{\"preview_running\":%s,\"width\":%d,\"height\":%d,"
        "\"frames\":%u,\"fps\":%u,\"drops\":%u,\"latest_jpeg_bytes\":%u,",
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
        (unsigned)status->camera.latest_jpeg_bytes));
    app_json_result_discard(cc_string_builder_appendf(&sb,
        "\"ui_preview\":{\"input_frames\":%u,\"published_frames\":%u,\"source_updates\":%u,"
        "\"rate_limited_frames\":%u,\"replaced_pending_frames\":%u,\"no_buffer_frames\":%u,"
        "\"cache_sync_errors\":%u,\"input_row_bytes\":%u,\"stride_bytes\":%u,"
        "\"buffer_bytes\":%u,\"buffer_count\":%u,"
        "\"integrity_checks\":%u,\"integrity_mismatches\":%u,\"last_mismatch_frame\":%u,"
        "\"pink_jump_candidates\":%u,\"last_pink_frame\":%u,"
        "\"last_pink_before_rgb\":%u,\"last_pink_after_rgb\":%u}},",
        (unsigned)ui_preview.input_frames, (unsigned)ui_preview.published_frames,
        (unsigned)ui_preview.source_updates, (unsigned)ui_preview.rate_limited_frames,
        (unsigned)ui_preview.replaced_pending_frames, (unsigned)ui_preview.no_buffer_frames,
        (unsigned)ui_preview.cache_sync_errors, (unsigned)ui_preview.input_row_bytes,
        (unsigned)ui_preview.stride_bytes, (unsigned)ui_preview.buffer_bytes,
        (unsigned)ui_preview.buffer_count,
        (unsigned)ui_preview.integrity_checks, (unsigned)ui_preview.integrity_mismatches,
        (unsigned)ui_preview.last_mismatch_frame, (unsigned)ui_preview.pink_jump_candidates,
        (unsigned)ui_preview.last_pink_frame, (unsigned)ui_preview.last_pink_before_rgb,
        (unsigned)ui_preview.last_pink_after_rgb));
    /* 第二段：STA/SoftAP、重连、SNTP 与 mDNS 名称；字符串统一走 JSON 转义。 */
    app_json_result_discard(cc_string_builder_appendf(&sb,
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
        status->network.time_synced ? "true" : "false"));
    app_json_escape_append(&sb, status->network.ssid);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"sta_ip\":"));
    app_json_escape_append(&sb, status->network.sta_ip);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"ap_ssid\":"));
    app_json_escape_append(&sb, status->network.ap_ssid);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"ap_ip\":"));
    app_json_escape_append(&sb, status->network.ap_ip);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"mdns_hostname\":"));
    app_json_escape_append(&sb, status->network.mdns_hostname);
    app_json_result_discard(cc_string_builder_appendf(&sb,
        ",\"command_high_water\":%u,\"wifi_event_coalesced\":%u,"
        "\"config_coalesced\":%u,\"owner_heartbeat_ms\":%llu,\"stopping\":%s",
        (unsigned)status->network.command_high_water,
        (unsigned)status->network.wifi_event_coalesced,
        (unsigned)status->network.config_coalesced,
        (unsigned long long)status->network.owner_heartbeat_ms,
        status->network.stopping ? "true" : "false"));
    /* 第三段：EventBus 生命周期、维护模式、丢弃计数、队列高水位和路由。 */
    app_json_result_discard(cc_string_builder_append(&sb, "},\"events\":{\"lifecycle\":"));
    app_json_escape_append(&sb, app_event_bus_lifecycle_string(events.lifecycle));
    app_json_result_discard(cc_string_builder_appendf(&sb,
        ",\"maintenance\":%s,\"maintenance_reason\":",
        events.maintenance_mode ? "true" : "false"));
    app_json_escape_append(&sb, events.maintenance_reason);
    app_json_result_discard(cc_string_builder_appendf(&sb,
        ",\"serial_debug\":%s,\"usb_log_enabled\":%s,\"usb_log_started\":%s,"
        "\"usb_fallback_serial\":%s,\"dropped\":%u,\"ring_overwrites\":%u,"
        "\"sink_dropped\":%u,\"published\":%u,\"maintenance_skipped\":%u,"
        "\"oom_dropped\":%u,\"usb_queue_dropped\":%u,\"display_queue_dropped\":%u,"
        "\"max_usb_queue_waiting\":%u,\"max_display_queue_waiting\":%u,"
        "\"oldest_seq\":%u,\"latest_seq\":%u,\"capacity\":%u,\"retained_count\":%u,\"retained_bytes\":%u,"
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
        (unsigned)events.retained_count,
        (unsigned)events.retained_bytes,
        (unsigned)events.usb_bytes_written));
    app_json_escape_append(&sb, events.usb_log_path);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"routes\":{\"log\":"));
    app_json_escape_append(&sb, events.log_route);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"chat\":"));
    app_json_escape_append(&sb, events.chat_route);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"thinking\":"));
    app_json_escape_append(&sb, events.thinking_route);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"tool\":"));
    app_json_escape_append(&sb, events.tool_route);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"status\":"));
    app_json_escape_append(&sb, events.status_route);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"error\":"));
    app_json_escape_append(&sb, events.error_route);
    app_json_result_discard(cc_string_builder_append(&sb, ",\"profile\":"));
    app_json_escape_append(&sb, events.profile_route);
    app_json_result_discard(cc_string_builder_append(&sb, "}"));
    /* 第四段：Interaction owner task 的状态、generation、队列和会话快照。 */
    app_json_result_discard(cc_string_builder_append(&sb, "},\"interaction\":{\"state\":"));
    app_json_escape_append(&sb, interaction_service_state_string(interaction.state));
    app_json_result_discard(cc_string_builder_appendf(&sb,
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
        interaction.stream_supported ? "true" : "false"));
    app_json_escape_append(&sb, interaction.session_id);
    app_json_result_discard(cc_string_builder_append(&sb, "},"));
    /* /clear 清除对话提示后，Web 状态仍呈现恢复模式；具体工具阻断继续由账本保管。 */
    app_json_result_discard(cc_string_builder_append(&sb, "\"recovery\":{\"mode\":"));
    app_json_escape_append(&sb, recovery_service_mode_name(s_recovery));
    app_json_result_discard(cc_string_builder_append(&sb, "},"));
    /* profile 自行追加带键名的 JSON 成员，最后由本函数闭合最外层对象。 */
    app_profile_append_status_json(&sb);
    app_json_result_discard(cc_string_builder_append(&sb, "}"));
    return cc_string_builder_take(&sb);
}

/**
 * @brief 将 Web 事件查询转交 App EventBus 的环与当前页面快照入口。
 * @param[in] since 环事件的排他序列游标，清屏代际与覆盖处理由总线决定。
 * @param[in] limit 请求条数，非正或超过当前 ring 容量时由总线归一到容量。
 * @param[in] kind_mask 事件种类位掩码，零表示全部种类。
 * @param[in] user_data 预留借用上下文，当前忽略，允许 NULL。
 * @return 调用者需要 free 的拥有 JSON，接纳关闭或快照/构造分配失败为 NULL。
 */
static char *app_events_json(uint32_t since, int limit, uint32_t kind_mask, void *user_data)
{
    (void)user_data;
    /* since 是排他游标；分页、过滤和 ring overwrite 语义由 EventBus 统一实现。 */
    return app_event_bus_json(since, limit, kind_mask);
}

/**
 * @brief 以 Web 来源通过 startup gate 提交文本或启动阶段清屏请求。
 * @param[in] text 同步借用的非空文本，下游按当前 gate 阶段处理。
 * @param[in] user_data 预留借用上下文，当前忽略，允许 NULL。
 * @return gate/交互入口的参数、状态或队列接纳结果，ESP_OK 表示请求已按对应路径处理。
 */
static esp_err_t app_submit_message(const char *text, void *user_data)
{
    (void)user_data;
    return startup_gate_submit(APP_EVENT_SOURCE_WEB, text);
}

/**
 * @brief 以串口来源通过同一 startup gate 提交文本。
 * @param[in] text 同步借用的非空文本，NULL/空串由 gate 返回参数错误。
 * @param[in] user_data 预留借用上下文，当前忽略，允许 NULL。
 * @return 原样返回 gate 处理或交互服务接纳结果。
 */
static esp_err_t app_submit_serial_message(const char *text, void *user_data)
{
    (void)user_data;
    return startup_gate_submit(APP_EVENT_SOURCE_SERIAL, text);
}

/**
 * @brief 为受限 Web 生成阶段状态，提升失败而恢复启动页时呈现 web_unavailable。
 * @param[in] user_data 预留借用上下文，当前忽略，允许 NULL。
 * @return 调用者需要 free 的拥有 JSON，分配失败为 NULL。
 * @note 阶段和降级标记分别原子读取，RUNNING 下的串口交互可继续使用。
 */
static char *app_startup_status_json(void *user_data)
{
    (void)user_data;
    /* 启动页恢复后呈现 Web 降级状态，结束前端反复提升重载，串口仍可使用已经就绪的交互。 */
    if (startup_gate_phase()==STARTUP_GATE_RUNNING &&
        __atomic_load_n(&s_boot_web_degraded,__ATOMIC_ACQUIRE)) {
        static const char status[]="{\"phase\":\"web_unavailable\",\"message\":\"Web 控制台未完全启动；串口仍可使用，请查看设备日志。\"}";
        char *copy=malloc(sizeof(status));
        if (copy) memcpy(copy,status,sizeof(status));
        return copy;
    }
    return startup_gate_status_json();
}

/**
 * @brief 用暂存配置启动网络和受限 Web 页面，保留初始网络及页面启动结果。
 * @param[in] config 完整暂存配置的同步借用，NULL 直接结束，网络入口复制配置，startup_only Web 复制所需 IO 标量。
 * @post 失败只记录警告，受限页面提供阶段、事件与 gated 输入，后续提升由启动 owner 决定。
 * @pre owner 仍处前检阶段，暂存和恢复对象持续保管至受限服务排空或页面成功提升。
 */
static void app_start_preflight_network(const app_config_t *config)
{
    if (!config) return;
    esp_err_t err=network_service_init();
    if (err==ESP_OK) err=network_service_set_web_port((uint16_t)config->io.web.port);
    if (err==ESP_OK) err=network_service_start_from_config(&config->network,CONFIG_NETWORK_APPLY_TIMEOUT_MS);
    s_boot_wifi_ok=err==ESP_OK;
    board_ui_set_wifi_state(s_boot_wifi_ok && config->network.enabled,s_boot_wifi_ok ? "ok" : "reconnecting");
    if (err!=ESP_OK) APP_LOGW("Startup network unavailable: %s",esp_err_to_name(err));
    const web_console_context_t context={
        .io=&config->io, .status_json=app_startup_status_json,
        .events_json=app_events_json, .message_submitted=app_submit_message,
        .user_data=&s_app, .startup_only=true,
    };
    err=web_console_start(&context);
    s_boot_web_started=err==ESP_OK && config->io.web.enabled;
    if (err!=ESP_OK) APP_LOGW("Startup Web unavailable: %s",esp_err_to_name(err));
}

/**
 * @brief 在配置事务锁内只读建立本盘配置 stage，并核对打开前后的挂载代数。
 * @param[out] out_generation 必填，成功写有效挂载代数，失败保持原值。
 * @return stage 成功且同盘为真，参数、已有 stage、锁、读配置或代数变化失败为假。
 * @post 失败清理本次 stage，成功由启动 owner 保管 stage 至释放路径，配置修复和 workspace 创建由后续步骤执行。
 */
static bool app_stage_config_from_current_mount(uint64_t *out_generation)
{
    uint64_t before=0,after=0;
    if (!out_generation || s_boot_stage || !usb_storage_service_current_generation(&before) ||
        !s_config_transaction_mutex || xSemaphoreTake(s_config_transaction_mutex,pdMS_TO_TICKS(5000U))!=pdTRUE)
        return false;
    esp_err_t err=app_config_stage_open(APP_CONFIG_FILE_PATH,&s_boot_stage);
    bool current=usb_storage_service_current_generation(&after) && before==after;
    if (err!=ESP_OK || !current) {
        app_config_stage_destroy(s_boot_stage);
        s_boot_stage=NULL;
    } else *out_generation=before;
    xSemaphoreGive(s_config_transaction_mutex);
    return err==ESP_OK && current;
}

/**
 * @brief 在启动前检换盘时关闭呈现，反复排空受限 Web/网络后释放 stage 与恢复对象。
 * @pre builder 和记忆 owner 尚未创建，由唯一启动 owner 调用。
 * @post 成功后暂存与恢复借用均结束，失败的服务停止每一百毫秒重试，等待期间继续保管依赖。
 * @note 本函数采用直到依赖退出的重试，具体网络调用使用自身五秒预算。
 */
static void app_discard_preflight(void)
{
    startup_recovery_reset();
    while (web_console_destroy()!=ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));
    while (!network_service_stop(5000U)) vTaskDelay(pdMS_TO_TICKS(100));
    while (network_service_destroy(5000U)!=ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));
    app_release_startup_recovery();
}

/**
 * @brief 修复配置、准备 workspace，再在事务锁内把已核对配置移入应用上下文。
 * @return 三步完成为真，任一恢复或五秒等锁失败记录原因并返回假。
 * @pre s_recovery 与 s_boot_stage 属于同次有效前检，启动 owner 排除其他装配者。
 * @post 解析配置成功时由 s_app.config 接管，原始 stage 快照继续保管供后续复查，返回消息均释放。
 */
static bool app_finish_staged_config(void)
{
    cc_result_t rc=startup_recovery_repair_config(s_recovery);
    if (rc.code==CC_OK) rc=startup_recovery_prepare_workspace(s_recovery);
    if (rc.code==CC_OK) {
        if (!s_config_transaction_mutex ||
            xSemaphoreTake(s_config_transaction_mutex,pdMS_TO_TICKS(5000U))!=pdTRUE) {
            cc_result_free(&rc);
            rc=cc_result_error(CC_ERR_TIMEOUT,"Cannot lock startup configuration");
        } else {
            rc=startup_recovery_take_staged_config(s_recovery,s_boot_stage,&s_app.config);
            xSemaphoreGive(s_config_transaction_mutex);
        }
    }
    bool success=rc.code==CC_OK;
    if (!success) APP_LOGE("Staged configuration setup failed: %s",rc.message ? rc.message : cc_error_string(rc.code));
    cc_result_free(&rc);
    return success;
}

/**
 * @brief 消费 USB 生命周期事件，完成本盘暂存、恢复前检和配置接管后结束启动等待。
 * @param[in] usb_events 已启动 USB host 的借用队列，覆盖等待期，结合 sticky connect/disconnect 消费。
 * @return 配置移入应用上下文后为真，pending-verify 等待在循环边界超过六十秒时为假。
 * @pre 唯一启动 owner 调用，builder 尚未创建，已有串口与启动显示可呈现 gate 提示。
 * @note 普通启动持续等待，子步骤按自身预算或重试等待。断盘先排空受限网络/Web，再释放前检对象和重试卸载。
 */
static bool wait_for_config(QueueHandle_t usb_events)
{
    /* 启动屏障持续保管诊断入口，后续服务只接纳同一盘已核对的配置快照。 */
    ota_manager_refresh_status();
    ota_manager_status_t ota_status;
    ota_manager_get_status(&ota_status);
    TickType_t ota_wait_started = xTaskGetTickCount();
    bool pending_verify = ota_status.rollback_pending;

    while (true) {
        usb_storage_event_t event;
        if (pending_verify &&
            xTaskGetTickCount() - ota_wait_started >= pdMS_TO_TICKS(OTA_CONFIG_WAIT_TIMEOUT_MS)) {
            APP_LOGE("USB configuration did not become available during OTA verification window");
            return false;
        }

        /* 短周期轮询同时处理 stage 保管期间的断开事件。 */
        msc_host_device_handle_t disconnected = NULL;
        uint64_t disconnect_generation = 0;
        bool got_event = usb_storage_service_take_pending_disconnect(&disconnected, &disconnect_generation);
        if (got_event) {
            event.id = USB_STORAGE_DEVICE_DISCONNECTED;
            event.data.handle = disconnected;
            event.generation = disconnect_generation;
        } else {
            got_event = usb_storage_service_take_pending_connect(&event) ||
                        xQueueReceive(usb_events, &event, pdMS_TO_TICKS(250)) == pdTRUE;
        }
        if (got_event) {
            if (event.id == USB_STORAGE_DEVICE_CONNECTED) {
                if (s_boot_stage || s_recovery || usb_storage_service_is_mounted()) continue;
                APP_LOGI("USB device connected, mounting...");

                esp_err_t mount_err = usb_storage_service_mount(event.data.address);
                if (mount_err != ESP_OK) {
                    APP_LOGE("USB mount failed: %s", esp_err_to_name(mount_err));
                    continue;
                }
                if (!usb_storage_service_is_mounted()) {
                    APP_LOGW("USB disconnected during mount; waiting for cleanup");
                    continue;
                }
                board_ui_set_usb_mounted(true);

                uint64_t generation=0;
                if (!app_stage_config_from_current_mount(&generation)) {
                    APP_LOGE("Cannot read startup configuration from %s",APP_CONFIG_FILE_PATH);
                    cc_result_t failure=startup_recovery_present_failure(RECOVERY_PREFLIGHT_OTHER);
                    cc_result_free(&failure);
                    continue;
                }
                recovery_preflight_failure_t failure=RECOVERY_PREFLIGHT_OTHER;
                cc_result_t rc=startup_recovery_prepare_staged(s_boot_stage,generation,&failure,&s_recovery);
                if (rc.code!=CC_OK) {
                    APP_LOGE("Storage preflight failed: %s",rc.message ? rc.message : cc_error_string(rc.code));
                    cc_result_free(&rc);
                    rc=startup_recovery_present_failure(failure);
                    cc_result_free(&rc);
                    continue;
                }
                cc_result_free(&rc);
                recovery_service_state_t state=recovery_service_state(s_recovery);
                if (state==RECOVERY_SERVICE_READY_TO_ATTACH)
                    app_start_preflight_network(app_config_stage_config(s_boot_stage));
            } else if (event.id == USB_STORAGE_DEVICE_DISCONNECTED) {
                if (!usb_storage_service_disconnect_is_current(event.data.handle, event.generation)) continue;
                APP_LOGW("USB device disconnected while waiting for config");
                app_discard_preflight();
                while (app_event_bus_start_serial_fallback_debug() != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                esp_err_t unmount_err;
                do {
                    unmount_err = usb_storage_service_unmount(event.data.handle);
                    if (unmount_err != ESP_OK && unmount_err != ESP_ERR_NOT_FOUND) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                } while (unmount_err != ESP_OK && unmount_err != ESP_ERR_NOT_FOUND);
                if (unmount_err == ESP_OK) board_ui_set_usb_mounted(false);
            }
        }
        if (s_recovery && !s_boot_config_attempted &&
            recovery_service_state(s_recovery)==RECOVERY_SERVICE_READY_TO_ATTACH &&
            startup_gate_phase()==STARTUP_GATE_LOADING) {
            s_boot_config_attempted=true;
            if (app_finish_staged_config()) {
                APP_LOGI("Configuration loaded from %s",APP_CONFIG_FILE_PATH);
                return true;
            }
            startup_gate_set_phase(STARTUP_GATE_FAILED);
            app_event_publish_boot_status(APP_EVENT_KIND_ERROR,APP_EVENT_SOURCE_SYSTEM,CC_LOG_ERROR,
                                          startup_gate_message(STARTUP_GATE_FAILED));
        }
    }
}

/**
 * @brief 由 app_main 建立组合根，依次完成本盘恢复、记忆和交互装配，再常驻处理 USB 换盘。
 * @pre 唯一启动 owner 调用一次，全局对象处于本次启动的初始状态。
 * @post 成功时先开放 RUNNING 文本，再完成 Web/Camera/OTA 并发布 boot_complete，随后常驻；初始化/NVS/关键服务失败可返回。
 * @note NVS 前检失败保留诊断，其他关键失败按调用点回滚。USB 换盘先归还 guard 再尝试关闭，记录 drain 失败后仍交给 esp_restart。
 * 全局配置、恢复与记忆由本 owner 保管，UI/Web/serial 借用经各自屏障结束后才释放。
 */
void app_lifecycle_run(void)
{
    /* 阶段 1：建立组合根和 EventBus，使后续启动日志已有统一观测出口。 */
    __atomic_store_n(&s_boot_complete,0,__ATOMIC_RELEASE);
    app_context_init(&s_app);
    if (!app_config_transaction_init()) {
        ESP_LOGE(TAG, "Config transaction mutex init failed");
        app_context_destroy(&s_app);
        (void)app_reject_pending_ota("configuration mutex initialization failed");
        return;
    }
    if (app_usb_owner_guard_init(&s_usb_owner_guard)!=ESP_OK) {
        ESP_LOGE(TAG,"USB lifecycle guard initialization failed");
        app_config_transaction_destroy();
        app_context_destroy(&s_app);
        (void)app_reject_pending_ota("USB lifecycle guard initialization failed");
        return;
    }
    s_shutdown_mutex=xSemaphoreCreateMutex();
    if (!s_shutdown_mutex) {
        ESP_LOGE(TAG,"Shutdown mutex initialization failed");
        app_usb_owner_guard_destroy(&s_usb_owner_guard);
        app_config_transaction_destroy();
        app_context_destroy(&s_app);
        (void)app_reject_pending_ota("shutdown mutex initialization failed");
        return;
    }
    if (app_profile_init() != ESP_OK) {
        ESP_LOGE(TAG, "Profile service init failed");
        (void)app_profile_destroy(0);
        app_usb_owner_guard_destroy(&s_usb_owner_guard);
        vSemaphoreDelete(s_shutdown_mutex); s_shutdown_mutex=NULL;
        app_config_transaction_destroy();
        app_context_destroy(&s_app);
        (void)app_reject_pending_ota("profile initialization failed");
        return;
    }
    if (app_event_bus_init(NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Application EventBus init failed");
        (void)app_event_bus_destroy(1000U);
        (void)app_profile_destroy(1000U);
        app_usb_owner_guard_destroy(&s_usb_owner_guard);
        vSemaphoreDelete(s_shutdown_mutex); s_shutdown_mutex=NULL;
        app_config_transaction_destroy();
        app_context_destroy(&s_app);
        (void)app_reject_pending_ota("event bus initialization failed");
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

    /* USB 配置等待前先启动经过 gate 的串口诊断。 */
    if (!serial_console_start_with_submit(app_submit_serial_message, &s_app)) {
        APP_LOGW("Serial console start failed");
    }

    /* NVS 保管凭据，前检失败保留内容并呈现修复阶段。 */
    cc_result_t nvs_check=startup_recovery_check_nvs();
    if (nvs_check.code!=CC_OK) {
        ESP_LOGE(TAG,"NVS preflight failed; storage preserved: %s",
                 nvs_check.message ? nvs_check.message : cc_error_string(nvs_check.code));
        cc_result_free(&nvs_check);
        return;
    }
    cc_result_free(&nvs_check);

    /* 阶段 3：USB host 和事件队列是配置加载前置条件，失败时无法继续完整启动。 */
    if (usb_storage_service_init(&s_app.usb_events) != ESP_OK ||
        usb_storage_service_start_host(s_app.usb_events) != ESP_OK) {
        APP_LOGE("Failed to start USB host");
        app_startup_rollback(false, false);
        return;
    }

    APP_LOGI("Insert a USB flash drive with %s", APP_CONFIG_FILE_PATH);
    if (!wait_for_config(s_app.usb_events)) {
        app_startup_rollback(false, false);
        return;
    }

    /* 阶段 4：配置核验后才能启动 USB 日志等配置依赖。 */
    cc_result_t storage_check=startup_recovery_validate_staged(s_recovery);
    if (storage_check.code!=CC_OK) {
        APP_LOGE("Startup media changed before services: %s",storage_check.message ? storage_check.message : cc_error_string(storage_check.code));
        cc_result_free(&storage_check);
        app_startup_rollback(false,false);
        return;
    }
    cc_result_free(&storage_check);
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

    /* 阶段 5：消息 API 自检提供诊断，随后准备强制恢复的 runtime 并核对实际返回值。 */
    runtime_service_run_self_test(app_config_runtime(&s_app.config));
    app_profile_span_t runtime_span = app_profile_span_begin("startup", "runtime_start", NULL, NULL);
    storage_check=startup_recovery_validate_staged(s_recovery);
    if (storage_check.code!=CC_OK) {
        APP_LOGE("Startup media changed before runtime: %s",storage_check.message ? storage_check.message : cc_error_string(storage_check.code));
        cc_result_free(&storage_check);
        app_profile_span_end(&runtime_span,"error");
        app_startup_rollback(false,false);
        return;
    }
    cc_result_free(&storage_check);
    if (!runtime_service_prepare_recovery(app_config_runtime(&s_app.config))) {
        app_profile_span_end(&runtime_span, "error");
        APP_LOGE("Runtime start failed");
        app_startup_rollback(false, false);
        return;
    }
    app_profile_span_end(&runtime_span, "ok");
    s_app.runtime_builder = runtime_service_builder();
    storage_check=startup_recovery_validate_staged(s_recovery);
    if (storage_check.code==CC_OK) storage_check=startup_recovery_attach(s_recovery,s_app.runtime_builder);
    if (storage_check.code!=CC_OK) {
        APP_LOGE("Recovery attachment failed: %s",storage_check.message ? storage_check.message : cc_error_string(storage_check.code));
        cc_result_free(&storage_check);
        app_startup_rollback(false,true);
        return;
    }
    cc_result_free(&storage_check);
    runtime_service_activate_observability();

    bool memory_guard_held=app_usb_owner_guard_enter(&s_usb_owner_guard,5000U);
    storage_check=memory_guard_held ? startup_recovery_validate_staged(s_recovery) :
        cc_result_error(CC_ERR_TIMEOUT,"Cannot guard automatic memory startup");
    if (storage_check.code==CC_OK)
        storage_check=startup_managed_memory_create(&s_app.config,s_app.runtime_builder,s_recovery,
                                                    &s_memory_owner);
    if (memory_guard_held) app_usb_owner_guard_leave(&s_usb_owner_guard);
    if (storage_check.code!=CC_OK) {
        APP_LOGE("Automatic memory startup failed: %s",
                 storage_check.message ? storage_check.message : cc_error_string(storage_check.code));
        cc_result_free(&storage_check);
        app_startup_rollback(false,true);
        return;
    }
    cc_result_free(&storage_check);
    if (startup_managed_memory_summary_prune_deferred(s_memory_owner))
        app_event_publish(APP_EVENT_KIND_STATUS,APP_EVENT_SOURCE_RUNTIME,CC_LOG_WARN,
            "摘要缓存暂未清理至配置上限，请检查 U 盘空间或记忆文件恢复状态；后续摘要写入会重试清理。");

    /* Interaction 依赖已创建的 runtime，并成为用户命令和 Agent run 的唯一业务 owner。 */
    app_profile_span_t interaction_span = app_profile_span_begin("startup", "interaction_start", NULL, NULL);
    esp_err_t interaction_err = interaction_service_start_with_managed_memory(app_config_io(&s_app.config),
        s_app.runtime_builder,s_recovery,MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY,
        startup_managed_memory_turns(s_memory_owner));
    if (interaction_err != ESP_OK) {
        app_profile_span_end(&interaction_span, "error");
        APP_LOGE("Interaction start failed: %s", esp_err_to_name(interaction_err));
        app_startup_rollback(true, true);
        return;
    }
    app_profile_span_end(&interaction_span, "ok");
    storage_check=startup_recovery_release_stage(s_recovery);
    bool boot_guard_held=false;
    if (storage_check.code==CC_OK) {
        app_config_stage_destroy(s_boot_stage);
        s_boot_stage=NULL;
        if (!app_usb_owner_guard_enter(&s_usb_owner_guard,5000U))
            storage_check=cc_result_error(CC_ERR_TIMEOUT,"Cannot guard startup USB lifecycle");
        else {
            boot_guard_held=true;
            storage_check=startup_recovery_mark_running(s_recovery);
        }
    }
    if (storage_check.code!=CC_OK) {
        APP_LOGE("Recovery startup could not open input: %s",storage_check.message ? storage_check.message : cc_error_string(storage_check.code));
        cc_result_free(&storage_check);
        if (boot_guard_held) app_usb_owner_guard_leave(&s_usb_owner_guard);
        app_startup_rollback(true,true);
        return;
    }
    cc_result_free(&storage_check);
    if (!serial_console_running() && !serial_console_start_with_submit(app_submit_serial_message, &s_app)) {
        APP_LOGW("Serial console restart failed");
    }
    APP_LOGI("Runtime and interaction started");
    APP_LOGI("长期记忆自动评估已启用；手动 Lua 独立于模型工具开关并受操作账本约束。");

    /* 网络已在受限启动页阶段初始化；首次未连上不阻止本地业务。 */
    bool wifi_ok = s_boot_wifi_ok;
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
        .file_read_allowed = app_web_file_path_allowed,
        .file_mutation_allowed = app_web_file_path_allowed,
        .user_data = &s_app,
    };
    app_profile_span_t web_span = app_profile_span_begin("startup", "web_console_start", NULL, NULL);
    bool startup_restored=false;
    esp_err_t web_err = s_boot_web_started ?
        web_console_promote_startup(&web_context,&startup_restored) : web_console_start(&web_context);
    __atomic_store_n(&s_boot_web_degraded,web_err!=ESP_OK && startup_restored,__ATOMIC_RELEASE);
    app_profile_span_end(&web_span, web_err == ESP_OK ? "ok" : "error");
    if (web_err != ESP_OK) {
        APP_LOGW("Web console start failed: %s (startup page restored: %d)",esp_err_to_name(web_err),startup_restored ? 1 : 0);
    }

    if (!wifi_ok) {
        APP_LOGW("WiFi is not connected yet; check the network status");
    }

    /* 阶段 7：Camera 放在交互和网络之后启动，失败时保留文本 Agent 与诊断能力。 */
    app_profile_span_t camera_span = app_profile_span_begin("startup", "camera_start", NULL, NULL);
    esp_err_t camera_err = start_camera_with_retry();
    app_profile_span_end(&camera_span, camera_err == ESP_OK ? "ok" : "error");
    /* 阶段 8：所有关键启动阶段执行后，再决定 pending-verify 镜像是否可标记 valid。 */
    app_mark_ota_valid_after_health_check(web_err == ESP_OK, camera_err == ESP_OK);

    APP_LOGI("Initialization complete; switching to main console");
    board_ui_boot_log_finish();
    __atomic_store_n(&s_boot_complete,1,__ATOMIC_RELEASE);
    app_usb_owner_guard_leave(&s_usb_owner_guard);

    /*
     * 阶段 9：常驻主循环只处理 USB 生命周期。
     * 拔出时先排空持有 USB 文件句柄的服务，再注销 VFS。再次插盘时走受控软件重启，
     * 由启动路径完整装载当前盘的配置、会话、记忆和操作账本。
     */
    msc_host_device_handle_t pending_unmount = NULL;
    TickType_t last_unmount_warning = 0;
    while (true) {
        if (!app_usb_owner_guard_enter(&s_usb_owner_guard,250U)) {
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }
        if (!pending_unmount) {
            msc_host_device_handle_t disconnected = NULL;
            uint64_t generation = 0;
            if (usb_storage_service_take_pending_disconnect(&disconnected, &generation) &&
                usb_storage_service_disconnect_is_current(disconnected, generation)) {
                APP_LOGW("USB device disconnected");
                startup_gate_set_phase(STARTUP_GATE_USB_DISCONNECTED);
                pending_unmount = disconnected;
                board_ui_set_usb_mounted(false);
            }
        }
        if (pending_unmount) {
            esp_err_t quiesce_err = app_quiesce_usb_storage_users(pending_unmount);
            if (quiesce_err == ESP_ERR_NOT_FOUND) {
                pending_unmount = NULL;
                app_usb_owner_guard_leave(&s_usb_owner_guard);
                continue;
            }
            if (quiesce_err == ESP_OK) {
                esp_err_t unmount_err = usb_storage_service_unmount(pending_unmount);
                if (unmount_err == ESP_OK || unmount_err == ESP_ERR_NOT_FOUND) {
                    if (unmount_err == ESP_OK) board_ui_set_usb_mounted(false);
                    pending_unmount = NULL;
                    app_usb_owner_guard_leave(&s_usb_owner_guard);
                    continue;
                }
                quiesce_err = unmount_err;
            }
            TickType_t now = xTaskGetTickCount();
            if (now - last_unmount_warning >= pdMS_TO_TICKS(5000U)) {
                APP_LOGW("USB unmount is waiting for active users: %s", esp_err_to_name(quiesce_err));
                last_unmount_warning = now;
            }
            app_usb_owner_guard_leave(&s_usb_owner_guard);
            vTaskDelay(pdMS_TO_TICKS(250U));
            continue;
        }
        usb_storage_event_t event;

        if (usb_storage_service_take_pending_connect(&event) ||
            xQueueReceive(s_app.usb_events, &event, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (event.id == USB_STORAGE_DEVICE_DISCONNECTED) {
                if (!usb_storage_service_disconnect_is_current(event.data.handle, event.generation)) {
                    app_usb_owner_guard_leave(&s_usb_owner_guard);
                    continue;
                }
                APP_LOGW("USB device disconnected");
                startup_gate_set_phase(STARTUP_GATE_USB_DISCONNECTED);
                pending_unmount = event.data.handle;
                board_ui_set_usb_mounted(false);
            } else if (event.id == USB_STORAGE_DEVICE_CONNECTED) {
                if (usb_storage_service_is_mounted()) {
                    app_usb_owner_guard_leave(&s_usb_owner_guard);
                    continue;
                }
                startup_gate_set_phase(STARTUP_GATE_USB_RELOADING);
                APP_LOGI("USB device connected; restarting to load its configuration and storage");
                /* 重启关闭会等待同一 guard，先结束当前持锁区间，新盘由下一次启动装载。 */
                app_usb_owner_guard_leave(&s_usb_owner_guard);
                esp_err_t drain_err=app_lifecycle_prepare_for_reboot(5000U);
                if (drain_err!=ESP_OK)
                    ESP_LOGW(TAG,"USB reload reboot after incomplete service drain: %s",esp_err_to_name(drain_err));
                esp_restart();
                return;
            }
        }
        app_usb_owner_guard_leave(&s_usb_owner_guard);
    }
}
