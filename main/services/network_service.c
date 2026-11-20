#include "network_service.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_event_bus.h"
#include "runtime_service.h"
#include "cc/ports/cc_platform.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "wifi_app.h"

#define network_service_TASK_STACK 4096U
#define network_service_TASK_PRIO 5
#define network_service_QUEUE_DEPTH 4U

static const char *TAG = "network_service";

typedef enum {
    NETWORK_JOB_APPLY_CONFIG,
    NETWORK_JOB_STOP,
} network_job_type_t;

typedef struct {
    network_job_type_t type;
} network_job_t;

static QueueHandle_t s_network_queue;
static QueueHandle_t s_wifi_event_queue;
static QueueSetHandle_t s_network_queue_set;
static bool s_command_queue_attached;
static bool s_wifi_queue_attached;
static SemaphoreHandle_t s_network_mutex;
static SemaphoreHandle_t s_config_apply_mutex;
static EventGroupHandle_t s_network_events;
static TaskHandle_t s_network_task;
/* 任务完成全部 SetBits/Give 尾部后发布，槽本身持续到进程结束。 */
static bool s_network_owner_exited = true;
static esp_err_t s_last_stop_result;
static bool s_event_registered;
static bool s_sntp_ready;
static bool s_time_synced;
static bool s_mdns_started;
static bool s_mdns_enabled;
static bool s_mdns_http_service_added;
static uint16_t s_mdns_http_service_port;
static bool s_mdns_ready_logged;
static char s_mdns_hostname[32] = "cclaw";
static uint16_t s_web_port = 80;
static network_config_t s_last_config;
static bool s_last_config_valid;
static bool s_network_quiet_shutdown;
static network_config_t s_pending_config;
static bool s_pending_config_dirty;
static uint64_t s_pending_config_generation;
static bool s_config_job_enqueued;
static uint64_t s_next_config_generation;
static uint64_t s_applied_config_generation;
static esp_err_t s_applied_config_result;
static bool s_stop_for_reboot;
static bool s_stopping;
static int s_stop_enqueue_failed;
static uint32_t s_command_high_water;
static uint32_t s_wifi_event_coalesced;
static uint32_t s_config_coalesced;
static uint64_t s_owner_heartbeat_ms;
/* 进程期准入槽在动态锁、队列和事件组排空及销毁期间保持有效。 */
static portMUX_TYPE s_stop_gate_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned s_stop_callers;
static bool s_stop_gate_closing;

#define NETWORK_EVENT_STOPPED       (1U << 0)
#define NETWORK_EVENT_CONFIG_APPLIED (1U << 1)

/**
 * @brief 将毫秒预算向上换算为有限 tick，避免原生宏乘法溢出。
 * @param[in] timeout_ms 毫秒数，零表示仅检查当前状态。
 * @return 正预算至少一个 tick，最大 portMAX_DELAY-1。
 */
static TickType_t network_timeout_ticks(uint32_t timeout_ms)
{
    uint64_t ticks = ((uint64_t)timeout_ms * configTICK_RATE_HZ + 999U) / 1000U;
    if (ticks >= (uint64_t)portMAX_DELAY) ticks = (uint64_t)portMAX_DELAY - 1U;
    return (TickType_t)ticks;
}

/**
 * @brief 按原始起点取得尚可使用的有限 tick。
 * @param[in] started 本次预算起点的原生 tick。
 * @param[in] timeout 有限相对预算，须小于原生无限等待哨兵。
 * @return 已耗尽返回零，其余返回剩余 tick，调用者立即执行下一次原生等待。
 */
static TickType_t network_stop_remaining(TickType_t started, TickType_t timeout)
{
    TickType_t elapsed = (TickType_t)(xTaskGetTickCount() - started);
    return elapsed >= timeout ? 0 : timeout - elapsed;
}

/**
 * @brief 为只接受毫秒的注销入口向下换算剩余 tick，避免重新给出完整预算。
 * @param[in] started 原始预算起点。
 * @param[in] timeout 原始有限 tick 预算。
 * @return 剩余整毫秒，封顶 UINT32_MAX，不足一毫秒或耗尽返回零作立即检查。
 */
static uint32_t network_remaining_milliseconds(TickType_t started, TickType_t timeout)
{
    uint64_t remaining = (uint64_t)network_stop_remaining(started, timeout) * 1000U / configTICK_RATE_HZ;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}

/**
 * @brief 在状态锁内更新本服务的时间同步标记。
 * @param[in] synced 本次判断的同步结果。
 * @post 有可用状态锁时完成更新，未初始化时跳过，调用返回前归还锁。
 */
static void network_set_time_synced(bool synced)
{
    if (s_network_mutex && xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
        s_time_synced = synced;
        xSemaphoreGive(s_network_mutex);
    }
}

/**
 * @brief 用二〇二四年元旦阈值判断本机日历时间是否已离开未校时状态。
 * @return time 结果不早于 1704067200 为 true，其余 false，无动态资源。
 * @note 这是是否需要 SNTP 的启发式检查，证书的实际日期及链验证仍由 TLS 完成。
 */
static bool system_time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return now >= 1704067200;
}

/**
 * @brief 通过当前 IDF 普通创建入口建立内部 RAM 栈的网络任务。
 * @param[in] task 必填任务入口，创建返回前即可调度。
 * @param[in] name 调用期稳定名称借用。
 * @param[in] stack_bytes 正栈字节数，使用 ESP-IDF 单位。
 * @param[in,out] arg 借给任务的上下文，允许 NULL，覆盖任务寿命。
 * @param[in] priority 原生优先级。
 * @param[out] out_handle 可空原生句柄槽，成功后的身份仅借用，任务最终自删。
 * @return pdPASS 成功，其余为原生创建失败。
 * @note Wi-Fi/NVS 操作可能进入冻结缓存的 Flash 路径，当前任务使用内部栈。
 */
