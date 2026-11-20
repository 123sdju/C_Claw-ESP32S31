#include "wifi_app.h"
#include "wifi_event_callback.h"

#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_memory_utils.h"
#include "cc/ports/cc_thread.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_DEFAULT_CONNECT_TIMEOUT_MS 20000
#define WIFI_DEFAULT_MAX_RETRY 5
#define WIFI_RECONNECT_BASE_MS 1000U
#define WIFI_RECONNECT_MAX_MS 30000U
#define WIFI_DEFAULT_AP_CHANNEL 6
#define WIFI_DEFAULT_AP_MAX_CONN 4
#define WIFI_AP_DEFAULT_IP "192.168.4.1"
#define WIFI_AP_DEFAULT_GW "192.168.4.1"
#define WIFI_AP_DEFAULT_NETMASK "255.255.255.0"

static const char *TAG = "wifi_app";
static portMUX_TYPE s_wifi_state_lock = portMUX_INITIALIZER_UNLOCKED;
/* 操作锁持续到进程结束，timer 只尝试立即取得，避免阻塞共享 timer daemon。 */
static cc_mutex_t s_wifi_operation_mutex;
static TaskHandle_t s_wifi_operation_owner;
/** @brief 当前驱动序列中的延后通知，每条独立保管按值状态快照。 */
typedef struct {
    wifi_app_event_t event;
    wifi_app_status_t status;
} wifi_deferred_notification_t;
/* 当前序列至多先报告已有连接，再报告超时后的重连，两条保留原顺序。 */
static wifi_deferred_notification_t s_wifi_operation_notifications[2];
static size_t s_wifi_operation_notification_count;
static bool s_wifi_event_registered;
static bool s_wifi_ip_registered;

static EventGroupHandle_t s_wifi_events;
static TimerHandle_t s_wifi_reconnect_timer;
static network_config_t s_wifi_config;
static esp_netif_t *s_wifi_sta_netif;
static esp_netif_t *s_wifi_ap_netif;
static bool s_wifi_ready;
/* netif、事件注册、定时器和同步状态跨 OTA 保留，Wi-Fi driver 关闭后重新初始化。 */
static bool s_wifi_driver_initialized;
static bool s_wifi_started;
static bool s_wifi_ap_started;
static bool s_wifi_config_valid;
static bool s_wifi_connected;
static bool s_wifi_keepalive;
static bool s_wifi_applying_config;
static int s_wifi_retry_count;
static uint16_t s_wifi_last_disconnect_reason;
static char s_wifi_ap_ssid[33];
static char s_wifi_sta_ip[16];
static char s_wifi_ap_ip[16] = WIFI_AP_DEFAULT_IP;

/**
 * @brief 创建进程期驱动操作锁并取得一次持有，timer 使用零等待。
 * @param[in] wait true 允许 owner 等待，false 只尝试立即取得。
 * @return 成功持锁为 true，初始化或取得失败为 false，错误诊断内部消费。
 * @note 当前 ESP32 thread 端口的 mutex 句柄是原生 SemaphoreHandle_t，借此使用零等待
 *       原语。锁永不删除，状态 portMUX 内不取得该锁，也不执行外部 API。
 */
static bool wifi_operation_enter(bool wait)
{
    cc_result_t rc = cc_mutex_init_once(&s_wifi_operation_mutex);
    bool initialized = rc.code == CC_OK;
    if (!initialized) ESP_LOGE(TAG, "WiFi operation mutex init failed");
    cc_result_free(&rc);
    if (!initialized || xSemaphoreTake((SemaphoreHandle_t)s_wifi_operation_mutex,
                                       wait ? portMAX_DELAY : 0) != pdTRUE) return false;
    portENTER_CRITICAL(&s_wifi_state_lock);
    s_wifi_operation_owner = xTaskGetCurrentTaskHandle();
    s_wifi_operation_notification_count = 0;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    return true;
}

/** @brief 归还进程期驱动操作锁。 @pre 本线程已经通过 wifi_operation_enter 持有锁。
 * @post 原生 Give 完全返回后依次派发本次操作保存的通知快照，回调可重入驱动入口。
 *       锁本身继续存活，其他操作可在通知派发前进入。
 */
static void wifi_operation_leave(void)
{
    portENTER_CRITICAL(&s_wifi_state_lock);
    size_t count = s_wifi_operation_notification_count;
    wifi_deferred_notification_t notifications[2];
    memcpy(notifications, s_wifi_operation_notifications, count * sizeof(*notifications));
    s_wifi_operation_notification_count = 0;
    s_wifi_operation_owner = NULL;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    xSemaphoreGive((SemaphoreHandle_t)s_wifi_operation_mutex);
    for (size_t i = 0; i < count; i++)
        wifi_event_callback_dispatch(notifications[i].event, &notifications[i].status);
}

/**
 * @brief 将毫秒向上换算为有限 tick，避免乘法回绕和正等待被截成零。
 * @param[in] milliseconds 毫秒数，零保持为零。
 * @return 正值至少一个 tick，最大 portMAX_DELAY-1。
 */
static TickType_t wifi_timeout_ticks(uint32_t milliseconds)
{
    uint64_t ticks = ((uint64_t)milliseconds * configTICK_RATE_HZ + 999U) / 1000U;
    if (ticks >= (uint64_t)portMAX_DELAY) ticks = (uint64_t)portMAX_DELAY - 1U;
    return (TickType_t)ticks;
}

/**
 * @brief 在短状态锁内取得当前配置的重试阈值。
 * @return 正配置原值，非正值采用默认五次，返回快照不持有资源。
 */
static int wifi_config_max_retry(void)
{
    portENTER_CRITICAL(&s_wifi_state_lock);
    int retries = s_wifi_config.max_retry;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    return retries > 0 ? retries : WIFI_DEFAULT_MAX_RETRY;
}

/**
 * @brief 将应用重试阈值限制到原生八位字段的表示范围。
 * @return 当前有效阈值与 UINT8_MAX 的较小值，无拥有资源。
 */
