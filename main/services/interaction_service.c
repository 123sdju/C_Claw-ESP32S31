/*
 * 交互服务是用户命令与 Agent Runtime 之间的单一业务拥有者。
 *
 * Web、串口与 UI 入口提交按类型区分的命令，本文件用 FreeRTOS 队列
 * 把文本、清屏、配置、取消和 OTA 维护命令串行交给 owner task 处理。
 * 这样 controller/session/run 的状态迁移集中在同一执行上下文，入口层不直接并发调用
 * cc_runtime_controller。
 *
 * 生命周期与资源边界如下。
 *   1. queue 传递命令和动态 payload 的所有权；成功入队后由 owner task 清理。
 *   2. mutex 只保护短状态快照，不包围模型请求、事件发布或其他长耗时操作。
 *   3. EventGroup 的 IDLE 位表示“当前没有服务级忙状态且命令队列为空”，不表示服务健康。
 *   4. 取消先带外触发线程安全令牌，再经队列收敛状态，维护命令由同一 owner 执行。
 *   5. 状态查询登记覆盖 controller/builder 借用，停止先关新登记并排空既有 API。
 *   6. 原生退出事件返回后另发布最终完成位，生命周期 owner 再回收动态同步资源。
 */
#include "interaction_service.h"

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_ipc.h"
#include "app_event_bus.h"
#include "app_profile.h"
#include "cc/app/cc_runtime_controller.h"
#include "cc/core/cc_stream_chunk.h"
#include "command_service.h"
#include "recovery_console.h"
#include "session_menu.h"
#include "manual_lua_service.h"
#include "memory_console.h"
#include "usb_storage_service.h"
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
#define INTERACTION_SHUTDOWN_REPLY_BIT BIT3
#define INTERACTION_MEMORY_RETRY_MS 30000U

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define INTERACTION_SERVICE_TASK_STACK_IN_PSRAM 1
#else
#define INTERACTION_SERVICE_TASK_STACK_IN_PSRAM 0
#endif

typedef enum {
    /* 以下命令全部由 interaction_service_task 串行解释，禁止入口层绕过队列改 controller。 */
    INTERACTION_COMMAND_START = 0,
    INTERACTION_COMMAND_SUBMIT_TEXT,
    INTERACTION_COMMAND_CLEAR_DISPLAY,
    INTERACTION_COMMAND_APPLY_CONFIG,
    INTERACTION_COMMAND_CANCEL_ACTIVE,
    INTERACTION_COMMAND_ENTER_MAINTENANCE,
    INTERACTION_COMMAND_LEAVE_MAINTENANCE,
    INTERACTION_COMMAND_SHUTDOWN,
} interaction_command_type_t;

typedef enum {
    INTERACTION_SHUTDOWN_NONE = 0,
    INTERACTION_SHUTDOWN_REQUESTED,
    INTERACTION_SHUTDOWN_RETRYABLE,
    INTERACTION_SHUTDOWN_EXITING,
} interaction_shutdown_phase_t;

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
    recovery_service_t *recovery;        /* 借用，必须比 interaction 长寿。 */
    manual_lua_policy_t manual_lua_policy;
    cc_managed_memory_editor_t *memory_editor; /* 借用。 */
    session_memory_turns_t *memory_turns; /* 借用，必须比 interaction 长寿。 */
    interaction_config_payload_t config;
    app_waiter_t *waiter;               /* 命令自有引用，调用者超时后继续存活至 owner 归还。 */
} interaction_command_t;

typedef struct {
    /* mutex 保护短读写，队列与事件组存活到本次服务最终完成并通过 API 排空屏障。 */
    interaction_service_lifecycle_t state;
    bool started;
    bool busy;
    bool maintenance_paused;
    bool stopping;
    interaction_shutdown_phase_t shutdown_phase;
    esp_err_t shutdown_result;
    app_request_id_t next_request_id;
    int queue_depth;                    /* 配置值；队列创建后的物理容量不会被热更新重建。 */
    int max_input_bytes;
    cc_runtime_builder_t *runtime_builder;
    cc_runtime_controller_t *controller;
    recovery_service_t *recovery;
    manual_lua_policy_t manual_lua_policy;
    session_menu_t *session_menu;       /* 仅 owner task 访问。 */
    recovery_console_t *recovery_console;
    memory_console_t *memory_console;
    session_memory_turns_t *memory_turns; /* 仅 owner task 使用，生命周期由外层持有。 */
    cc_cancel_source_t *memory_cancel;   /* mutex 保护；仅 owner task 销毁。 */
    cc_error_code_t memory_last_error;
    QueueHandle_t queue;
    bool queue_with_caps;
    TaskHandle_t task;
    SemaphoreHandle_t mutex;
    EventGroupHandle_t events;
    uint32_t queue_high_water;
    uint32_t state_generation;
    uint32_t late_reply_count;
    uint32_t cancel_latency_ms;
    uint64_t cancel_requested_ms;
    uint64_t owner_heartbeat_ms;
} interaction_service_state_t;

static interaction_service_state_t s_interaction;
static portMUX_TYPE s_interaction_access_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_interaction_access_open;
static unsigned s_interaction_access_users;
static bool s_interaction_task_completed = true;

/**
 * @brief 在进程期短临界区登记一个允许使用动态服务资源的 API 调用。
 * @return 入口开放且计数有容量时返回 true，调用者随后必须 release。
 * @note 登记完成后才可读取队列、mutex、controller 或借用的 builder。
 */
static bool interaction_access_acquire(void)
{
    portENTER_CRITICAL(&s_interaction_access_mux);
    bool allowed = s_interaction_access_open && s_interaction_access_users < UINT_MAX;
    if (allowed) s_interaction_access_users++;
    portEXIT_CRITICAL(&s_interaction_access_mux);
    return allowed;
}

/** @brief 归还当前 API 的动态资源登记。 @post 最后一个已登记调用归还后可完成关闭屏障。 */
static void interaction_access_release(void)
{
    portENTER_CRITICAL(&s_interaction_access_mux);
    s_interaction_access_users--;
    portEXIT_CRITICAL(&s_interaction_access_mux);
}

/**
 * @brief 发布 API 接纳开关。
 * @param[in] open true 允许后续登记，false 关闭新登记。
 * @post 既有登记继续存活，关闭本身不等待其返回。
 * @pre 开放由 START owner 在状态锁内发布，关闭与生命周期 owner 协调。
 */
static void interaction_set_access_open(bool open)
{
    portENTER_CRITICAL(&s_interaction_access_mux);
    s_interaction_access_open = open;
    portEXIT_CRITICAL(&s_interaction_access_mux);
}

/** @brief 查询当前 API 登记是否已经全部归还。 @return 无在途登记时为 true，瞬时检查不重新开放入口。 */
static bool interaction_access_drained(void)
{
    portENTER_CRITICAL(&s_interaction_access_mux);
    bool drained = s_interaction_access_users == 0;
    portEXIT_CRITICAL(&s_interaction_access_mux);
    return drained;
}

/** @brief acquire 读取 worker 是否结束所有应用资源访问。 @return true 允许 owner 继续回收，内核任务回收另由端口负责。 */
static bool interaction_task_completed(void)
{
    return __atomic_load_n(&s_interaction_task_completed, __ATOMIC_ACQUIRE);
}

/**
 * @brief 将有限毫秒预算向上转换为有限 tick。
 * @param[in] milliseconds 毫秒数，零交付立即检查。
 * @return 正值至少一 tick，超大值限为 portMAX_DELAY 前一个 tick。
 */
static TickType_t interaction_timeout_ticks(uint32_t milliseconds)
{
    uint64_t ticks = ((uint64_t)milliseconds * configTICK_RATE_HZ + 999U) / 1000U;
    if (ticks >= (uint64_t)portMAX_DELAY) ticks = (uint64_t)portMAX_DELAY - 1U;
    return (TickType_t)ticks;
}

/**
 * @brief 按同一个起点计算剩余 tick 预算。
 * @param[in] started 本次有限等待的起点。
 * @param[in] budget 小于 portMAX_DELAY 的预算 tick。
 * @return 已耗尽时返回零，允许调用方立即检查当前完成条件。
 */
static TickType_t interaction_remaining_ticks(TickType_t started, TickType_t budget)
{
    TickType_t elapsed = (TickType_t)(xTaskGetTickCount() - started);
    return elapsed < budget ? budget - elapsed : 0;
}

/**
 * @brief 将交互生命周期转换成进程期稳定的观测文本。
 * @param[in] state 待转换状态，允许未知枚举数值。
 * @return 对应只读字符串借用，未知状态为 unknown，调用者无释放责任。
 */
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

