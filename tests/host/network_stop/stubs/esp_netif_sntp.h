#pragma once
#include "esp_err.h"
typedef struct { int unused; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(host) ((esp_sntp_config_t){0})
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
esp_err_t esp_netif_sntp_start(void);
esp_err_t esp_netif_sntp_sync_wait(unsigned timeout);