static uint8_t wifi_config_driver_retry_count(void)
{
    int retry = wifi_config_max_retry();
    if (retry > UINT8_MAX) {
        retry = UINT8_MAX;
    }
    return (uint8_t)retry;
}

/**
 * @brief 完整复制 NUL 文本，容量不足时交付空串。
 * @param[out] dst 实际可写目标，NULL 时返回 false。
 * @param[in] dst_size 目标字节容量，零时不写。
 * @param[in] src 调用期稳定文本借用，NULL 按空串，须与目标不重叠。
 * @return 完整复制含终止符为 true，参数无效或容不下为 false，此时有效目标首字节清零。
 */
static bool copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return false;
    }
    const char *text = src ? src : "";
    size_t len = strlen(text);
    if (len >= dst_size) {
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, text, len + 1);
    return true;
}

/**
 * @brief 将借用 IPv4 地址格式化到固定缓冲。
 * @param[out] dst 可写目标，NULL 时跳过。
 * @param[in] dst_size 实际字节容量，零时跳过，较小容量可能产生截断文本。
 * @param[in] ip 同步稳定地址借用，NULL 使有效目标为空。
 * @post 非空且有容量的目标以 NUL 结束，无分配。
 */
static void copy_ip(char *dst, size_t dst_size, const esp_ip4_addr_t *ip)
{
    if (!dst || dst_size == 0) {
        return;
    }
    if (!ip) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, IPSTR, IP2STR(ip));
}

/**
 * @brief 使用 AP MAC 后三字节生成默认 SSID，查询失败回退 C-Claw。
 * @param[out] out 可写目标，NULL 时跳过。
 * @param[in] out_size 实际字节容量，零时跳过。
 * @post 有效目标获得终止文本，MAC 分支可能截断，短缓冲的固定回退可能为空串。
 * @pre driver 已初始化，调用者持操作锁，正常私有调用提供三十三字节目标。
 */
static void wifi_default_ap_ssid(char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    uint8_t mac[6] = {0};
    if (esp_wifi_get_mac(WIFI_IF_AP, mac) == ESP_OK) {
        snprintf(out, out_size, "C-Claw-%02X%02X%02X", mac[3], mac[4], mac[5]);
    } else {
        copy_text(out, out_size, "C-Claw");
    }
}

/**
 * @brief 在状态锁内核对已安装缓存的 SoftAP 参数是否相同。
 * @param[in] config 可空稳定候选，其字符串已在所属数组内终止。
 * @return 缓存有效且开关、SSID、密码、信道和连接数相等为 true，其他 false。
 */
static bool wifi_softap_config_matches(const network_config_t *config)
{
    if (!config) return false;
    portENTER_CRITICAL(&s_wifi_state_lock);
    bool matches = s_wifi_config_valid && s_wifi_config.softap_enabled == config->softap_enabled &&
           strcmp(s_wifi_config.softap_ssid, config->softap_ssid) == 0 &&
           strcmp(s_wifi_config.softap_password, config->softap_password) == 0 &&
           s_wifi_config.softap_channel == config->softap_channel &&
           s_wifi_config.softap_max_connections == config->softap_max_connections;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    return matches;
}

/**
 * @brief 按两个配置开关选择 STA、AP、APSTA 或关闭模式。
 * @param[in] config 稳定配置借用，可为 NULL，此时选择 WIFI_MODE_NULL。
 * @return 对应原生模式值，无共享状态访问或资源分配。
 */
static wifi_mode_t wifi_mode_for_config(const network_config_t *config)
{
    bool sta = config && config->enabled;
    bool ap = config && config->softap_enabled;
    if (sta && ap) {
        return WIFI_MODE_APSTA;
    }
    if (sta) {
        return WIFI_MODE_STA;
    }
    if (ap) {
        return WIFI_MODE_AP;
    }
    return WIFI_MODE_NULL;
}

/**
 * @brief 在同一次短临界区复制应用配置、异步连接状态及地址。
 * @param[out] out_status 可空值输出，非空先清零再写完整快照。
 * @post 输出不借用内部字符串，无动态释放责任，锁返回前已经归还。
 * @note 该快照表达应用跟踪状态，原生事件与驱动调用的物理时刻可能不同。
 */
