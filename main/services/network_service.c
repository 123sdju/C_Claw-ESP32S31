#include "network_service.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_event_bus.h"
#include "runtime_service.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "wifi_app.h"

#define network_service_TASK_STACK 4096U
#define network_service_TASK_PRIO 5
#define network_service_QUEUE_DEPTH 4U

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define network_service_TASK_STACK_IN_PSRAM 1
#else
#define network_service_TASK_STACK_IN_PSRAM 0
#endif

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
static SemaphoreHandle_t s_network_mutex;
static SemaphoreHandle_t s_config_apply_mutex;
static EventGroupHandle_t s_network_events;
static TaskHandle_t s_network_task;
static bool s_event_registered;
static bool s_sntp_ready;
static bool s_time_synced;
static bool s_mdns_started;
static bool s_mdns_enabled;
static bool s_mdns_http_service_added;
static bool s_mdns_ready_logged;
static char s_mdns_hostname[32] = "cclaw";
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
static uint32_t s_command_high_water;
static uint32_t s_wifi_event_coalesced;
static uint32_t s_config_coalesced;
static uint64_t s_owner_heartbeat_ms;

#define NETWORK_EVENT_STOPPED       (1U << 0)
#define NETWORK_EVENT_CONFIG_APPLIED (1U << 1)

static void network_set_time_synced(bool synced)
{
    if (s_network_mutex && xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
        s_time_synced = synced;
        xSemaphoreGive(s_network_mutex);
    }
}

/*
 * 检查系统时间是否有效（是否晚于 2024-01-01 时间戳 1704067200），
 * 用于判断 TLS 证书验证所需的系统时间是否已同步。
 * 返回: true 时间有效
 */
static bool system_time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return now >= 1704067200; /* 2024-01-01, enough for public TLS cert validation */
}

/*
 * 创建 FreeRTOS 任务，优先分配 PSRAM 堆栈，失败则退回内部 RAM。
 * 返回: pdPASS 成功
 */
static BaseType_t network_create_task(TaskFunction_t task,
                                      const char *name,
                                      uint32_t stack_bytes,
                                      void *arg,
                                      UBaseType_t priority,
                                      TaskHandle_t *out_handle)
{
#if network_service_TASK_STACK_IN_PSRAM
    BaseType_t ok = xTaskCreateWithCaps(task,
                                        name,
                                        stack_bytes,
                                        arg,
                                        priority,
                                        out_handle,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) return ok;

    ESP_LOGW(TAG, "PSRAM stack allocation failed for %s, retrying internal", name);
    return xTaskCreateWithCaps(task,
                               name,
                               stack_bytes,
                               arg,
                               priority,
                               out_handle,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(task, name, stack_bytes, arg, priority, out_handle);
#endif
}

/*
 * WiFi 连接后同步 SNTP 时间（使用 ntp.aliyun.com，15 秒超时）。
 * 如果时间已有效则直接标记 s_time_synced。
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
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
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

/*
 * 重置所有 HTTP 持久连接（WIFI 断开或重启时调用），避免后续 TLS 建立失败。
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

/*
 * 安全复制文本到固定大小缓冲区，自动截断并保证 '\0' 结尾。
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

/*
 * 应用 mDNS 配置：启用/禁用 mDNS、设置主机名/实例名、注册 HTTP 服务。
 */
static void network_apply_mdns_config(const network_config_t *config)
{
    if (!config) {
        return;
    }
    if (s_network_mutex && xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
        s_mdns_enabled = config->mdns_enabled;
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
        err = mdns_service_add("C-Claw Web Console", "_http", "_tcp", 80, NULL, 0);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_mdns_http_service_added = true;
        } else {
            ESP_LOGW(TAG, "mDNS http service add failed: %s", esp_err_to_name(err));
        }
    }
    if (!s_mdns_ready_logged) {
        ESP_LOGI(TAG, "mDNS ready: http://%s.local/", s_mdns_hostname);
        s_mdns_ready_logged = true;
    }
}

/*
 * 处理 WIFI 事件：连接事件触发 mDNS 配置和时间同步，断开事件重置 HTTP 连接。
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

/*
 * 网络任务主函数：从队列接收 WIFI 事件或配置变更，分发到对应处理函数。
 */
