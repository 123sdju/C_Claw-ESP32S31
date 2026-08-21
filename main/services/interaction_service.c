/*
 * Interaction Service：用户命令与 Agent Runtime 之间的单一业务所有者。
 *
 * Web、Serial、UI 等入口只负责提交 typed command；本文件用一个 FreeRTOS queue
 * 把文本、清会话、配置、取消和 OTA maintenance 命令串行交给 owner task 处理。
 * 这样 controller/session/run 的状态迁移集中在同一执行上下文，入口层不直接并发调用
 * cc_runtime_controller。
 *
 * 关键边界：
 *   1. queue 传递命令和动态 payload 的所有权；成功入队后由 owner task 清理。
 *   2. mutex 只保护短状态快照，不包围模型请求、事件发布或其他长耗时操作。
 *   3. EventGroup 的 IDLE 位表示“当前没有服务级忙状态且命令队列为空”，不表示服务健康。
 *   4. maintenance/cancel 也经同一 queue 串行处理，不是能从任意线程抢占当前调用的带外通道。
 *   5. controller 内部仍可能有 pending/busy；对外状态会合并 service 与 controller 两层快照。
 */
#include "interaction_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_ipc.h"
#include "app_event_bus.h"
#include "app_profile.h"
#include "cc/app/cc_runtime_controller.h"
#include "cc/core/cc_stream_chunk.h"
#include "command_service.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define INTERACTION_SERVICE_TASK_STACK 12288U
#define INTERACTION_SERVICE_TASK_PRIORITY 4
#define INTERACTION_SYNC_TIMEOUT_MS 30000U
/* IDLE 供 OTA 等调用者等待队列排空；MAINTENANCE_OFF 表示本服务本地 pause 标志已解除。 */
#define INTERACTION_IDLE_BIT BIT0
#define INTERACTION_MAINTENANCE_OFF_BIT BIT1
#define INTERACTION_TASK_EXITED_BIT BIT2

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define INTERACTION_SERVICE_TASK_STACK_IN_PSRAM 1
#else
#define INTERACTION_SERVICE_TASK_STACK_IN_PSRAM 0
#endif

typedef enum {
    /* 以下命令全部由 interaction_service_task 串行解释，禁止入口层绕过队列改 controller。 */
    INTERACTION_COMMAND_START = 0,
    INTERACTION_COMMAND_SUBMIT_TEXT,
    INTERACTION_COMMAND_CLEAR_SESSION,
    INTERACTION_COMMAND_APPLY_CONFIG,
    INTERACTION_COMMAND_CANCEL_ACTIVE,
    INTERACTION_COMMAND_ENTER_MAINTENANCE,
    INTERACTION_COMMAND_LEAVE_MAINTENANCE,
    INTERACTION_COMMAND_SHUTDOWN,
} interaction_command_type_t;

typedef struct {
    /* session_id 是命令自有副本；queue_depth/max_input_bytes 是应用配置快照。 */
    char *session_id;
    int queue_depth;
    int max_input_bytes;
} interaction_config_payload_t;

typedef struct {
    /* 命令头：type 决定 union-like payload 的有效字段，request_id 用于同步回复关联。 */
    interaction_command_type_t type;
    app_request_id_t request_id;
    app_event_source_t source;
    char *text;                         /* SUBMIT_TEXT 的堆副本，owner task 处理后释放。 */
    char *reason;                       /* maintenance 原因的堆副本。 */
    cc_runtime_builder_t *runtime_builder;
    interaction_config_payload_t config;
    app_waiter_t *waiter;               /* 可选同步等待器，仅在调用者等待期间有效。 */
} interaction_command_t;

typedef struct {
    /* mutex 保护本结构的短读写；queue/task/events 创建后在当前固件生命周期内常驻。 */
    interaction_service_lifecycle_t state;
    bool started;
    bool busy;
    bool maintenance_paused;
    app_request_id_t next_request_id;
    int queue_depth;                    /* 配置值；队列创建后的物理容量不会被热更新重建。 */
    int max_input_bytes;
    cc_runtime_builder_t *runtime_builder;
    cc_runtime_controller_t *controller;
    QueueHandle_t queue;
    TaskHandle_t task;
    SemaphoreHandle_t mutex;
    EventGroupHandle_t events;
    uint32_t queue_high_water;
    uint32_t late_reply_count;
    uint32_t cancel_latency_ms;
    uint64_t cancel_requested_ms;
    uint64_t owner_heartbeat_ms;
} interaction_service_state_t;

static interaction_service_state_t s_interaction;

/* 将内部生命周期枚举转成稳定的观测字符串，供 status JSON 和日志使用。 */
const char *interaction_service_state_string(interaction_service_lifecycle_t state)
{
    switch (state) {
    case INTERACTION_SERVICE_STOPPED: return "stopped";
    case INTERACTION_SERVICE_STARTING: return "starting";
    case INTERACTION_SERVICE_IDLE: return "idle";
    case INTERACTION_SERVICE_QUEUED: return "queued";
    case INTERACTION_SERVICE_RUNNING: return "running";
    case INTERACTION_SERVICE_CLEARING: return "clearing";
    case INTERACTION_SERVICE_PAUSED_MAINTENANCE: return "paused_maintenance";
    case INTERACTION_SERVICE_CANCELLING: return "cancelling";
    case INTERACTION_SERVICE_FAILED: return "failed";
    default: return "unknown";
    }
}

/*
 * 判断生命周期是否属于“正在推进工作”的状态。
 * PAUSED/FAILED/STOPPED 不算 busy，但仍可能不可接收业务，因此不能只看 busy 判定可用性。
 */
static bool interaction_state_is_busy(interaction_service_lifecycle_t state)
{
    return state == INTERACTION_SERVICE_STARTING ||
           state == INTERACTION_SERVICE_QUEUED ||
           state == INTERACTION_SERVICE_RUNNING ||
           state == INTERACTION_SERVICE_CLEARING ||
           state == INTERACTION_SERVICE_CANCELLING;
}

/* 获取服务状态 mutex；只允许保护短临界区，禁止持锁调用 Runtime、EventBus 或阻塞队列。 */
static void interaction_lock(void)
{
    if (s_interaction.mutex) {
        xSemaphoreTake(s_interaction.mutex, portMAX_DELAY);
    }
}