static void wifi_fill_status(wifi_app_status_t *out_status)
{
    if (!out_status) {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    portENTER_CRITICAL(&s_wifi_state_lock);
    out_status->enabled = s_wifi_config_valid && s_wifi_config.enabled;
    out_status->started = s_wifi_started;
    out_status->connected = s_wifi_connected;
    out_status->ap_enabled = s_wifi_config_valid && s_wifi_config.softap_enabled;
    out_status->ap_started = s_wifi_ap_started;
    out_status->keepalive = s_wifi_keepalive;
    out_status->retry_count = s_wifi_retry_count;
    out_status->max_retry = s_wifi_config.max_retry > 0 ? s_wifi_config.max_retry : WIFI_DEFAULT_MAX_RETRY;
    out_status->last_disconnect_reason = s_wifi_last_disconnect_reason;
    memcpy(out_status->ssid, s_wifi_config.ssid, sizeof(out_status->ssid));
    memcpy(out_status->ap_ssid, s_wifi_ap_ssid, sizeof(out_status->ap_ssid));
    memcpy(out_status->sta_ip, s_wifi_sta_ip, sizeof(out_status->sta_ip));
    memcpy(out_status->ap_ip, s_wifi_ap_ip, sizeof(out_status->ap_ip));
    portEXIT_CRITICAL(&s_wifi_state_lock);
}

/**
 * @brief 取独立状态快照，锁外通知或延后到本次驱动操作锁退出后通知。
 * @param[in] event 按值交付的连接、断开、重连或禁用事件。
 * @post 当前操作线程的至多两条通知按顺序保存在进程期槽，其他任务立即同步派发。
 * @note 只在任务上下文调用，回调借用在 dispatch 返回前有效，所有外部回调位于操作锁外。
 */
static void wifi_notify_event(wifi_app_event_t event)
{
    wifi_app_status_t status;
    wifi_fill_status(&status);
    portENTER_CRITICAL(&s_wifi_state_lock);
    bool defer = s_wifi_operation_owner && s_wifi_operation_owner == xTaskGetCurrentTaskHandle();
    bool overflow = false;
    if (defer) {
        size_t index = s_wifi_operation_notification_count;
        if (index < 2) s_wifi_operation_notification_count++;
        else { index = 1; overflow = true; }
        s_wifi_operation_notifications[index].event = event;
        s_wifi_operation_notifications[index].status = status;
    }
    portEXIT_CRITICAL(&s_wifi_state_lock);
    if (overflow) ESP_LOGW(TAG, "WiFi driver sequence exceeded notification bound");
    if (defer) return;
    wifi_event_callback_dispatch(event, &status);
}

/**
 * @brief 读取重试计数并计算一秒起步、三十秒封顶的指数退避。
 * @return 一千至三万毫秒，移位前将次数限制在一至六，消除计数极值的移位风险。
 */
static uint32_t wifi_reconnect_delay_ms(void)
{
    portENTER_CRITICAL(&s_wifi_state_lock);
    int attempt = s_wifi_retry_count;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    if (attempt < 1) {
        attempt = 1;
    }
    if (attempt > 6) {
        attempt = 6;
    }

    uint32_t delay = WIFI_RECONNECT_BASE_MS << (attempt - 1);
    return delay > WIFI_RECONNECT_MAX_MS ? WIFI_RECONNECT_MAX_MS : delay;
}

/**
 * @brief 在状态锁内核对已安装缓存的 STA 参数。
 * @param[in] config 可空稳定候选，SSID 和密码在各自数组内终止。
 * @return 缓存有效且开关、SSID、密码和重试阈值相同为 true，其余 false。
 */
static bool wifi_station_config_matches(const network_config_t *config)
{
    if (!config) return false;
    portENTER_CRITICAL(&s_wifi_state_lock);
    bool matches = s_wifi_config_valid && s_wifi_config.enabled == config->enabled &&
           strcmp(s_wifi_config.ssid, config->ssid) == 0 &&
           strcmp(s_wifi_config.password, config->password) == 0 &&
           s_wifi_config.max_retry == config->max_retry;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    return matches;
}

/**
 * @brief 尽力向长期重连 timer 提交零等待停止命令。
 * @post 无 timer 时跳过，队列拒绝时记录诊断，操作完成前已经接纳的 callback 仍可能执行。
 * @note driver 生命周期通过操作锁和 keepalive 状态挡住迟到 timer 的驱动操作。
 */
static void wifi_stop_reconnect_timer(void)
{
    if (s_wifi_reconnect_timer) {
        if (xTimerStop(s_wifi_reconnect_timer, 0) != pdPASS)
            ESP_LOGW(TAG, "WiFi reconnect timer stop command rejected");
    }
}

/**
 * @brief 状态允许时发布重连通知并尽力设置单次退避 timer。
 * @param[in] reason 可空调用期日志说明借用。
 * @post 保活关闭、STA 禁用或正在配置时跳过，否则提交原生零等待命令，拒绝时记录诊断。
 * @note timer 为进程期资源，命令队列满时无法保证本次定时已建立，后续事件或配置可再次调度。
 */
static void wifi_schedule_reconnect(const char *reason)
{
    portENTER_CRITICAL(&s_wifi_state_lock);
    bool enabled = s_wifi_keepalive && s_wifi_config.enabled && !s_wifi_applying_config;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    if (!s_wifi_reconnect_timer || !enabled) {
        return;
    }

    uint32_t delay_ms = wifi_reconnect_delay_ms();
    ESP_LOGW(TAG, "WiFi keepalive reconnect scheduled in %" PRIu32 "ms%s%s",
             delay_ms,
             reason ? ", reason=" : "",
             reason ? reason : "");
    wifi_notify_event(WIFI_APP_EVENT_RECONNECTING);
    if (xTimerChangePeriod(s_wifi_reconnect_timer, wifi_timeout_ticks(delay_ms), 0) != pdPASS) {
        ESP_LOGW(TAG, "WiFi keepalive timer schedule failed");
    }
}

/**
 * @brief 零等待取得驱动操作锁后尝试启流和 STA 连接，失败继续尽力退避。
 * @param[in] timer 原生 timer 借用，注册后沿进程期有效。
 * @post 忙时检查 Reset 的提交结果后退出，不阻塞共享 timer daemon，驱动调用后归锁再派发通知。
 * @note 获锁后重新核对 keepalive、配置、连接和应用状态，关闭操作等待已接纳的驱动序列结束。
 */
static void wifi_reconnect_timer_cb(TimerHandle_t timer)
{
    if (!wifi_operation_enter(false)) {
        if (timer && xTimerReset(timer, 0) != pdPASS)
            ESP_LOGW(TAG, "WiFi reconnect timer busy retry rejected");
        return;
    }
    portENTER_CRITICAL(&s_wifi_state_lock);
    bool enabled = s_wifi_keepalive && s_wifi_config.enabled && !s_wifi_connected && !s_wifi_applying_config;
    bool started = s_wifi_started;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    if (!enabled) {
        wifi_operation_leave();
        return;
    }

    if (!started) {
        esp_err_t start_err = esp_wifi_start();
        if (start_err == ESP_OK || start_err == ESP_ERR_INVALID_STATE || start_err == ESP_ERR_WIFI_STATE) {
            portENTER_CRITICAL(&s_wifi_state_lock);
            s_wifi_started = true;
            portEXIT_CRITICAL(&s_wifi_state_lock);
        } else {
            ESP_LOGW(TAG, "WiFi keepalive start failed: %s", esp_err_to_name(start_err));
            wifi_schedule_reconnect("start_failed");
            wifi_operation_leave();
            return;
        }
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "WiFi keepalive connect failed: %s", esp_err_to_name(err));
        wifi_schedule_reconnect("connect_failed");
    }
    wifi_operation_leave();
}

