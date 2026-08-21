#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "network_config.h"
#include "esp_err.h"

typedef struct {
    bool enabled;
    bool started;
    bool connected;
    bool ap_enabled;
    bool ap_started;
    bool keepalive;
    int retry_count;
    int max_retry;
    uint16_t last_disconnect_reason;
    char ssid[33];
    char ap_ssid[33];
    char sta_ip[16];
    char ap_ip[16];
} wifi_app_status_t;

typedef enum {
    WIFI_APP_EVENT_CONNECTED,
    WIFI_APP_EVENT_DISCONNECTED,
    WIFI_APP_EVENT_RECONNECTING,
    WIFI_APP_EVENT_DISABLED,
} wifi_app_event_t;

typedef void (*wifi_app_event_callback_t)(wifi_app_event_t event,
                                          const wifi_app_status_t *status,
                                          void *user_data);

/*
 * 注册 WiFi 事件回调函数，在 STA 连接/断开/重连/禁用时通知调用方。
 * 参数: callback   - 回调函数指针
 * 参数: user_data  - 传递给回调的用户数据
 */
void wifi_app_set_event_callback(wifi_app_event_callback_t callback, void *user_data);
/*
 * 根据 network_config_t 初始化并连接 WiFi（STA + SoftAP），包含首次初始化、模式切换、SoftAP 配置等。
 * 参数: config - 网络配置
 * 返回: true 表示在超时时间内连接成功（或 SoftAP 配置成功），false 表示超时或失败
 */
bool wifi_app_connect_from_config(const network_config_t *config);
/*
 * 重启前准备：停止重连定时器、关闭保活、清除重试计数和 FAIL 标志。
 */
void wifi_app_prepare_for_reboot(void);
/*
 * OTA 前彻底关闭 WiFi：断开连接、停止驱动、deinit，释放资源。
 * 返回: ESP_OK 或首次遇到的错误码
 */
esp_err_t wifi_app_shutdown_for_ota(void);
/*
 * 查询 STA 是否已连接（获取 IP 且未断连）。
 * 返回: true 已连接，false 未连接
 */
bool wifi_app_is_connected(void);
/*
 * 获取 WiFi 应用的当前状态快照（STA/AP/重连计数/IP 等）。
 * 参数: out_status - 输出状态结构体指针
 */
void wifi_app_get_status(wifi_app_status_t *out_status);
