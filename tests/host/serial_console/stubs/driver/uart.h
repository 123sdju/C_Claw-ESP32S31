#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int uart_port_t;
typedef struct uart_config { int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk; } uart_config_t;
enum { UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1, UART_HW_FLOWCTRL_DISABLE, UART_SCLK_DEFAULT };
esp_err_t uart_param_config(uart_port_t port,const uart_config_t *config);
esp_err_t uart_driver_install(uart_port_t port,int rx,int tx,int queue,void *event_queue,int flags);
int uart_read_bytes(uart_port_t port,uint8_t *bytes,uint32_t length,TickType_t timeout);
esp_err_t uart_flush_input(uart_port_t port);