static BaseType_t network_create_task(TaskFunction_t task,
                                      const char *name,
                                      uint32_t stack_bytes,
                                      void *arg,
                                      UBaseType_t priority,
                                      TaskHandle_t *out_handle)
{
    return xTaskCreate(task, name, stack_bytes, arg, priority, out_handle);
}

/**
 * @brief 由网络 owner 在连接后初始化或重启 SNTP，并等待十五秒校时窗口。
 * @post 更新锁内 time_synced，成功且日历有效时尽力发布应用校时事件，错误仅记录日志。
 * @note SNTP/native 状态归本 owner 串行管理，日历已经越过阈值时直接复用。原生初始化
 *       与事件回调耗时遵循其实现，SNTP wait 的十五秒单独于调用者的配置确认预算。
 */
static void sync_time_after_connect(void)
{
    if (system_time_is_valid()) {
        network_set_time_synced(true);
        return;
    }

    if (!s_sntp_ready) {
        esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        esp_err_t err = esp_netif_sntp_init(&sntp_config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
            network_set_time_synced(false);
            return;
        }
        s_sntp_ready = true;
    } else {
        esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SNTP restart failed: %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "Waiting for SNTP time sync");
    esp_err_t err = esp_netif_sntp_sync_wait(network_timeout_ticks(15000));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP time sync timed out: %s", esp_err_to_name(err));
        network_set_time_synced(system_time_is_valid());
        return;
    }

    time_t now = 0;
    time(&now);
    bool time_synced = system_time_is_valid();
    network_set_time_synced(time_synced);
    ESP_LOGI(TAG, "System time synchronized: %lld", (long long)now);
    if (time_synced) {
        app_event_bus_note_time_synced();
    }
}

/**
 * @brief 请求 runtime 清理当前 HTTP 持久连接，消费所有结果诊断。
 * @param[in] reason 可空调用期日志说明借用。
 * @post 已尝试重置，尚无 runtime 的 INVALID_STATE 按预期跳过，其余错误记录后继续。
 * @note 在网络 owner 的状态锁之外调用，真实 HTTP 在途关闭由 runtime/端口协议完成。
 */
static void reset_http_connections(const char *reason)
{
    ESP_LOGW(TAG, "Resetting HTTP persistent connections%s%s",
             reason ? ": " : "",
             reason ? reason : "");
    cc_result_t rc = runtime_service_reset_http_connections();
    if (rc.code != CC_OK && rc.code != CC_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "HTTP connection reset failed: %s", rc.message ? rc.message : "unknown");
    }
    cc_result_free(&rc);
}

/**
 * @brief 将稳定文本按字节截断到固定缓冲并补 NUL。
 * @param[out] dst 可写目标，NULL 时跳过。
 * @param[in] dst_size 实际字节容量，零时跳过。
 * @param[in] src 稳定 NUL 文本借用，NULL 按空串，须与目标范围不重叠。
 * @post 有效目标最多写 dst_size-1 字节正文并终止，不分配资源。
 */
static void network_copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    const char *text = src ? src : "";
    size_t len = strlen(text);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, text, len);
    dst[len] = '\0';
}

/**
 * @brief 由网络 owner 尽力应用 mDNS 名称、开关和当前 HTTP 广告端口。
 * @param[in] config 调用期稳定配置，可为 NULL，此时跳过。
 * @post 锁内发布请求的开关及主机名，锁外初始化/释放 mDNS 和添加 HTTP 服务，失败仅记录。
 * @note mDNS 原生状态由同一 owner 串行修改，端口变更时重建服务，调用不保证所有原生步骤成功。
 */
static void network_apply_mdns_config(const network_config_t *config)
{
    if (!config) {
        return;
    }
    uint16_t web_port = 80;
    if (s_network_mutex && xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
        s_mdns_enabled = config->mdns_enabled;
        web_port = s_web_port ? s_web_port : 80;
        network_copy_text(s_mdns_hostname,
                          sizeof(s_mdns_hostname),
                          config->mdns_hostname[0] ? config->mdns_hostname : "cclaw");
        xSemaphoreGive(s_network_mutex);
    }

    if (!s_mdns_enabled) {
        if (s_mdns_started) {
            mdns_free();
            s_mdns_started = false;
        }
        s_mdns_http_service_added = false;
        s_mdns_ready_logged = false;
        ESP_LOGI(TAG, "mDNS disabled");
        return;
    }

    if (s_mdns_started && s_mdns_http_service_added &&
        s_mdns_http_service_port != web_port) {
        mdns_free();
        s_mdns_started = false;
        s_mdns_http_service_added = false;
        s_mdns_ready_logged = false;
    }
    if (!s_mdns_started) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(err));
            return;
        }
        s_mdns_started = true;
    }

    esp_err_t err = mdns_hostname_set(s_mdns_hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return;
    }
    err = mdns_instance_name_set("C-Claw Web Console");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance set failed: %s", esp_err_to_name(err));
    }
    if (!s_mdns_http_service_added) {
        err = mdns_service_add("C-Claw Web Console", "_http", "_tcp", web_port, NULL, 0);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_mdns_http_service_added = true;
            s_mdns_http_service_port = web_port;
        } else {
            ESP_LOGW(TAG, "mDNS http service add failed: %s", esp_err_to_name(err));
        }
    }
    if (!s_mdns_ready_logged) {
        ESP_LOGI(TAG, "mDNS ready: http://%s.local/", s_mdns_hostname);
        s_mdns_ready_logged = true;
    }
}

