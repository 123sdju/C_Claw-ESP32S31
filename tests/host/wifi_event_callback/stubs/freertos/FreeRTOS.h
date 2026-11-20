#pragma once
#include <pthread.h>
#include <stdint.h>
typedef pthread_mutex_t portMUX_TYPE;
typedef uint32_t TickType_t;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configTICK_RATE_HZ 1000U
#define portMAX_DELAY UINT32_MAX
