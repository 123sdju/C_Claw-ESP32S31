#include "wifi_app.h"

#include <inttypes.h>
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
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include "esp_memory_utils.h"

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

static EventGroupHandle_t s_wifi_events;
static TimerHandle_t s_wifi_reconnect_timer;
static network_config_t s_wifi_config;
static esp_netif_t *s_wifi_sta_netif;
static esp_netif_t *s_wifi_ap_netif;
static bool s_wifi_ready;
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
static wifi_app_event_callback_t s_wifi_event_callback;
static void *s_wifi_event_user_data;

/*
 * 返回配置的最大重试次数，0 表示使用默认值 WIFI_DEFAULT_MAX_RETRY (5)。
 */
static int wifi_config_max_retry(void)
{
    return s_wifi_config.max_retry > 0 ? s_wifi_config.max_retry : WIFI_DEFAULT_MAX_RETRY;
}

/*
 * 返回受限的驱动重试次数（uint8_t），上限 255。
 */
static uint8_t wifi_config_driver_retry_count(void)
{
    int retry = wifi_config_max_retry();
    if (retry > UINT8_MAX) {
        retry = UINT8_MAX;
    }
    return (uint8_t)retry;
}

/*
 * 安全复制文本到固定大小缓冲区（含 '\0'）。
 * 返回: true 成功（未截断），false 截断
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

/*
 * 将 IP4 地址转为点分十进制字符串写入 dst。
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

/*
 * 生成默认 SoftAP SSID（C-Claw-XXYYZZ，以 MAC 后三字节为标识）。
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

/*
 * 判断新旧配置的 SoftAP 部分是否一致。
 */
static bool wifi_softap_config_matches(const network_config_t *config)
{
    if (!config || !s_wifi_config_valid) {
        return false;
    }
    return s_wifi_config.softap_enabled == config->softap_enabled &&
           strcmp(s_wifi_config.softap_ssid, config->softap_ssid) == 0 &&
           strcmp(s_wifi_config.softap_password, config->softap_password) == 0 &&
           s_wifi_config.softap_channel == config->softap_channel &&
           s_wifi_config.softap_max_connections == config->softap_max_connections;
}

/*
 * 根据配置计算所需的 WiFi 模式（WIFI_MODE_STA / WIFI_MODE_AP / WIFI_MODE_APSTA / WIFI_MODE_NULL）。
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

/*
 * 填充 wifi_app_status_t 结构体（连接状态、IP、重连计数等）。
 */