/**
 * @brief 分类状态自身是否表示正在启动、排队、运行、清屏或取消。
 * @param[in] state 待检查生命周期。
 * @return 属于上述推进集合时为 true，维护/失败/停止为 false。
 * @note 服务 busy 还合并物理队列计数，接纳新输入另检查 started 与停止门。
 */
static bool interaction_state_is_busy(interaction_service_lifecycle_t state)
{
    return state == INTERACTION_SERVICE_STARTING ||
           state == INTERACTION_SERVICE_QUEUED ||
           state == INTERACTION_SERVICE_RUNNING ||
           state == INTERACTION_SERVICE_CLEARING ||
           state == INTERACTION_SERVICE_CANCELLING;
}

/**
 * @brief 获取已装配的动态状态锁。
 * @post mutex 存在时阻塞至取得，未创建时跳过。
 * @pre 持有 API 登记或处于串行生命周期/owner 路径，动态锁保持存活。
 */
static void interaction_lock(void)
{
    if (s_interaction.mutex) {
        xSemaphoreTake(s_interaction.mutex, portMAX_DELAY);
    }
}

/** @brief 归还当前调用持有的状态锁。 @post 原生 Give 返回后才结束本次锁借用。 @pre 与 interaction_lock 配对且锁保持存活。 */
static void interaction_unlock(void)
{
    if (s_interaction.mutex) {
        xSemaphoreGive(s_interaction.mutex);
    }
}

/**
 * @brief 在锁外同步空闲及维护位，并按状态版本重试覆盖迟到写入。
 * @post 队列为空且状态属于空闲集合时置 IDLE 位，维护标志决定 MAINTENANCE_OFF。
 * @pre 调用者持有 API 登记或位于生命周期/owner 路径，事件组与队列覆盖整个循环。
 */
static void interaction_sync_event_bits(void)
{
    if (!s_interaction.events) return;
    for (;;) {
        interaction_lock();
        uint32_t generation = s_interaction.state_generation;
        bool maintenance_off = !s_interaction.maintenance_paused;
        bool idle_like = (s_interaction.state == INTERACTION_SERVICE_IDLE ||
                          s_interaction.state == INTERACTION_SERVICE_PAUSED_MAINTENANCE ||
                          s_interaction.state == INTERACTION_SERVICE_FAILED ||
                          s_interaction.state == INTERACTION_SERVICE_STOPPED) &&
                         (!s_interaction.queue || uxQueueMessagesWaiting(s_interaction.queue) == 0);
        EventGroupHandle_t events = s_interaction.events;
        interaction_unlock();

        if (idle_like) xEventGroupSetBits(events, INTERACTION_IDLE_BIT);
        else xEventGroupClearBits(events, INTERACTION_IDLE_BIT);
        if (maintenance_off) xEventGroupSetBits(events, INTERACTION_MAINTENANCE_OFF_BIT);
        else xEventGroupClearBits(events, INTERACTION_MAINTENANCE_OFF_BIT);

        interaction_lock();
        bool stable = generation == s_interaction.state_generation && events == s_interaction.events;
        interaction_unlock();
        if (stable) return;
    }
}

/**
 * @brief 为命令创建优先 PSRAM、回退内部 RAM 的独立文本副本。
 * @param[in] text 可为 NULL 的调用期 NUL 文本借用，NULL 按空串。
 * @return 含 NUL 的拥有分配，由调用者 heap_caps_free，容量或分配失败为 NULL。
 */
static char *interaction_strdup(const char *text)
{
    size_t len = text ? strlen(text) : 0;
    if (len == SIZE_MAX) return NULL;
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) copy = heap_caps_malloc(len + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!copy) return NULL;
    if (len > 0 && text) memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

/**
 * @brief 跳过前导并原地终止尾部 ASCII 空白。
 * @param[in,out] text 可为 NULL 的可写 NUL 文本，识别空格、制表、回车和换行。
 * @return 原缓冲内的首个非空白位置，NULL 输入返回 NULL，原分配地址继续由调用者持有。
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

/**
 * @brief 在已持有状态锁时提交生命周期、忙标志和版本。
 * @param[in] state 新生命周期，忙标志同时计入物理队列中的已接纳命令。
 * @post 状态版本推进，事件组仍由调用者解锁后同步。
 * @pre 调用者持有 interaction 状态锁，队列在当前生命周期内存活。
 */
static void interaction_set_state_locked(interaction_service_lifecycle_t state)
{
    s_interaction.state = state;
    s_interaction.busy = interaction_state_is_busy(state) ||
        (s_interaction.queue && uxQueueMessagesWaiting(s_interaction.queue) > 0);
    s_interaction.state_generation++;
}

/**
 * @brief 锁内提交状态快照，再在锁外同步事件组。
 * @param[in] state 新生命周期。
 * @post 状态版本推进，busy 合并队列工作，事件位最终对应稳定版本。
 * @pre 动态资源由当前 API 登记或 lifecycle/owner 路径保管。
 */
static void interaction_set_state(interaction_service_lifecycle_t state)
{
    interaction_lock();
    interaction_set_state_locked(state);
    interaction_unlock();
    interaction_sync_event_bits();
}

/**
 * @brief 在状态锁内分配下一个非零请求序号。
 * @return 本次 uint32 请求 ID，回绕时跳过零，用于当前同步回复配对。
 * @pre 当前路径保管状态锁寿命，请求序号用于回复关联且不作为永久业务身份。
 */
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

/**
 * @brief 归还配置命令拥有的会话基础名并清零配置载荷。
 * @param[in,out] config 可为 NULL 的独占配置，session_id 为本模块创建的拥有分配。
 * @post 非 NULL 结构归零，可对已清理的空结构重复调用。
 */
static void interaction_free_config_payload(interaction_config_payload_t *config)
{
    if (!config) return;
    heap_caps_free(config->session_id);
    memset(config, 0, sizeof(*config));
}

/**
 * @brief 将 IO 配置复制为跨任务命令载荷。
 * @param[in] io 必填调用期借用配置，非正队列/输入限额取 8/1024。
 * @param[out] out_config 必填新槽，参数有效后清零并交付会话名拥有副本。
 * @return 成功为 ESP_OK，参数无效为 INVALID_ARG，文本分配失败为 NO_MEM。
 * @pre 输出槽当前不持有需要释放的成员，成功后由 free_config_payload 消费。
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

/**
 * @brief 尝试完成同步回复并归还命令的等待器引用。
 * @param[in,out] cmd 可为 NULL 的独占命令，异步命令的 waiter 为空。
 * @param[in] code 本次命令结果。
 * @param[in] message 可为空的调用期文本借用，等待器按固定槽复制。
 * @post waiter 指针清空，未接纳回复时饱和累计迟到计数，异步命令无回复副作用。
 */
static void interaction_complete(interaction_command_t *cmd, esp_err_t code, const char *message)
{
    if (cmd && cmd->waiter) {
        bool delivered = app_waiter_complete(cmd->waiter, code, message);
        if (!delivered) {
            interaction_lock();
            if (s_interaction.late_reply_count < UINT32_MAX) s_interaction.late_reply_count++;
            interaction_unlock();
        }
        app_waiter_release(cmd->waiter);
        cmd->waiter = NULL;
    }
}

/**
 * @brief 消费命令自有文本、配置和剩余等待器引用。
 * @param[in,out] cmd 可为 NULL 的独占命令结构。
 * @post 非 NULL 结构归零，借用的 builder/recovery/记忆依赖由外层继续保管。
 * @pre 入队失败时由生产者调用，成功接纳后由出队 owner 唯一调用。
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

/**
 * @brief 在同一状态锁内核对停止门、零等待接纳命令并发布 QUEUED。
 * @param[in,out] cmd 必填独占命令，填入非零请求 ID，成功转移全部动态成员责任。
 * @return 接纳为 ESP_OK，入口/队列无效为 INVALID_STATE，物理队列满为 TIMEOUT。
 * @pre 普通命令持有 API 登记，START/SHUTDOWN 由串行生命周期 owner 提交。
 * @note 失败时生产者仍须 cleanup，成功后只由出队 owner 清理，原文本借用不进入队列。
 */
