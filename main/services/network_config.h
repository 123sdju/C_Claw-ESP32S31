#pragma once

#include <stdbool.h>

typedef struct {
    char password_ref[32];
    char softap_password_ref[32];
    bool enabled;                    /* STA 模式是否启用 */
    char ssid[33];                   /* WiFi SSID，最多 32 字节 */
    char password[65];               /* WiFi 密码，最多 64 字节 */
    int connect_timeout_ms;          /* 连接超时（毫秒），0 使用默认值 20000 */
    int max_retry;                   /* 最大重试次数，0 使用默认值 5 */
    bool softap_enabled;             /* SoftAP 模式是否启用 */
    char softap_ssid[33];            /* SoftAP SSID，空则用 MAC 派生名 */
    char softap_password[65];        /* SoftAP 密码，小于 8 字节则开放认证 */
    int softap_channel;              /* SoftAP 信道（1-13），0 使用默认值 6 */
    int softap_max_connections;      /* SoftAP 最大连接数，0 使用默认值 4 */
    bool mdns_enabled;               /* mDNS 服务是否启用 */
    char mdns_hostname[32];          /* mDNS 主机名（.local 域名前缀），默认 "cclaw" */
} network_config_t;
