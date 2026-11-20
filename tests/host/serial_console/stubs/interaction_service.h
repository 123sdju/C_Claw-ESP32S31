#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t interaction_service_submit_clear(int source);
esp_err_t interaction_service_submit_text(int source,const char *text);
bool interaction_service_wait_idle(uint32_t timeout_ms);
