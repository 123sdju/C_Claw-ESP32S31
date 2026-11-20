#pragma once
void host_serial_log_error(const char *tag, const char *format, ...);
#define ESP_LOGE(...) host_serial_log_error(__VA_ARGS__)
#define ESP_LOGI(...) ((void)0)