/* 释放服务状态 mutex。 */
static void interaction_unlock(void)
{
    if (s_interaction.mutex) {
        xSemaphoreGive(s_interaction.mutex);
    }
}

/*
 * 为跨 task payload 创建独立副本，优先使用 PSRAM，失败时回退 Internal RAM。
 * 返回值统一用 heap_caps_free() 释放；空字符串也会分配一个结尾 NUL。
 */
static char *interaction_strdup(const char *text)
{
    size_t len = text ? strlen(text) : 0;
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) copy = heap_caps_malloc(len + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!copy) return NULL;
    if (len > 0 && text) memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

/*
 * 原地裁剪 ASCII 空白，返回原 buffer 内部的首字符指针。
 * 调用者仍须保存并释放原始分配地址，不能对返回的偏移指针执行 free。
 */
static char *trim_ascii(char *text)
{
    if (!text) return NULL;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return text;
}

/*
 * 原子更新 lifecycle/busy 快照，并据“状态 + 命令队列是否为空”维护 EventGroup。
 * EventGroup 操作放在 mutex 外，避免在服务锁内进入 FreeRTOS 唤醒路径。
 */
static void interaction_set_state(interaction_service_lifecycle_t state)
{
    interaction_lock();
    s_interaction.state = state;
    s_interaction.busy = interaction_state_is_busy(state);
    bool maintenance_off = !s_interaction.maintenance_paused;
    bool idle_like = (state == INTERACTION_SERVICE_IDLE ||
                      state == INTERACTION_SERVICE_PAUSED_MAINTENANCE ||
                      state == INTERACTION_SERVICE_FAILED ||
                      state == INTERACTION_SERVICE_STOPPED) &&
                     (!s_interaction.queue || uxQueueMessagesWaiting(s_interaction.queue) == 0);
    interaction_unlock();

    if (s_interaction.events) {
        if (idle_like) xEventGroupSetBits(s_interaction.events, INTERACTION_IDLE_BIT);
        else xEventGroupClearBits(s_interaction.events, INTERACTION_IDLE_BIT);
        if (maintenance_off) xEventGroupSetBits(s_interaction.events, INTERACTION_MAINTENANCE_OFF_BIT);
        else xEventGroupClearBits(s_interaction.events, INTERACTION_MAINTENANCE_OFF_BIT);
    }
}

/* 生成非零 request id；uint32 回绕到 0 时再递增一次，把 0 保留为“未分配”。 */
static app_request_id_t interaction_next_request_id(void)
{
    interaction_lock();
    app_request_id_t id = ++s_interaction.next_request_id;
    if (id == 0) {
        id = ++s_interaction.next_request_id;
    }
    interaction_unlock();
    return id;
}

/* 释放配置命令持有的 session_id，并清零 payload，允许失败路径重复调用 cleanup。 */
static void interaction_free_config_payload(interaction_config_payload_t *config)
{
    if (!config) return;
    heap_caps_free(config->session_id);
    memset(config, 0, sizeof(*config));
}

/*
 * 把 app_config 中的 IO 配置复制成可跨 task 传递的命令 payload。
 * queue depth 和最大输入长度在非正值时使用 8/1024；session id 使用独立堆副本。
 */
static esp_err_t interaction_copy_config_payload(
    const app_runtime_io_config_t *io,
    interaction_config_payload_t *out_config
)
{
    if (!io || !out_config) return ESP_ERR_INVALID_ARG;
    memset(out_config, 0, sizeof(*out_config));
    out_config->queue_depth = io->queue_depth > 0 ? io->queue_depth : 8;
    out_config->max_input_bytes = io->max_input_bytes > 0 ? io->max_input_bytes : 1024;
    out_config->session_id = interaction_strdup(io->session_id ? io->session_id : "esp32s31-main");
    return out_config->session_id ? ESP_OK : ESP_ERR_NO_MEM;
}

/* 完成可选同步 waiter；异步命令的 waiter 为 NULL，因此该函数是空操作。 */
static void interaction_complete(interaction_command_t *cmd, esp_err_t code, const char *message)
{
    if (cmd && cmd->waiter) {
        bool delivered = app_waiter_complete(cmd->waiter, code, message);
        if (!delivered) {
            interaction_lock();
            s_interaction.late_reply_count++;
            interaction_unlock();
        }
        app_waiter_release(cmd->waiter);
        cmd->waiter = NULL;
    }
}

/*
 * 释放命令的所有动态字段并清零结构。
 * 仅由“入队失败的生产者”或“成功出队后的 owner task”调用，避免双重释放。
 */
static void interaction_command_cleanup(interaction_command_t *cmd)
{
    if (!cmd) return;
    heap_caps_free(cmd->text);
    heap_caps_free(cmd->reason);
    if (cmd->waiter) app_waiter_release(cmd->waiter);
    interaction_free_config_payload(&cmd->config);
    memset(cmd, 0, sizeof(*cmd));
}

/*
 * 非阻塞入队 typed command。
 *
 * 成功：FreeRTOS queue 浅拷贝结构，text/reason/config 指针的释放责任转移给 owner task。
 * 失败：调用者仍持有 payload，必须调用 interaction_command_cleanup()。
 * 队列满直接返回 ESP_ERR_TIMEOUT，不在入口 task 中等待，形成显式背压。
 */
