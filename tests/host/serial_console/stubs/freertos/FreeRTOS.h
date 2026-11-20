#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configTICK_RATE_HZ 1000U
#define portMAX_DELAY UINT32_MAX
#define configSUPPORT_STATIC_ALLOCATION 0