static esp_err_t interaction_enqueue_command(interaction_command_t *cmd)
{
    if (!cmd) return ESP_ERR_INVALID_STATE;
    cmd->request_id = interaction_next_request_id();
    if (cmd->waiter) {
        cmd->waiter->request_id = cmd->request_id;
    }
    interaction_lock();
    bool lifecycle_command = cmd->type == INTERACTION_COMMAND_START ||
                             cmd->type == INTERACTION_COMMAND_SHUTDOWN;
    if (!s_interaction.queue || (!lifecycle_command &&
        (!s_interaction.started || s_interaction.stopping))) {
        interaction_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueSend(s_interaction.queue, cmd, 0) != pdTRUE) {
        interaction_unlock();
        return ESP_ERR_TIMEOUT;
    }
    UBaseType_t queued = uxQueueMessagesWaiting(s_interaction.queue);
    if ((uint32_t)queued > s_interaction.queue_high_water) {
        s_interaction.queue_high_water = (uint32_t)queued;
    }
    /* 与接纳处于同一锁内，快速 owner 不能先完成后再被生产者写回过期 QUEUED。 */
    interaction_set_state_locked(INTERACTION_SERVICE_QUEUED);
    interaction_unlock();
    interaction_sync_event_bits();
    return ESP_OK;
}

/**
 * @brief 为控制命令建立双引用回复通道，并等待 owner 结果。
 * @param[in,out] cmd 必填新命令，成功接纳后清零，失败时继续持有待 cleanup 的成员。
 * @param[out] out_reply 可为空，收到回复即交付独立按值副本，未接纳或未收到时原槽保留。
 * @param[in] timeout_ms 本次回复等待的有限毫秒预算，零仅立即检查。
 * @return 回复 code 或自等待、分配、接纳和等待错误。
 * @pre 生命周期 owner 或持有 API 登记的调用方保管服务资源，cmd 初始无等待器引用。
 * @note 当前 owner 自等待立即拒绝，超时只归还调用者引用，已接纳命令仍会执行。
 */
static esp_err_t interaction_enqueue_sync_timeout(
    interaction_command_t *cmd,
    app_reply_t *out_reply,
    uint32_t timeout_ms)
{
    interaction_lock();
    bool self_wait = !interaction_task_completed() &&
                     s_interaction.task == xTaskGetCurrentTaskHandle();
    interaction_unlock();
    if (self_wait) return ESP_ERR_INVALID_STATE;
    app_waiter_t *waiter = app_waiter_create(0);
    if (!waiter) return ESP_ERR_NO_MEM;
    app_waiter_retain(waiter); /* 命令及消费 owner 的独立引用。 */
    cmd->waiter = waiter;
    esp_err_t err = interaction_enqueue_command(cmd);
    if (err == ESP_OK) {
        memset(cmd, 0, sizeof(*cmd));
        err = app_waiter_wait(waiter, interaction_timeout_ticks(timeout_ms), out_reply);
    }
    app_waiter_release(waiter); /* 调用者引用；超时后 owner 仍安全持有对象 */
    return err;
}

/**
 * @brief 按默认三十秒回复预算提交同步控制命令。
 * @param[in,out] cmd 必填新命令，接纳后转移动态成员并清零。
 * @param[out] out_reply 可为空的按值回复槽，收到时更新。
 * @return 原样透传等待器、接纳及 owner 命令结果。
 */
static esp_err_t interaction_enqueue_sync(interaction_command_t *cmd, app_reply_t *out_reply)
{
    return interaction_enqueue_sync_timeout(cmd, out_reply, INTERACTION_SYNC_TIMEOUT_MS);
}

/**
 * @brief 将本次同步运行的流片段转换成产品事件。
 * @param[in] event 可为空的调用期借用事件，内部 chunk 为空时跳过。
 * @param[in,out] user_data 可为空的 bool 槽，用于本次调用首次片段采样，须覆盖全部同步回调。
 * @post 下游接纳各类文本、工具和状态副本，usage 由 runtime 观测层消费。
 * @pre provider/controller 保证回调在提交返回前退出，本次首片段槽由同一串行运行使用。
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
    case CC_STREAM_CHUNK_USAGE:
        /* runtime 已将用量作为观测元数据消费。 */
        break;
    }
}

/**
 * @brief 在状态锁内取得 controller 借用快照。
 * @return 当前指针或 NULL，无所有权转移。
 * @pre 当前 owner 路径或 API 登记覆盖使用期间，关闭方先排空登记再销毁 controller。
 */
static cc_runtime_controller_t *interaction_controller_snapshot(void)
{
    interaction_lock();
    cc_runtime_controller_t *controller = s_interaction.controller;
    interaction_unlock();
    return controller;
}

/**
 * @brief 在状态锁内取得装配 builder 的借用快照。
 * @return 当前 builder 或 NULL，实际 runtime 通过其代际租约取得。
 * @pre owner 或当前 API 登记覆盖借用期间，外层持有 builder 至服务销毁成功。
 */
static cc_runtime_builder_t *interaction_builder_snapshot(void)
{
    interaction_lock();
    cc_runtime_builder_t *builder = s_interaction.runtime_builder;
    interaction_unlock();
    return builder;
}

/**
 * @brief 在状态锁内按命令结果、当前队列和维护标志收敛生命周期。
 * @param[in] ok 本次操作是否成功，false 记录 FAILED，true 优先显示尚有命令的 QUEUED。
 * @post 状态、busy 和版本一致提交，再同步事件位，失败后 owner 继续接收后续命令。
 */
static void interaction_apply_state_after_work(bool ok)
{
    interaction_lock();
    interaction_service_lifecycle_t state = INTERACTION_SERVICE_FAILED;
    if (ok) {
        bool pending = s_interaction.queue && uxQueueMessagesWaiting(s_interaction.queue) > 0;
        state = pending ? INTERACTION_SERVICE_QUEUED :
            s_interaction.maintenance_paused ? INTERACTION_SERVICE_PAUSED_MAINTENANCE :
                                             INTERACTION_SERVICE_IDLE;
    }
    interaction_set_state_locked(state);
    interaction_unlock();
    interaction_sync_event_bits();
}

/**
 * @brief 核对维护、当前挂载和恢复存储门。
 * @param[in] source 门关闭时的状态事件来源。
 * @return 任一门关闭时返回 true，完整门条件满足时返回 false。
 */
static bool interaction_input_blocked_for_maintenance(app_event_source_t source);

/**
 * @brief 在 owner 中创建或配置 controller，并发布会话身份。
 * @param[in] builder 必填借用 builder，存活期覆盖控制器及服务。
 * @param[in] config 必填命令配置，session_id 借用至本次调用结束。
 * @return controller 配置成功且会话 ID 非空为 ESP_OK，参数无效或 SDK 错误另报。
 * @post 新 controller 成功后交给服务持有，runtime 仍由外层 builder 管理。
 * @note 开启迟到事件过滤，装配恢复服务时使用延迟持久化的新会话模式。
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
    controller_config.fresh_session_on_start = s_interaction.recovery != NULL;

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

/**
 * @brief 绑定 START 借用依赖，装配 controller 与菜单并完成启动回复。
 * @param[in,out] cmd owner 已取得的 START 命令，引用依赖由外层保管，等待器引用在此消费。
 * @post 成功在状态锁内开放新 API，失败保持关闭并回收已创建菜单，controller 交后续停止清理。
 * @note 停止已经开始时即使装配成功也拒绝重新开放入口，原命令载荷随后由循环 cleanup。
 */
static void interaction_handle_start(interaction_command_t *cmd)
{
    interaction_set_state(INTERACTION_SERVICE_STARTING);
    interaction_lock();
    s_interaction.runtime_builder = cmd->runtime_builder;
    s_interaction.recovery = cmd->recovery;
    s_interaction.manual_lua_policy = cmd->manual_lua_policy;
    s_interaction.memory_turns = cmd->memory_turns;
    s_interaction.queue_depth = cmd->config.queue_depth;
    s_interaction.max_input_bytes = cmd->config.max_input_bytes;
    interaction_unlock();

    esp_err_t err = cmd->recovery && !recovery_service_is_current(cmd->recovery) ?
                    ESP_ERR_INVALID_STATE : interaction_apply_controller_config(cmd->runtime_builder, &cmd->config);
    if (err == ESP_OK && cmd->recovery) {
        cc_result_t rc = cmd->memory_turns ?
            session_menu_create_with_memory(interaction_controller_snapshot(),cmd->runtime_builder,
                                            cmd->recovery,cmd->memory_turns,&s_interaction.session_menu) :
            session_menu_create(interaction_controller_snapshot(),cmd->runtime_builder,
                                cmd->recovery,&s_interaction.session_menu);
        if (rc.code == CC_OK) {
            cc_result_free(&rc);
            rc = recovery_console_create(cmd->recovery, &s_interaction.recovery_console);
        }
        if (rc.code == CC_OK && cmd->memory_editor) {
            cc_result_free(&rc);
            rc = cmd->memory_turns ?
                memory_console_create_with_turns(cmd->memory_editor,cmd->recovery,cmd->memory_turns,
                                                 &s_interaction.memory_console) :
                memory_console_create(cmd->memory_editor,cmd->recovery,&s_interaction.memory_console);
        }
        if (rc.code != CC_OK) {
            app_event_publish(APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_RUNTIME, CC_LOG_ERROR,
                              rc.message ? rc.message : cc_error_string(rc.code));
            err = ESP_FAIL;
        }
        cc_result_free(&rc);
        if (err == ESP_OK) err = session_menu_present(s_interaction.session_menu);
        if (err != ESP_OK) {
            memory_console_destroy(s_interaction.memory_console); s_interaction.memory_console = NULL;
            recovery_console_destroy(s_interaction.recovery_console); s_interaction.recovery_console = NULL;
            session_menu_destroy(s_interaction.session_menu); s_interaction.session_menu = NULL;
        }
    }
    interaction_lock();
    if (s_interaction.stopping && err == ESP_OK) err = ESP_ERR_INVALID_STATE;
    s_interaction.started = err == ESP_OK;
    if (s_interaction.started) interaction_set_access_open(true);
    if (err != ESP_OK) s_interaction.memory_turns = NULL;
    interaction_unlock();
    interaction_apply_state_after_work(err == ESP_OK);
    interaction_complete(cmd, err, err == ESP_OK ? "started" : "start failed");
}

