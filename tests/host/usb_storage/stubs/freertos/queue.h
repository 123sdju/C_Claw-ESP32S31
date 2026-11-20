#pragma once
#include <stddef.h>
#include "freertos/FreeRTOS.h"
typedef void *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t count,size_t item_size);
BaseType_t xQueueSend(QueueHandle_t queue,const void *item,unsigned wait);
