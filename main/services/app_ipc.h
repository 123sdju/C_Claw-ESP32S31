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

/**
 * @brief 创建带单回复队列的拥有等待器。
 * @param[in] request_id 待关联请求 ID，可在发布命令前由生产者更新。
 * @return 初始持有一份引用的等待器，结构或队列分配失败为 NULL。
 * @note 调用者把命令交给 owner 前另 retain 一份引用，最终各自 release。
 */
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

/**
 * @brief 为后续独立拥有者增加一份等待器引用。
 * @param[in,out] waiter 可为 NULL，非 NULL 时调用者已经保有覆盖本次调用的有效引用。
 * @post 非 NULL 对象的原子引用计数增加，调用方对应一次后续 release。
 * @pre 活跃引用总数可由 unsigned 表示，禁止在最后释放后再次登记。
 */
static inline void app_waiter_retain(app_waiter_t *waiter)
{
    if (waiter) (void)__atomic_add_fetch(&waiter->refs, 1U, __ATOMIC_ACQ_REL);
}

/**
 * @brief 消费一份等待器引用，并由最后拥有者回收回复队列和结构。
 * @param[in,out] waiter 可为 NULL 的拥有引用，函数不清调用者变量。
 * @post 最后一次原子归还后对象失效，其他拥有者保有引用时队列继续存活。
 * @pre 非 NULL 参数对应调用者尚未归还的一份引用。
 */
static inline void app_waiter_release(app_waiter_t *waiter)
{
    if (!waiter) return;
    if (__atomic_sub_fetch(&waiter->refs, 1U, __ATOMIC_ACQ_REL) != 0U) return;
    if (waiter->queue) vQueueDelete(waiter->queue);
    waiter->queue = NULL;
    free(waiter);
}

/**
 * @brief 为尚未标记过期的等待器零等待发送一条按值回复。
 * @param[in,out] waiter 可为空的借用对象，调用方持有引用至本次函数返回。
 * @param[in] code 转交给等待者的 ESP 状态码。
 * @param[in] message 可为 NULL 的 NUL 文本借用，按 96 B 消息槽截断并终止。
 * @return 回复成功进入队列为 true，参数无效、已观察过期或队满为 false。
 * @note 超时标记可能与本次入队并发，true 只证明排队，调用方随后仍需归还 owner 引用。
 */
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

/**
 * @brief 等待唯一回复，超时时原子发布过期标记。
 * @param[in,out] waiter 必填借用对象，调用者保有一份引用直到返回及后续 release。
 * @param[in] timeout 原生 tick 等待预算，零立即检查，portMAX_DELAY 沿 FreeRTOS 配置语义。
 * @param[out] out_reply 可为空，收到回复即交付按值副本，参数无效或未收到回复时保留原槽。
 * @return 收到时返回回复的 code，参数无效为 INVALID_ARG，未收到为 TIMEOUT。
 * @pre 每个等待器只有一个等待者，超时不撤销 owner 命令，也不消费调用者引用。
 */
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