/**
 * @brief 在唯一网络 owner 中处理最新 Wi-Fi 状态事件。
 * @param[in] event 已按值出队的事件编号，未知值跳过。
 * @post 连接时尝试 mDNS/SNTP，断开或禁用时按静默关闭状态决定是否重置 HTTP，禁用清校时标志。
 * @note 所有潜在阻塞操作均在回调转发之后执行，Wi-Fi 原生事件线程只发布值事件。
 */
static void network_handle_wifi_event(wifi_app_event_t event)
{
    switch (event) {
    case WIFI_APP_EVENT_CONNECTED:
        if (s_last_config_valid) {
            network_apply_mdns_config(&s_last_config);
        }
        sync_time_after_connect();
        break;
    case WIFI_APP_EVENT_DISCONNECTED:
        if (!s_network_quiet_shutdown) {
            reset_http_connections("wifi disconnected");
        }
        break;
    case WIFI_APP_EVENT_RECONNECTING:
        break;
    case WIFI_APP_EVENT_DISABLED:
        network_set_time_synced(false);
        if (!s_network_quiet_shutdown) {
            reset_http_connections("wifi disabled");
        }
        break;
    default:
        break;
    }
}

/**
 * @brief 成功停机后公布退出事件，并在全部动态原语尾部返回后发布最终完成。
 * @post 锁内设置 STOPPED、清任务身份及提交标志，Give 返回后 release 进程期完成原子。
 * @pre 由唯一 owner 的成功关闭路径调用，随后只能内核自删，不再访问服务资源。
 */
static void network_owner_finish_stop(void)
{
    if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
        /* 事件与锁归还均可能在唤醒等待者后继续访问原语，最终原子在两者返回后发布。 */
        xEventGroupSetBits(s_network_events, NETWORK_EVENT_STOPPED);
        s_network_task = NULL;
        s_stopping = false;
        s_config_job_enqueued = false;
        xSemaphoreGive(s_network_mutex);
    }
    __atomic_store_n(&s_network_owner_exited, true, __ATOMIC_RELEASE);
}

/**
 * @brief 常驻处理配置副本、最新 Wi-Fi 状态和停止命令的唯一网络 owner。
 * @param[in] arg 未使用的可空任务上下文。
 * @post 配置结果按 generation 存入服务字段并发事件，停止成功完成交接后自删，停止失败
 *       保留任务和资源等待后续 STOP 重试，错误不会伪装为已经退出。
 * @note Wi-Fi/mDNS/SNTP/HTTP 操作在状态锁之外，配置调用者超时不取消已接纳任务。
 */
static void network_task(void *arg)
{
    (void)arg;

    for (;;) {
        QueueSetMemberHandle_t ready = xQueueSelectFromSet(s_network_queue_set, portMAX_DELAY);
        if (s_network_mutex && xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
            s_owner_heartbeat_ms = cc_platform_monotonic_ms();
            xSemaphoreGive(s_network_mutex);
        }
        if (ready == s_wifi_event_queue) {
            wifi_app_event_t event;
            if (xQueueReceive(s_wifi_event_queue, &event, 0) == pdTRUE) {
                network_handle_wifi_event(event);
            }
            continue;
        }
        if (ready != s_network_queue) continue;

        network_job_t job;
        if (xQueueReceive(s_network_queue, &job, 0) != pdTRUE) continue;
        if (job.type == NETWORK_JOB_APPLY_CONFIG) {
            for (;;) {
                network_config_t config;
                uint64_t generation = 0;
                bool have_config = false;
                if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
                    if (s_pending_config_dirty) {
                        config = s_pending_config;
                        generation = s_pending_config_generation;
                        s_pending_config_dirty = false;
                        s_network_quiet_shutdown = false;
                        have_config = true;
                    } else {
                        s_config_job_enqueued = false;
                    }
                    xSemaphoreGive(s_network_mutex);
                }
                if (!have_config) break;
                bool ok = wifi_app_connect_from_config(&config);
                network_apply_mdns_config(&config);
                if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
                    if (ok) {
                        s_last_config = config;
                        s_last_config_valid = true;
                    }
                    s_applied_config_generation = generation;
                    s_applied_config_result = ok ? ESP_OK : ESP_FAIL;
                    bool repeat = s_pending_config_dirty;
                    if (!repeat) s_config_job_enqueued = false;
                    xSemaphoreGive(s_network_mutex);
                    xEventGroupSetBits(s_network_events, NETWORK_EVENT_CONFIG_APPLIED);
                    ESP_LOGI(TAG, "WiFi config apply %s", ok ? "ok" : "failed");
                    if (!repeat) break;
                } else {
                    break;
                }
            }
        } else if (job.type == NETWORK_JOB_STOP) {
            bool reboot = false;
            if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
                reboot = s_stop_for_reboot;
                xSemaphoreGive(s_network_mutex);
            }
            if (s_mdns_started) {
                mdns_free();
                s_mdns_started = false;
            }
            s_mdns_http_service_added = false;
            s_mdns_ready_logged = false;
            network_set_time_synced(false);
            reset_http_connections(reboot ? "reboot" : "service stop");
            s_network_quiet_shutdown = true;
            esp_err_t stop_result = ESP_OK;
            if (reboot) wifi_app_prepare_for_reboot();
            else stop_result = wifi_app_shutdown_for_ota();
            if (stop_result != ESP_OK) {
                if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
                    s_last_stop_result = stop_result;
                    s_stopping = false;
                    xEventGroupSetBits(s_network_events, NETWORK_EVENT_STOPPED);
                    xSemaphoreGive(s_network_mutex);
                }
                continue;
            }
            network_owner_finish_stop();
            vTaskDelete(NULL);
        }
    }
}