/**
 * @brief 为本次原生设置分配清零 wifi_config_t，优先 PSRAM 并回退通用堆。
 * @return 成功为调用者拥有且兼容 free 的分配，全部分配失败为 NULL，来源写入诊断日志。
 * @note 不在状态临界区分配，原生 set_config 返回后可释放该临时对象。
 */
static wifi_config_t *wifi_config_alloc_psram_preferred(void)
{
    wifi_config_t *cfg = NULL;

#if CONFIG_SPIRAM
    cfg = (wifi_config_t *)heap_caps_calloc(1, sizeof(wifi_config_t),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cfg) {
        ESP_LOGI(TAG, "wifi_config=%p bytes=%u place=PSRAM",
                 cfg, (unsigned)sizeof(wifi_config_t));
        return cfg;
    }
    ESP_LOGW(TAG, "wifi_config PSRAM alloc failed, bytes=%u; falling back to default heap",
             (unsigned)sizeof(wifi_config_t));
#endif

    cfg = (wifi_config_t *)calloc(1, sizeof(wifi_config_t));
    if (cfg) {
        ESP_LOGI(TAG, "wifi_config=%p bytes=%u place=%s",
                 cfg,
                 (unsigned)sizeof(wifi_config_t),
                 esp_ptr_external_ram(cfg) ? "PSRAM" : "DEFAULT");
    } else {
        ESP_LOGE(TAG, "wifi_config alloc failed, bytes=%u", (unsigned)sizeof(wifi_config_t));
    }
    return cfg;
}

/**
 * @brief 构造临时 SoftAP 原生配置并在成功后发布 SSID 快照。
 * @param[in] config 已归一化稳定配置借用，字符串已终止，空 SSID 使用 MAC 派生名。
 * @return 原生设置成功为 true，未就绪、容量、密码、分配或驱动错误为 false，临时对象始终归还。
 * @pre 调用者持操作锁，s_wifi_ready 已建立，状态锁只覆盖成功快照复制。
 */
static bool wifi_apply_softap_config(const network_config_t *config)
{
    if (!config || !s_wifi_ready) {
        return false;
    }

    wifi_config_t *ap_config = wifi_config_alloc_psram_preferred();
    if (!ap_config) {
        return false;
    }

    char ap_ssid[33] = {0};
    if (config->softap_ssid[0]) {
        copy_text(ap_ssid, sizeof(ap_ssid), config->softap_ssid);
    } else {
        wifi_default_ap_ssid(ap_ssid, sizeof(ap_ssid));
    }

    size_t ssid_len = strlen(ap_ssid);
    size_t password_len = strlen(config->softap_password);
    if (ssid_len == 0 || ssid_len > sizeof(ap_config->ap.ssid)) {
        ESP_LOGE(TAG, "SoftAP ssid is invalid, max %u bytes", (unsigned)sizeof(ap_config->ap.ssid));
        free(ap_config);
        return false;
    }
    if (password_len < 8) {
        ESP_LOGE(TAG, "SoftAP password must be at least 8 bytes");
        free(ap_config);
        return false;
    }
    if (password_len > sizeof(ap_config->ap.password)) {
        ESP_LOGE(TAG, "SoftAP password is too long, max %u bytes", (unsigned)sizeof(ap_config->ap.password));
        free(ap_config);
        return false;
    }

    memcpy(ap_config->ap.ssid, ap_ssid, ssid_len);
    ap_config->ap.ssid_len = ssid_len;
    if (password_len > 0) {
        memcpy(ap_config->ap.password, config->softap_password, password_len);
        ap_config->ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config->ap.authmode = WIFI_AUTH_OPEN;
    }
    ap_config->ap.channel = (uint8_t)(config->softap_channel > 0 ? config->softap_channel : WIFI_DEFAULT_AP_CHANNEL);
    ap_config->ap.max_connection = (uint8_t)(config->softap_max_connections > 0 ?
                                             config->softap_max_connections :
                                             WIFI_DEFAULT_AP_MAX_CONN);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, ap_config);
    free(ap_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        return false;
    }

    portENTER_CRITICAL(&s_wifi_state_lock);
    memcpy(s_wifi_ap_ssid, ap_ssid, sizeof(s_wifi_ap_ssid));
    portEXIT_CRITICAL(&s_wifi_state_lock);
    ESP_LOGI(TAG, "SoftAP configured SSID=%s IP=%s channel=%d max_conn=%d",
             ap_ssid,
             s_wifi_ap_ip,
             config->softap_channel,
             config->softap_max_connections);
    return true;
}