static void wifi_fill_status(wifi_app_status_t *out_status)
{
    if (!out_status) {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->enabled = s_wifi_config_valid && s_wifi_config.enabled;
    out_status->started = s_wifi_started;
    out_status->connected = s_wifi_connected;
    out_status->ap_enabled = s_wifi_config_valid && s_wifi_config.softap_enabled;
    out_status->ap_started = s_wifi_ap_started;
    out_status->keepalive = s_wifi_keepalive;
    out_status->retry_count = s_wifi_retry_count;
    out_status->max_retry = wifi_config_max_retry();
    out_status->last_disconnect_reason = s_wifi_last_disconnect_reason;
    snprintf(out_status->ssid, sizeof(out_status->ssid), "%s", s_wifi_config.ssid);
    snprintf(out_status->ap_ssid, sizeof(out_status->ap_ssid), "%s", s_wifi_ap_ssid);
    snprintf(out_status->sta_ip, sizeof(out_status->sta_ip), "%s", s_wifi_sta_ip);
    snprintf(out_status->ap_ip, sizeof(out_status->ap_ip), "%s", s_wifi_ap_ip);
}

/*
 * 通知注册的事件回调（连接/断开/重连/禁用）。
 */
static void wifi_notify_event(wifi_app_event_t event)
{
    wifi_app_event_callback_t callback = s_wifi_event_callback;
    if (!callback) {
        return;
    }

    wifi_app_status_t status;
    wifi_fill_status(&status);
    callback(event, &status, s_wifi_event_user_data);
}

/*
 * 计算指数退避重连延迟（1s→2s→4s→8s→16s→30s 上限）。
 */
static uint32_t wifi_reconnect_delay_ms(void)
{
    int attempt = s_wifi_retry_count;
    if (attempt < 1) {
        attempt = 1;
    }
    if (attempt > 6) {
        attempt = 6;
    }

    uint32_t delay = WIFI_RECONNECT_BASE_MS << (attempt - 1);
    return delay > WIFI_RECONNECT_MAX_MS ? WIFI_RECONNECT_MAX_MS : delay;
}

/*
 * 判断新旧配置的 STA 部分是否一致。
 */
static bool wifi_station_config_matches(const network_config_t *config)
{
    if (!config || !s_wifi_config_valid) {
        return false;
    }
    return s_wifi_config.enabled == config->enabled &&
           strcmp(s_wifi_config.ssid, config->ssid) == 0 &&
           strcmp(s_wifi_config.password, config->password) == 0 &&
           s_wifi_config.max_retry == config->max_retry;
}

/*
 * 停止重连定时器。
 */
static void wifi_stop_reconnect_timer(void)
{
    if (s_wifi_reconnect_timer) {
        xTimerStop(s_wifi_reconnect_timer, 0);
    }
}

/*
 * 安排指数退避重连：设置定时器在延迟后触发 wifi_reconnect_timer_cb。
 */
static void wifi_schedule_reconnect(const char *reason)
{
    if (!s_wifi_reconnect_timer || !s_wifi_keepalive || !s_wifi_config.enabled || s_wifi_applying_config) {
        return;
    }

    uint32_t delay_ms = wifi_reconnect_delay_ms();
    ESP_LOGW(TAG, "WiFi keepalive reconnect scheduled in %" PRIu32 "ms%s%s",
             delay_ms,
             reason ? ", reason=" : "",
             reason ? reason : "");
    wifi_notify_event(WIFI_APP_EVENT_RECONNECTING);
    if (xTimerChangePeriod(s_wifi_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0) != pdPASS) {
        ESP_LOGW(TAG, "WiFi keepalive timer schedule failed");
    }
}

/*
 * 重连定时器回调：尝试 esp_wifi_connect，失败则继续调度重连。
 */
static void wifi_reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    if (!s_wifi_keepalive || !s_wifi_config.enabled || s_wifi_connected || s_wifi_applying_config) {
        return;
    }

    if (!s_wifi_started) {
        esp_err_t start_err = esp_wifi_start();
        if (start_err == ESP_OK || start_err == ESP_ERR_INVALID_STATE || start_err == ESP_ERR_WIFI_STATE) {
            s_wifi_started = true;
            if (start_err == ESP_OK) {
                return;
            }
        } else {
            ESP_LOGW(TAG, "WiFi keepalive start failed: %s", esp_err_to_name(start_err));
            wifi_schedule_reconnect("start_failed");
            return;
        }
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "WiFi keepalive connect failed: %s", esp_err_to_name(err));
        wifi_schedule_reconnect("connect_failed");
    }
}

