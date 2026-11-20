#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