/**
 * @brief 处理默认 ESP 事件循环中的 STA/AP 状态与 IP 通知，更新短锁快照。
 * @param[in] arg 未使用的可空注册上下文。
 * @param[in] event_base 原生静态事件域借用，只处理 WIFI_EVENT/IP_EVENT。
 * @param[in] event_id 对应原生事件编号，其他编号跳过。
 * @param[in] event_data 原生回调期借用载荷，断开允许 NULL 原因为零，GOT_IP 为 NULL 时跳过。
 * @post 必要事件位在状态更新后发布，重试计数饱和 INT_MAX，回调及 timer 命令在短锁之外执行。
 * @note 不等待驱动操作锁，也不在事件线程调用 connect/start，实际启动调用者完成连接请求。
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_started = true;
        portEXIT_CRITICAL(&s_wifi_state_lock);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_started = false;
        s_wifi_connected = false;
        s_wifi_sta_ip[0] = '\0';
        portEXIT_CRITICAL(&s_wifi_state_lock);
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_ap_started = true;
        portEXIT_CRITICAL(&s_wifi_state_lock);
        ESP_LOGI(TAG, "WiFi SoftAP started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_ap_started = false;
        portEXIT_CRITICAL(&s_wifi_state_lock);
        ESP_LOGI(TAG, "WiFi SoftAP stopped");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_connected = false;
        s_wifi_sta_ip[0] = '\0';
        s_wifi_last_disconnect_reason = event ? event->reason : 0;
        bool applying = s_wifi_applying_config;
        bool reconnect = s_wifi_keepalive && s_wifi_config.enabled;
        int retries = s_wifi_retry_count;
        int max_retry = s_wifi_config.max_retry > 0 ? s_wifi_config.max_retry : WIFI_DEFAULT_MAX_RETRY;
        uint16_t reason = s_wifi_last_disconnect_reason;
        if (!applying && reconnect) {
            if (s_wifi_retry_count < INT_MAX) s_wifi_retry_count++;
            retries = s_wifi_retry_count;
        }
        portEXIT_CRITICAL(&s_wifi_state_lock);
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
            if (!applying && reconnect && retries >= max_retry)
                xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }

        wifi_notify_event(WIFI_APP_EVENT_DISCONNECTED);

        if (applying) {
            ESP_LOGI(TAG, "WiFi disconnected while applying new config, reason=%u",
                     (unsigned)reason);
            return;
        }

        if (!reconnect) {
            ESP_LOGI(TAG, "WiFi disconnected, keepalive disabled, reason=%u",
                     (unsigned)reason);
            return;
        }

        ESP_LOGW(TAG, "WiFi disconnected, reason=%u, retry=%d/%d%s",
                 (unsigned)reason,
                 retries,
                 max_retry,
                 retries >= max_retry ? ", background keepalive continues" : "");
        wifi_schedule_reconnect("disconnected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (!event) return;
        char address[16] = {0};
        copy_ip(address, sizeof(address), &event->ip_info.ip);
        portENTER_CRITICAL(&s_wifi_state_lock);
        bool accept = s_wifi_keepalive && s_wifi_config.enabled && !s_wifi_applying_config;
        if (!accept) {
            portEXIT_CRITICAL(&s_wifi_state_lock);
            return;
        }
        s_wifi_connected = true;
        s_wifi_retry_count = 0;
        s_wifi_last_disconnect_reason = 0;
        memcpy(s_wifi_sta_ip, address, sizeof(s_wifi_sta_ip));
        portEXIT_CRITICAL(&s_wifi_state_lock);
        wifi_stop_reconnect_timer();
        ESP_LOGI(TAG, "WiFi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        wifi_notify_event(WIFI_APP_EVENT_CONNECTED);
    }
}

/**
 * @brief 创建缺失的 Wi-Fi driver，并为新建或已有实例重试 RAM 存储设置。
 * @return 原生初始化或 set_storage 结果，driver 已创建但存储设置失败时保留 driver 供重试。
 * @pre 调用者持操作锁，driver 标志由 owner/timer 驱动序列独占。
 */
static esp_err_t init_wifi_driver(void)
{
    if (!s_wifi_driver_initialized) {
        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        esp_err_t err = esp_wifi_init(&init_config);
        if (err != ESP_OK) return err;
        s_wifi_driver_initialized = true;
    }
    /* driver 已创建但上次 RAM 存储设置失败时，仍重新执行设置。 */
    return esp_wifi_set_storage(WIFI_STORAGE_RAM);
}

/**
 * @brief 用公开分步 API 创建默认 Wi-Fi netif，避免便捷创建器在 OOM 时 assert。
 * @param[in] ap true 创建 SoftAP，false 创建 STA，配置保持 IDF 默认值。
 * @param[in,out] slot 必填进程期槽，已有对象直接复用，成功后由本服务长期保管。
 * @return 成功、NO_MEM 或原生 attach/默认事件注册错误，失败归还本次新建对象。
 * @pre 本线程持有 Wi-Fi 操作锁，默认 netif 及 handler 的配置由本服务独占。
 */
static esp_err_t wifi_create_default_netif(bool ap, esp_netif_t **slot)
{
    if (*slot) return ESP_OK;
    esp_netif_config_t configuration = ESP_NETIF_DEFAULT_WIFI_STA();
    if (ap) {
        esp_netif_config_t ap_configuration = ESP_NETIF_DEFAULT_WIFI_AP();
        configuration = ap_configuration;
    }
    esp_netif_t *netif = esp_netif_new(&configuration);
    if (!netif) return ESP_ERR_NO_MEM;
    esp_err_t err = ap ? esp_netif_attach_wifi_ap(netif) : esp_netif_attach_wifi_station(netif);
    if (err == ESP_OK)
        err = ap ? esp_wifi_set_default_wifi_ap_handlers() : esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) {
        esp_netif_destroy_default_wifi(netif);
        return err;
    }
    *slot = netif;
    return ESP_OK;
}

/**
 * @brief 逐步建立保留 NVS 数据的 Wi-Fi 进程期依赖，并补齐失败前未完成的阶段。
 * @return 全部成功为 ESP_OK，NVS/netif/事件循环/driver/注册等原生错误直接返回，资源不足为 NO_MEM。
 * @pre 调用者持操作锁，默认 Wi-Fi netif 由本模块独占装配。
 * @note 不擦除 NVS，不对可恢复注册错误 abort。成功 netif、事件组、timer 和 handler 保留，
 *       只有缺失阶段重建，全部完成后才发布 ready。OTA 后复用这些对象并仅重建 driver。
 */