/*
 * 分配 wifi_config_t，优先使用 PSRAM，记录分配来源。
 * 返回: 分配的指针（调用方需 free）
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

/*
 * 应用 SoftAP 配置：SSID/密码/信道/最大连接数，SSID 为空则用 MAC 派生名。
 * 返回: true 成功
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

    char ap_ssid[33];
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
    if (password_len > 0 && password_len < 8) {
        ESP_LOGW(TAG, "SoftAP password is shorter than 8 bytes; using open auth");
        password_len = 0;
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

    copy_text(s_wifi_ap_ssid, sizeof(s_wifi_ap_ssid), ap_ssid);
    ESP_LOGI(TAG, "SoftAP configured SSID=%s IP=%s channel=%d max_conn=%d",
             s_wifi_ap_ssid,
             s_wifi_ap_ip,
             config->softap_channel,
             config->softap_max_connections);
    return true;
}

/*
 * WiFi 事件处理：STA_START/STOP/AP_START/STOP/DISCONNECT/GOT_IP，
 * 管理连接状态、重连逻辑和事件回调通知。
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_wifi_started = true;
        if (s_wifi_keepalive && s_wifi_config.enabled) {
            esp_err_t conn_err = esp_wifi_connect();
            if (conn_err != ESP_OK && conn_err != ESP_ERR_WIFI_CONN) {
                ESP_LOGW(TAG, "esp_wifi_connect on start failed: %s", esp_err_to_name(conn_err));
                wifi_schedule_reconnect("start_connect_failed");
            }
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_STOP) {
        s_wifi_started = false;
        s_wifi_connected = false;
        s_wifi_sta_ip[0] = '\0';
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        s_wifi_ap_started = true;
        ESP_LOGI(TAG, "WiFi SoftAP started, SSID=%s IP=%s", s_wifi_ap_ssid, s_wifi_ap_ip);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        s_wifi_ap_started = false;
        ESP_LOGI(TAG, "WiFi SoftAP stopped");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_wifi_connected = false;
        s_wifi_sta_ip[0] = '\0';
        s_wifi_last_disconnect_reason = event ? event->reason : 0;
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }

        wifi_notify_event(WIFI_APP_EVENT_DISCONNECTED);

        if (s_wifi_applying_config) {
            ESP_LOGI(TAG, "WiFi disconnected while applying new config, reason=%u",
                     (unsigned)s_wifi_last_disconnect_reason);
            return;
        }

        if (!s_wifi_keepalive || !s_wifi_config.enabled) {
            ESP_LOGI(TAG, "WiFi disconnected, keepalive disabled, reason=%u",
                     (unsigned)s_wifi_last_disconnect_reason);
            return;
        }

        s_wifi_retry_count++;
        int max_retry = wifi_config_max_retry();
        if (s_wifi_retry_count >= max_retry && s_wifi_events) {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }

        ESP_LOGW(TAG, "WiFi disconnected, reason=%u, retry=%d/%d%s",
                 (unsigned)s_wifi_last_disconnect_reason,
                 s_wifi_retry_count,
                 max_retry,
                 s_wifi_retry_count >= max_retry ? ", background keepalive continues" : "");
        wifi_schedule_reconnect("disconnected");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_connected = true;
        s_wifi_retry_count = 0;
        s_wifi_last_disconnect_reason = 0;
        wifi_stop_reconnect_timer();
        copy_ip(s_wifi_sta_ip, sizeof(s_wifi_sta_ip), &event->ip_info.ip);
        ESP_LOGI(TAG, "WiFi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
            xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        }
        wifi_notify_event(WIFI_APP_EVENT_CONNECTED);
    }
}

/*
 * 一次性初始化 NVS/netif/WiFi/事件组/重连定时器。
 * 返回: ESP_OK 成功
 */
static esp_err_t init_wifi_once(void)
{
    if (s_wifi_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
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

    s_wifi_sta_netif = esp_netif_create_default_wifi_sta();
    s_wifi_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_wifi_sta_netif || !s_wifi_ap_netif) {
        return ESP_ERR_NO_MEM;
    }

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
        copy_ip(s_wifi_ap_ip, sizeof(s_wifi_ap_ip), &ap_ip.ip);
    } else {
        ESP_LOGW(TAG, "SoftAP IP config failed: %s", esp_err_to_name(ip_err));
    }
    dhcp_err = esp_netif_dhcps_start(s_wifi_ap_netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGW(TAG, "SoftAP DHCP start failed: %s", esp_err_to_name(dhcp_err));
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        return err;
    }
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_reconnect_timer = xTimerCreate("wifi_keepalive",
                                          pdMS_TO_TICKS(WIFI_RECONNECT_BASE_MS),
                                          pdFALSE,
                                          NULL,
                                          wifi_reconnect_timer_cb);
    if (!s_wifi_reconnect_timer) {
        vEventGroupDelete(s_wifi_events);
        s_wifi_events = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_wifi_ready = true;
    return ESP_OK;
}

/*
 * 从 network_config_t 初始化并连接 WiFi：NVS 初始化 → netif/WiFi 初始化 → 模式切换 →
 * STA/AP 配置 → 连接 → 等待结果（含超时和 keepalive 保活）。
 * 返回: true 表示在超时内连接成功或仅有 SoftAP 且配置成功
 */