static esp_err_t interaction_enqueue_command(interaction_command_t *cmd)
{
    if (!cmd || !s_interaction.queue) return ESP_ERR_INVALID_STATE;
    cmd->request_id = interaction_next_request_id();
    if (cmd->waiter) {
        cmd->waiter->request_id = cmd->request_id;
    }
    if (s_interaction.events) {
        xEventGroupClearBits(s_interaction.events, INTERACTION_IDLE_BIT);
    }
    if (xQueueSend(s_interaction.queue, cmd, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    UBaseType_t queued = uxQueueMessagesWaiting(s_interaction.queue);
    interaction_lock();
    if ((uint32_t)queued > s_interaction.queue_high_water) {
        s_interaction.queue_high_water = (uint32_t)queued;
    }
    interaction_unlock();
    interaction_set_state(INTERACTION_SERVICE_QUEUED);
    return ESP_OK;
}

/*
 * 入队控制命令并永久等待 owner task 回复。
 * waiter 位于调用者栈上，所以必须等回复后才能销毁；成功入队后先清零本地 cmd，
 * 防止调用者错误清理已转移的 payload。
 *
 * 前置条件：不能从 interaction owner task 自己调用，否则 owner 会等待自己处理队列而死锁。
 */
static esp_err_t interaction_enqueue_sync_timeout(
    interaction_command_t *cmd,
    app_reply_t *out_reply,
    uint32_t timeout_ms)
{
    app_waiter_t *waiter = app_waiter_create(0);
    if (!waiter) return ESP_ERR_NO_MEM;
    app_waiter_retain(waiter); /* command/owner 引用 */
    cmd->waiter = waiter;
    esp_err_t err = interaction_enqueue_command(cmd);
    if (err == ESP_OK) {
        memset(cmd, 0, sizeof(*cmd));
        err = app_waiter_wait(waiter, pdMS_TO_TICKS(timeout_ms), out_reply);
    }
    app_waiter_release(waiter); /* 调用者引用；超时后 owner 仍安全持有对象 */
    return err;
}

static esp_err_t interaction_enqueue_sync(interaction_command_t *cmd, app_reply_t *out_reply)
{
    return interaction_enqueue_sync_timeout(cmd, out_reply, INTERACTION_SYNC_TIMEOUT_MS);
}

/*
 * Runtime streaming 回调：把 SDK chunk 翻译为固件 EventBus 事实事件。
 * first_chunk 只在本次提交首次收到有效 chunk 时采样；回调不拼装最终文本，
 * 文本/思考/tool/status/error 由下游 sink 按事件类型分别消费。
 */
static void stream_chunk_cb(const cc_runtime_controller_stream_event_t *event, void *user_data)
{
    bool *first_chunk_seen = (bool *)user_data;
    const cc_stream_chunk_t *chunk = event ? event->chunk : NULL;
    if (!chunk) return;
    if (first_chunk_seen && !*first_chunk_seen) {
        *first_chunk_seen = true;
        app_profile_sample("interaction", "first_chunk", false);
    }
    switch (chunk->type) {
    case CC_STREAM_CHUNK_TEXT:
        app_event_publish(APP_EVENT_KIND_ASSISTANT, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, chunk->content ? chunk->content : "");
        break;
    case CC_STREAM_CHUNK_THINKING:
        app_event_publish(APP_EVENT_KIND_THINKING, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, chunk->content ? chunk->content : "");
        break;
    case CC_STREAM_CHUNK_TOOL_START:
        app_event_publishf(APP_EVENT_KIND_TOOL,
                           APP_EVENT_SOURCE_TOOL,
                           CC_LOG_INFO,
                           "tool start %s",
                           chunk->tool_name ? chunk->tool_name : "(unknown)");
        break;
    case CC_STREAM_CHUNK_TOOL_DELTA:
        app_event_publishf(APP_EVENT_KIND_TOOL,
                           APP_EVENT_SOURCE_TOOL,
                           CC_LOG_DEBUG,
                           "args %s",
                           chunk->content ? chunk->content : "");
        break;
    case CC_STREAM_CHUNK_TOOL_END:
        app_event_publish(APP_EVENT_KIND_TOOL, APP_EVENT_SOURCE_TOOL, CC_LOG_INFO, "tool end");
        break;
    case CC_STREAM_CHUNK_FINISHED:
        app_profile_sample("interaction", "stream_finished", false);
        app_event_publish(APP_EVENT_KIND_RUN_STATE, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, "stream finished");
        break;
    case CC_STREAM_CHUNK_ARTIFACT:
        app_event_publish(APP_EVENT_KIND_TOOL, APP_EVENT_SOURCE_TOOL, CC_LOG_INFO, "artifact");
        break;
    case CC_STREAM_CHUNK_PROVIDER_WARNING:
        app_event_publish(APP_EVENT_KIND_STATUS, APP_EVENT_SOURCE_RUNTIME, CC_LOG_WARN, chunk->content ? chunk->content : "provider warning");
        break;
    case CC_STREAM_CHUNK_ERROR:
        app_event_publish(APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_RUNTIME, CC_LOG_ERROR, chunk->content ? chunk->content : "stream error");
        break;
    }
}

/* 在 mutex 内读取 controller 指针；只提供瞬时快照，不转移所有权。 */
static cc_runtime_controller_t *interaction_controller_snapshot(void)
{
    interaction_lock();
    cc_runtime_controller_t *controller = s_interaction.controller;
    interaction_unlock();
    return controller;
}

/* 在 mutex 内读取 runtime 指针；runtime 生命周期由 runtime_service 持有。 */
static cc_runtime_builder_t *interaction_builder_snapshot(void)
{
    interaction_lock();
    cc_runtime_builder_t *builder = s_interaction.runtime_builder;
    interaction_unlock();
    return builder;
}

/*
 * 一次命令完成后的统一状态收敛：失败进入 FAILED，成功则回到 IDLE 或 maintenance pause。
 * 后续排队命令仍可被 owner task 处理，FAILED 不是 task 退出状态。
 */
static void interaction_apply_state_after_work(bool ok)
{
    interaction_lock();
    bool paused = s_interaction.maintenance_paused;
    interaction_unlock();
    if (!ok) {
        interaction_set_state(INTERACTION_SERVICE_FAILED);
    } else {
        interaction_set_state(paused ? INTERACTION_SERVICE_PAUSED_MAINTENANCE : INTERACTION_SERVICE_IDLE);
    }
}

static bool interaction_input_blocked_for_maintenance(app_event_source_t source);

/*
 * 创建或热更新 Runtime Controller。
 * Controller 只允许一个 active run，业务排队由 owner task 独占；
 * drop_late_events 防止过期事件污染新的 run/generation。
 * controller 首次创建后由服务长期持有，本函数只替换/应用配置，不在这里销毁 runtime。
 */
static esp_err_t interaction_apply_controller_config(
    cc_runtime_builder_t *builder,
    const interaction_config_payload_t *config
)
{
    if (!builder || !config) return ESP_ERR_INVALID_ARG;

    cc_runtime_controller_config_t controller_config;
    memset(&controller_config, 0, sizeof(controller_config));
    controller_config.size = sizeof(controller_config);
    controller_config.session_base = config->session_id ? config->session_id : "esp32s31-main";
    controller_config.drop_late_events = 1;

    cc_result_t rc;
    interaction_lock();
    cc_runtime_controller_t *controller = s_interaction.controller;
    interaction_unlock();

    if (!controller) {
        cc_runtime_controller_t *new_controller = NULL;
        rc = cc_runtime_controller_create(builder, &controller_config, &new_controller);
        if (rc.code == CC_OK) {
            interaction_lock();
            s_interaction.controller = new_controller;
            interaction_unlock();
        }
    } else {
        rc = cc_runtime_controller_apply_config(controller, &controller_config);
    }

    if (rc.code != CC_OK) {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          APP_EVENT_SOURCE_RUNTIME,
                          CC_LOG_ERROR,
                          rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        return ESP_FAIL;
    }
    cc_result_free(&rc);

    cc_runtime_controller_status_t controller_status;
    cc_runtime_controller_get_status(interaction_controller_snapshot(), &controller_status);
    app_event_publishf(APP_EVENT_KIND_STATUS,
                       APP_EVENT_SOURCE_RUNTIME,
                       CC_LOG_INFO,
                       "session %s",
                       controller_status.session_id);
    return controller_status.session_id[0] ? ESP_OK : ESP_FAIL;
}

/* 处理 START：在 owner task 内绑定 runtime、应用 controller 配置并发布同步启动结果。 */
static void interaction_handle_start(interaction_command_t *cmd)
{
    interaction_set_state(INTERACTION_SERVICE_STARTING);
    interaction_lock();
    s_interaction.runtime_builder = cmd->runtime_builder;
    s_interaction.queue_depth = cmd->config.queue_depth;
    s_interaction.max_input_bytes = cmd->config.max_input_bytes;
    interaction_unlock();

    esp_err_t err = interaction_apply_controller_config(cmd->runtime_builder, &cmd->config);
    interaction_lock();
    s_interaction.started = err == ESP_OK;
    interaction_unlock();
    interaction_apply_state_after_work(err == ESP_OK);
    interaction_complete(cmd, err, err == ESP_OK ? "started" : "start failed");
}

/*
 * 处理配置热应用。生命周期暂记为 CLEARING，以阻止外部把重配置误判为 idle。
 * queue_depth 是物理资源，只在 start 创建 queue 时生效；热应用不得把状态快照更新为
 * 尚未实际生效的新容量。配置事务会用 restart_required 暴露这一差异。
 */
static void interaction_handle_apply_config(interaction_command_t *cmd)
{
    interaction_set_state(INTERACTION_SERVICE_CLEARING);
    interaction_lock();
    s_interaction.max_input_bytes = cmd->config.max_input_bytes;
    cc_runtime_builder_t *builder = s_interaction.runtime_builder;
    interaction_unlock();

    esp_err_t err = interaction_apply_controller_config(builder, &cmd->config);
    interaction_apply_state_after_work(err == ESP_OK);
    interaction_complete(cmd, err, err == ESP_OK ? "config applied" : "config failed");
}

/*
 * 清理 controller 当前 session，并在成功后清空 EventBus 对话视图。
 * maintenance 期间拒绝清理，避免 OTA 窗口发生 session/file 写入或新的控制状态迁移。
 */
static esp_err_t interaction_handle_clear(app_event_source_t source)
{
    if (interaction_input_blocked_for_maintenance(source)) {
        app_event_publish(APP_EVENT_KIND_STATUS,
                          source,
                          CC_LOG_WARN,
                          "OTA upload active; clear paused");
        return ESP_ERR_INVALID_STATE;
    }
    interaction_set_state(INTERACTION_SERVICE_CLEARING);
    cc_runtime_controller_t *controller = interaction_controller_snapshot();
    if (!controller) {
        app_event_publish(APP_EVENT_KIND_ERROR, source, CC_LOG_ERROR, "runtime unavailable");
        interaction_apply_state_after_work(false);
        return ESP_ERR_INVALID_STATE;
    }

    cc_result_t rc = cc_runtime_controller_clear_session(controller);
    if (rc.code != CC_OK) {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          source,
                          CC_LOG_ERROR,
                          rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        interaction_apply_state_after_work(false);
        return ESP_FAIL;
    }
    cc_result_free(&rc);
    app_event_bus_clear_conversation();
    app_event_publish(APP_EVENT_KIND_STATUS, source, CC_LOG_INFO, "session cleared");
    interaction_apply_state_after_work(true);
    return ESP_OK;
}

/*
 * 请求 controller 取消当前 generation，并将结果回复给同步调用者。
 * 该命令与文本命令共用 owner queue；若 owner 正阻塞在一次 submit_text 中，取消不会带外抢占，
 * 只能在 owner task 能继续取队列时执行。需要即时中断时应由 controller 提供独立线程安全通道。
 */
static void interaction_handle_cancel(interaction_command_t *cmd)
{
    interaction_set_state(INTERACTION_SERVICE_CANCELLING);
    uint64_t observed_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    interaction_lock();
    if (s_interaction.cancel_requested_ms > 0 &&
        observed_ms >= s_interaction.cancel_requested_ms) {
        uint64_t latency = observed_ms - s_interaction.cancel_requested_ms;
        s_interaction.cancel_latency_ms = latency > UINT32_MAX ?
            UINT32_MAX : (uint32_t)latency;
    }
    interaction_unlock();
    cc_runtime_controller_t *controller = interaction_controller_snapshot();
    esp_err_t err = ESP_OK;
    if (controller) {
        cc_runtime_controller_status_t status;
        cc_runtime_controller_get_status(controller, &status);
        cc_result_t rc = status.active_run_id ?
            cc_runtime_controller_cancel(controller, status.active_run_id) : cc_result_ok();
        err = rc.code == CC_OK ? ESP_OK : ESP_FAIL;
        if (rc.code != CC_OK) {
            app_event_publish(APP_EVENT_KIND_ERROR,
                              cmd->source,
                              CC_LOG_ERROR,
                              rc.message ? rc.message : cc_error_string(rc.code));
        }
        cc_result_free(&rc);
    }
    app_event_publish(APP_EVENT_KIND_RUN_STATE, cmd->source, CC_LOG_INFO, "cancel requested");
    interaction_apply_state_after_work(err == ESP_OK);
    interaction_complete(cmd, err, err == ESP_OK ? "cancelled" : "cancel failed");
}

/*
 * 串行进入/退出 maintenance pause，并维护本地 pause 标志和 lifecycle。
 * OTA 流程通常应先 wait_idle 再进入 pause，避免把“已排队”误当成“当前 run 已被抢占”。
 */
static void interaction_handle_maintenance(interaction_command_t *cmd, bool paused)
{
    interaction_lock();
    s_interaction.maintenance_paused = paused;
    interaction_unlock();
    if (paused) {
        app_event_publishf(APP_EVENT_KIND_STATUS,
                           cmd->source,
                           CC_LOG_WARN,
                           "interaction paused%s%s",
                           cmd->reason && cmd->reason[0] ? ": " : "",
                           cmd->reason && cmd->reason[0] ? cmd->reason : "");
        interaction_set_state(INTERACTION_SERVICE_PAUSED_MAINTENANCE);
    } else {
        app_event_publish(APP_EVENT_KIND_STATUS, cmd->source, CC_LOG_INFO, "interaction resumed");
        interaction_set_state(INTERACTION_SERVICE_IDLE);
    }
    interaction_complete(cmd, ESP_OK, paused ? "paused" : "resumed");
}

static void interaction_handle_shutdown(interaction_command_t *cmd)
{
    cc_runtime_controller_t *controller = interaction_controller_snapshot();
    esp_err_t err = ESP_OK;
    if (controller) {
        cc_result_t rc = cc_runtime_controller_destroy(
            controller, (int)INTERACTION_SYNC_TIMEOUT_MS);
        if (rc.code != CC_OK) {
            err = rc.code == CC_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
            app_event_publish(APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_RUNTIME,
                CC_LOG_ERROR, rc.message ? rc.message : cc_error_string(rc.code));
        } else {
            interaction_lock();
            if (s_interaction.controller == controller) s_interaction.controller = NULL;
            interaction_unlock();
        }
        cc_result_free(&rc);
    }
    interaction_lock();
    s_interaction.started = false;
    interaction_unlock();
    interaction_set_state(err == ESP_OK ? INTERACTION_SERVICE_STOPPED : INTERACTION_SERVICE_FAILED);
    interaction_complete(cmd, err, err == ESP_OK ? "stopped" : "stop failed");
}

/*
 * 在真正执行文本/清会话前再次检查本地 pause 与 EventBus 全局 maintenance。
 * 检查放在消费侧可覆盖“命令入队后、执行前”才进入 OTA 的竞态窗口。
 */
static bool interaction_input_blocked_for_maintenance(app_event_source_t source)
{
    interaction_lock();
    bool paused = s_interaction.maintenance_paused;
    interaction_unlock();
    if (!paused && !app_event_bus_maintenance_mode_enabled()) {
        return false;
    }
    app_event_publish(APP_EVENT_KIND_STATUS,
                      source,
                      CC_LOG_WARN,
                      "OTA upload active; input paused");
    interaction_set_state(INTERACTION_SERVICE_PAUSED_MAINTENANCE);
    return true;
}

/*
 * 文本命令主链：裁剪输入 -> maintenance gate -> 本地命令分流 -> controller 提交 ->
 * stream 事件转发 -> 非流式结果补发 -> 状态/profile 收敛。
 * 所有步骤都在 owner task 中串行执行，cmd->text 的原始地址由外层统一 cleanup。
 */
static void interaction_handle_text(interaction_command_t *cmd)
{
    app_profile_span_t job_span = app_profile_span_begin("interaction", "handle_text", NULL, NULL);
    char *input = trim_ascii(cmd->text);
    if (!input || input[0] == '\0') {
        app_profile_span_end(&job_span, "empty");
        interaction_apply_state_after_work(true);
        return;
    }
    if (interaction_input_blocked_for_maintenance(cmd->source)) {
        app_profile_span_end(&job_span, "maintenance");
        return;
    }

    interaction_set_state(INTERACTION_SERVICE_RUNNING);
    /* `/clear` 等本地命令优先消费；未处理文本才进入模型 Runtime。 */
    cc_runtime_generation_t *command_generation = NULL;
    cc_runtime_builder_t *builder = interaction_builder_snapshot();
    cc_result_t command_lease = builder ?
        cc_runtime_builder_acquire_generation(builder, &command_generation) :
        cc_result_error(CC_ERR_INVALID_STATE, "Runtime builder unavailable");
    command_service_context_t command_ctx = {
        .runtime = command_lease.code == CC_OK ?
            cc_runtime_generation_runtime(command_generation) : NULL,
        .source = cmd->source,
    };
    app_profile_span_t command_span = app_profile_span_begin("interaction", "command_dispatch", NULL, NULL);
    command_service_result_t command = command_service_handle(&command_ctx, input);
    if (command_generation) cc_runtime_generation_release(command_generation);
    cc_result_free(&command_lease);
    app_profile_span_end(&command_span, command == COMMAND_SERVICE_NOT_HANDLED ? "pass" : "handled");
    if (command == COMMAND_SERVICE_HANDLED) {
        interaction_apply_state_after_work(true);
        app_profile_span_end(&job_span, "command");
        return;
    }
    if (command == COMMAND_SERVICE_CLEAR) {
        esp_err_t clear_err = interaction_handle_clear(cmd->source);
        app_profile_span_end(&job_span, clear_err == ESP_OK ? "clear" : "clear_error");
        return;
    }

    cc_runtime_controller_t *controller = interaction_controller_snapshot();
    if (!controller) {
        app_event_publish(APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_RUNTIME, CC_LOG_ERROR, "runtime unavailable");
        interaction_apply_state_after_work(false);
        app_profile_span_end(&job_span, "error");
        return;
    }

    app_event_publish(APP_EVENT_KIND_USER, cmd->source, CC_LOG_INFO, input);
    app_event_publish(APP_EVENT_KIND_RUN_STATE, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, "working");

    char *response = NULL;
    bool first_chunk_seen = false;
    cc_runtime_controller_status_t submit_status;
    cc_runtime_controller_get_status(controller, &submit_status);
    app_profile_span_t submit_span = app_profile_span_begin("interaction",
                                                            "runtime_submit",
                                                            submit_status.session_id,
                                                            NULL);
    cc_result_t rc = cc_runtime_controller_submit_text(
        controller,
        input,
        stream_chunk_cb,
        &first_chunk_seen,
        &response);
    app_profile_span_end(&submit_span, rc.code == CC_OK ? "ok" : "error");
    /* 流式 provider 已通过 callback 发布文本；仅非流式 provider 需要补发完整 response。 */
    if (rc.code == CC_OK && response && response[0] && !first_chunk_seen) {
        app_event_publish(APP_EVENT_KIND_STATUS, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, "provider stream unsupported");
        app_event_publish(APP_EVENT_KIND_ASSISTANT, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, response);
    }
    if (rc.code != CC_OK) {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          APP_EVENT_SOURCE_RUNTIME,
                          CC_LOG_ERROR,
                          rc.message ? rc.message : cc_error_string(rc.code));
    }
    free(response);
    bool ok = rc.code == CC_OK;
    cc_result_free(&rc);
    app_event_publish(APP_EVENT_KIND_RUN_STATE, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, "idle");
    app_profile_sample("interaction", "idle", false);
    interaction_apply_state_after_work(ok);
    app_profile_span_end(&job_span, ok ? "ok" : "error");
}

/*
 * Interaction owner task 主循环。
 * 每次只取一个 typed command，handler 返回后统一释放 payload，再依据队列/忙状态收敛到 idle-like。
 * profile 中保留的 `worker_dequeue` 是指标名称；当前架构语义应表述为 single owner task。
 */
static void interaction_service_task(void *arg)
{
    (void)arg;
    interaction_command_t cmd;
    bool stop = false;
    while (!stop) {
        if (xQueueReceive(s_interaction.queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        interaction_lock();
        s_interaction.owner_heartbeat_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
        interaction_unlock();
        app_profile_sample("interaction", "worker_dequeue", false);
        switch (cmd.type) {
        case INTERACTION_COMMAND_START:
            interaction_handle_start(&cmd);
            break;
        case INTERACTION_COMMAND_SUBMIT_TEXT:
            interaction_handle_text(&cmd);
            break;
        case INTERACTION_COMMAND_CLEAR_SESSION: {
            esp_err_t err = interaction_handle_clear(cmd.source);
            interaction_complete(&cmd, err, err == ESP_OK ? "cleared" : "clear failed");
            break;
        }
        case INTERACTION_COMMAND_APPLY_CONFIG:
            interaction_handle_apply_config(&cmd);
            break;
        case INTERACTION_COMMAND_CANCEL_ACTIVE:
            interaction_handle_cancel(&cmd);
            break;
        case INTERACTION_COMMAND_ENTER_MAINTENANCE:
            interaction_handle_maintenance(&cmd, true);
            break;
        case INTERACTION_COMMAND_LEAVE_MAINTENANCE:
            interaction_handle_maintenance(&cmd, false);
            break;
        case INTERACTION_COMMAND_SHUTDOWN:
            interaction_handle_shutdown(&cmd);
            stop = true;
            break;
        default:
            interaction_complete(&cmd, ESP_ERR_INVALID_ARG, "unknown command");
            interaction_apply_state_after_work(false);
            break;
        }
        interaction_command_cleanup(&cmd);
        interaction_lock();
        interaction_service_lifecycle_t state = s_interaction.state;
        bool paused = s_interaction.maintenance_paused;
        bool busy = s_interaction.busy;
        bool empty = !s_interaction.queue || uxQueueMessagesWaiting(s_interaction.queue) == 0;
        interaction_unlock();
        if (!stop && empty && !busy && state != INTERACTION_SERVICE_FAILED && state != INTERACTION_SERVICE_STOPPED) {
            interaction_set_state(paused ? INTERACTION_SERVICE_PAUSED_MAINTENANCE : INTERACTION_SERVICE_IDLE);
        }
    }
    interaction_lock();
    s_interaction.task = NULL;
    interaction_unlock();
    if (s_interaction.events) xEventGroupSetBits(s_interaction.events, INTERACTION_TASK_EXITED_BIT);
    vTaskDelete(NULL);
}

/*
 * 创建 owner task。允许扩展内存栈时优先分配 12 KiB PSRAM 栈，失败回退 Internal RAM；
 * 未启用相关配置时使用普通 xTaskCreate。任务优先级固定为 4。
 */
static BaseType_t create_interaction_task(void)
{
#if INTERACTION_SERVICE_TASK_STACK_IN_PSRAM
    BaseType_t ok = xTaskCreateWithCaps(interaction_service_task,
                                        "interaction_service",
                                        INTERACTION_SERVICE_TASK_STACK,
                                        NULL,
                                        INTERACTION_SERVICE_TASK_PRIORITY,
                                        &s_interaction.task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) return ok;
    return xTaskCreateWithCaps(interaction_service_task,
                               "interaction_service",
                               INTERACTION_SERVICE_TASK_STACK,
                               NULL,
                               INTERACTION_SERVICE_TASK_PRIORITY,
                               &s_interaction.task,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(interaction_service_task,
                       "interaction_service",
                       INTERACTION_SERVICE_TASK_STACK,
                       NULL,
                       INTERACTION_SERVICE_TASK_PRIORITY,
                       &s_interaction.task);
#endif
}

/*
 * 按 mutex -> EventGroup -> queue -> task 的顺序创建服务原语。
 * queue_depth 只在首次 start 创建物理 queue 时生效，后续变更由配置层标记重启生效。
 * queue 优先使用 capability-aware PSRAM 分配，失败回退普通 FreeRTOS queue。
 */
static esp_err_t ensure_interaction_primitives(int queue_depth)
{
    if (!s_interaction.mutex) {
        s_interaction.mutex = xSemaphoreCreateMutex();
        if (!s_interaction.mutex) return ESP_ERR_NO_MEM;
    }
    if (!s_interaction.events) {
        s_interaction.events = xEventGroupCreate();
        if (!s_interaction.events) return ESP_ERR_NO_MEM;
        xEventGroupSetBits(s_interaction.events, INTERACTION_IDLE_BIT | INTERACTION_MAINTENANCE_OFF_BIT);
    }
    if (!s_interaction.queue) {
        int depth = queue_depth > 0 ? queue_depth : 8;
#if configSUPPORT_STATIC_ALLOCATION
        s_interaction.queue = xQueueCreateWithCaps((UBaseType_t)depth,
                                                   sizeof(interaction_command_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
        if (!s_interaction.queue) {
            s_interaction.queue = xQueueCreate((UBaseType_t)depth, sizeof(interaction_command_t));
        }
        if (!s_interaction.queue) return ESP_ERR_NO_MEM;
    }
    if (!s_interaction.task) {
        xEventGroupClearBits(s_interaction.events, INTERACTION_TASK_EXITED_BIT);
        BaseType_t ok = create_interaction_task();
        if (ok != pdPASS) return ESP_FAIL;
        app_profile_register_task("interaction", s_interaction.task);
    }
    return ESP_OK;
}

/*
 * 同步启动服务：确保原语存在，把 runtime/config 作为 START 命令交给 owner task，等待结果。
 * runtime 指针只借用、不销毁；配置字符串在入队前复制，避免调用者对象生命周期穿越 task。
 */
esp_err_t interaction_service_start(const app_runtime_io_config_t *io, cc_runtime_builder_t *builder)
{
    if (!io || !builder) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ensure_interaction_primitives(io->queue_depth);
    if (err != ESP_OK) return err;

    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = INTERACTION_COMMAND_START;
    cmd.source = APP_EVENT_SOURCE_RUNTIME;
    cmd.runtime_builder = builder;
    err = interaction_copy_config_payload(io, &cmd.config);
    if (err != ESP_OK) return err;

    app_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    err = interaction_enqueue_sync(&cmd, &reply);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
    }
    return err;
}

esp_err_t interaction_service_stop(uint32_t timeout_ms)
{
    if (!s_interaction.task) return ESP_OK;
    if (timeout_ms == 0) timeout_ms = INTERACTION_SYNC_TIMEOUT_MS;

    interaction_lock();
    s_interaction.started = false;
    interaction_unlock();

    cc_runtime_controller_t *controller = interaction_controller_snapshot();
    if (controller) {
        cc_runtime_controller_status_t status;
        cc_runtime_controller_get_status(controller, &status);
        if (status.active_run_id) {
            cc_result_t cancel = cc_runtime_controller_cancel(controller, status.active_run_id);
            cc_result_free(&cancel);
        }
    }

    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = INTERACTION_COMMAND_SHUTDOWN;
    cmd.source = APP_EVENT_SOURCE_SYSTEM;
    app_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    esp_err_t err = interaction_enqueue_sync_timeout(&cmd, &reply, timeout_ms);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
        return err;
    }
    EventBits_t bits = xEventGroupWaitBits(s_interaction.events,
        INTERACTION_TASK_EXITED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & INTERACTION_TASK_EXITED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t interaction_service_destroy(void)
{
    interaction_lock();
    bool can_destroy = s_interaction.task == NULL && s_interaction.controller == NULL;
    interaction_unlock();
    if (!can_destroy) return ESP_ERR_INVALID_STATE;

    interaction_command_t pending;
    while (s_interaction.queue && xQueueReceive(s_interaction.queue, &pending, 0) == pdTRUE) {
        interaction_command_cleanup(&pending);
    }
    if (s_interaction.queue) vQueueDelete(s_interaction.queue);
    if (s_interaction.events) vEventGroupDelete(s_interaction.events);
    if (s_interaction.mutex) vSemaphoreDelete(s_interaction.mutex);
    memset(&s_interaction, 0, sizeof(s_interaction));
    return ESP_OK;
}

/*
 * 同步热应用 session/controller 配置。
 * 该接口等待 owner task 完成；调用者不能位于 interaction owner task，也不能假设 queue 物理容量改变。
 */
esp_err_t interaction_service_apply_config(const app_runtime_io_config_t *io)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!s_interaction.queue) return ESP_ERR_INVALID_STATE;
    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = INTERACTION_COMMAND_APPLY_CONFIG;
    cmd.source = APP_EVENT_SOURCE_SYSTEM;
    esp_err_t err = interaction_copy_config_payload(io, &cmd.config);
    if (err != ESP_OK) return err;

    app_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    err = interaction_enqueue_sync(&cmd, &reply);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
    }
    return err;
}

/*
 * 带外请求取消：先直接触发 controller cancel source，再异步排队状态收敛命令。
 * 这条路径不等待 owner task，因此模型/HTTP 阻塞期间也能及时传播取消。
 */
esp_err_t interaction_service_request_cancel(app_event_source_t source)
{
    cc_runtime_controller_t *controller = interaction_controller_snapshot();
    if (!controller) return ESP_ERR_INVALID_STATE;

    interaction_lock();
    s_interaction.cancel_requested_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    interaction_unlock();
    cc_runtime_controller_status_t status;
    cc_runtime_controller_get_status(controller, &status);
    if (status.active_run_id == 0) return ESP_OK;
    cc_result_t rc = cc_runtime_controller_cancel(controller, status.active_run_id);
    if (rc.code != CC_OK) {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          source,
                          CC_LOG_ERROR,
                          rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        return ESP_FAIL;
    }
    cc_result_free(&rc);
    app_event_publish(APP_EVENT_KIND_RUN_STATE, source, CC_LOG_INFO, "cancel signalled");

    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = INTERACTION_COMMAND_CANCEL_ACTIVE;
    cmd.source = source;
    esp_err_t err = interaction_enqueue_command(&cmd);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
        /* cancel 已成功传播；队列满只影响观测状态收敛，不改变取消结果。 */
        return ESP_OK;
    }
    memset(&cmd, 0, sizeof(cmd));
    return ESP_OK;
}

/*
 * 同步切换 maintenance pause，reason 被复制后随命令转移所有权。
 * paused 只控制 Interaction 输入面；完整 OTA maintenance 还依赖 EventBus/Camera/Web 等模块协作。
 */
esp_err_t interaction_service_set_maintenance_paused(bool paused, const char *reason)
{
    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = paused ? INTERACTION_COMMAND_ENTER_MAINTENANCE : INTERACTION_COMMAND_LEAVE_MAINTENANCE;
    cmd.source = APP_EVENT_SOURCE_SYSTEM;
    cmd.reason = interaction_strdup(reason ? reason : "");
    if (!cmd.reason) return ESP_ERR_NO_MEM;
    app_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    esp_err_t err = interaction_enqueue_sync(&cmd, &reply);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
    }
    return err;
}

/*
 * 异步提交用户文本。
 * 入口先按当前 max_input_bytes 限长，再复制 payload 并零等待入队；`/clear` 被编码成专用命令。
 * 返回 ESP_OK 只表示已经排队，不表示模型执行成功；最终结果通过 EventBus 和状态接口观察。
 */
esp_err_t interaction_service_submit_text(app_event_source_t source, const char *text)
{
    if (!text || !s_interaction.queue) return ESP_ERR_INVALID_STATE;
    interaction_lock();
    bool accepting = s_interaction.started;
    interaction_unlock();
    if (!accepting) return ESP_ERR_INVALID_STATE;
    app_profile_span_t span = app_profile_span_begin("interaction", "submit_text", NULL, NULL);

    interaction_lock();
    int max_input_bytes = s_interaction.max_input_bytes > 0 ? s_interaction.max_input_bytes : 1024;
    interaction_unlock();
    size_t len = strlen(text);
    if (len == 0 || len > (size_t)max_input_bytes) {
        app_profile_span_end(&span, "invalid_size");
        return ESP_ERR_INVALID_SIZE;
    }

    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = strcmp(text, "/clear") == 0 ? INTERACTION_COMMAND_CLEAR_SESSION : INTERACTION_COMMAND_SUBMIT_TEXT;
    cmd.source = source;
    cmd.text = interaction_strdup(text);
    if (!cmd.text) {
        app_profile_span_end(&span, "oom");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = interaction_enqueue_command(&cmd);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
        app_event_publish(APP_EVENT_KIND_STATUS,
                          source,
                          CC_LOG_WARN,
                          "interaction queue full");
        app_profile_span_end(&span, "queue_full");
        return err;
    }
    app_event_publish(APP_EVENT_KIND_RUN_STATE, source, CC_LOG_INFO, "queued");
    app_profile_span_end(&span, "queued");
    return ESP_OK;
}

/* 异步提交显式清会话命令；返回语义同 submit_text，仅表示是否成功进入 owner queue。 */
esp_err_t interaction_service_submit_clear(app_event_source_t source)
{
    if (!s_interaction.queue) return ESP_ERR_INVALID_STATE;
    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = INTERACTION_COMMAND_CLEAR_SESSION;
    cmd.source = source;
    esp_err_t err = interaction_enqueue_command(&cmd);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
        app_event_publish(APP_EVENT_KIND_STATUS,
                          source,
                          CC_LOG_WARN,
                          "interaction queue full");
        return err;
    }
    app_event_publish(APP_EVENT_KIND_RUN_STATE, source, CC_LOG_INFO, "queued");
    return ESP_OK;
}