/**
 * @brief 在 owner 中热提交最大输入字节数并回复结果。
 * @param[in,out] cmd 已接纳配置命令，其等待器引用在本次处理消费。
 * @post 更新输入限制后收敛状态，会话身份与物理队列容量由启动装配。
 */
static void interaction_handle_apply_config(interaction_command_t *cmd)
{
    interaction_set_state(INTERACTION_SERVICE_CLEARING);
    interaction_lock();
    s_interaction.max_input_bytes = cmd->config.max_input_bytes;
    interaction_unlock();

    /* 会话身份由重启装配，可回滚的热应用只提交输入限额。 */
    interaction_apply_state_after_work(true);
    interaction_complete(cmd, ESP_OK, "config applied");
}

/**
 * @brief 通过产品事件总线清空会话显示并失效当前菜单视图。
 * @param[in] source 清屏状态事件来源。
 * @return 维护或存储门关闭时为 INVALID_STATE，完成显示请求为 ESP_OK。
 * @note 该操作只更新 LCD/Web 视图，调用上下文为串行 owner。
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
    app_event_bus_clear_conversation();
    session_menu_invalidate_view(s_interaction.session_menu);
    recovery_console_invalidate_view(s_interaction.recovery_console);
    memory_console_invalidate_view(s_interaction.memory_console);
    app_event_publish(APP_EVENT_KIND_STATUS, source, CC_LOG_INFO, "display cleared");
    interaction_apply_state_after_work(true);
    return ESP_OK;
}

/**
 * @brief 在 owner 中收敛取消状态，饱和记录从带外请求到消费命令的时延。
 * @param[in,out] cmd 已接纳取消命令，等待器存在时在此消费其 owner 引用。
 * @post 再次取消当前仍活跃的运行并发布结果，随后按队列和维护状态收敛。
 * @note 即时令牌取消已经由外部 request_cancel 路径完成，本命令按 FIFO 消费。
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

/**
 * @brief 在 owner 中提交维护暂停标志并完成同步回复。
 * @param[in,out] cmd 已接纳维护命令，reason 为命令拥有文本，等待器引用在此消费。
 * @param[in] paused true 设置维护暂停，false 解除。
 * @post 发布维护事件并更新生命周期，其他已排队命令仍由 owner 继续取出核对门。
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

/**
 * @brief 释放 owner 菜单并尝试关闭 controller，发布本次关闭阶段结果。
 * @param[in,out] cmd 已接纳关闭命令，可选等待器引用在此消费，载荷随后由循环清理。
 * @return controller 已回收为 ESP_OK，失败映射 TIMEOUT/ESP_FAIL 并保留可重试 controller。
 * @post 成功进入 EXITING，失败进入 RETRYABLE 并保留 owner，通知回复事件后才返回。
 * @pre 新 API 已关闭且所有登记已归还，本 owner 独占 controller 销毁。
 */
static esp_err_t interaction_handle_shutdown(interaction_command_t *cmd)
{
    memory_console_destroy(s_interaction.memory_console); s_interaction.memory_console = NULL;
    recovery_console_destroy(s_interaction.recovery_console); s_interaction.recovery_console = NULL;
    session_menu_destroy(s_interaction.session_menu); s_interaction.session_menu = NULL;
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
    if (err == ESP_OK) {
        s_interaction.recovery = NULL;
        s_interaction.manual_lua_policy = 0;
        s_interaction.memory_turns = NULL;
        s_interaction.memory_last_error = 0;
    }
    s_interaction.shutdown_result = err;
    s_interaction.shutdown_phase = err == ESP_OK ?
        INTERACTION_SHUTDOWN_EXITING : INTERACTION_SHUTDOWN_RETRYABLE;
    interaction_unlock();
    interaction_set_state(err == ESP_OK ? INTERACTION_SERVICE_STOPPED : INTERACTION_SERVICE_FAILED);
    interaction_complete(cmd, err, err == ESP_OK ? "stopped" : "stop failed");
    xEventGroupSetBits(s_interaction.events, INTERACTION_SHUTDOWN_REPLY_BIT);
    return err;
}

/**
 * @brief 在 owner 消费时核对本地暂停、全局维护、挂载与恢复代际。
 * @param[in] source 门关闭时的状态事件来源。
 * @return 全部门满足为 false，任一阻断为 true 并发布暂停状态。
 * @pre 借用 recovery 由当前服务生命周期保管，检查结果只适用于本次观察。
 */
static bool interaction_input_blocked_for_maintenance(app_event_source_t source)
{
    interaction_lock();
    bool paused = s_interaction.maintenance_paused;
    recovery_service_t *recovery = s_interaction.recovery;
    interaction_unlock();
    bool usb_available = usb_storage_service_is_mounted();
    bool recovery_current = !recovery || recovery_service_is_current(recovery);
    if (!paused && !app_event_bus_maintenance_mode_enabled() && usb_available && recovery_current) {
        return false;
    }
    app_event_publish(APP_EVENT_KIND_STATUS,
                      source,
                      CC_LOG_WARN,
                      !usb_available ? "USB disconnected; input paused" :
                      !recovery_current ? "Recovery storage changed; input paused" : "Maintenance active; input paused");
    interaction_set_state(INTERACTION_SERVICE_PAUSED_MAINTENANCE);
    return true;
}

/**
 * @brief 在会话 owner 中尝试处理待评估或待写入的长期记忆。
 * @post 完整门满足时建立临时取消源，处理后解绑并销毁，同类连续错误只提示一次。
 * @note 失败时待处理内容继续由 turns 保管，停止和维护可在状态锁内触发当前取消源。
 * @pre 仅由 owner 调用，memory_turns/recovery 由外层保管至全部 owner 工作结束。
 */
static void interaction_pump_memory(void)
{
    session_memory_turns_t *turns=s_interaction.memory_turns;
    if (!turns || app_event_bus_maintenance_mode_enabled() || !usb_storage_service_is_mounted() ||
        !s_interaction.recovery || !recovery_service_is_current(s_interaction.recovery)) return;
    cc_cancel_source_t *source=NULL;
    cc_result_t rc=cc_cancel_source_create(&source);
    if (rc.code==CC_OK) {
        interaction_lock();
        bool allowed=s_interaction.started && !s_interaction.maintenance_paused;
        if (allowed) s_interaction.memory_cancel=source;
        interaction_unlock();
        if (!allowed) { cc_cancel_source_destroy(source); cc_result_free(&rc); return; }
        session_memory_pump_result_t result={0};
        cc_result_free(&rc);
        rc=session_memory_turns_pump(turns,(int64_t)time(NULL),cc_cancel_source_token(source),&result);
        interaction_lock();
        if (s_interaction.memory_cancel==source) s_interaction.memory_cancel=NULL;
        interaction_unlock();
        cc_cancel_source_destroy(source);
    }
    if (rc.code!=CC_OK) {
        if (s_interaction.memory_last_error!=rc.code) {
            const char *message=(rc.code==CC_ERR_STORAGE || rc.code==CC_ERR_IO) ?
                "长期记忆暂时无法写入；请检查 U 盘和剩余空间，必要时清理或导出。待处理内容保留在内存中。" :
                (rc.code==CC_ERR_LIMIT_EXCEEDED || rc.code==CC_ERR_QUEUE_FULL) ?
                "长期记忆待处理空间不足；已保留输入与答复，处理空间恢复后重试。" :
                "长期记忆评估暂未完成；待处理内容保留在内存中，稍后重试。";
            app_event_publish(APP_EVENT_KIND_STATUS,APP_EVENT_SOURCE_RUNTIME,CC_LOG_WARN,message);
        }
        s_interaction.memory_last_error=rc.code;
    } else if (s_interaction.memory_last_error!=CC_OK) {
        app_event_publish(APP_EVENT_KIND_STATUS,APP_EVENT_SOURCE_RUNTIME,CC_LOG_INFO,
                          "长期记忆处理已恢复；未决项目仍需核对。");
        s_interaction.memory_last_error=CC_OK;
    }
    cc_result_free(&rc);
    session_menu_refresh_memory_view(s_interaction.session_menu);
}