bool wifi_app_connect_from_config(const network_config_t *config)
{
    if (!config) {
        ESP_LOGW(TAG, "WiFi config is missing");
        return false;
    }

    network_config_t next = *config;
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
    s_wifi_config = next;
    s_wifi_config_valid = true;
    s_wifi_keepalive = next.enabled;

    wifi_mode_t mode = wifi_mode_for_config(&next);
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    bool have_current_mode = esp_wifi_get_mode(&current_mode) == ESP_OK;
    bool mode_changed = !have_current_mode || current_mode != mode;
    if (mode == WIFI_MODE_NULL) {
        if (s_wifi_started) {
            esp_err_t stop_err = esp_wifi_stop();
            if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "esp_wifi_stop while disabling WiFi failed: %s", esp_err_to_name(stop_err));
            }
        }
        ESP_LOGI(TAG, "WiFi STA and SoftAP disabled by config");
        s_wifi_started = false;
        s_wifi_connected = false;
        s_wifi_ap_started = false;
        s_wifi_sta_ip[0] = '\0';
        s_wifi_ap_ssid[0] = '\0';
        s_wifi_keepalive = false;
        s_wifi_retry_count = 0;
        s_wifi_last_disconnect_reason = 0;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        wifi_stop_reconnect_timer();
        wifi_notify_event(WIFI_APP_EVENT_DISABLED);
        return true;
    }
    if (s_wifi_started && mode_changed) {
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "esp_wifi_stop before mode change failed: %s", esp_err_to_name(stop_err));
        }
        s_wifi_started = false;
        s_wifi_ap_started = false;
        s_wifi_connected = false;
        s_wifi_sta_ip[0] = '\0';
    }
    if (mode_changed) {
        err = esp_wifi_set_mode(mode);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(err));
            return false;
        }
    }

    xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
    if (!same_softap) {
        if (next.softap_enabled) {
            if (!wifi_apply_softap_config(&next)) {
                return false;
            }
        } else {
            s_wifi_ap_ssid[0] = '\0';
        }
    }

    bool station_already_connected = false;
    if (!next.enabled) {
        ESP_LOGI(TAG, "WiFi STA disabled by config; SoftAP %s",
                 next.softap_enabled ? "enabled" : "disabled");
        s_wifi_connected = false;
        s_wifi_sta_ip[0] = '\0';
        s_wifi_retry_count = 0;
        s_wifi_last_disconnect_reason = 0;
        if (s_wifi_events) {
            xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        }
        wifi_stop_reconnect_timer();
        if (s_wifi_started) {
            esp_err_t disc_err = esp_wifi_disconnect();
            if (disc_err != ESP_OK && disc_err != ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGW(TAG, "esp_wifi_disconnect while disabling STA failed: %s", esp_err_to_name(disc_err));
            }
        }
    } else if (same_station && s_wifi_connected) {
        ESP_LOGI(TAG, "WiFi already connected SSID=%s", next.ssid);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        wifi_notify_event(WIFI_APP_EVENT_CONNECTED);
        station_already_connected = true;
    } else if (!same_station) {
        wifi_config_t *wifi_config = wifi_config_alloc_psram_preferred();
        if (!wifi_config) {
            return false;
        }

        size_t ssid_len = strlen(next.ssid);
        size_t password_len = strlen(next.password);
        if (ssid_len > sizeof(wifi_config->sta.ssid)) {
            ESP_LOGE(TAG, "WiFi ssid is too long, max %u bytes", (unsigned)sizeof(wifi_config->sta.ssid));
            free(wifi_config);
            return false;
        }
        if (password_len > sizeof(wifi_config->sta.password)) {
            ESP_LOGE(TAG, "WiFi password is too long, max %u bytes", (unsigned)sizeof(wifi_config->sta.password));
            free(wifi_config);
            return false;
        }

        memcpy(wifi_config->sta.ssid, next.ssid, ssid_len);
        memcpy(wifi_config->sta.password, next.password, password_len);
        wifi_config->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wifi_config->sta.failure_retry_cnt = wifi_config_driver_retry_count();

        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        s_wifi_retry_count = 0;
        s_wifi_last_disconnect_reason = 0;
        s_wifi_connected = false;
        wifi_stop_reconnect_timer();

        /*
         * esp_wifi_set_config copies the configuration into the Wi-Fi driver, so the
         * temporary wifi_config_t can live in PSRAM and be freed right after set_config.
         */
        s_wifi_applying_config = true;
        if (s_wifi_started) {
            esp_err_t disc_err = esp_wifi_disconnect();
            if (disc_err != ESP_OK && disc_err != ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGW(TAG, "esp_wifi_disconnect before config failed: %s", esp_err_to_name(disc_err));
            }
        }

        err = esp_wifi_set_config(WIFI_IF_STA, wifi_config);
        free(wifi_config);
        wifi_config = NULL;
        s_wifi_applying_config = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
            return false;
        }
    } else if (next.enabled) {
        ESP_LOGI(TAG, "WiFi config unchanged; keeping station config");
    }

    bool manual_connect = s_wifi_started && !station_already_connected;
    err = esp_wifi_start();
    if (err == ESP_OK) {
        s_wifi_started = true;
    } else if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_WIFI_STATE) {
        s_wifi_started = true;
        manual_connect = true;
    } else {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!next.softap_enabled) {
        s_wifi_ap_started = false;
    }

    /* Disable modem sleep for this camera/LCD workload. It avoids extra timing
     * churn while RGB LCD, V4L2 camera, USB MSC and TLS are active together.
     */
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

    ESP_LOGI(TAG, "Connecting WiFi SSID=%s timeout=%dms keepalive=on", next.ssid, next.connect_timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(next.connect_timeout_ms));

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
}

