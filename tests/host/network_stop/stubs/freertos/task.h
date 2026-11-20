#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskCreate(TaskFunction_t task,const char *name,uint32_t stack,void *arg,
                       UBaseType_t priority,TaskHandle_t *out);
void vTaskDelete(TaskHandle_t task);
