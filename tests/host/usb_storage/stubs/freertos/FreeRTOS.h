#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define configSUPPORT_STATIC_ALLOCATION 0