/**
 * @brief 将 Wi-Fi 状态变更零等待合并到长度一队列。
 * @param[in] event 按值复制，RECONNECTING 省略，其他状态覆盖旧通知。
 * @param[in] status 未使用的回调期借用快照，可为 NULL。
 * @param[in] user_data 未使用的可空注册上下文。
 * @post 新事件已尝试覆盖，存在旧事件时尽力饱和累计合并计数。
 * @note 回调借用由 wifi_event_callback 的进程期准入槽保护，destroy 先注销排空再删除队列。
 */
static void network_wifi_event_cb(wifi_app_event_t event,
                                  const wifi_app_status_t *status,
                                  void *user_data)
{
    (void)status;
    (void)user_data;

    if (event == WIFI_APP_EVENT_RECONNECTING || !s_wifi_event_queue) return;
    if (uxQueueMessagesWaiting(s_wifi_event_queue) > 0 && s_network_mutex &&
        xSemaphoreTake(s_network_mutex, 0) == pdTRUE) {
        if (s_wifi_event_coalesced < UINT32_MAX) s_wifi_event_coalesced++;
        xSemaphoreGive(s_network_mutex);
    }
    /* 长度一队列只保留最新状态，不在原生 Wi-Fi 事件线程等待 owner。 */
    (void)xQueueOverwrite(s_wifi_event_queue, &event);
}

/**
 * @brief 逐步创建状态/提交锁、事件组、四深度命令队列和单值 Wi-Fi 队列并注册回调。
 * @return 成功为 ESP_OK，分配失败为 NO_MEM，其余为回调注册错误。
 * @note 唯一 lifecycle owner 串行初始化，失败保留已经创建的可重试前缀，也可在关闭入口后
 *       destroy。成功只建立资源，不启动任务，回调可能已经向稳定队列发布状态。
 */