/*
 * 重启前准备：停止重连定时器、关闭保活、清除失败标志。
 */
void wifi_app_prepare_for_reboot(void)
{
    wifi_stop_reconnect_timer();
    s_wifi_keepalive = false;
    s_wifi_retry_count = 0;
    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
    }
}

/*
 * OTA 前彻底关闭 WiFi：断开→停止→deinit，返回第一个错误码。
 */
esp_err_t wifi_app_shutdown_for_ota(void)
{
    wifi_stop_reconnect_timer();
    s_wifi_keepalive = false;
    s_wifi_retry_count = 0;
    s_wifi_last_disconnect_reason = 0;
    if (s_wifi_events) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    if (!s_wifi_ready) {
        s_wifi_started = false;
        s_wifi_connected = false;
        s_wifi_ap_started = false;
        s_wifi_sta_ip[0] = '\0';
        s_wifi_ap_ssid[0] = '\0';
        wifi_notify_event(WIFI_APP_EVENT_DISABLED);
        return ESP_OK;
    }

    esp_err_t first_err = ESP_OK;
    s_wifi_applying_config = true;

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK &&
        err != ESP_ERR_WIFI_NOT_INIT &&
        err != ESP_ERR_WIFI_NOT_STARTED &&
        err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "esp_wifi_disconnect for OTA failed: %s", esp_err_to_name(err));
        first_err = err;
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop for OTA failed: %s", esp_err_to_name(err));
        if (first_err == ESP_OK) {
            first_err = err;
        }
    }

    err = esp_wifi_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "esp_wifi_deinit for OTA failed: %s", esp_err_to_name(err));
        if (first_err == ESP_OK) {
            first_err = err;
        }
    }

    s_wifi_applying_config = false;
    s_wifi_started = false;
    s_wifi_connected = false;
    s_wifi_ap_started = false;
    s_wifi_sta_ip[0] = '\0';
    s_wifi_ap_ssid[0] = '\0';

    ESP_LOGI(TAG, "WiFi shutdown for OTA complete");
    wifi_notify_event(WIFI_APP_EVENT_DISABLED);
    return first_err;
}

/*
 * 查询 STA 是否已连接。
 * 返回: true 已连接
 */
bool wifi_app_is_connected(void)
{
    return s_wifi_connected;
}

/*
 * 注册 WiFi 事件回调函数。
 * 参数: callback - 回调函数指针
 * 参数: user_data - 用户数据指针
 */
void wifi_app_set_event_callback(wifi_app_event_callback_t callback, void *user_data)
{
    s_wifi_event_callback = callback;
    s_wifi_event_user_data = user_data;
}

/*
 * 获取 WiFi 应用当前状态快照。
 * 参数: out_status - 输出状态结构体指针
 */
void wifi_app_get_status(wifi_app_status_t *out_status)
{
    wifi_fill_status(out_status);
}
