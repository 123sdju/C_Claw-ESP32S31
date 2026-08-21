#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "network_config.h"
#include "esp_err.h"

typedef struct {
    bool enabled;                    /* WiFi 是否启用 */
    bool started;                    /* WiFi 驱动是否已启动 */
    bool connected;                  /* STA 是否已连接 */
    bool ap_enabled;                 /* SoftAP 是否启用 */
    bool ap_started;                 /* SoftAP 是否已启动 */
    bool mdns_enabled;               /* mDNS 是否启用 */
    bool keepalive;                  /* 保活/自动重连是否激活 */
    bool time_synced;                /* SNTP 时间是否已同步 */
    int retry_count;                 /* 当前重连计数 */
    int max_retry;                   /* 配置的最大重试次数 */
    uint16_t last_disconnect_reason; /* 上次断开的原因码 */
    char ssid[33];                   /* 当前 STA SSID */
    char ap_ssid[33];                /* SoftAP SSID */
    char sta_ip[16];                 /* STA 获得的 IP 地址 */
    char ap_ip[16];                  /* SoftAP IP 地址 */
    char mdns_hostname[32];          /* mDNS 主机名 */
    uint32_t command_high_water;
    uint32_t wifi_event_coalesced;
    uint32_t config_coalesced;
    uint64_t owner_heartbeat_ms;
    bool stopping;
} network_service_status_t;

/* 显式创建队列、QueueSet、锁和事件组；不启动 owner task。 */
esp_err_t network_service_init(void);

/*
 * 首次启动网络服务：初始化队列/任务、注册 WiFi 事件回调、连接 WiFi 并应用 mDNS 配置。
 * 参数: config - 网络配置（WiFi/mDNS 参数）
 * 返回: ESP_OK 表示 owner task 已应用配置；超时或应用失败返回对应错误。
 */
esp_err_t network_service_start_from_config(const network_config_t *config, uint32_t timeout_ms);
/*
 * 应用 WiFi 配置变更：通过 latest-value slot 交给 network owner task，并在有限时间内
 * 等待对应 generation 完成。调用者超时后不会访问 owner task 的迟到回复内存。
 * 参数: config - 新的网络配置
 *       timeout_ms - 等待 owner task 完成的最大时间
 * 返回: ESP_OK 成功，ESP_ERR_TIMEOUT 超时，其他错误表示提交或应用失败
 */
esp_err_t network_service_apply_wifi_config(const network_config_t *config, uint32_t timeout_ms);
/* stop 后使用 owner 保存的最后一份已应用配置重启服务。 */
esp_err_t network_service_resume(uint32_t timeout_ms);
/* 有限时间排空并停止 owner task；超时返回 false。 */
bool network_service_stop(uint32_t timeout_ms);
/* stop 完成后释放 Network 自有 RTOS 资源。 */
esp_err_t network_service_destroy(void);
/*
 * 重启前准备：停止 mDNS、重置 HTTP 连接、重置时间同步标志，标记静默关闭以防止断开回调重置连接。
 */
void network_service_prepare_for_reboot(void);
/*
 * OTA 刷写前关闭网络：停止 mDNS、重置 HTTP 连接，调用 wifi_app_shutdown_for_ota。
 * 返回: ESP_OK 成功
 */
esp_err_t network_service_shutdown_for_ota(void);
/*
 * 获取网络服务综合状态快照（WiFi STA/AP/mDNS/时间同步等）。
 * 参数: out_status - 输出状态结构体指针
 */
void network_service_get_status(network_service_status_t *out_status);