esp_err_t network_service_init(void)
{
    if (!s_network_mutex) {
        s_network_mutex = xSemaphoreCreateMutex();
        if (!s_network_mutex) return ESP_ERR_NO_MEM;
    }
    if (!s_config_apply_mutex) {
        s_config_apply_mutex = xSemaphoreCreateMutex();
        if (!s_config_apply_mutex) return ESP_ERR_NO_MEM;
    }
    if (!s_network_events) {
        s_network_events = xEventGroupCreate();
        if (!s_network_events) return ESP_ERR_NO_MEM;
    }
    if (!s_network_queue) {
        s_network_queue = xQueueCreate(network_service_QUEUE_DEPTH, sizeof(network_job_t));
        if (!s_network_queue) {
            ESP_LOGE(TAG, "Network queue create failed");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_wifi_event_queue) {
        s_wifi_event_queue = xQueueCreate(1, sizeof(wifi_app_event_t));
        if (!s_wifi_event_queue) return ESP_ERR_NO_MEM;
    }
    if (!s_network_queue_set) {
        s_network_queue_set = xQueueCreateSet(network_service_QUEUE_DEPTH + 1U);
        if (!s_network_queue_set) return ESP_ERR_NO_MEM;
    }
    if (!s_command_queue_attached) {
        if (xQueueAddToSet(s_network_queue, s_network_queue_set) != pdPASS)
            return ESP_ERR_NO_MEM;
        s_command_queue_attached = true;
    }
    if (!s_wifi_queue_attached) {
        if (xQueueAddToSet(s_wifi_event_queue, s_network_queue_set) != pdPASS)
            return ESP_ERR_NO_MEM;
        s_wifi_queue_attached = true;
    }

    if (!s_event_registered) {
        esp_err_t err=wifi_app_set_event_callback(network_wifi_event_cb, NULL);
        if (err!=ESP_OK) return err;
        s_event_registered = true;
    }

    return ESP_OK;
}

/**
 * @brief 锁内设置后续 mDNS 应用使用的 HTTP 端口。
 * @param[in] port 广告端口，零归一为八十。
 * @return 未初始化为 INVALID_STATE，取得锁失败为 TIMEOUT，其余 ESP_OK。
 * @note 原生锁等待无独立预算，实际服务更新由下一次配置或连接事件触发。
 */
esp_err_t network_service_set_web_port(uint16_t port)
{
    if (!s_network_mutex) return ESP_ERR_INVALID_STATE;
    if (port == 0) port = 80;
    if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_web_port = port;
    xSemaphoreGive(s_network_mutex);
    return ESP_OK;
}

/**
 * @brief 在状态锁下复用活动 owner，或创建并发布一个新的内部栈任务。
 * @param[in] started 公开操作的原始 tick 起点。
 * @param[in] timeout 从该起点计算的有限预算。
 * @return 成功为 ESP_OK，缺资源/正在关闭/旧尾部未完成为 INVALID_STATE，等待或创建失败返回实际错误。
 * @note 创建输出写私有栈槽，持状态锁安装任务身份，旧完成 acquire 后才复用资源。
 */
static esp_err_t network_service_start_owner(TickType_t started, TickType_t timeout)
{
    if (!s_network_mutex || !s_network_events || !s_network_queue || !s_network_queue_set) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_network_mutex, network_stop_remaining(started, timeout)) != pdTRUE)
        return ESP_ERR_TIMEOUT;
    if (s_stopping) {
        xSemaphoreGive(s_network_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_network_task) {
        xSemaphoreGive(s_network_mutex);
        return ESP_OK;
    }
    if (!__atomic_load_n(&s_network_owner_exited, __ATOMIC_ACQUIRE)) {
        xSemaphoreGive(s_network_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(s_network_events, NETWORK_EVENT_STOPPED);
    __atomic_store_n(&s_network_owner_exited, false, __ATOMIC_RELEASE);
    TaskHandle_t created_task = NULL;
    BaseType_t ok = network_create_task(network_task,
                                        "network_service",
                                        network_service_TASK_STACK,
                                        NULL,
                                        network_service_TASK_PRIO,
                                        &created_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Network manager task create failed");
        s_network_task = NULL;
        __atomic_store_n(&s_network_owner_exited, true, __ATOMIC_RELEASE);
        xSemaphoreGive(s_network_mutex);
        return ESP_ERR_NO_MEM;
    }
    s_network_task = created_task;
    xSemaphoreGive(s_network_mutex);
    return ESP_OK;
}

/**
 * @brief 在原始预算内发布配置副本并等待对应 generation 的拥有结果。
 * @param[in] config 必填完整配置借用，发布时按值复制，此后调用者可修改原对象。
 * @param[in] started 本次公开操作的原生 tick 起点。
 * @param[in] wait_ticks 同一起点的有限相对预算，零仅检查可立即完成的步骤。
 * @return 参数/状态、等待、排队或 owner 应用结果。成功接纳后超时仍可迟到应用，
 *         结果存于服务字段，不保留调用者栈借用。
 * @note 配置提交锁串行等待者，状态锁同时覆盖准入和零等待队列发布，STOP 后拒绝新配置。
 */
static esp_err_t network_apply_config_until(const network_config_t *config,
                                           TickType_t started, TickType_t wait_ticks)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!memchr(config->mdns_hostname, '\0', sizeof(config->mdns_hostname)))
        return ESP_ERR_INVALID_ARG;
    if (!s_network_mutex || !s_config_apply_mutex ||
        !s_network_events || !s_network_queue) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_config_apply_mutex, network_stop_remaining(started, wait_ticks)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    uint64_t generation = 0;
    if (xSemaphoreTake(s_network_mutex, network_stop_remaining(started, wait_ticks)) != pdTRUE) {
        xSemaphoreGive(s_config_apply_mutex);
        return ESP_ERR_TIMEOUT;
    }
    if (!s_network_task || s_stopping) {
        xSemaphoreGive(s_network_mutex);
        xSemaphoreGive(s_config_apply_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    xEventGroupClearBits(s_network_events, NETWORK_EVENT_CONFIG_APPLIED);
    if (s_pending_config_dirty && s_config_coalesced < UINT32_MAX) s_config_coalesced++;
    s_pending_config = *config;
    s_pending_config_dirty = true;
    generation = ++s_next_config_generation;
    if (generation == 0) generation = ++s_next_config_generation;
    s_pending_config_generation = generation;
    if (!s_config_job_enqueued) {
        s_config_job_enqueued = true;
        network_job_t job = { .type = NETWORK_JOB_APPLY_CONFIG };
        if (xQueueSend(s_network_queue, &job, 0) != pdTRUE) {
            s_config_job_enqueued = false;
            s_pending_config_dirty = false;
            xSemaphoreGive(s_network_mutex);
            ESP_LOGW(TAG, "Network command queue full, WiFi config apply rejected");
            xSemaphoreGive(s_config_apply_mutex);
            return ESP_ERR_NO_MEM;
        }
        UBaseType_t queued = uxQueueMessagesWaiting(s_network_queue);
        if ((uint32_t)queued > s_command_high_water) s_command_high_water = (uint32_t)queued;
    }
    xSemaphoreGive(s_network_mutex);
    esp_err_t result = ESP_ERR_TIMEOUT;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_network_events,
                                               NETWORK_EVENT_CONFIG_APPLIED,
                                               pdTRUE,
                                               pdFALSE,
                                               network_stop_remaining(started, wait_ticks));
        if ((bits & NETWORK_EVENT_CONFIG_APPLIED) == 0) break;
        if (xSemaphoreTake(s_network_mutex, network_stop_remaining(started, wait_ticks)) != pdTRUE) break;
        bool matched = s_applied_config_generation == generation;
        if (matched) {
            result = s_applied_config_result;
        }
        xSemaphoreGive(s_network_mutex);
        if (matched) break;

        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= wait_ticks) break;
    }
    xSemaphoreGive(s_config_apply_mutex);
    return result;
}

/**
 * @brief 在已初始化服务上启动 owner，再以同一预算发布首次或后续配置。
 * @param[in] config 必填调用期稳定配置，服务按值复制。
 * @param[in] timeout_ms 整个启动与配置确认的毫秒预算，零仅作立即检查。
 * @return owner 创建或配置确认结果，超时可能已有配置接纳并在稍后应用。
 * @note 生命周期 owner 排除并发销毁，驱动调用由内部 RAM 栈的 network task 完成。
 */
esp_err_t network_service_start_from_config(const network_config_t *config, uint32_t timeout_ms)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = network_timeout_ticks(timeout_ms);
    esp_err_t err = network_service_start_owner(started, timeout);
    if (err != ESP_OK) return err;
    return network_apply_config_until(config, started, timeout);
}