/*
 * 等待服务进入 idle-like：命令队列为空，且 lifecycle 不处于 busy 集合。
 * timeout_ms=0 使用服务默认有限超时；FAILED/STOPPED/PAUSED 在队列为空时也会置 IDLE 位，
 * 因此返回 true 只证明“无进行中的服务级工作”，调用者仍须另查 status 判断健康/maintenance。
 */
bool interaction_service_wait_idle(uint32_t timeout_ms)
{
    if (!s_interaction.events) return true;
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms == 0 ?
                                     INTERACTION_SYNC_TIMEOUT_MS : timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_interaction.events,
                                           INTERACTION_IDLE_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           ticks);
    return (bits & INTERACTION_IDLE_BIT) != 0;
}

/*
 * 获取观测快照：先复制 service 状态，再合并 controller 的 busy/generation/pending/session。
 * service queue pending 与 controller pending 相加，busy 取两层 OR；快照不用于控制状态迁移。
 */
void interaction_service_get_status(interaction_service_status_t *out_status)
{
    if (!out_status) return;
    memset(out_status, 0, sizeof(*out_status));

    interaction_lock();
    cc_runtime_controller_t *controller = s_interaction.controller;
    cc_runtime_builder_t *builder = s_interaction.runtime_builder;
    out_status->state = s_interaction.state;
    out_status->started = s_interaction.started;
    out_status->busy = s_interaction.busy;
    out_status->queue_depth = s_interaction.queue_depth;
    out_status->max_input_bytes = s_interaction.max_input_bytes;
    out_status->pending = s_interaction.queue ? (int)uxQueueMessagesWaiting(s_interaction.queue) : 0;
    out_status->queue_high_water = s_interaction.queue_high_water;
    out_status->late_reply_count = s_interaction.late_reply_count;
    out_status->cancel_latency_ms = s_interaction.cancel_latency_ms;
    out_status->owner_heartbeat_ms = s_interaction.owner_heartbeat_ms;
    interaction_unlock();

    cc_runtime_controller_status_t controller_status;
    memset(&controller_status, 0, sizeof(controller_status));
    cc_runtime_controller_get_status(controller, &controller_status);
    out_status->busy = out_status->busy || controller_status.active_run_id != 0;
    out_status->generation = (unsigned)controller_status.runtime_generation;
    cc_runtime_generation_t *generation = NULL;
    cc_result_t lease = builder ? cc_runtime_builder_acquire_generation(builder, &generation) :
        cc_result_error(CC_ERR_INVALID_STATE, "Runtime builder unavailable");
    cc_agent_runtime_t *runtime = lease.code == CC_OK ?
        cc_runtime_generation_runtime(generation) : NULL;
    out_status->thinking_enabled = runtime ? cc_agent_runtime_get_thinking_mode(runtime) != 0 : false;
    out_status->stream_supported = runtime ? cc_agent_runtime_supports_stream(runtime) != 0 : false;
    if (generation) cc_runtime_generation_release(generation);
    cc_result_free(&lease);
    snprintf(out_status->session_id, sizeof(out_status->session_id), "%s", controller_status.session_id);
}