static esp_err_t init_wifi_once(void)
{
    if (s_wifi_ready) {
        return init_wifi_driver();
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed (%s); preserving data, repair is required",
                 esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = wifi_create_default_netif(false, &s_wifi_sta_netif);
    if (err != ESP_OK) return err;
    err = wifi_create_default_netif(true, &s_wifi_ap_netif);
    if (err != ESP_OK) return err;

    esp_netif_ip_info_t ap_ip = {0};
    esp_netif_str_to_ip4(WIFI_AP_DEFAULT_IP, &ap_ip.ip);
    esp_netif_str_to_ip4(WIFI_AP_DEFAULT_GW, &ap_ip.gw);
    esp_netif_str_to_ip4(WIFI_AP_DEFAULT_NETMASK, &ap_ip.netmask);
    esp_err_t dhcp_err = esp_netif_dhcps_stop(s_wifi_ap_netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "SoftAP DHCP stop failed: %s", esp_err_to_name(dhcp_err));
    }
    esp_err_t ip_err = esp_netif_set_ip_info(s_wifi_ap_netif, &ap_ip);
    if (ip_err == ESP_OK) {
        char address[16] = {0};
        copy_ip(address, sizeof(address), &ap_ip.ip);
        portENTER_CRITICAL(&s_wifi_state_lock);
        memcpy(s_wifi_ap_ip, address, sizeof(s_wifi_ap_ip));
        portEXIT_CRITICAL(&s_wifi_state_lock);
    } else {
        ESP_LOGW(TAG, "SoftAP IP config failed: %s", esp_err_to_name(ip_err));
    }
    dhcp_err = esp_netif_dhcps_start(s_wifi_ap_netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "SoftAP DHCP start failed: %s", esp_err_to_name(dhcp_err));
    }

    err = init_wifi_driver();
    if (err != ESP_OK) {
        return err;
    }
    if (!s_wifi_events) s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    if (!s_wifi_reconnect_timer) s_wifi_reconnect_timer = xTimerCreate("wifi_keepalive",
                                          wifi_timeout_ticks(WIFI_RECONNECT_BASE_MS),
                                          pdFALSE,
                                          NULL,
                                          wifi_reconnect_timer_cb);
    if (!s_wifi_reconnect_timer) {
        return ESP_ERR_NO_MEM;
    }

    if (!s_wifi_event_registered) {
        err = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL);
        if (err != ESP_OK) return err;
        s_wifi_event_registered = true;
    }
    if (!s_wifi_ip_registered) {
        err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL);
        if (err != ESP_OK) return err;
        s_wifi_ip_registered = true;
    }

    s_wifi_ready = true;
    return ESP_OK;
}

/**
 * @brief 在操作锁内归一化候选配置，逐步切换驱动并等待本次 STA 连接窗口。
 * @param[in] config 必填稳定配置，先复制，缺少终止符的字段在任何 strlen 前拒绝。
 * @return STA 在窗口内成功、仅 AP 成功或明确全部关闭为 true，其他为 false，已完成驱动变更保留。
 * @pre 当前线程持有操作锁，归锁和外部通知由公开包装入口负责。
 * @note 安装失败使配置匹配缓存失效，同参重试重新执行原生设置。单纯连接超时仍保留
 *       已安装配置及尽力保活。候选中的非正超时/重试及超界 AP 数值沿原规则归一化。
 */