/**
 * @brief 在 owner 中依次处理维护门、记忆/恢复/会话命令、本地控制和模型提交。
 * @param[in,out] cmd 已取得的文本命令，text 被原地裁剪，原始拥有地址由外层最终 cleanup。
 * @post 发布显示及运行事件，消费本次结果诊断，按实际失败或后续队列收敛状态。
 * @note 手动 Lua 经当前盘账本入口执行，模型提交持有相应代际；回调借用本栈首片段槽。
 *       受管理记忆路径的响应借自 turns，本地 controller 路径响应由本函数归还。
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
    if (s_interaction.memory_turns && session_menu_handle_memory(s_interaction.session_menu,input,cmd->source)) {
        recovery_console_invalidate_view(s_interaction.recovery_console);
        memory_console_invalidate_view(s_interaction.memory_console);
        if (strncmp(input,"/memory-retry ",14)==0 || strncmp(input,"/memory-abandon ",16)==0)
            interaction_pump_memory();
        interaction_apply_state_after_work(true);
        app_profile_span_end(&job_span,"memory_pending_command");
        return;
    }
    if (s_interaction.recovery_console && recovery_console_handles(input)) {
        session_menu_invalidate_view(s_interaction.session_menu);
        memory_console_invalidate_view(s_interaction.memory_console);
        recovery_console_handle(s_interaction.recovery_console, input, cmd->source);
        interaction_apply_state_after_work(true);
        app_profile_span_end(&job_span, "recovery_command");
        return;
    }
    recovery_console_invalidate_view(s_interaction.recovery_console);
    if (s_interaction.memory_console && memory_console_handles(input)) {
        session_menu_invalidate_view(s_interaction.session_menu);
        time_t now=time(NULL);
        memory_console_handle(s_interaction.memory_console,input,cmd->source,(int64_t)now);
        interaction_apply_state_after_work(true);
        app_profile_span_end(&job_span,"memory_command");
        return;
    }
    memory_console_invalidate_view(s_interaction.memory_console);
    if (s_interaction.session_menu && session_menu_waiting(s_interaction.session_menu)) {
        /* 放弃中断任务也须评估用户输入，尚未装配记忆服务时保留任务供后续处理。 */
        if (!s_interaction.memory_turns && strncmp(input,"/abandon ",9)==0) {
            app_event_publish(APP_EVENT_KIND_ERROR,cmd->source,CC_LOG_WARN,
                "长期记忆服务尚未配置；为保留记忆评估，暂不能放弃中断任务。可继续任务或选择新会话。");
            interaction_apply_state_after_work(true);
            app_profile_span_end(&job_span,"memory_not_configured");
            return;
        }
        bool first_chunk_seen = false;
        if (session_menu_handle(s_interaction.session_menu, input, cmd->source,
                                stream_chunk_cb, &first_chunk_seen)) {
            if (s_interaction.memory_turns &&
                (strncmp(input,"/continue ",10)==0 || strncmp(input,"/abandon ",9)==0))
                interaction_pump_memory();
            interaction_apply_state_after_work(true);
            app_profile_span_end(&job_span, "session_menu");
            return;
        }
    }
    if (strcmp(input, "/lua") == 0 || strcmp(input, "/luafile") == 0 ||
        strncmp(input, "/lua ", 5) == 0 || strncmp(input, "/luafile ", 9) == 0) {
        if (!s_interaction.recovery) {
            app_event_publish(APP_EVENT_KIND_ERROR, cmd->source, CC_LOG_WARN,
                              "手动 Lua 需要当前 U 盘的操作账本，请先完成恢复服务装配。");
            interaction_apply_state_after_work(true);
            app_profile_span_end(&job_span, "manual_lua_unavailable");
            return;
        }
        cc_tool_result_t result = {0};
        cc_result_t rc = manual_lua_service_execute(interaction_controller_snapshot(),
            s_interaction.recovery, input, s_interaction.manual_lua_policy, &result);
        if (rc.code != CC_OK)
            app_event_publish(APP_EVENT_KIND_ERROR, cmd->source, CC_LOG_ERROR,
                              rc.message ? rc.message : cc_error_string(rc.code));
        else app_event_publish(result.ok ? APP_EVENT_KIND_TOOL : APP_EVENT_KIND_ERROR,
                               cmd->source, result.ok ? CC_LOG_INFO : CC_LOG_WARN,
                               result.ok ? (result.text ? result.text : "lua ok") :
                                           (result.error ? result.error : "lua failed"));
        cc_result_free(&rc); cc_tool_result_cleanup(&result);
        interaction_apply_state_after_work(true);
        app_profile_span_end(&job_span, "manual_lua");
        return;
    }
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

    char *owned_response = NULL;
    const char *response = NULL;
    bool first_chunk_seen = false;
    bool completed = false;
    bool awaiting_memory = false;
    bool memory_queue_full = false;
    cc_runtime_controller_status_t submit_status;
    cc_runtime_controller_get_status(controller, &submit_status);
    app_profile_span_t submit_span = app_profile_span_begin("interaction",
                                                            "runtime_submit",
                                                            submit_status.session_id,
                                                            NULL);
    cc_result_t rc;
    if (s_interaction.memory_turns) {
        session_menu_invalidate_view(s_interaction.session_menu);
        interaction_pump_memory();
        session_memory_turn_result_t result={0};
        rc=session_menu_submit_turn(s_interaction.session_menu,input,stream_chunk_cb,&first_chunk_seen,&result);
        response=result.response; completed=result.completed; awaiting_memory=result.awaiting_resolution;
        memory_queue_full=rc.code==CC_ERR_LIMIT_EXCEEDED && !result.run_id &&
                          !result.completed && !result.awaiting_resolution;
    } else {
        rc=cc_runtime_controller_submit_text(controller,input,stream_chunk_cb,&first_chunk_seen,&owned_response);
        response=owned_response;
    }
    app_profile_span_end(&submit_span, rc.code == CC_OK ? "ok" : "error");
    /* 流式 provider 已通过 callback 发布文本；仅非流式 provider 需要补发完整 response。 */
    if ((rc.code == CC_OK || completed) && response && response[0] && !first_chunk_seen) {
        app_event_publish(APP_EVENT_KIND_STATUS, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, "provider stream unsupported");
        app_event_publish(APP_EVENT_KIND_ASSISTANT, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, response);
    }
    if (rc.code != CC_OK) {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          APP_EVENT_SOURCE_RUNTIME,
                          CC_LOG_ERROR,
                          memory_queue_full ?
                          "待处理记忆队列已满，本条消息未处理；请先处理队列或清理存储，之后重新发送。" :
                          (rc.message ? rc.message : cc_error_string(rc.code)));
    }
    if (awaiting_memory) app_event_publish(APP_EVENT_KIND_STATUS,APP_EVENT_SOURCE_RUNTIME,CC_LOG_WARN,
        "该对话的记忆评估仍待处理，可使用 /memory-pending 查看并重试。");
    free(owned_response);
    bool ok = rc.code == CC_OK;
    cc_result_free(&rc);
    if (s_interaction.memory_turns) interaction_pump_memory();
    app_event_publish(APP_EVENT_KIND_RUN_STATE, APP_EVENT_SOURCE_RUNTIME, CC_LOG_INFO, "idle");
    app_profile_sample("interaction", "idle", false);
    interaction_apply_state_after_work(ok);
    app_profile_span_end(&job_span, ok ? "ok" : "error");
}

/**
 * @brief 发布任务身份后串行消费服务命令，并在关闭成功后完成最终退出屏障。
 * @param[in] arg 未使用的任务参数，创建时为 NULL。
 * @post 每条命令唯一 cleanup，退出事件返回后 release 发布进程期完成位，再请求内核自删。
 * @note 关闭失败继续保留任务处理下一次 SHUTDOWN，空闲时按记忆重试周期检查待处理项。
 */