static void network_task(void *arg)
{
    (void)arg;

    for (;;) {
        QueueSetMemberHandle_t ready = xQueueSelectFromSet(s_network_queue_set, portMAX_DELAY);
        if (s_network_mutex && xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
            s_owner_heartbeat_ms = (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
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
                    ESP_LOGI(TAG, "WiFi config apply %s%s%s",
                             ok ? "ok" : "failed",
                             config.ssid[0] ? ", ssid=" : "",
                             config.ssid[0] ? config.ssid : "");
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
            if (reboot) wifi_app_prepare_for_reboot();
            else (void)wifi_app_shutdown_for_ota();
            if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
                s_network_task = NULL;
                s_stopping = false;
                s_config_job_enqueued = false;
                xSemaphoreGive(s_network_mutex);
            }
            xEventGroupSetBits(s_network_events, NETWORK_EVENT_STOPPED);
            vTaskDelete(NULL);
        }
    }
}

/*
 * 注册 WiFi 事件回调，将事件通过队列转发给 network_task。
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
        s_wifi_event_coalesced++;
        xSemaphoreGive(s_network_mutex);
    }
    /* Length-one latest-value queue: state notifications are coalesced, never silently dropped. */
    (void)xQueueOverwrite(s_wifi_event_queue, &event);
}

/*
 * 初始化一次性的网络资源（队列、任务、事件回调注册）。
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
        bool command_added = s_network_queue_set &&
            xQueueAddToSet(s_network_queue, s_network_queue_set) == pdPASS;
        bool wifi_added = command_added &&
            xQueueAddToSet(s_wifi_event_queue, s_network_queue_set) == pdPASS;
        if (!wifi_added) {
            ESP_LOGE(TAG, "Network queue set create failed");
            if (command_added) {
                (void)xQueueRemoveFromSet(s_network_queue, s_network_queue_set);
            }
            if (s_network_queue_set) {
                vQueueDelete(s_network_queue_set);
                s_network_queue_set = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_event_registered) {
        wifi_app_set_event_callback(network_wifi_event_cb, NULL);
        s_event_registered = true;
    }

    return ESP_OK;
}

static esp_err_t network_service_start_owner(void)
{
    if (!s_network_mutex || !s_network_events || !s_network_queue || !s_network_queue_set) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_network_task) return ESP_OK;
    xEventGroupClearBits(s_network_events, NETWORK_EVENT_STOPPED);
    BaseType_t ok = network_create_task(network_task,
                                        "network_service",
                                        network_service_TASK_STACK,
                                        NULL,
                                        network_service_TASK_PRIO,
                                        &s_network_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Network manager task create failed");
        s_network_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/*
 * 入口一：同步启动网络（首次初始化），直接调用 wifi_app_connect_from_config。
 */
esp_err_t network_service_start_from_config(const network_config_t *config, uint32_t timeout_ms)
{
    esp_err_t err = network_service_start_owner();
    if (err != ESP_OK) return err;
    return network_service_apply_wifi_config(config, timeout_ms);
}

/*
 * 入口二：提交 latest-value 配置，并等待 owner task 确认对应 generation。
 */
esp_err_t network_service_apply_wifi_config(const network_config_t *config, uint32_t timeout_ms)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (!s_network_task || !s_network_mutex || !s_config_apply_mutex ||
        !s_network_events || !s_network_queue) return ESP_ERR_INVALID_STATE;
    TickType_t wait_ticks = pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_config_apply_mutex, wait_ticks) != pdTRUE) return ESP_ERR_TIMEOUT;

    bool enqueue = false;
    uint64_t generation = 0;
    xEventGroupClearBits(s_network_events, NETWORK_EVENT_CONFIG_APPLIED);
    if (xSemaphoreTake(s_network_mutex, wait_ticks) != pdTRUE) {
        xSemaphoreGive(s_config_apply_mutex);
        return ESP_ERR_TIMEOUT;
    }
    s_network_quiet_shutdown = false;
    if (s_pending_config_dirty) s_config_coalesced++;
    s_pending_config = *config;
    s_pending_config_dirty = true;
    generation = ++s_next_config_generation;
    if (generation == 0) generation = ++s_next_config_generation;
    s_pending_config_generation = generation;
    if (!s_config_job_enqueued) {
        s_config_job_enqueued = true;
        enqueue = true;
    }
    xSemaphoreGive(s_network_mutex);

    if (enqueue) {
        network_job_t job = { .type = NETWORK_JOB_APPLY_CONFIG };
        if (xQueueSend(s_network_queue, &job, 0) != pdTRUE) {
            if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
                s_config_job_enqueued = false;
                xSemaphoreGive(s_network_mutex);
            }
            ESP_LOGW(TAG, "Network command queue full, WiFi config apply rejected");
            xSemaphoreGive(s_config_apply_mutex);
            return ESP_ERR_NO_MEM;
        }
        UBaseType_t queued = uxQueueMessagesWaiting(s_network_queue);
        if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
            if ((uint32_t)queued > s_command_high_water) s_command_high_water = (uint32_t)queued;
            xSemaphoreGive(s_network_mutex);
        }
    }
    esp_err_t result = ESP_ERR_TIMEOUT;
    TickType_t started = xTaskGetTickCount();
    TickType_t remaining = wait_ticks;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_network_events,
                                               NETWORK_EVENT_CONFIG_APPLIED,
                                               pdTRUE,
                                               pdFALSE,
                                               remaining);
        if ((bits & NETWORK_EVENT_CONFIG_APPLIED) == 0) break;
        if (xSemaphoreTake(s_network_mutex, remaining) != pdTRUE) break;
        bool matched = s_applied_config_generation == generation;
        if (matched) {
            result = s_applied_config_result;
        }
        xSemaphoreGive(s_network_mutex);
        if (matched) break;

        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= wait_ticks) break;
        remaining = wait_ticks - elapsed;
    }
    xSemaphoreGive(s_config_apply_mutex);
    return result;
}

