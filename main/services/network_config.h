#pragma once

#include <stdbool.h>

/** @brief 网络配置按值记录，所有文本为结构内 NUL 终止数组。
 * @note 无动态所有权，无函数入口，副本应由配置 owner 串行发布给网络服务。
 * password_ref/softap_password_ref 保存凭据引用标识，password/softap_password
 * 保存运行时密码，复制记录的 owner 负责敏感字段生命周期及清理。
 * 数值默认值和合法范围由消费端解释，结构声明自身不执行校验或同步。
 */
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
    char softap_password[65];        /* SoftAP 密码，启用 AP 时至少 8 字节 */
    int softap_channel;              /* SoftAP 信道（1-13），0 使用默认值 6 */
    int softap_max_connections;      /* SoftAP 最大连接数，0 使用默认值 4 */
    bool mdns_enabled;               /* mDNS 服务是否启用 */
    char mdns_hostname[32];          /* mDNS 主机名（.local 域名前缀），默认 "cclaw" */
} network_config_t;
