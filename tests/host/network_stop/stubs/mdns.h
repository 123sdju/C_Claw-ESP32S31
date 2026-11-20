#pragma once
#include "esp_err.h"
esp_err_t mdns_init(void);
void mdns_free(void);
esp_err_t mdns_hostname_set(const char *name);
esp_err_t mdns_instance_name_set(const char *name);
esp_err_t mdns_service_add(const char *instance, const char *service, const char *protocol,
                           unsigned port, void *txt, unsigned count);
