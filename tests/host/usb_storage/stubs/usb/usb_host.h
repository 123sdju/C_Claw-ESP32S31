#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct { int intr_flags; } usb_host_config_t;
#define USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS 1U
esp_err_t usb_host_install(const usb_host_config_t *config);
esp_err_t usb_host_uninstall(void);
esp_err_t usb_host_lib_handle_events(uint32_t timeout,uint32_t *flags);
esp_err_t usb_host_device_free_all(void);