static bool wifi_connect_config_locked(const network_config_t *config)
{
    if (!config) {
        ESP_LOGW(TAG, "WiFi config is missing");
        return false;
    }

    network_config_t next = *config;
    if (!memchr(next.ssid, '\0', sizeof(next.ssid)) ||
        !memchr(next.password, '\0', sizeof(next.password)) ||
        !memchr(next.softap_ssid, '\0', sizeof(next.softap_ssid)) ||
        !memchr(next.softap_password, '\0', sizeof(next.softap_password))) {
        ESP_LOGE(TAG, "WiFi configuration contains an unterminated field");
        return false;
    }
    if (next.connect_timeout_ms <= 0) {
        next.connect_timeout_ms = WIFI_DEFAULT_CONNECT_TIMEOUT_MS;
    }
    if (next.max_retry <= 0) {
        next.max_retry = WIFI_DEFAULT_MAX_RETRY;
    }
    if (next.softap_channel <= 0 || next.softap_channel > 13) {
        next.softap_channel = WIFI_DEFAULT_AP_CHANNEL;
    }
    if (next.softap_max_connections <= 0 || next.softap_max_connections > 10) {
        next.softap_max_connections = WIFI_DEFAULT_AP_MAX_CONN;
    }
    if (next.softap_enabled && strlen(next.softap_password) < 8) {
        ESP_LOGW(TAG, "SoftAP disabled because its password is shorter than 8 bytes");
        next.softap_enabled = false;
    }

    if (next.enabled && next.ssid[0] == '\0') {
        ESP_LOGW(TAG, "WiFi enabled but ssid is empty");
        return false;
    }

    esp_err_t err = init_wifi_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi init failed: %s", esp_err_to_name(err));
        return false;
    }

    bool same_station = wifi_station_config_matches(&next);
    bool same_softap = wifi_softap_config_matches(&next);
    portENTER_CRITICAL(&s_wifi_state_lock);
    s_wifi_config = next;
    s_wifi_config_valid = true;
    s_wifi_keepalive = next.enabled;
    s_wifi_applying_config = true;
    bool started = s_wifi_started;
    portEXIT_CRITICAL(&s_wifi_state_lock);

    wifi_mode_t mode = wifi_mode_for_config(&next);
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    bool have_current_mode = esp_wifi_get_mode(&current_mode) == ESP_OK;
    bool mode_changed = !have_current_mode || current_mode != mode;
    if (mode == WIFI_MODE_NULL) {
        if (started) {
            esp_err_t stop_err = esp_wifi_stop();
            if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "esp_wifi_stop while disabling WiFi failed: %s", esp_err_to_name(stop_err));
                goto configuration_failed;
            }
        }
        ESP_LOGI(TAG, "WiFi STA and SoftAP disabled by config");
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_started = false;
        s_wifi_connected = false;
        s_wifi_ap_started = false;
        s_wifi_sta_ip[0] = '\0';
        s_wifi_ap_ssid[0] = '\0';
        s_wifi_keepalive = false;
        s_wifi_retry_count = 0;
        s_wifi_last_disconnect_reason = 0;
        s_wifi_applying_config = false;
        portEXIT_CRITICAL(&s_wifi_state_lock);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        wifi_stop_reconnect_timer();
        wifi_notify_event(WIFI_APP_EVENT_DISABLED);
        return true;
    }
    if (started && mode_changed) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "esp_wifi_stop before mode change failed: %s", esp_err_to_name(stop_err));
            goto configuration_failed;
        }
        started = false;
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_started = false;
        s_wifi_ap_started = false;
        s_wifi_connected = false;
        s_wifi_sta_ip[0] = '\0';
        portEXIT_CRITICAL(&s_wifi_state_lock);
    }
    if (mode_changed) {
        err = esp_wifi_set_mode(mode);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
            goto configuration_failed;
        }
    }

    xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
    if (!same_softap) {
        if (next.softap_enabled) {
            if (!wifi_apply_softap_config(&next)) {
                goto configuration_failed;
            }
        } else {
            portENTER_CRITICAL(&s_wifi_state_lock);
            s_wifi_ap_ssid[0] = '\0';
            portEXIT_CRITICAL(&s_wifi_state_lock);
        }
    }

    bool station_already_connected = false;
    portENTER_CRITICAL(&s_wifi_state_lock);
    bool connected = s_wifi_connected;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    if (!next.enabled) {
        ESP_LOGI(TAG, "WiFi STA disabled by config; SoftAP %s",
                 next.softap_enabled ? "enabled" : "disabled");
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_connected = false;
        s_wifi_sta_ip[0] = '\0';
        s_wifi_retry_count = 0;
        s_wifi_last_disconnect_reason = 0;
        portEXIT_CRITICAL(&s_wifi_state_lock);
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        }
        wifi_stop_reconnect_timer();
        if (started) {
            esp_err_t disc_err = esp_wifi_disconnect();
            if (disc_err != ESP_OK && disc_err != ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGW(TAG, "esp_wifi_disconnect while disabling STA failed: %s", esp_err_to_name(disc_err));
            }
        }
    } else if (same_station && connected) {
        ESP_LOGI(TAG, "WiFi already connected");
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        wifi_notify_event(WIFI_APP_EVENT_CONNECTED);
        station_already_connected = true;
    } else if (!same_station) {
        wifi_config_t *wifi_config = wifi_config_alloc_psram_preferred();
        if (!wifi_config) {
            goto configuration_failed;
        }

        size_t ssid_len = strlen(next.ssid);
        size_t password_len = strlen(next.password);
        if (ssid_len > sizeof(wifi_config->sta.ssid)) {
            ESP_LOGE(TAG, "WiFi ssid is too long, max %u bytes", (unsigned)sizeof(wifi_config->sta.ssid));
            free(wifi_config);
            goto configuration_failed;
        }
        if (password_len > sizeof(wifi_config->sta.password)) {
            ESP_LOGE(TAG, "WiFi password is too long, max %u bytes", (unsigned)sizeof(wifi_config->sta.password));
            free(wifi_config);
            goto configuration_failed;
        }

        memcpy(wifi_config->sta.ssid, next.ssid, ssid_len);
        memcpy(wifi_config->sta.password, next.password, password_len);
        wifi_config->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config->sta.failure_retry_cnt = wifi_config_driver_retry_count();

        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_retry_count = 0;
        s_wifi_last_disconnect_reason = 0;
        s_wifi_connected = false;
        portEXIT_CRITICAL(&s_wifi_state_lock);
        wifi_stop_reconnect_timer();

        /* 原生 set_config 同步复制配置，临时 PSRAM 对象只借用至该调用返回。 */
        if (started) {
            esp_err_t disc_err = esp_wifi_disconnect();
            if (disc_err != ESP_OK && disc_err != ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGW(TAG, "esp_wifi_disconnect before config failed: %s", esp_err_to_name(disc_err));
            }
        }

        err = esp_wifi_set_config(WIFI_IF_STA, wifi_config);
        free(wifi_config);
        wifi_config = NULL;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
            goto configuration_failed;
        }
    } else if (next.enabled) {
        ESP_LOGI(TAG, "WiFi config unchanged; keeping station config");
    }

    bool manual_connect = next.enabled && !station_already_connected;
    err = esp_wifi_start();
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_started = true;
        portEXIT_CRITICAL(&s_wifi_state_lock);
    } else if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_WIFI_STATE) {
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_started = true;
        portEXIT_CRITICAL(&s_wifi_state_lock);
        manual_connect = true;
    } else {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        goto configuration_failed;
    }

    portENTER_CRITICAL(&s_wifi_state_lock);
    if (!next.softap_enabled) {
        s_wifi_ap_started = false;
    }
    s_wifi_applying_config = false;
    portEXIT_CRITICAL(&s_wifi_state_lock);

    /* 当前 camera/LCD 负载关闭 modem sleep，减少外设和 TLS 并发时的额外时序变化。 */
    esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(WIFI_PS_NONE) failed: %s", esp_err_to_name(ps_err));
    }

    if (manual_connect && next.enabled) {
        esp_err_t conn_err = esp_wifi_connect();
        if (conn_err != ESP_OK && conn_err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(conn_err));
        }
    }

    if (!next.enabled) {
        wifi_notify_event(WIFI_APP_EVENT_DISABLED);
        return next.softap_enabled;
    }

    ESP_LOGI(TAG, "Connecting WiFi timeout=%dms keepalive=on", next.connect_timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        wifi_timeout_ticks((uint32_t)next.connect_timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connect failed after %d retries; background keepalive continues", next.max_retry);
    } else {
        ESP_LOGE(TAG, "WiFi connect timed out; background keepalive continues");
        wifi_schedule_reconnect("initial_timeout");
    }
    return false;

configuration_failed:
    portENTER_CRITICAL(&s_wifi_state_lock);
    s_wifi_config_valid = false;
    s_wifi_applying_config = false;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    return false;
}

/**
 * @brief 串行应用一份 Wi-Fi 配置，结束驱动操作锁后再派发本次状态通知。
 * @param[in] config 必填完整稳定配置，本次按值复制，字符串须在各自数组范围内终止。
 * @return STA 在连接窗口内成功、仅 AP 配置成功或明确全部禁用为 true，参数/资源/驱动/连接失败为 false。
 * @note 连接等待使用配置的毫秒预算，操作锁及原生调用遵循原生等待语义。失败可能已有
 *       驱动部分变更，配置安装失败使缓存失效以允许相同参数重试，超时后仍尽力保活。
 */
