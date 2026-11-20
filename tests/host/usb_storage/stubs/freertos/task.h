#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreate(TaskFunction_t fn,const char *name,uint32_t stack,void *arg,UBaseType_t priority,TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t handle);