static void interaction_service_task(void *arg)
{
    interaction_lock();
    s_interaction.task = xTaskGetCurrentTaskHandle();
    interaction_unlock();
    app_profile_register_task("interaction", xTaskGetCurrentTaskHandle());
    (void)arg;
    interaction_command_t cmd;
    bool stop = false;
    while (!stop) {
        TickType_t wait=s_interaction.memory_turns ? interaction_timeout_ticks(INTERACTION_MEMORY_RETRY_MS) : portMAX_DELAY;
        if (xQueueReceive(s_interaction.queue, &cmd, wait) != pdTRUE) {
            interaction_lock();
            bool can_retry=s_interaction.started && !s_interaction.maintenance_paused;
            interaction_unlock();
            if (can_retry && s_interaction.memory_turns && !app_event_bus_maintenance_mode_enabled() &&
                usb_storage_service_is_mounted() && recovery_service_is_current(s_interaction.recovery)) {
                interaction_set_state(INTERACTION_SERVICE_RUNNING);
                interaction_pump_memory();
                interaction_apply_state_after_work(true);
            }
            continue;
        }
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
        case INTERACTION_COMMAND_CLEAR_DISPLAY: {
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
            stop = interaction_handle_shutdown(&cmd) == ESP_OK;
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
    app_profile_unregister_task(xTaskGetCurrentTaskHandle());
    interaction_lock();
    s_interaction.task = NULL;
    interaction_unlock();
    if (s_interaction.events) xEventGroupSetBits(s_interaction.events, INTERACTION_TASK_EXITED_BIT);
    __atomic_store_n(&s_interaction_task_completed, true, __ATOMIC_RELEASE);
#if INTERACTION_SERVICE_TASK_STACK_IN_PSRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

/**
 * @brief 创建优先 PSRAM、回退内部 RAM 的十二 KiB owner 任务。
 * @return 原生 pdPASS 或创建失败值，临时输出句柄只供创建端使用。
 * @note worker 在任何外部回调前自行发布身份，创建端不再覆盖可能已经退出的任务句柄。
 * @pre 原语已准备，进程期完成位置 false，生命周期 owner 串行创建。
 */
static BaseType_t create_interaction_task(void)
{
    TaskHandle_t created_task = NULL;
#if INTERACTION_SERVICE_TASK_STACK_IN_PSRAM
    BaseType_t ok = xTaskCreateWithCaps(interaction_service_task,
                                        "interaction_service",
                                        INTERACTION_SERVICE_TASK_STACK,
                                        NULL,
                                        INTERACTION_SERVICE_TASK_PRIORITY,
                                        &created_task,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) return ok;
    return xTaskCreateWithCaps(interaction_service_task,
                               "interaction_service",
                               INTERACTION_SERVICE_TASK_STACK,
                               NULL,
                               INTERACTION_SERVICE_TASK_PRIORITY,
                               &created_task,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(interaction_service_task,
                       "interaction_service",
                       INTERACTION_SERVICE_TASK_STACK,
                       NULL,
                       INTERACTION_SERVICE_TASK_PRIORITY,
                       &created_task);
#endif
}

/**
 * @brief 按状态锁、事件组、队列及 worker 顺序装配本次服务原语。
 * @param[in] queue_depth 物理队列容量，非正取八，乘法容量须可表示。
 * @return 完整装配为 ESP_OK，容量、分配或任务创建失败另报，部分原语交启动清理路径处理。
 * @note 队列优先使用 capability-aware 分配，创建失败恢复最终完成位。
 * @pre 外层串行 owner 已清理上一周期，当前 API 接纳保持关闭。
 */
static esp_err_t ensure_interaction_primitives(int queue_depth)
{
    int depth = queue_depth > 0 ? queue_depth : 8;
    if ((size_t)depth > SIZE_MAX / sizeof(interaction_command_t)) return ESP_ERR_INVALID_SIZE;
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
#if configSUPPORT_STATIC_ALLOCATION
        s_interaction.queue = xQueueCreateWithCaps((UBaseType_t)depth,
                                                   sizeof(interaction_command_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_interaction.queue_with_caps = s_interaction.queue != NULL;
#endif
        if (!s_interaction.queue) {
            s_interaction.queue = xQueueCreate((UBaseType_t)depth, sizeof(interaction_command_t));
        }
        if (!s_interaction.queue) return ESP_ERR_NO_MEM;
    }
    if (!s_interaction.task) {
        xEventGroupClearBits(s_interaction.events, INTERACTION_TASK_EXITED_BIT | INTERACTION_SHUTDOWN_REPLY_BIT);
        __atomic_store_n(&s_interaction_task_completed, false, __ATOMIC_RELEASE);
        BaseType_t ok = create_interaction_task();
        if (ok != pdPASS) {
            __atomic_store_n(&s_interaction_task_completed, true, __ATOMIC_RELEASE);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/**
 * @brief 清理已停止周期，复制配置并同步提交本次 START。
 * @param[in] io 必填调用期借用配置，动态字段在创建任务前复制。
 * @param[in] builder 必填借用 builder，覆盖未决 START 和 owner 工作期。
 * @param[in] recovery 可为 NULL 的借用当前盘恢复服务。
 * @param[in] manual_lua_policy 已由公开入口验证的手动 Lua 策略。
 * @param[in] memory_editor 可为空的借用记忆编辑器。
 * @param[in] memory_turns 可为空的借用自动记忆桥接。
 * @return START 完成为 ESP_OK，活跃旧周期/参数/分配/装配/接纳/等待错误另报。
 * @note 未接纳时清理本次拥有配置，接纳后等待超时仍保留任务和借用依赖供停止回收。
 */
static esp_err_t interaction_service_start_internal(const app_runtime_io_config_t *io,
                                                    cc_runtime_builder_t *builder,
                                                    recovery_service_t *recovery,
                                                    manual_lua_policy_t manual_lua_policy,
                                                    cc_managed_memory_editor_t *memory_editor,
                                                    session_memory_turns_t *memory_turns)
{
    if (!io || !builder) return ESP_ERR_INVALID_ARG;
    if (!interaction_task_completed()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = interaction_service_destroy();
    if (err != ESP_OK) return err;

    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = INTERACTION_COMMAND_START;
    cmd.source = APP_EVENT_SOURCE_RUNTIME;
    cmd.runtime_builder = builder;
    cmd.recovery = recovery;
    cmd.manual_lua_policy = manual_lua_policy;
    cmd.memory_editor = memory_editor;
    cmd.memory_turns = memory_turns;
    err = interaction_copy_config_payload(io, &cmd.config);
    if (err != ESP_OK) return err;
    err = ensure_interaction_primitives(io->queue_depth);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
        (void)interaction_service_destroy();
        return err;
    }

    app_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    err = interaction_enqueue_sync(&cmd, &reply);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
    }
    return err;
}

/**
 * @brief 以基础 controller 模式启动交互 owner。
 * @param[in] io 必填调用期 IO 配置，字段在 START 接纳前复制。
 * @param[in] builder 必填借用 builder，覆盖未决命令及 owner 运行期。
 * @return 原样返回启动、接纳及等待结果，超时保留已接纳 START。
 */
esp_err_t interaction_service_start(const app_runtime_io_config_t *io, cc_runtime_builder_t *builder)
{ return interaction_service_start_internal(io, builder, NULL, 0, NULL, NULL); }

/**
 * @brief 验证恢复身份和手动 Lua 策略，再启动恢复菜单模式。
 * @param[in] io 必填调用期 IO 配置。
 * @param[in] builder 须与 recovery 一致的借用 builder。
 * @param[in] recovery 必填借用恢复服务，装配期已绑定当前盘。
 * @param[in] manual_lua_policy PENDING、模型注册表或独立手动入口三种已定义策略之一。
 * @param[in] memory_editor 可为空的借用编辑器，存在时创建记忆管理菜单。
 * @return 身份不符为 INVALID_STATE，策略无效为 INVALID_ARG，其余透传启动结果。
 */
esp_err_t interaction_service_start_with_recovery(const app_runtime_io_config_t *io,
                                                  cc_runtime_builder_t *builder,
                                                  recovery_service_t *recovery,
                                                  manual_lua_policy_t manual_lua_policy,
                                                  cc_managed_memory_editor_t *memory_editor)
{
    if (!recovery || !recovery_service_uses_builder(recovery,builder)) return ESP_ERR_INVALID_STATE;
    if (manual_lua_policy != MANUAL_LUA_POLICY_PENDING && manual_lua_policy != MANUAL_LUA_USE_MODEL_REGISTRY &&
        manual_lua_policy != MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY) return ESP_ERR_INVALID_ARG;
    return interaction_service_start_internal(io, builder, recovery, manual_lua_policy, memory_editor, NULL);
}

/**
 * @brief 核对受管理记忆桥接及 editor，并启动完整记忆模式。
 * @param[in] io 必填调用期 IO 配置。
 * @param[in] builder 与 recovery/turns 同一装配的借用 builder。
 * @param[in] recovery 必填当前盘恢复服务。
 * @param[in] manual_lua_policy 已定义的手动 Lua 策略。
 * @param[in] memory_turns 必填借用桥接，须提供一致的 editor 和依赖身份。
 * @return 身份/editor 不符为 INVALID_STATE，策略无效为 INVALID_ARG，其余透传启动结果。
 */
esp_err_t interaction_service_start_with_managed_memory(const app_runtime_io_config_t *io,
    cc_runtime_builder_t *builder, recovery_service_t *recovery,
    manual_lua_policy_t manual_lua_policy, session_memory_turns_t *memory_turns)
{
    if (!recovery || !recovery_service_uses_builder(recovery,builder) ||
        !session_memory_turns_uses(memory_turns,builder,recovery)) return ESP_ERR_INVALID_STATE;
    if (manual_lua_policy != MANUAL_LUA_POLICY_PENDING && manual_lua_policy != MANUAL_LUA_USE_MODEL_REGISTRY &&
        manual_lua_policy != MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY) return ESP_ERR_INVALID_ARG;
    cc_managed_memory_editor_t *editor=session_memory_turns_editor(memory_turns);
    if (!editor) return ESP_ERR_INVALID_STATE;
    return interaction_service_start_internal(io,builder,recovery,manual_lua_policy,editor,memory_turns);
}

/**
 * @brief 在同一有限预算内关闭 API 接纳、排空既有调用并等待 owner 最终退出。
 * @param[in] timeout_ms 毫秒预算，零取默认三十秒，正值向上转换为有限 tick。
 * @return 最终完成为 ESP_OK，自 owner 停止为 INVALID_STATE，其余为入队、controller 关闭或超时错误。
 * @note REQUESTED/EXITING 阶段复用已有关闭命令，RETRYABLE 允许下一次调用重新提交。
 *       controller 销毁开始前先排空 API 登记，退出事件返回后再 acquire 确认最终完成位。
 * @pre 外层生命周期 owner 串行 start/stop/destroy 并关闭外部入口，失败时继续保管依赖。
 */
esp_err_t interaction_service_stop(uint32_t timeout_ms)
{
    if (interaction_task_completed()) return ESP_OK;
    if (timeout_ms == 0) timeout_ms = INTERACTION_SYNC_TIMEOUT_MS;
    TickType_t started = xTaskGetTickCount();
    TickType_t budget = interaction_timeout_ticks(timeout_ms);

    interaction_lock();
    if (s_interaction.task == xTaskGetCurrentTaskHandle()) {
        interaction_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_interaction.stopping = true;
    s_interaction.started = false;
    interaction_set_access_open(false);
    if (s_interaction.memory_cancel) cc_cancel_source_cancel(s_interaction.memory_cancel);
    bool request_shutdown = s_interaction.shutdown_phase != INTERACTION_SHUTDOWN_REQUESTED &&
                            s_interaction.shutdown_phase != INTERACTION_SHUTDOWN_EXITING;
    cc_runtime_controller_t *controller = request_shutdown ? s_interaction.controller : NULL;
    interaction_unlock();

    if (controller) {
        cc_runtime_controller_status_t status;
        cc_runtime_controller_get_status(controller, &status);
        if (status.active_run_id) {
            cc_result_t cancel = cc_runtime_controller_cancel(controller, status.active_run_id);
            cc_result_free(&cancel);
        }
    }

    while (!interaction_access_drained()) {
        if (interaction_remaining_ticks(started, budget) == 0) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    if (request_shutdown) {
        interaction_command_t cmd = {0};
        cmd.type = INTERACTION_COMMAND_SHUTDOWN;
        cmd.source = APP_EVENT_SOURCE_SYSTEM;
        interaction_lock();
        s_interaction.shutdown_phase = INTERACTION_SHUTDOWN_REQUESTED;
        s_interaction.shutdown_result = ESP_ERR_NOT_FINISHED;
        interaction_unlock();
        xEventGroupClearBits(s_interaction.events, INTERACTION_SHUTDOWN_REPLY_BIT);
        esp_err_t err = interaction_enqueue_command(&cmd);
        if (err != ESP_OK) {
            interaction_lock();
            s_interaction.shutdown_result = err;
            s_interaction.shutdown_phase = INTERACTION_SHUTDOWN_RETRYABLE;
            interaction_unlock();
            interaction_command_cleanup(&cmd);
            return err;
        }
    }
    for (;;) {
        interaction_lock();
        interaction_shutdown_phase_t phase = s_interaction.shutdown_phase;
        esp_err_t result = s_interaction.shutdown_result;
        interaction_unlock();
        if (phase == INTERACTION_SHUTDOWN_RETRYABLE) return result;
        if (phase == INTERACTION_SHUTDOWN_EXITING) break;
        TickType_t remaining = interaction_remaining_ticks(started, budget);
        if (remaining == 0) return ESP_ERR_TIMEOUT;
        (void)xEventGroupWaitBits(s_interaction.events, INTERACTION_SHUTDOWN_REPLY_BIT,
                                 pdTRUE, pdTRUE, remaining);
    }

    EventBits_t bits = xEventGroupWaitBits(s_interaction.events,
        INTERACTION_TASK_EXITED_BIT, pdFALSE, pdTRUE,
        interaction_remaining_ticks(started, budget));
    if ((bits & INTERACTION_TASK_EXITED_BIT) == 0) return ESP_ERR_TIMEOUT;
    while (!interaction_task_completed()) {
        if (interaction_remaining_ticks(started, budget) == 0) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    return ESP_OK;
}

/**
 * @brief 在最终完成与登记排空后消费剩余命令并释放动态同步原语。
 * @return 完成或已清理为 ESP_OK，仍有 worker/controller/API 登记为 INVALID_STATE。
 * @post 成功清零服务状态，进程期入口门保持关闭，外层可重建新周期。
 * @pre 生命周期 owner 串行调用，成功后禁止发起借用旧资源的 API。
 */
esp_err_t interaction_service_destroy(void)
{
    if (!interaction_task_completed()) return ESP_ERR_INVALID_STATE;
    interaction_lock();
    bool can_destroy = s_interaction.task == NULL && s_interaction.controller == NULL;
    interaction_unlock();
    if (!can_destroy) return ESP_ERR_INVALID_STATE;
    interaction_set_access_open(false);
    if (!interaction_access_drained()) return ESP_ERR_INVALID_STATE;

    interaction_command_t pending;
    while (s_interaction.queue && xQueueReceive(s_interaction.queue, &pending, 0) == pdTRUE) {
        interaction_command_cleanup(&pending);
    }
    if (s_interaction.queue) {
#if configSUPPORT_STATIC_ALLOCATION
        if (s_interaction.queue_with_caps) vQueueDeleteWithCaps(s_interaction.queue);
        else
#endif
            vQueueDelete(s_interaction.queue);
    }
    if (s_interaction.events) vEventGroupDelete(s_interaction.events);
    if (s_interaction.mutex) vSemaphoreDelete(s_interaction.mutex);
    memset(&s_interaction, 0, sizeof(s_interaction));
    return ESP_OK;
}

/**
 * @brief 登记当前 API 并通过同步命令提交输入限额。
 * @param[in] io 必填调用期借用配置，动态字段在等待前复制。
 * @return 参数、关闭、分配、接纳及 owner 回复结果，等待超时不撤销已接纳命令。
 * @note 本次登记覆盖等待器与状态同步，owner 自等待由同步 helper 拒绝。
 */
esp_err_t interaction_service_apply_config(const app_runtime_io_config_t *io)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (!interaction_access_acquire()) return ESP_ERR_INVALID_STATE;
    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = INTERACTION_COMMAND_APPLY_CONFIG;
    cmd.source = APP_EVENT_SOURCE_SYSTEM;
    esp_err_t err = interaction_copy_config_payload(io, &cmd.config);
    if (err != ESP_OK) {
        interaction_access_release();
        return err;
    }

    app_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    err = interaction_enqueue_sync(&cmd, &reply);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
    }
    interaction_access_release();
    return err;
}

/**
 * @brief 在 API 登记覆盖下取消当前运行与记忆模型，再异步提交状态收敛。
 * @param[in] source 取消状态事件来源。
 * @return 令牌已经传播或当前空闲为 ESP_OK，入口/指针无效为 INVALID_STATE，SDK 错误为 ESP_FAIL。
 * @note 收敛命令未接纳仍归还已完成的取消结果，借用 controller 直到 API 登记归还。
 */
esp_err_t interaction_service_request_cancel(app_event_source_t source)
{
    if (!interaction_access_acquire()) return ESP_ERR_INVALID_STATE;
    cc_runtime_controller_t *controller = interaction_controller_snapshot();
    if (!controller) {
        interaction_access_release();
        return ESP_ERR_INVALID_STATE;
    }

    interaction_lock();
    s_interaction.cancel_requested_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    bool memory_active=s_interaction.memory_cancel!=NULL;
    if (s_interaction.memory_cancel) cc_cancel_source_cancel(s_interaction.memory_cancel);
    interaction_unlock();
    cc_runtime_controller_status_t status;
    cc_runtime_controller_get_status(controller, &status);
    if (status.active_run_id == 0) {
        if (memory_active) app_event_publish(APP_EVENT_KIND_RUN_STATE,source,CC_LOG_INFO,"memory cancellation signalled");
        interaction_access_release();
        return ESP_OK;
    }
    cc_result_t rc = cc_runtime_controller_cancel(controller, status.active_run_id);
    if (rc.code != CC_OK) {
        app_event_publish(APP_EVENT_KIND_ERROR,
                          source,
                          CC_LOG_ERROR,
                          rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        interaction_access_release();
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
        interaction_access_release();
        return ESP_OK;
    }
    memset(&cmd, 0, sizeof(cmd));
    interaction_access_release();
    return ESP_OK;
}

/**
 * @brief 登记 API 并同步请求进入或解除交互维护暂停。
 * @param[in] paused true 提前关闭维护输入门并取消记忆模型，false 由 owner 解除。
 * @param[in] reason 可为空的调用期 NUL 文本，复制后转给命令。
 * @return 关闭/自等待、分配、入队、等待或 owner 命令结果。
 * @note 进入请求在后续失败前可能已设置暂停及取消，外层按实际结果协调完整 OTA 生命周期。
 */
esp_err_t interaction_service_set_maintenance_paused(bool paused, const char *reason)
{
    if (!interaction_access_acquire()) return ESP_ERR_INVALID_STATE;
    interaction_lock();
    bool self_wait = s_interaction.task == xTaskGetCurrentTaskHandle();
    interaction_unlock();
    if (self_wait) {
        interaction_access_release();
        return ESP_ERR_INVALID_STATE;
    }
    if (paused) {
        /* 等待串行命令前先关闭维护门，并取消当前独立记忆模型请求。 */
        interaction_lock();
        s_interaction.maintenance_paused = true;
        if (s_interaction.memory_cancel) cc_cancel_source_cancel(s_interaction.memory_cancel);
        interaction_unlock();
    }
    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = paused ? INTERACTION_COMMAND_ENTER_MAINTENANCE : INTERACTION_COMMAND_LEAVE_MAINTENANCE;
    cmd.source = APP_EVENT_SOURCE_SYSTEM;
    cmd.reason = interaction_strdup(reason ? reason : "");
    if (!cmd.reason) {
        interaction_access_release();
        return ESP_ERR_NO_MEM;
    }
    app_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    esp_err_t err = interaction_enqueue_sync(&cmd, &reply);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
    }
    interaction_access_release();
    return err;
}

/**
 * @brief 登记 API、限长和复制输入，再零等待提交给 owner。
 * @param[in] source 后续命令及显示事件来源。
 * @param[in] text 必填调用期 NUL 文本，原分配继续由调用者持有。
 * @return ESP_OK 表示已接纳，关闭为 INVALID_STATE，空/超长为 INVALID_SIZE，分配及队满另报。
 * @note 接纳点在状态锁内再次核对停止门，所有失败归还本次副本和 API 登记。
 */
esp_err_t interaction_service_submit_text(app_event_source_t source, const char *text)
{
    if (!text || !interaction_access_acquire()) return ESP_ERR_INVALID_STATE;
    interaction_lock();
    bool accepting = s_interaction.started;
    interaction_unlock();
    if (!accepting) {
        interaction_access_release();
        return ESP_ERR_INVALID_STATE;
    }
    app_profile_span_t span = app_profile_span_begin("interaction", "submit_text", NULL, NULL);

    interaction_lock();
    int max_input_bytes = s_interaction.max_input_bytes > 0 ? s_interaction.max_input_bytes : 1024;
    interaction_unlock();
    size_t len = strlen(text);
    if (len == 0 || len > (size_t)max_input_bytes) {
        app_profile_span_end(&span, "invalid_size");
        interaction_access_release();
        return ESP_ERR_INVALID_SIZE;
    }

    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = strcmp(text, "/clear") == 0 ? INTERACTION_COMMAND_CLEAR_DISPLAY : INTERACTION_COMMAND_SUBMIT_TEXT;
    cmd.source = source;
    cmd.text = interaction_strdup(text);
    if (!cmd.text) {
        app_profile_span_end(&span, "oom");
        interaction_access_release();
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
        interaction_access_release();
        return err;
    }
    app_event_publish(APP_EVENT_KIND_RUN_STATE, source, CC_LOG_INFO, "queued");
    app_profile_span_end(&span, "queued");
    interaction_access_release();
    return ESP_OK;
}

/**
 * @brief 登记 API 并零等待提交专用清屏命令。
 * @param[in] source 清屏状态事件来源。
 * @return 接纳为 ESP_OK，关闭为 INVALID_STATE，队满为 TIMEOUT。
 * @note 真正清屏由 owner 再次检查维护门，整个提交期间保有 API 登记。
 */
esp_err_t interaction_service_submit_clear(app_event_source_t source)
{
    if (!interaction_access_acquire()) return ESP_ERR_INVALID_STATE;
    interaction_lock();
    bool accepting = s_interaction.started;
    interaction_unlock();
    if (!accepting) {
        interaction_access_release();
        return ESP_ERR_INVALID_STATE;
    }
    interaction_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = INTERACTION_COMMAND_CLEAR_DISPLAY;
    cmd.source = source;
    esp_err_t err = interaction_enqueue_command(&cmd);
    if (err != ESP_OK) {
        interaction_command_cleanup(&cmd);
        app_event_publish(APP_EVENT_KIND_STATUS,
                          source,
                          CC_LOG_WARN,
                          "interaction queue full");
        interaction_access_release();
        return err;
    }
    app_event_publish(APP_EVENT_KIND_RUN_STATE, source, CC_LOG_INFO, "queued");
    interaction_access_release();
    return ESP_OK;
}

/**
 * @brief 在 API 登记覆盖期间等待空闲事件位。
 * @param[in] timeout_ms 有限毫秒预算，零取默认三十秒。
 * @return 观察到空闲为 true，超时/owner 自等待为 false，入口关闭时返回最终完成快照。
 * @note FAILED/STOPPED/维护暂停在空队列时也可满足空闲，调用方另行判断健康与维护状态。
 */
bool interaction_service_wait_idle(uint32_t timeout_ms)
{
    if (!interaction_access_acquire()) return interaction_task_completed();
    interaction_lock();
    bool self_wait = s_interaction.task == xTaskGetCurrentTaskHandle();
    interaction_unlock();
    if (self_wait) {
        interaction_access_release();
        return false;
    }
    TickType_t ticks = interaction_timeout_ticks(timeout_ms == 0 ?
                                     INTERACTION_SYNC_TIMEOUT_MS : timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_interaction.events,
                                           INTERACTION_IDLE_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           ticks);
    interaction_access_release();
    return (bits & INTERACTION_IDLE_BIT) != 0;
}

/**
 * @brief 在 API 登记覆盖下复制服务、controller 及 runtime 代际能力快照。
 * @param[out] out_status 可为空，非 NULL 先清零，入口关闭时保留零快照。
 * @post 输出物理排队数、合并的 busy、会话副本和能力标志，代际租约失败时能力为 false。
 * @note 各来源按各自锁取样，输出只用于观测，本函数归还全部诊断和代际租约。
 */
void interaction_service_get_status(interaction_service_status_t *out_status)
{
    if (!out_status) return;
    memset(out_status, 0, sizeof(*out_status));
    if (!interaction_access_acquire()) return;

    interaction_lock();
    cc_runtime_controller_t *controller = s_interaction.controller;
    cc_runtime_builder_t *builder = s_interaction.runtime_builder;
    out_status->state = s_interaction.state;
    out_status->started = s_interaction.started;
    out_status->busy = s_interaction.busy;
    out_status->queue_depth = s_interaction.queue_depth;
    out_status->max_input_bytes = s_interaction.max_input_bytes;
    out_status->pending = s_interaction.queue ? (int)uxQueueMessagesWaiting(s_interaction.queue) : 0;
    out_status->busy = out_status->busy || out_status->pending > 0;
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
    interaction_access_release();
}