bool wifi_app_connect_from_config(const network_config_t *config)
{
    if (!config || !wifi_operation_enter(true)) return false;
    bool connected = wifi_connect_config_locked(config);
    wifi_operation_leave();
    return connected;
}

/**
 * @brief 在已接纳驱动序列结束后停止重连请求，供外层重启准备。
 * @post 操作锁可用时关闭 keepalive、清重试及 FAIL 位，资源和 driver 保留，锁外派发已保管通知。
 * @note timer Stop 为零等待尽力提交，迟到 callback 获锁后会再次检查关闭状态。
 */
void wifi_app_prepare_for_reboot(void)
{
    if (!wifi_operation_enter(true)) return;
    wifi_stop_reconnect_timer();
    portENTER_CRITICAL(&s_wifi_state_lock);
    s_wifi_keepalive = false;
    s_wifi_retry_count = 0;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
    }
    wifi_operation_leave();
}

/**
 * @brief 在操作锁内依次停止保活、断开、停止和反初始化 driver，保留首错。
 * @return 成功或首个原生错误，成功完成的清理保持，失败未确认关闭的状态继续保留。
 * @pre 当前线程持操作锁，操作通知暂存到归锁之后派发。
 * @note 只有成功 deinit 或确认 NOT_INIT 才清 driver/cache 标志，进程期 netif/handler/timer 不销毁。
 */
static esp_err_t wifi_shutdown_locked(void)
{
    wifi_stop_reconnect_timer();
    portENTER_CRITICAL(&s_wifi_state_lock);
    s_wifi_keepalive = false;
    s_wifi_retry_count = 0;
    s_wifi_last_disconnect_reason = 0;
    s_wifi_applying_config = true;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    if (!s_wifi_driver_initialized) {
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_started = false;
        s_wifi_connected = false;
        s_wifi_ap_started = false;
        s_wifi_sta_ip[0] = '\0';
        s_wifi_ap_ssid[0] = '\0';
        s_wifi_applying_config = false;
        portEXIT_CRITICAL(&s_wifi_state_lock);
        wifi_notify_event(WIFI_APP_EVENT_DISABLED);
        return ESP_OK;
    }

    esp_err_t first_err = ESP_OK;
    esp_err_t err = esp_wifi_disconnect();
    bool disconnected = err == ESP_OK || err == ESP_ERR_WIFI_NOT_INIT ||
                        err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_NOT_CONNECT;
    if (err != ESP_OK &&
        err != ESP_ERR_WIFI_NOT_INIT &&
        err != ESP_ERR_WIFI_NOT_STARTED &&
        err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "esp_wifi_disconnect for OTA failed: %s", esp_err_to_name(err));
        first_err = err;
    }

    err = esp_wifi_stop();
    bool stopped = err == ESP_OK || err == ESP_ERR_WIFI_NOT_INIT || err == ESP_ERR_WIFI_NOT_STARTED;
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop for OTA failed: %s", esp_err_to_name(err));
        if (first_err == ESP_OK) {
            first_err = err;
        }
    }

    err = esp_wifi_deinit();
    if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_INIT) {
        s_wifi_driver_initialized = false;
        /* RAM 配置已丢弃，相同 STA/AP 参数在下一次初始化后也必须重新安装。 */
        portENTER_CRITICAL(&s_wifi_state_lock);
        s_wifi_config_valid = false;
        portEXIT_CRITICAL(&s_wifi_state_lock);
    }
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_deinit for OTA failed: %s", esp_err_to_name(err));
        if (first_err == ESP_OK) {
            first_err = err;
        }
    }

    portENTER_CRITICAL(&s_wifi_state_lock);
    s_wifi_applying_config = false;
    if (stopped || !s_wifi_driver_initialized) {
        s_wifi_started = false;
        s_wifi_ap_started = false;
    }
    if (disconnected || stopped || !s_wifi_driver_initialized) {
        s_wifi_connected = false;
        s_wifi_sta_ip[0] = '\0';
    }
    if (!s_wifi_driver_initialized) s_wifi_ap_ssid[0] = '\0';
    portEXIT_CRITICAL(&s_wifi_state_lock);

    ESP_LOGI(TAG, "WiFi shutdown for OTA complete");
    wifi_notify_event(WIFI_APP_EVENT_DISABLED);
    return first_err;
}

/**
 * @brief 等待已接纳驱动操作结束，关闭重连并依次断开、停止和反初始化 Wi-Fi。
 * @return 成功为 ESP_OK，操作锁初始化失败为 NO_MEM，其余保留首个原生错误，已完成清理不回滚。
 * @note netif、事件组、handler、timer 和操作锁为进程期资源，驱动成功 deinit 才使配置缓存
 *       失效。驱动操作失败保持尚未确认关闭的状态供重试，通知在操作锁完全归还后派发。
 */
esp_err_t wifi_app_shutdown_for_ota(void)
{
    if (!wifi_operation_enter(true)) return ESP_ERR_NO_MEM;
    esp_err_t err = wifi_shutdown_locked();
    wifi_operation_leave();
    return err;
}

/**
 * @brief 在进程期状态锁内读取最近异步事件跟踪的 STA 连接状态。
 * @return 当前连接标志，查询不建立 driver 或任务借用。
 */
bool wifi_app_is_connected(void)
{
    portENTER_CRITICAL(&s_wifi_state_lock);
    bool connected = s_wifi_connected;
    portEXIT_CRITICAL(&s_wifi_state_lock);
    return connected;
}

/**
 * @brief 获取独立的 Wi-Fi 应用值快照。
 * @param[out] out_status 可空输出槽，非空先清零再填完整状态。
 * @post 输出不借用内部字符串，无动态释放责任，锁返回前已归还。
 */
void wifi_app_get_status(wifi_app_status_t *out_status)
{
    wifi_fill_status(out_status);
}
