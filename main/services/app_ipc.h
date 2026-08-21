#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef uint32_t app_request_id_t;

typedef struct {
    app_request_id_t request_id;
    esp_err_t code;
    char message[96];
} app_reply_t;

typedef struct {
    app_request_id_t request_id;
    QueueHandle_t queue;
    unsigned refs;
    bool expired;
} app_waiter_t;

/* waiter 是双引用对象：调用者和 owner command 各持有一个引用。 */
static inline app_waiter_t *app_waiter_create(app_request_id_t request_id)
{
    app_waiter_t *waiter = (app_waiter_t *)calloc(1, sizeof(*waiter));
    if (!waiter) return NULL;
    waiter->request_id = request_id;
    waiter->queue = xQueueCreate(1, sizeof(app_reply_t));
    if (!waiter->queue) {
        free(waiter);
        return NULL;
    }
    waiter->refs = 1;
    return waiter;
}

static inline void app_waiter_retain(app_waiter_t *waiter)
{
    if (waiter) (void)__atomic_add_fetch(&waiter->refs, 1U, __ATOMIC_ACQ_REL);
}

static inline void app_waiter_release(app_waiter_t *waiter)
{
    if (!waiter) return;
    if (__atomic_sub_fetch(&waiter->refs, 1U, __ATOMIC_ACQ_REL) != 0U) return;
    if (waiter->queue) vQueueDelete(waiter->queue);
    waiter->queue = NULL;
    free(waiter);
}

static inline bool app_waiter_complete(app_waiter_t *waiter, esp_err_t code, const char *message)
{
    if (!waiter || !waiter->queue ||
        __atomic_load_n(&waiter->expired, __ATOMIC_ACQUIRE)) return false;
    app_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.request_id = waiter->request_id;
    reply.code = code;
    snprintf(reply.message, sizeof(reply.message), "%s", message ? message : "");
    /* 调用者可能已超时；零等待发送让迟到回复安全丢弃，绝不阻塞 owner task。 */
    return xQueueSend(waiter->queue, &reply, 0) == pdTRUE;
}

static inline esp_err_t app_waiter_wait(app_waiter_t *waiter, TickType_t timeout, app_reply_t *out_reply)
{
    if (!waiter || !waiter->queue) return ESP_ERR_INVALID_ARG;
    app_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    if (xQueueReceive(waiter->queue, &reply, timeout) != pdTRUE) {
        __atomic_store_n(&waiter->expired, true, __ATOMIC_RELEASE);
        return ESP_ERR_TIMEOUT;
    }
    if (out_reply) {
        *out_reply = reply;
    }
    return reply.code;
}