/**
 * @brief 将完整配置快照交给运行中的网络 owner 并等待该代确认。
 * @param[in] config 必填稳定配置借用，接纳时复制，未持有其后续借用。
 * @param[in] timeout_ms 包含提交锁、状态锁及结果等待的总毫秒预算，零不阻塞。
 * @return 应用成功为 ESP_OK，参数/状态、排队和等待失败返回对应错误，应用失败为 ESP_FAIL。
 * @note 超时不会撤销已接纳配置，外层配置事务负责后续回滚或核对。
 */
esp_err_t network_service_apply_wifi_config(const network_config_t *config, uint32_t timeout_ms)
{
    return network_apply_config_until(config, xTaskGetTickCount(), network_timeout_ticks(timeout_ms));
}

/**
 * @brief 复制最后成功应用的配置，并在同一预算内重新启动 owner 和发布该配置。
 * @param[in] timeout_ms 取快照、启动和确认的总毫秒预算，零不阻塞。
 * @return 无已应用配置或资源状态不符为 INVALID_STATE，其余为锁、创建及应用结果。
 * @note 配置快照属于本次栈，内部提交立即复制，迟到处理不再借用该栈。
 */
esp_err_t network_service_resume(uint32_t timeout_ms)
{
    if (!s_network_mutex) return ESP_ERR_INVALID_STATE;
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = network_timeout_ticks(timeout_ms);
    network_config_t config;
    if (xSemaphoreTake(s_network_mutex, network_stop_remaining(started, timeout)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    bool have_config = s_last_config_valid;
    config = s_last_config;
    xSemaphoreGive(s_network_mutex);
    if (!have_config) return ESP_ERR_INVALID_STATE;
    esp_err_t err = network_service_start_owner(started, timeout);
    if (err != ESP_OK) return err;
    return network_apply_config_until(&config, started, timeout);
}

/**
 * @brief 共用一次预算提交或等待 STOP，并确认任务原语尾部已完全结束。
 * @param[in] reboot true 仅准备重启，false 执行可重试的 Wi-Fi 驱动关闭。
 * @param[in] timeout_ms 总毫秒预算，零兼容为五千毫秒，命令入队至多等待一百毫秒。
 * @return owner 已结束且最终完成位可见为 true，状态、原生关闭或等待失败为 false。
 * @pre 已通过进程期 stop 调用准入，动态资源的销毁等待该借用出栈。
 */
static bool network_service_request_stop_impl(bool reboot, uint32_t timeout_ms)
{
    if (!s_network_mutex) return s_network_task==NULL;
    TickType_t timeout=network_timeout_ticks(timeout_ms==0 ? 5000U : timeout_ms);
    TickType_t started=xTaskGetTickCount();
    if (xSemaphoreTake(s_network_mutex, timeout) != pdTRUE) return false;
    if (s_network_task && s_network_task == xTaskGetCurrentTaskHandle()) {
        xSemaphoreGive(s_network_mutex);
        return false;
    }
    if (!s_network_task) {
        xSemaphoreGive(s_network_mutex);
        while (!__atomic_load_n(&s_network_owner_exited, __ATOMIC_ACQUIRE)) {
            if (!network_stop_remaining(started, timeout)) return false;
            vTaskDelay(1);
        }
        return true;
    }
    if (!s_network_events || !s_network_queue) { xSemaphoreGive(s_network_mutex); return false; }
    /* 上次入队失败只发布恢复标志，重试在锁内重新开放一次 STOP 提交。 */
    if (__atomic_exchange_n(&s_stop_enqueue_failed,0,__ATOMIC_ACQ_REL)) s_stopping=false;
    bool enqueue=!s_stopping;
    if (enqueue) {
        s_stopping = true;
        s_stop_for_reboot = reboot;
        s_last_stop_result = ESP_OK;
        xEventGroupClearBits(s_network_events, NETWORK_EVENT_STOPPED);
    }
    xSemaphoreGive(s_network_mutex);
    if (enqueue) {
        network_job_t job = { .type = NETWORK_JOB_STOP };
        TickType_t wait=network_stop_remaining(started,timeout);
        TickType_t max_enqueue=network_timeout_ticks(100);
        if (wait>max_enqueue) wait=max_enqueue;
        if (xQueueSend(s_network_queue, &job, wait) != pdTRUE) {
            __atomic_store_n(&s_stop_enqueue_failed,1,__ATOMIC_RELEASE);
            return false;
        }
    }
    EventBits_t bits = xEventGroupWaitBits(s_network_events, NETWORK_EVENT_STOPPED,
                                            pdFALSE, pdTRUE, network_stop_remaining(started,timeout));
    if ((bits & NETWORK_EVENT_STOPPED)==0 ||
        xSemaphoreTake(s_network_mutex,network_stop_remaining(started,timeout))!=pdTRUE) return false;
    bool stopped=s_network_task==NULL;
    xSemaphoreGive(s_network_mutex);
    while (stopped && !__atomic_load_n(&s_network_owner_exited, __ATOMIC_ACQUIRE)) {
        if (!network_stop_remaining(started, timeout)) return false;
        vTaskDelay(1);
    }
    return stopped;
}

/**
 * @brief 在进程期门锁中登记 stop 调用，动态访问全部返回后再归还计数。
 * @param[in] reboot 本次停止模式，已有 STOP 时复用先前请求的模式。
 * @param[in] timeout_ms 转交给停止实现的毫秒预算。
 * @param[out] out_error 可空原生诊断槽，先置 TIMEOUT，停止成功置 ESP_OK，失败时在借用内尽力复制关闭错误。
 * @return 停止结果，destroy 已在成功停机后关闭准入时直接返回 true，计数饱和为 false。
 * @note 计数归还发生在所有动态锁/事件调用完整返回后，销毁等待该计数归零。
 */
static bool network_service_request_stop(bool reboot, uint32_t timeout_ms, esp_err_t *out_error)
{
    if (out_error) *out_error = ESP_ERR_TIMEOUT;
    portENTER_CRITICAL(&s_stop_gate_lock);
    bool closing=s_stop_gate_closing;
    bool admitted=!s_stop_gate_closing && s_stop_callers<UINT_MAX;
    if (admitted) s_stop_callers++;
    portEXIT_CRITICAL(&s_stop_gate_lock);
    /* owner 已完成后才关闭准入，销毁重试期间的 stop 不再触碰动态资源。 */
    if (!admitted) {
        if (closing && out_error) *out_error = ESP_OK;
        return closing;
    }
    bool stopped=network_service_request_stop_impl(reboot,timeout_ms);
    if (out_error) {
        if (stopped) *out_error = ESP_OK;
        else if (s_network_mutex && xSemaphoreTake(s_network_mutex, 0) == pdTRUE) {
            if (s_last_stop_result != ESP_OK) *out_error = s_last_stop_result;
            xSemaphoreGive(s_network_mutex);
        }
    }
    portENTER_CRITICAL(&s_stop_gate_lock);
    s_stop_callers--;
    portEXIT_CRITICAL(&s_stop_gate_lock);
    return stopped;
}

/**
 * @brief 关闭 stop 准入并等待已经登记的调用全部离开动态资源。
 * @param[in] started destroy 的原始 tick 起点。
 * @param[in] timeout 同一起点的有限预算，零仅检查。
 * @return 已排空为 true，预算耗尽为 false，超时仍保持准入关闭供 destroy 重试。
 * @pre lifecycle owner 排空外部 start/apply/status 等借用并串行销毁操作。
 */
static bool network_service_drain_stop_callers(TickType_t started, TickType_t timeout)
{
    portENTER_CRITICAL(&s_stop_gate_lock);
    s_stop_gate_closing=true;
    portEXIT_CRITICAL(&s_stop_gate_lock);
    for (;;) {
        bool expired=network_stop_remaining(started, timeout)==0;
        portENTER_CRITICAL(&s_stop_gate_lock);
        bool drained=s_stop_callers==0;
        portEXIT_CRITICAL(&s_stop_gate_lock);
        if (drained) return true;
        if (expired) return false;
        vTaskDelay(1);
    }
}

/**
 * @brief 请求可恢复网络关闭并等待已接纳任务与通知尾部退出。
 * @param[in] timeout_ms 总毫秒预算，零采用五千毫秒。
 * @return 成功停机为 true，等待、队列、驱动关闭或自等待失败为 false，资源保留供重试。
 */
bool network_service_stop(uint32_t timeout_ms)
{
    return network_service_request_stop(false, timeout_ms, NULL);
}

/**
 * @brief 在 owner 已退出后关闭 stop 准入、排空回调并释放本服务动态 RTOS 资源。
 * @param[in] timeout_ms 状态检查、stop 调用排空与回调排空共用的毫秒预算，零仅检查。
 * @return 成功为 ESP_OK，owner 活动或队列成员移除失败为 INVALID_STATE，尾部/等待未完成为 TIMEOUT，
 *         其余为回调注销错误。
 * @pre lifecycle owner 先停止服务、关闭其他 API 入口并排空 start/apply/status/web-port 借用，
 *      销毁与重建串行。已经登记的 stop 调用与回调由本函数排空。
 * @note 失败保留尚未释放资源及关闭状态可重试，队列事件丢弃后再移除成员。
 *       Wi-Fi 的进程期 netif/handler/timer 由其模块保管。
 */
esp_err_t network_service_destroy(uint32_t timeout_ms)
{
    TickType_t started = xTaskGetTickCount();
    TickType_t timeout = network_timeout_ticks(timeout_ms);
    if (s_network_mutex) {
        if (xSemaphoreTake(s_network_mutex,network_stop_remaining(started, timeout))!=pdTRUE) return ESP_ERR_TIMEOUT;
        bool running=s_network_task!=NULL;
        xSemaphoreGive(s_network_mutex);
        if (running) return ESP_ERR_INVALID_STATE;
    } else if (s_network_task) return ESP_ERR_INVALID_STATE;
    if (!__atomic_load_n(&s_network_owner_exited, __ATOMIC_ACQUIRE)) return ESP_ERR_TIMEOUT;
    if (!network_service_drain_stop_callers(started, timeout)) return ESP_ERR_TIMEOUT;
    if (s_event_registered) {
        esp_err_t err=wifi_app_clear_event_callback(network_remaining_milliseconds(started, timeout));
        if (err!=ESP_OK) return err;
        s_event_registered = false;
    }
    if (s_network_queue_set) {
        /* owner 与回调均已退出，丢弃末次通知后才满足原生移除成员的空队列前提。 */
        network_job_t discarded_job;
        wifi_app_event_t discarded_wifi;
        while (xQueueReceive(s_network_queue, &discarded_job, 0) == pdTRUE) {}
        while (xQueueReceive(s_wifi_event_queue, &discarded_wifi, 0) == pdTRUE) {}
        if (s_command_queue_attached) {
            if (xQueueRemoveFromSet(s_network_queue, s_network_queue_set) != pdPASS)
                return ESP_ERR_INVALID_STATE;
            s_command_queue_attached = false;
        }
        if (s_wifi_queue_attached) {
            if (xQueueRemoveFromSet(s_wifi_event_queue, s_network_queue_set) != pdPASS)
                return ESP_ERR_INVALID_STATE;
            s_wifi_queue_attached = false;
        }
        vQueueDelete(s_network_queue_set);
        s_network_queue_set = NULL;
    }
    if (s_network_queue) {
        vQueueDelete(s_network_queue);
        s_network_queue = NULL;
    }
    if (s_wifi_event_queue) {
        vQueueDelete(s_wifi_event_queue);
        s_wifi_event_queue = NULL;
    }
    if (s_network_events) {
        vEventGroupDelete(s_network_events);
        s_network_events = NULL;
    }
    if (s_config_apply_mutex) {
        vSemaphoreDelete(s_config_apply_mutex);
        s_config_apply_mutex = NULL;
    }
    if (s_network_mutex) {
        vSemaphoreDelete(s_network_mutex);
        s_network_mutex = NULL;
    }
    s_last_config_valid = false;
    s_pending_config_dirty = false;
    s_config_job_enqueued = false;
    s_stopping = false;
    __atomic_store_n(&s_stop_enqueue_failed,0,__ATOMIC_RELEASE);
    s_next_config_generation = 0;
    s_pending_config_generation = 0;
    s_applied_config_generation = 0;
    s_applied_config_result = ESP_OK;
    s_last_stop_result = ESP_OK;
    portENTER_CRITICAL(&s_stop_gate_lock);
    s_stop_gate_closing=false;
    portEXIT_CRITICAL(&s_stop_gate_lock);
    return ESP_OK;
}

/**
 * @brief 提交重启模式的 STOP 并确认 owner 已结束板级访问。
 * @param[in] timeout_ms 毫秒预算，零采用五千毫秒。
 * @return 成功 true，等待或状态失败 false。Wi-Fi 只关闭保活，设备实际重启由外层执行。
 */
bool network_service_prepare_for_reboot(uint32_t timeout_ms)
{
    if (!network_service_request_stop(true, timeout_ms, NULL)) {
        ESP_LOGW(TAG, "Network owner stop timed out before reboot");
        return false;
    }
    return true;
}

/**
 * @brief 以五秒等待窗口请求网络及 Wi-Fi 驱动关闭供 OTA 使用。
 * @return 成功为 ESP_OK，已记录的原生关闭错误优先返回，其余未完成为 TIMEOUT。
 * @note 失败 owner 仍存活并可重试 STOP，已经完成的 mDNS/HTTP 清理保持，资源不强制回收。
 */
esp_err_t network_service_shutdown_for_ota(void)
{
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (network_service_request_stop(false, 5000U, &result)) return ESP_OK;
    return result;
}

/**
 * @brief 合并 Wi-Fi 值快照与本服务状态锁保护的统计。
 * @param[out] out_status 可空输出，非空先取得 Wi-Fi 快照再清零填充整体记录。
 * @post 无动态释放责任，状态锁五十毫秒超时时保留 Wi-Fi 和当前日历判断，其他网络字段为零。
 * @note 两层快照不保证同一时刻，调用者与最终 destroy 排空交接由外层协调。
 */
void network_service_get_status(network_service_status_t *out_status)
{
    if (!out_status) {
        return;
    }

    wifi_app_status_t wifi;
    wifi_app_get_status(&wifi);

    memset(out_status, 0, sizeof(*out_status));
    out_status->enabled = wifi.enabled;
    out_status->started = wifi.started;
    out_status->connected = wifi.connected;
    out_status->ap_enabled = wifi.ap_enabled;
    out_status->ap_started = wifi.ap_started;
    out_status->keepalive = wifi.keepalive;
    out_status->retry_count = wifi.retry_count;
    out_status->max_retry = wifi.max_retry;
    out_status->last_disconnect_reason = wifi.last_disconnect_reason;
    snprintf(out_status->ssid, sizeof(out_status->ssid), "%s", wifi.ssid);
    snprintf(out_status->ap_ssid, sizeof(out_status->ap_ssid), "%s", wifi.ap_ssid);
    snprintf(out_status->sta_ip, sizeof(out_status->sta_ip), "%s", wifi.sta_ip);
    snprintf(out_status->ap_ip, sizeof(out_status->ap_ip), "%s", wifi.ap_ip);
    if (s_network_mutex && xSemaphoreTake(s_network_mutex, network_timeout_ticks(50)) == pdTRUE) {
        out_status->mdns_enabled = s_mdns_enabled;
        out_status->time_synced = s_time_synced || system_time_is_valid();
        snprintf(out_status->mdns_hostname, sizeof(out_status->mdns_hostname), "%s",
                 s_mdns_hostname);
        out_status->command_high_water = s_command_high_water;
        out_status->wifi_event_coalesced = s_wifi_event_coalesced;
        out_status->config_coalesced = s_config_coalesced;
        out_status->owner_heartbeat_ms = s_owner_heartbeat_ms;
        out_status->stopping = s_stopping;
        xSemaphoreGive(s_network_mutex);
    } else {
        /* 锁竞争时返回保守快照，不从 owner 正在修改的字段做无锁读取。 */
        out_status->time_synced = system_time_is_valid();
    }
}
