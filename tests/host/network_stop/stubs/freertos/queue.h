#pragma once
#include <stddef.h>
#include "FreeRTOS.h"
typedef void *QueueHandle_t;
typedef void *QueueSetHandle_t;
typedef void *QueueSetMemberHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t count,size_t size);
QueueSetHandle_t xQueueCreateSet(UBaseType_t count);
BaseType_t xQueueAddToSet(QueueHandle_t queue,QueueSetHandle_t set);
BaseType_t xQueueRemoveFromSet(QueueHandle_t queue,QueueSetHandle_t set);
QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t set,TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue,void *item,TickType_t wait);
BaseType_t xQueueSend(QueueHandle_t queue,const void *item,TickType_t wait);
BaseType_t xQueueOverwrite(QueueHandle_t queue,const void *item);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);
void vQueueDelete(QueueHandle_t queue);