esp_err_t network_service_resume(uint32_t timeout_ms)
{
    if (!s_network_mutex) return ESP_ERR_INVALID_STATE;
    network_config_t config;
    if (xSemaphoreTake(s_network_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    bool have_config = s_last_config_valid;
    config = s_last_config;
    xSemaphoreGive(s_network_mutex);
    if (!have_config) return ESP_ERR_INVALID_STATE;
    esp_err_t err = network_service_start_owner();
    if (err != ESP_OK) return err;
    return network_service_apply_wifi_config(&config, timeout_ms);
}

static bool network_service_request_stop(bool reboot, uint32_t timeout_ms)
{
    if (!s_network_task) return true;
    if (!s_network_mutex || !s_network_events || !s_network_queue) return false;
    if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) != pdTRUE) return false;
    if (s_stopping) {
        xSemaphoreGive(s_network_mutex);
    } else {
        s_stopping = true;
        s_stop_for_reboot = reboot;
        xSemaphoreGive(s_network_mutex);
        xEventGroupClearBits(s_network_events, NETWORK_EVENT_STOPPED);
        network_job_t job = { .type = NETWORK_JOB_STOP };
        if (xQueueSend(s_network_queue, &job, pdMS_TO_TICKS(100)) != pdTRUE) {
            if (xSemaphoreTake(s_network_mutex, portMAX_DELAY) == pdTRUE) {
                s_stopping = false;
                xSemaphoreGive(s_network_mutex);
            }
            return false;
        }
    }
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms == 0 ? 5000U : timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_network_events, NETWORK_EVENT_STOPPED,
                                            pdFALSE, pdTRUE, ticks);
    return (bits & NETWORK_EVENT_STOPPED) != 0;
}

bool network_service_stop(uint32_t timeout_ms)
{
    return network_service_request_stop(false, timeout_ms);
}

esp_err_t network_service_destroy(void)
{
    if (s_network_task) return ESP_ERR_INVALID_STATE;
    if (s_event_registered) {
        wifi_app_set_event_callback(NULL, NULL);
        s_event_registered = false;
    }
    if (s_network_queue_set) {
        if (s_network_queue) (void)xQueueRemoveFromSet(s_network_queue, s_network_queue_set);
        if (s_wifi_event_queue) (void)xQueueRemoveFromSet(s_wifi_event_queue, s_network_queue_set);
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
    s_next_config_generation = 0;
    s_pending_config_generation = 0;
    s_applied_config_generation = 0;
    s_applied_config_result = ESP_OK;
    return ESP_OK;
}

/*
 * 重启前网络准备：标记静默关闭、停止 mDNS、重置 HTTP 连接和时间同步标志。
 */
void network_service_prepare_for_reboot(void)
{
    if (!network_service_request_stop(true, 5000U)) {
        ESP_LOGW(TAG, "Network owner stop timed out before reboot");
    }
}

/*
 * 为 OTA 关闭所有网络服务：停止 mDNS、重置 HTTP 连接、调用 wifi_app_shutdown_for_ota。
 */
esp_err_t network_service_shutdown_for_ota(void)
{
    return network_service_request_stop(false, 5000U) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/*
 * 获取网络服务综合状态快照：合并 wifi_app 和本模块的 mDNS/SNTP 状态。
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
    if (s_network_mutex && xSemaphoreTake(s_network_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
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
