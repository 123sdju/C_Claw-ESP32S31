#include "app_profile.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_event_bus.h"
#include "cc/app/cc_tool_executor_pool.h"
#include "cc/ports/cc_event_bus.h"
#include "cc/util/cc_json.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#define APP_PROFILE_TASK_MAX 8
#define APP_PROFILE_LAST_JSON_MAX 4096
#define APP_PROFILE_CPU_TASK_MAX 8
#define APP_PROFILE_CPU_PREV_MAX 48
#define APP_PROFILE_CPU_JSON_MAX 4096
#define APP_PROFILE_CPU_MIN_WINDOW_MS 100U
#define APP_PROFILE_TASK_STACK 4096
#define APP_PROFILE_TASK_PRIORITY 2
#define APP_PROFILE_STREAM_TEXT_EVERY 20U
#define APP_PROFILE_STREAM_TEXT_MIN_INTERVAL_US 1000000ULL
#define APP_PROFILE_STREAM_THINKING_EVERY 20U
#define APP_PROFILE_STREAM_THINKING_MIN_INTERVAL_US 1000000ULL
#define APP_PROFILE_TASK_EXITED_BIT (1U << 0)

#ifndef portNUM_PROCESSORS
#if defined(configNUMBER_OF_CORES)
#define APP_PROFILE_CPU_CORES configNUMBER_OF_CORES
#else
#define APP_PROFILE_CPU_CORES 1U
#endif
#else
#define APP_PROFILE_CPU_CORES portNUM_PROCESSORS
#endif

#if (configUSE_TRACE_FACILITY == 1) && !CONFIG_FREERTOS_SMP && (APP_PROFILE_CPU_CORES > 1)
#include "esp_private/freertos_idf_additions_priv.h"
#endif

#if (configUSE_TRACE_FACILITY == 1)
/** @brief 预分配的任务身份副本，名字与核心编号在内核同步窗口内复制。 */
typedef struct {
    char name[configMAX_TASK_NAME_LEN];
    int core_id;
} app_profile_cpu_identity_t;
#endif

/** @brief 固定容量注册槽，复制任务名并借用句柄，删除任务前须注销。 */
typedef struct {
    char name[24];
    TaskHandle_t handle;
} app_profile_task_t;

/** @brief 以任务编号关联的运行计数基线，seen 表示槽已使用。 */
typedef struct {
    UBaseType_t task_number;
    uint32_t runtime_counter;
    bool seen;
} app_profile_cpu_prev_t;

/** @brief 本次 CPU 排序项，name 借用本次拥有快照，百分比以万分之一表示。 */
typedef struct {
    const char *name;
    UBaseType_t task_id;
    UBaseType_t state;
    UBaseType_t priority;
    uint32_t stack_high_water_words;
    int core_id;
    uint32_t runtime_counter;
    uint32_t runtime_delta;
    uint32_t cpu_pct_x100;
    uint32_t core_pct_x100;
} app_profile_cpu_task_sample_t;

/** @brief 进程级 profile owner，mutex 保护字段，初始化/销毁由生命周期外层独占。 */
typedef struct {
    bool enabled;
    app_profile_level_t level;
    int interval_ms;
    uint32_t samples;
    uint32_t dropped;
    bool runtime_subscribed;
    cc_event_subscription_t runtime_subscription;
    TaskHandle_t sampler_task;
    EventGroupHandle_t events;
    bool stopping;
    uint32_t stream_text_chunks;
    uint32_t stream_text_suppressed;
    size_t stream_text_bytes;
    uint64_t stream_text_last_emit_us;
    uint32_t stream_thinking_chunks;
    uint32_t stream_thinking_suppressed;
    size_t stream_thinking_bytes;
    uint64_t stream_thinking_last_emit_us;
    uint32_t cpu_prev_total_runtime;
    bool cpu_prev_total_valid;
    uint64_t cpu_prev_sample_us;
    uint32_t cpu_sample_seq;
    char last_cpu_json[APP_PROFILE_CPU_JSON_MAX];
    cc_runtime_builder_t *runtime_builder;
    cc_cache_metrics_t *cache_metrics;
    app_profile_cpu_prev_t cpu_prev[APP_PROFILE_CPU_PREV_MAX];
    char last_snapshot[APP_PROFILE_LAST_JSON_MAX];
    app_profile_task_t tasks[APP_PROFILE_TASK_MAX];
    SemaphoreHandle_t mutex;
} app_profile_state_t;

static app_profile_state_t s_profile = {
    .enabled = false,
    .level = APP_PROFILE_LEVEL_BASIC,
    .interval_ms = 0,
};
/* 此完成位持续进程期，发布在 sampler 最后一次 mutex Give 完全返回之后。 */
static int s_sampler_tail_complete = 1;

/** @brief 阻塞取得已创建的 profile mutex，未初始化时跳过。
 * @post mutex 存在时当前线程持锁，owner 保证锁寿命及原生取得成功。
 */
static void profile_lock(void);
/** @brief 归还当前持有的 profile mutex，未初始化时跳过。
 * @post 当前锁区间结束，owner 排除并发销毁。
 */
static void profile_unlock(void);
/** @brief 在持锁状态清空 CPU 差分与缓存窗口。
 * @post 下一新窗口从基线开始计算。
 */
static void reset_cpu_baseline_locked(void);

/**
 * @brief 注册自身后循环采样，响应停止通知，完成全局状态交接后自行删除。
 * @param[in] arg 预留上下文，当前忽略，创建时为 NULL。
 * @post 注销任务并在锁内清句柄、置退出位，最后 mutex Give 返回后 release 发布完成，此后只自删。
 * @note 配置每轮锁内取快照，当前等待可由 stop 通知打断，禁用时保持任务并每秒检查。
 */
static void profile_sampler_task(void *arg)
{
    (void)arg;
    app_profile_register_task("app_profile", xTaskGetCurrentTaskHandle());
    while (true) {
        profile_lock();
        bool enabled = s_profile.enabled;
        int interval_ms = s_profile.interval_ms;
        bool stopping = s_profile.stopping;
        profile_unlock();

        if (stopping) break;

        if (enabled && interval_ms > 0) {
            app_profile_sample("periodic", "interval", false);
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((uint32_t)interval_ms));
        } else {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        }
    }
    app_profile_unregister_task(xTaskGetCurrentTaskHandle());
    profile_lock();
    s_profile.sampler_task = NULL;
    if (s_profile.events) xEventGroupSetBits(s_profile.events, APP_PROFILE_TASK_EXITED_BIT);
    profile_unlock();
    __atomic_store_n(&s_sampler_tail_complete, 1, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

/**
 * @brief 在配置允许且未停止时按需创建唯一 sampler。
 * @post 创建前清尾部完成位，创建与句柄安装在锁内完成，失败在同锁内恢复完成，已有任务保持不变。
 * @pre 初始化/销毁由外层协调，FreeRTOS 新任务可先运行但等待同一 profile 锁。
 */
static void ensure_sampler_task(void)
{
    profile_lock();
    bool need_task = s_profile.mutex && s_profile.events && !s_profile.stopping &&
                     s_profile.enabled && s_profile.interval_ms > 0 &&
                     !s_profile.sampler_task;
    if (!need_task) {
        profile_unlock();
        return;
    }
    __atomic_store_n(&s_sampler_tail_complete, 0, __ATOMIC_RELEASE);
    xEventGroupClearBits(s_profile.events, APP_PROFILE_TASK_EXITED_BIT);
    BaseType_t ok = xTaskCreate(profile_sampler_task,
                                "app_profile",
                                APP_PROFILE_TASK_STACK,
                                NULL,
                                APP_PROFILE_TASK_PRIORITY,
                                &s_profile.sampler_task);
    if (ok != pdPASS) {
        s_profile.dropped++;
        xEventGroupSetBits(s_profile.events, APP_PROFILE_TASK_EXITED_BIT);
        __atomic_store_n(&s_sampler_tail_complete, 1, __ATOMIC_RELEASE);
    }
    profile_unlock();
}

/**
 * @brief 由生命周期 owner 创建或复用 profile mutex、退出事件组及缓存指标对象。
 * @return 成功为 ESP_OK，任一资源分配失败为 ESP_ERR_NO_MEM，已建部分资源留待 destroy。
 * @pre 首次任务注册之前调用，外层排除并发初始化/销毁，复用时 sampler 已停止。
 * @note 成功复位 stopping 并设置无 sampler 退出位，业务配置随后独立应用。
 */
esp_err_t app_profile_init(void)
{
    if (!s_profile.mutex) {
        s_profile.mutex = xSemaphoreCreateMutex();
    }
    if (!s_profile.mutex) return ESP_ERR_NO_MEM;
    if (!s_profile.events) {
        s_profile.events = xEventGroupCreate();
    }
    if (!s_profile.events) return ESP_ERR_NO_MEM;
    if (!s_profile.cache_metrics) {
        cc_result_t rc = cc_cache_metrics_create(&s_profile.cache_metrics);
        if (rc.code != CC_OK) {
            cc_result_free(&rc);
            return ESP_ERR_NO_MEM;
        }
        cc_result_free(&rc);
    }
    s_profile.stopping = false;
    xEventGroupSetBits(s_profile.events, APP_PROFILE_TASK_EXITED_BIT);
    return ESP_OK;
}

/**
 * @brief 阻塞取得 profile mutex，未创建时直接结束。
 * @post 已初始化时持有一次普通 FreeRTOS mutex，须配对 unlock。
 * @pre 非递归持锁，生命周期保持 mutex 有效，原生取得结果没有独立返回通道。
 */
static void profile_lock(void)
{
    if (s_profile.mutex) {
        xSemaphoreTake(s_profile.mutex, portMAX_DELAY);
    }
}

/**
 * @brief 归还 profile mutex，未创建时直接结束。
 * @pre mutex 存在时当前执行者已取得它，owner 排除并发销毁。
 * @post 当前临界区结束。
 */
static void profile_unlock(void)
{
    if (s_profile.mutex) {
        xSemaphoreGive(s_profile.mutex);
    }
}

/**
 * @brief 将配置文本映射为当前支持的采样级别。
 * @param[in] level 调用期借用字符串，NULL 或非精确 trace 使用 BASIC。
 * @return TRACE 或 BASIC，无资源分配。
 */
static app_profile_level_t parse_level(const char *level)
{
    return level && strcmp(level, "trace") == 0 ?
        APP_PROFILE_LEVEL_TRACE : APP_PROFILE_LEVEL_BASIC;
}

/**
 * @brief 返回级别的稳定可读名称。
 * @param[in] level TRACE 以外均映射 BASIC。
 * @return 静态 trace/basic 字符串借用，无需释放。
 */
const char *app_profile_level_string(app_profile_level_t level)
{
    return level == APP_PROFILE_LEVEL_TRACE ? "trace" : "basic";
}

/**
 * @brief 锁内复制采样配置，启用边沿重置 CPU 基线，再尝试启动 sampler。
 * @param[in] config 同步借用配置，NULL 使用禁用、BASIC 与零间隔，不保留配置指针。
 * @return 同步原语缺失为 INVALID_STATE，配置已写入为 ESP_OK，任务创建失败只累计 dropped。
 * @note 停止状态阻止新建 sampler，重新启用生命周期须先由 owner 完成 init 交接。
 */
esp_err_t app_profile_apply_config(const app_profile_config_t *config)
{
    if (!s_profile.mutex || !s_profile.events) return ESP_ERR_INVALID_STATE;
    profile_lock();
    bool was_enabled = s_profile.enabled;
    s_profile.enabled = config && config->enabled != 0;
    s_profile.level = parse_level(config ? config->level : NULL);
    s_profile.interval_ms = config ? config->interval_ms : 0;
    if (s_profile.enabled && !was_enabled) {
        reset_cpu_baseline_locked();
    }
    profile_unlock();
    ensure_sampler_task();
    return ESP_OK;
}

/**
 * @brief 以 acquire 确认 sampler 最后一次 mutex Give 尾部已完成。
 * @param[in] started_us 本次 stop 入口的单调微秒值。
 * @param[in] timeout_ms 本次 stop 总毫秒等待预算，零只作一次完成检查。
 * @return 已发布完成为 ESP_OK，剩余预算耗尽为 ESP_ERR_TIMEOUT。
 * @pre owner 已关闭并排空任务创建入口，stopping 阻止新任务，等待期排除 init/destroy。
 * @note 未完成时每次最多等待一 tick 后重查，原生调度/时钟粒度决定实际返回时刻。
 * 成功 stop 之后 owner 才允许下一轮 init/create，旧任务发布完成后只自删，因此不会覆盖下一轮完成位。
 */
static esp_err_t profile_wait_for_sampler_tail(uint64_t started_us, uint32_t timeout_ms)
{
    while (!__atomic_load_n(&s_sampler_tail_complete, __ATOMIC_ACQUIRE)) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        uint64_t elapsed_us = now_us >= started_us ? now_us - started_us : 0;
        if (elapsed_us >= (uint64_t)timeout_ms * 1000U) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    return ESP_OK;
}

/**
 * @brief 禁用采样并通知 sampler，等待退出位及进程期完成位后结束原语借用。
 * @param[in] timeout_ms 事件和尾部等待共用的毫秒预算，零立即检查，mutex 取得沿端口阻塞语义。
 * @return 无同步对象、无任务或已完成交接为 ESP_OK，退出位或句柄仍未退场为 TIMEOUT。
 * @pre owner 已关闭并排空配置/命令创建入口，当前位于 sampler 之外，排除并发 init/destroy。
 * @note 超时保持 stopping 与资源供重试，SDK 事件订阅由 unsubscribe_runtime 独立排空。
 * 正剩余预算用 64 位计算向上取整为 tick，避开 portMAX_DELAY 哨兵，超过原生上限时分段重查同一截止时间。
 */
esp_err_t app_profile_stop(uint32_t timeout_ms)
{
    uint64_t started_us = (uint64_t)esp_timer_get_time();
    if (!s_profile.mutex || !s_profile.events)
        return profile_wait_for_sampler_tail(started_us, timeout_ms);
    profile_lock();
    s_profile.enabled = false;
    s_profile.stopping = true;
    TaskHandle_t task = s_profile.sampler_task;
    /* 句柄借用与通知都在锁内，采样任务清空句柄后不再接收通知。 */
    if (task) xTaskNotifyGive(task);
    profile_unlock();
    if (!task) {
        return profile_wait_for_sampler_tail(started_us, timeout_ms);
    }
    for (;;) {
        uint64_t now_us = (uint64_t)esp_timer_get_time();
        uint64_t elapsed_us = now_us >= started_us ? now_us - started_us : 0;
        uint64_t budget_us = (uint64_t)timeout_ms * 1000U;
        uint32_t remaining_ms = elapsed_us >= budget_us ? 0U :
            (uint32_t)((budget_us - elapsed_us + 999U) / 1000U);
        uint64_t ticks = ((uint64_t)remaining_ms * configTICK_RATE_HZ + 999U) / 1000U;
        TickType_t maximum_wait = portMAX_DELAY - 1U;
        TickType_t wait_ticks = ticks > maximum_wait ? maximum_wait : (TickType_t)ticks;
        EventBits_t bits = xEventGroupWaitBits(s_profile.events,
                                               APP_PROFILE_TASK_EXITED_BIT,
                                               pdFALSE,
                                               pdTRUE,
                                               wait_ticks);
        if (bits & APP_PROFILE_TASK_EXITED_BIT) break;
        if (remaining_ms == 0) return profile_wait_for_sampler_tail(started_us, timeout_ms);
    }
    return profile_wait_for_sampler_tail(started_us, timeout_ms);
}

/**
 * @brief 先停止 sampler，再释放事件组、mutex 与缓存指标并清空 owner。
 * @param[in] timeout_ms 传给 stop 的毫秒等待预算。
 * @return stop 错误原样返回，仍有 runtime 订阅/借用为 INVALID_STATE，完整清理为 ESP_OK。
 * @pre 外层已排空所有配置、采样、状态查询、注册及 SDK 事件使用者，订阅须先成功退订。
 * @note 失败保持未完成资源，成功后级别恢复 BASIC，其他字段归零。
 */
esp_err_t app_profile_destroy(uint32_t timeout_ms)
{
    esp_err_t err = app_profile_stop(timeout_ms);
    if (err != ESP_OK) return err;
    if (s_profile.runtime_subscribed || s_profile.runtime_builder) {
        return ESP_ERR_INVALID_STATE;
    }
    EventGroupHandle_t events = s_profile.events;
    SemaphoreHandle_t mutex = s_profile.mutex;
    cc_cache_metrics_t *cache_metrics = s_profile.cache_metrics;
    s_profile.events = NULL;
    s_profile.mutex = NULL;
    if (events) vEventGroupDelete(events);
    if (mutex) vSemaphoreDelete(mutex);
    cc_cache_metrics_destroy(cache_metrics);
    memset(&s_profile, 0, sizeof(s_profile));
    s_profile.level = APP_PROFILE_LEVEL_BASIC;
    return ESP_OK;
}

/**
 * @brief 在 profile 锁内把后续缓存指标关联到给定 provider。
 * @param[in] provider 必填同步借用文本，由缓存指标对象复制所需内容。
 * @return NULL 为 INVALID_ARG，成功 ESP_OK，缓存对象缺失或指标更新失败为 ESP_FAIL。
 */
esp_err_t app_profile_set_cache_provider(const char *provider)
{
    if (!provider) return ESP_ERR_INVALID_ARG;
    profile_lock();
    cc_result_t rc = s_profile.cache_metrics ?
        cc_cache_metrics_set_provider(s_profile.cache_metrics, provider) :
        cc_result_error(CC_ERR_INVALID_STATE, "Cache metrics are unavailable");
    profile_unlock();
    esp_err_t result = rc.code == CC_OK ? ESP_OK : ESP_FAIL;
    cc_result_free(&rc);
    return result;
}

/**
 * @brief 锁内更新开关，启用边沿清空 CPU 基线，随后尝试创建 sampler。
 * @param[in] enabled true 启用，false 禁用但保留已有 sampler 等待循环。
 * @post 配置已更新，停止状态阻止新任务，创建失败仅累计 dropped。
 * @pre 业务调用期 profile 已初始化，生命周期排除并发销毁。
 */
void app_profile_set_enabled(bool enabled)
{
    profile_lock();
    bool was_enabled = s_profile.enabled;
    s_profile.enabled = enabled;
    if (enabled && !was_enabled) {
        reset_cpu_baseline_locked();
    }
    profile_unlock();
    ensure_sampler_task();
}

/** @brief 锁内取得当前 profile 开关。
 * @return 当前 enabled 值，生命周期保持 owner 有效。
 */
bool app_profile_enabled(void)
{
    bool enabled = false;
    profile_lock();
    enabled = s_profile.enabled;
    profile_unlock();
    return enabled;
}

/** @brief 锁内更新采样级别。
 * @param[in] level TRACE 保留，其他值归一 BASIC。
 * @post 后续事件过滤使用新级别，不重置现有流式计数。
 */
void app_profile_set_level(app_profile_level_t level)
{
    profile_lock();
    s_profile.level = level == APP_PROFILE_LEVEL_TRACE ?
        APP_PROFILE_LEVEL_TRACE : APP_PROFILE_LEVEL_BASIC;
    profile_unlock();
}

/** @brief 锁内读取采样级别。
 * @return 当前 BASIC 或 TRACE 值。
 */
app_profile_level_t app_profile_level(void)
{
    app_profile_level_t level;
    profile_lock();
    level = s_profile.level;
    profile_unlock();
    return level;
}

/** @brief 锁内设置间隔并尝试启动 sampler。
 * @param[in] interval_ms 毫秒间隔，负值归零，非正值停用周期采样。
 * @post 已有 sampler 在下次唤醒使用新值，本入口不主动打断其当前等待。
 */
void app_profile_set_interval_ms(int interval_ms)
{
    if (interval_ms < 0) {
        interval_ms = 0;
    }
    profile_lock();
    s_profile.interval_ms = interval_ms;
    profile_unlock();
    ensure_sampler_task();
}

/**
 * @brief 锁内安装任务水位监测槽，同句柄或与已存名字精确匹配时替换，满表跳过。
 * @param[in] name 同步借用非空名，复制最多 23 字节并保证 NUL 结尾。
 * @param[in] handle 必填存活任务借用，任务删除前须通过 unregister 排空水位读取。
 * @post 合法输入尽力记录于八个槽，空参数无动作，原槽被替换后旧任务不再被读取。
 */
void app_profile_register_task(const char *name, TaskHandle_t handle)
{
    if (!name || !name[0] || !handle) {
        return;
    }
    profile_lock();
    int free_index = -1;
    for (int i = 0; i < APP_PROFILE_TASK_MAX; i++) {
        if (s_profile.tasks[i].handle == handle ||
            strcmp(s_profile.tasks[i].name, name) == 0) {
            snprintf(s_profile.tasks[i].name, sizeof(s_profile.tasks[i].name), "%s", name);
            s_profile.tasks[i].handle = handle;
            profile_unlock();
            return;
        }
        if (free_index < 0 && !s_profile.tasks[i].handle) {
            free_index = i;
        }
    }
    if (free_index >= 0) {
        snprintf(s_profile.tasks[free_index].name,
                 sizeof(s_profile.tasks[free_index].name),
                 "%s",
                 name);
        s_profile.tasks[free_index].handle = handle;
    }
    profile_unlock();
}

/**
 * @brief 等待锁内水位采样结束并移除所有匹配句柄。
 * @param[in] handle 仍存活的任务句柄借用，NULL 跳过。
 * @post 匹配槽已清零，可在外层完成任务删除，重复注销无动作。
 */
void app_profile_unregister_task(TaskHandle_t handle)
{
    if (!handle) return;
    profile_lock();
    for (int i = 0; i < APP_PROFILE_TASK_MAX; i++) {
        if (s_profile.tasks[i].handle == handle) {
            memset(&s_profile.tasks[i], 0, sizeof(s_profile.tasks[i]));
        }
    }
    profile_unlock();
}

/**
 * @brief 消费一次 JSON 构造结果，保留 builder 的 sticky 错误供最终 take 检查。
 * @param[in,out] result 按值交付的拥有结果，原副本不再保管消息。
 * @post 动态结果消息释放，无资源向外移交。
 */
static void profile_json_result_discard(cc_result_t result)
{
    cc_result_free(&result);
}

/**
 * @brief 将借用正文追加为带引号的 JSON 字符串，首个失败后停止本字段构造。
 * @param[in,out] sb 已初始化的调用者 builder，NULL 时消费参数错误并结束。
 * @param[in] text 同步借用的 NUL 结尾正文，NULL 按空串，普通字节原样复制。
 * @post 本字段已转义或 builder 处于失败状态，每次错误结果消息均已清理。
 */
static void json_escape_append(cc_string_builder_t *sb, const char *text)
{
    cc_result_t rc = cc_string_builder_append_char(sb, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : "");
         rc.code == CC_OK && *p; p++) {
        switch (*p) {
        case '\\': rc = cc_string_builder_append(sb, "\\\\"); break;
        case '"': rc = cc_string_builder_append(sb, "\\\""); break;
        case '\n': rc = cc_string_builder_append(sb, "\\n"); break;
        case '\r': rc = cc_string_builder_append(sb, "\\r"); break;
        case '\t': rc = cc_string_builder_append(sb, "\\t"); break;
        default:
            if (*p < 0x20) rc = cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p);
            else rc = cc_string_builder_append_char(sb, (char)*p);
            break;
        }
    }
    if (rc.code == CC_OK) rc = cc_string_builder_append_char(sb, '"');
    cc_result_free(&rc);
}

/**
 * @brief 清空 CPU 总计数、任务历史及缓存，使重新启用后的首帧从新基线开始。
 * @pre 当前持 profile mutex。
 * @post 采样序号、时间、缓存与已编译的任务差分槽归零。
 */
static void reset_cpu_baseline_locked(void)
{
    s_profile.cpu_prev_total_runtime = 0;
    s_profile.cpu_prev_total_valid = false;
    s_profile.cpu_prev_sample_us = 0;
    s_profile.cpu_sample_seq = 0;
    s_profile.last_cpu_json[0] = '\0';
#if (configGENERATE_RUN_TIME_STATS == 1)
    memset(s_profile.cpu_prev, 0, sizeof(s_profile.cpu_prev));
#endif
}

/** @brief 在 profile 锁内读取普通发布准入。
 * @pre 当前持 profile mutex。
 * @return 当前 enabled 值，强制发布由上层独立处理。
 */
static bool profile_should_emit_locked(void)
{
    if (!s_profile.enabled) {
        return false;
    }
    return true;
}

/**
 * @brief 查询 App EventBus 维护模式，普通发布在维护时累计丢弃。
 * @param[in] force true 跳过本次维护检查。
 * @return 本次须丢弃为 true，否则 false。
 * @note 总线查询在 profile 锁外进行，只在更新 dropped 时持本模块锁。
 */
static bool profile_drop_for_maintenance(bool force)
{
    if (force || !app_event_bus_maintenance_mode_enabled()) {
        return false;
    }
    profile_lock();
    s_profile.dropped++;
    profile_unlock();
    return true;
}

/**
 * @brief 读取八个注册槽的存活句柄水位并追加 stack 对象。
 * @param[in,out] sb 已初始化的调用者 builder，追加结果消息在此消费。
 * @pre 当前持 profile mutex，任务 owner 删除任务前通过同锁注销。
 * @post 名称和值已序列化或 builder 失败，所有句柄借用在解锁前结束。
 */
static void append_stack_json_locked(cc_string_builder_t *sb)
{
    profile_json_result_discard(cc_string_builder_append(sb, "\"stack\":{"));
    bool first = true;
    for (int i = 0; i < APP_PROFILE_TASK_MAX; i++) {
        if (!s_profile.tasks[i].handle || !s_profile.tasks[i].name[0]) {
            continue;
        }
        if (!first) {
            profile_json_result_discard(cc_string_builder_append_char(sb, ','));
        }
        first = false;
        json_escape_append(sb, s_profile.tasks[i].name);
        profile_json_result_discard(cc_string_builder_appendf(sb,
                                  ":%u",
                                  (unsigned)uxTaskGetStackHighWaterMark(s_profile.tasks[i].handle)));
    }
    profile_json_result_discard(cc_string_builder_append_char(sb, '}'));
}

#if (configGENERATE_RUN_TIME_STATS == 1)
/**
 * @brief 在有限任务历史表中查找并更新运行计数基线。
 * @param[in] task_number 本次原生快照的任务编号。
 * @param[in] current 当前 uint32 运行计数，单位沿 FreeRTOS 统计时钟。
 * @param[out] known 可选输出，先置 false，命中已有任务时置 true。
 * @return 命中时返回旧计数，首次或满表时返回 current。
 * @pre 当前持 profile mutex，空槽最多保存四十八个编号，重置基线时清空。
 */
static uint32_t cpu_previous_counter_locked(UBaseType_t task_number,
                                           uint32_t current,
                                           bool *known)
{
    if (known) {
        *known = false;
    }
    int free_index = -1;
    for (int i = 0; i < APP_PROFILE_CPU_PREV_MAX; i++) {
        if (s_profile.cpu_prev[i].seen &&
            s_profile.cpu_prev[i].task_number == task_number) {
            uint32_t previous = s_profile.cpu_prev[i].runtime_counter;
            s_profile.cpu_prev[i].runtime_counter = current;
            if (known) {
                *known = true;
            }
            return previous;
        }
        if (free_index < 0 && !s_profile.cpu_prev[i].seen) {
            free_index = i;
        }
    }
    if (free_index >= 0) {
        s_profile.cpu_prev[free_index].seen = true;
        s_profile.cpu_prev[free_index].task_number = task_number;
        s_profile.cpu_prev[free_index].runtime_counter = current;
    }
    return current;
}
#endif

/**
 * @brief 识别 IDLE 或 IDLE 后首字节为数字的任务名。
 * @param[in] name 同步借用名字，NULL 返回 false。
 * @return 符合当前空闲任务命名规则为 true，后续字符不作额外校验。
 */
static bool app_profile_is_idle_task_name(const char *name)
{
    if (!name) {
        return false;
    }
    if (strcmp(name, "IDLE") == 0) {
        return true;
    }
    return strncmp(name, "IDLE", 4) == 0 &&
           name[4] >= '0' && name[4] <= '9';
}

/**
 * @brief 对已识别的 IDLE 名字取最后一个十进制数字作为后备核心号。
 * @param[in] name 同步借用名字，NULL 或非 IDLE 规则名无结果。
 * @return 最后数字对应的零至九，不能推断为 -1，上层再核对实际核心范围。
 */
static int app_profile_core_from_task_name(const char *name)
{
    if (!app_profile_is_idle_task_name(name)) {
        return -1;
    }
    size_t len = strlen(name);
    if (len == 0) {
        return -1;
    }
    char last = name[len - 1];
    if (last >= '0' && last <= '9') {
        return (int)(last - '0');
    }
    return -1;
}

#if (configUSE_TRACE_FACILITY == 1)
/**
 * @brief 在内核同步窗口内采集标量并复制任务名字和核心，阻止 TCB 删除导致借用失效。
 * @param[out] tasks 必填预分配 TaskStatus 数组，至少 capacity 个槽，名字改指向 identities，句柄清空。
 * @param[out] identities 必填同容量拥有数组，返回后由调用者保管到任务 JSON 序列化结束。
 * @param[in] capacity 数组容量，以任务个数计，任务增多超过容量时原生快照返回零。
 * @param[out] total_runtime 可选总运行计数输出，NULL 不采集，由原生 API 写入。
 * @return 原生实际填写任务数，全部输出内存由调用者释放。
 * @note 当前 IDF 双核内核取同一可嵌套 kernel lock，单核和 upstream SMP 用 suspend/resume；
 * 窗口内仅原生快照、身份复制和核心查询，无堆分配、JSON、profile 锁或事件发布。
 */
static UBaseType_t collect_cpu_snapshot(TaskStatus_t *tasks,
                                        app_profile_cpu_identity_t *identities,
                                        UBaseType_t capacity,
                                        uint32_t *total_runtime)
{
#if !CONFIG_FREERTOS_SMP && (APP_PROFILE_CPU_CORES > 1)
    prvTakeKernelLock();
#else
    vTaskSuspendAll();
#endif
    UBaseType_t filled = uxTaskGetSystemState(tasks, capacity, total_runtime);
    for (UBaseType_t i = 0; i < filled; i++) {
        const char *name = tasks[i].pcTaskName;
        size_t length = 0;
        while (name && length + 1U < sizeof(identities[i].name) && name[length]) {
            identities[i].name[length] = name[length];
            length++;
        }
        identities[i].name[length] = '\0';
        identities[i].core_id = APP_PROFILE_CPU_CORES == 1U ? 0 : -1;
#if (APP_PROFILE_CPU_CORES > 1)
#if (configTASKLIST_INCLUDE_COREID == 1)
        BaseType_t core_id = tasks[i].xCoreID;
#else
        BaseType_t core_id = tasks[i].xHandle ? xTaskGetCoreID(tasks[i].xHandle) : -1;
#endif
        if (core_id >= 0 && core_id < (BaseType_t)APP_PROFILE_CPU_CORES)
            identities[i].core_id = (int)core_id;
#endif
        tasks[i].pcTaskName = identities[i].name;
        tasks[i].xHandle = NULL;
    }
#if !CONFIG_FREERTOS_SMP && (APP_PROFILE_CPU_CORES > 1)
    prvReleaseKernelLock();
#else
    (void)xTaskResumeAll();
#endif
    return filled;
}
#endif

/**
 * @brief 采集一次 CPU 差分窗口并追加核心利用率和前八个任务。
 * @param[in,out] sb 已初始化的借用 builder，保留 sticky 失败状态。
 * @post 快照和身份副本释放，基线锁内更新，分配或配置不足时追加可诊断不可用原因。
 * @pre 唯一周期 sampler 推进窗口，生命周期排除与 destroy 并发。
 */
static void append_cpu_json_fresh(cc_string_builder_t *sb)
{
    uint32_t sample_seq = 0;
    profile_lock();
    sample_seq = ++s_profile.cpu_sample_seq;
    profile_unlock();
#if (configUSE_TRACE_FACILITY == 1)
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        profile_json_result_discard(cc_string_builder_appendf(sb, "\"cpu\":{\"available\":false,\"reason\":\"no_tasks\",\"sample_seq\":%u}",
                                  (unsigned)sample_seq));
        return;
    }
    TaskStatus_t *tasks = heap_caps_calloc((size_t)task_count,
                                           sizeof(TaskStatus_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    app_profile_cpu_identity_t *identities = heap_caps_calloc((size_t)task_count,
        sizeof(*identities), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tasks || !identities) {
        heap_caps_free(tasks);
        heap_caps_free(identities);
        profile_json_result_discard(cc_string_builder_appendf(sb, "\"cpu\":{\"available\":false,\"reason\":\"oom\",\"sample_seq\":%u}",
                                  (unsigned)sample_seq));
        return;
    }
#if (configGENERATE_RUN_TIME_STATS == 1)
    uint32_t total_runtime = 0;
    UBaseType_t filled = collect_cpu_snapshot(tasks, identities, task_count, &total_runtime);
    uint32_t previous_total = 0;
    bool have_previous_total = false;
    profile_lock();
    previous_total = s_profile.cpu_prev_total_runtime;
    /* 总计数倒退表示时钟重置或回绕，本次从基线开始，避免放大差分窗口。 */
    have_previous_total = s_profile.cpu_prev_total_valid && total_runtime >= previous_total;
    s_profile.cpu_prev_total_runtime = total_runtime;
    s_profile.cpu_prev_total_valid = true;
    profile_unlock();
    uint32_t total_delta = have_previous_total ? total_runtime - previous_total : 0;
#else
    UBaseType_t filled = collect_cpu_snapshot(tasks, identities, task_count, NULL);
#endif
    app_profile_cpu_task_sample_t samples[APP_PROFILE_CPU_TASK_MAX];
    UBaseType_t sample_count = 0;
#if (configGENERATE_RUN_TIME_STATS == 1)
    /* ESP timer 给出共同时间窗口，多核任务可同时累积计数，因此全局比例除以总核容量，
     * 已固定核心的任务再换算为该核比例。 */
    uint64_t aggregate_total_delta = (uint64_t)total_delta * APP_PROFILE_CPU_CORES;
    uint32_t idle_delta_by_core[APP_PROFILE_CPU_CORES] = {0};
    bool idle_seen_by_core[APP_PROFILE_CPU_CORES] = {false};
    uint32_t core_pct_x100[APP_PROFILE_CPU_CORES] = {0};
    uint32_t core_idle_pct_x100[APP_PROFILE_CPU_CORES] = {0};
#endif
#if (configGENERATE_RUN_TIME_STATS == 1)
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint64_t previous_sample_us = 0;
    bool have_previous_sample = false;
#endif
    for (UBaseType_t i = 0; i < filled; i++) {
        app_profile_cpu_task_sample_t sample = {
            .name = tasks[i].pcTaskName ? tasks[i].pcTaskName : "",
            .task_id = tasks[i].xTaskNumber,
            .state = tasks[i].eCurrentState,
            .priority = tasks[i].uxCurrentPriority,
            .stack_high_water_words = (uint32_t)tasks[i].usStackHighWaterMark,
            .core_id = APP_PROFILE_CPU_CORES == 1U ? 0 :
                       app_profile_core_from_task_name(tasks[i].pcTaskName),
            .runtime_counter = 0,
            .runtime_delta = 0,
            .cpu_pct_x100 = 0,
        };
        if (identities[i].core_id >= 0) sample.core_id = identities[i].core_id;
#if (configGENERATE_RUN_TIME_STATS == 1)
        uint32_t current = (uint32_t)tasks[i].ulRunTimeCounter;
        bool known = false;
        profile_lock();
        uint32_t previous = cpu_previous_counter_locked(tasks[i].xTaskNumber, current, &known);
        profile_unlock();
        sample.runtime_counter = current;
        /* uint32_t 减法本身可处理一次计数器回绕；异常大的差值视为任务重置。 */
        sample.runtime_delta = known ? current - previous : 0;
        if (!known || sample.runtime_delta > total_delta) {
            sample.runtime_delta = 0;
        }
        sample.cpu_pct_x100 = aggregate_total_delta > 0 && have_previous_total &&
                              sample.runtime_delta <= total_delta ?
            (uint32_t)(((uint64_t)sample.runtime_delta * 10000ULL) / aggregate_total_delta) : 0;
        if (sample.core_id >= 0 && sample.cpu_pct_x100 > 0) {
            uint64_t core_pct_x100 = (uint64_t)sample.cpu_pct_x100 * APP_PROFILE_CPU_CORES;
            sample.core_pct_x100 = core_pct_x100 > 10000ULL ? 10000U : (uint32_t)core_pct_x100;
        }
        if (have_previous_total && total_delta > 0 &&
            app_profile_is_idle_task_name(sample.name) &&
            sample.core_id >= 0 && sample.core_id < (int)APP_PROFILE_CPU_CORES) {
            idle_seen_by_core[sample.core_id] = true;
            idle_delta_by_core[sample.core_id] += sample.runtime_delta;
        }
#endif
        UBaseType_t pos = sample_count;
        if (pos < APP_PROFILE_CPU_TASK_MAX) {
            sample_count++;
        } else if (sample.runtime_delta <= samples[APP_PROFILE_CPU_TASK_MAX - 1].runtime_delta) {
            continue;
        } else {
            pos = APP_PROFILE_CPU_TASK_MAX - 1;
        }
        while (pos > 0 && sample.runtime_delta > samples[pos - 1].runtime_delta) {
            samples[pos] = samples[pos - 1];
            pos--;
        }
        samples[pos] = sample;
    }
#if (configGENERATE_RUN_TIME_STATS == 1)
    profile_lock();
    previous_sample_us = s_profile.cpu_prev_sample_us;
    have_previous_sample = previous_sample_us > 0;
    s_profile.cpu_prev_sample_us = now_us;
    profile_unlock();
    uint64_t sample_window_ms_u64 = have_previous_sample && now_us > previous_sample_us ?
        (now_us - previous_sample_us) / 1000ULL : 0;
    uint32_t sample_window_ms = sample_window_ms_u64 > UINT32_MAX ?
        UINT32_MAX : (uint32_t)sample_window_ms_u64;
    bool window_ready = have_previous_total && total_delta > 0 &&
                        sample_window_ms >= APP_PROFILE_CPU_MIN_WINDOW_MS && filled > 0;
    uint32_t total_cpu_pct_x100 = 0;
    UBaseType_t observed_core_count = 0;
    if (total_delta > 0) {
        for (UBaseType_t core = 0; core < APP_PROFILE_CPU_CORES; core++) {
            if (!idle_seen_by_core[core]) {
                continue;
            }
            uint64_t idle_pct_x100 = ((uint64_t)idle_delta_by_core[core] * 10000ULL) /
                                     (uint64_t)total_delta;
            core_idle_pct_x100[core] = idle_pct_x100 >= 10000ULL ?
                10000U : (uint32_t)idle_pct_x100;
            core_pct_x100[core] = idle_pct_x100 >= 10000ULL ? 0U : (uint32_t)(10000ULL - idle_pct_x100);
            total_cpu_pct_x100 += core_pct_x100[core];
            observed_core_count++;
        }
        if (observed_core_count > 0) {
            total_cpu_pct_x100 /= observed_core_count;
        }
    }
    bool sample_valid = window_ready &&
                        observed_core_count == APP_PROFILE_CPU_CORES;
    const char *invalid_reason = sample_valid ? "" :
        (!have_previous_total ? ",\"reason\":\"baseline\"" :
         sample_window_ms < APP_PROFILE_CPU_MIN_WINDOW_MS ? ",\"reason\":\"window_too_short\"" :
         observed_core_count < APP_PROFILE_CPU_CORES ? ",\"reason\":\"core_idle_missing\"" :
         !sample_valid ? ",\"reason\":\"zero_window\"" :
                         ",\"reason\":\"core_idle_missing\"");
    profile_json_result_discard(cc_string_builder_appendf(sb,
                              "\"cpu\":{\"sample_seq\":%u,\"available\":true,\"runtime_stats\":true,"
                              "\"valid\":%s,\"window_ms\":%u,"
                              "\"task_count\":%u,\"cores\":%u,\"tr\":%u,"
                              "\"td\":%u,\"total_pct\":",
                              (unsigned)sample_seq,
                              sample_valid ? "true" : "false",
                              (unsigned)sample_window_ms,
                              (unsigned)filled,
                              (unsigned)APP_PROFILE_CPU_CORES,
                              (unsigned)total_runtime,
                              (unsigned)total_delta));
    if (sample_valid && observed_core_count > 0) {
        profile_json_result_discard(cc_string_builder_appendf(sb,
                                  "%u.%02u",
                                  (unsigned)(total_cpu_pct_x100 / 100U),
                                  (unsigned)(total_cpu_pct_x100 % 100U)));
    } else {
        profile_json_result_discard(cc_string_builder_append(sb, "null"));
    }
    profile_json_result_discard(cc_string_builder_appendf(sb, "%s,\"core_pct\":[", invalid_reason));
    for (UBaseType_t core = 0; core < APP_PROFILE_CPU_CORES; core++) {
        if (core > 0) {
            profile_json_result_discard(cc_string_builder_append_char(sb, ','));
        }
        if (window_ready && idle_seen_by_core[core]) {
            profile_json_result_discard(cc_string_builder_appendf(sb, "%u.%02u",
                                      (unsigned)(core_pct_x100[core] / 100U),
                                      (unsigned)(core_pct_x100[core] % 100U)));
        } else {
            profile_json_result_discard(cc_string_builder_append(sb, "null"));
        }
    }
    profile_json_result_discard(cc_string_builder_append(sb, "],\"core_idle_pct\":["));
    for (UBaseType_t core = 0; core < APP_PROFILE_CPU_CORES; core++) {
        if (core > 0) {
            profile_json_result_discard(cc_string_builder_append_char(sb, ','));
        }
        if (window_ready && idle_seen_by_core[core]) {
            profile_json_result_discard(cc_string_builder_appendf(sb, "%u.%02u",
                                      (unsigned)(core_idle_pct_x100[core] / 100U),
                                      (unsigned)(core_idle_pct_x100[core] % 100U)));
        } else {
            profile_json_result_discard(cc_string_builder_append(sb, "null"));
        }
    }
    profile_json_result_discard(cc_string_builder_append(sb, "],\"tasks\":["));
#else
    profile_json_result_discard(cc_string_builder_appendf(sb,
                              "\"cpu\":{\"sample_seq\":%u,\"available\":true,\"runtime_stats\":false,"
                              "\"valid\":false,\"reason\":\"runtime_stats_disabled\","
                              "\"task_count\":%u,\"tasks\":[",
                              (unsigned)sample_seq,
                              (unsigned)filled));
#endif
    for (UBaseType_t i = 0; i < sample_count; i++) {
        if (i > 0) {
            profile_json_result_discard(cc_string_builder_append_char(sb, ','));
        }
        profile_json_result_discard(cc_string_builder_appendf(sb, "{\"id\":%u,\"n\":",
                                  (unsigned)samples[i].task_id));
        json_escape_append(sb, samples[i].name);
        profile_json_result_discard(cc_string_builder_appendf(sb,
                                  ",\"s\":%u,\"pri\":%u,\"stk\":%u,\"c\":%d",
                                  (unsigned)samples[i].state,
                                  (unsigned)samples[i].priority,
                                  (unsigned)samples[i].stack_high_water_words,
                                  samples[i].core_id));
#if (configGENERATE_RUN_TIME_STATS == 1)
        profile_json_result_discard(cc_string_builder_appendf(sb,
                                  ",\"r\":%u,\"d\":%u,\"p\":%u.%02u",
                                  (unsigned)samples[i].runtime_counter,
                                  (unsigned)samples[i].runtime_delta,
                                  (unsigned)(samples[i].cpu_pct_x100 / 100U),
                                  (unsigned)(samples[i].cpu_pct_x100 % 100U)));
        if (samples[i].core_id >= 0) {
            profile_json_result_discard(cc_string_builder_appendf(sb,
                                      ",\"core_p\":%u.%02u",
                                      (unsigned)(samples[i].core_pct_x100 / 100U),
                                      (unsigned)(samples[i].core_pct_x100 % 100U)));
        }
#endif
        profile_json_result_discard(cc_string_builder_append_char(sb, '}'));
    }
    if (filled > sample_count) {
        profile_json_result_discard(cc_string_builder_appendf(sb, "],\"truncated\":true,\"reported_tasks\":%u}",
                                  (unsigned)sample_count));
    } else {
        profile_json_result_discard(cc_string_builder_append(sb, "],\"truncated\":false}"));
    }
    heap_caps_free(tasks);
    heap_caps_free(identities);
#else
    profile_json_result_discard(cc_string_builder_appendf(sb,
                             "\"cpu\":{\"available\":false,"
                             "\"reason\":\"CONFIG_FREERTOS_USE_TRACE_FACILITY disabled\","
                             "\"sample_seq\":%u}",
                             (unsigned)sample_seq));
#endif
}

/**
 * @brief 追加最近 CPU 窗口，只有专用周期调用才推进基线并刷新缓存。
 * @param[in,out] sb 已初始化的借用 builder，失败保留其 sticky 状态。
 * @param[in] collect_window true 构造新窗口，false 在锁内复制最近缓存或基线占位。
 * @post 本次临时 builder、正文和拥有结果清理，超出缓存容量的完整窗口只追加给调用者。
 */
static void append_cpu_json(cc_string_builder_t *sb, bool collect_window)
{
    if (!collect_window) {
        profile_lock();
        if (s_profile.last_cpu_json[0]) {
            profile_json_result_discard(cc_string_builder_append(sb, s_profile.last_cpu_json));
        } else {
            profile_json_result_discard(cc_string_builder_appendf(
                sb,
                "\"cpu\":{\"available\":%s,\"runtime_stats\":%s,"
                "\"valid\":false,\"reason\":\"baseline\",\"sample_seq\":0}",
                configUSE_TRACE_FACILITY == 1 ? "true" : "false",
                configUSE_TRACE_FACILITY == 1 && configGENERATE_RUN_TIME_STATS == 1 ? "true" : "false"));
        }
        profile_unlock();
        return;
    }

    cc_string_builder_t cpu;
    cc_result_t init_rc = cc_string_builder_init(&cpu);
    bool initialized = init_rc.code == CC_OK;
    cc_result_free(&init_rc);
    if (!initialized) {
        profile_json_result_discard(cc_string_builder_appendf(
            sb,
            "\"cpu\":{\"available\":%s,\"runtime_stats\":%s,"
            "\"valid\":false,\"reason\":\"snapshot_builder_oom\",\"sample_seq\":0}",
            configUSE_TRACE_FACILITY == 1 ? "true" : "false",
            configUSE_TRACE_FACILITY == 1 && configGENERATE_RUN_TIME_STATS == 1 ? "true" : "false"));
        return;
    }
    append_cpu_json_fresh(&cpu);
    char *snapshot = cc_string_builder_take(&cpu);
    if (!snapshot) {
        profile_json_result_discard(cc_string_builder_appendf(
            sb,
            "\"cpu\":{\"available\":%s,\"runtime_stats\":%s,"
            "\"valid\":false,\"reason\":\"snapshot_empty\",\"sample_seq\":0}",
            configUSE_TRACE_FACILITY == 1 ? "true" : "false",
            configUSE_TRACE_FACILITY == 1 && configGENERATE_RUN_TIME_STATS == 1 ? "true" : "false"));
        return;
    }

    profile_lock();
    if (strlen(snapshot) < sizeof(s_profile.last_cpu_json)) {
        snprintf(s_profile.last_cpu_json,
                 sizeof(s_profile.last_cpu_json),
                 "%s",
                 snapshot);
    } else {
        s_profile.last_cpu_json[0] = '\0';
    }
    profile_unlock();
    profile_json_result_discard(cc_string_builder_append(sb, snapshot));
    free(snapshot);
}

/**
 * @brief 取得 App EventBus 独立状态快照并追加计数成员。
 * @param[in,out] sb 已初始化的借用 builder，NULL 或追加失败的结果消息在此消费。
 * @post 不保留总线借用，builder 保管正文或 sticky 失败。
 */
static void append_event_bus_json(cc_string_builder_t *sb)
{
    app_event_bus_status_t events;
    app_event_bus_get_status(&events);
    profile_json_result_discard(cc_string_builder_appendf(sb,
                              "\"event_bus\":{\"published\":%u,\"dropped\":%u,"
                              "\"ring_overwrites\":%u,\"sink_dropped\":%u,"
                              "\"maintenance\":%s,\"maintenance_skipped\":%u,"
                              "\"oom_dropped\":%u,\"usb_queue_dropped\":%u,"
                              "\"display_queue_dropped\":%u,\"max_usb_queue_waiting\":%u,"
                              "\"max_display_queue_waiting\":%u,\"oldest_seq\":%u,"
                              "\"latest_seq\":%u,\"capacity\":%u}",
                              (unsigned)events.published,
                              (unsigned)events.dropped,
                              (unsigned)events.ring_overwrites,
                              (unsigned)events.sink_dropped,
                              events.maintenance_mode ? "true" : "false",
                              (unsigned)events.maintenance_skipped,
                              (unsigned)events.oom_dropped,
                              (unsigned)events.usb_queue_dropped,
                              (unsigned)events.display_queue_dropped,
                              (unsigned)events.max_usb_queue_waiting,
                              (unsigned)events.max_display_queue_waiting,
                              (unsigned)events.oldest_seq,
                              (unsigned)events.latest_seq,
                              (unsigned)events.capacity));
}

/**
 * @brief 获取工具池指标并在代际租约有效期间把借用的 lane 名称写入 JSON。
 * @pre profile owner 保证借用的 runtime_builder 在取租约期间仍存活，sb 已初始化。
 * @param[in,out] sb 借用的字符串构建器，追加工具池快照或不可用原因，保持其既有错误状态。
 * @post 每条退出路径归还本次取得的代际，成功快照中的名称在最后释放前完成序列化。
 * @note 未编译工具池时只追加不可用状态，实际并发 reload 与目标板资源另行验证。
 */
static void append_tool_pool_json(cc_string_builder_t *sb)
{
#if CC_ENABLE_TOOL_POOL
    cc_runtime_builder_t *builder = NULL;
    profile_lock();
    builder = s_profile.runtime_builder;
    profile_unlock();
    cc_runtime_generation_t *generation = NULL;
    cc_result_t lease = builder ? cc_runtime_builder_acquire_generation(builder, &generation) :
        cc_result_error(CC_ERR_INVALID_STATE, "Runtime builder unavailable");
    cc_agent_runtime_t *runtime = lease.code == CC_OK ?
        cc_runtime_generation_runtime(generation) : NULL;
    cc_tool_executor_pool_t *pool = cc_agent_runtime_tool_pool(runtime);
    if (!pool) {
        if (generation) cc_runtime_generation_release(generation);
        cc_result_free(&lease);
        profile_json_result_discard(cc_string_builder_append(sb, "\"tool_pool\":{\"available\":false}"));
        return;
    }

    cc_tool_executor_pool_lane_metrics_t lanes[12];
    cc_tool_executor_pool_metrics_t metrics;
    cc_result_t rc = cc_tool_executor_pool_get_metrics(pool,
                                                       &metrics,
                                                       lanes,
                                                       sizeof(lanes) / sizeof(lanes[0]));
    cc_result_free(&lease);
    if (rc.code != CC_OK) {
        cc_runtime_generation_release(generation);
        cc_result_free(&rc);
        profile_json_result_discard(cc_string_builder_append(sb, "\"tool_pool\":{\"available\":false,\"reason\":\"metrics_error\"}"));
        return;
    }
    cc_result_free(&rc);

    profile_json_result_discard(cc_string_builder_appendf(sb,
                              "\"tool_pool\":{\"available\":true,\"lane_count\":%u,"
                              "\"reported_lane_count\":%u,\"default_concurrency\":%d,"
                              "\"default_timeout_ms\":%d,\"in_flight\":%d,\"waiting\":%d,"
                              "\"max_waiting\":%d,\"acquired\":%lu,\"released\":%lu,"
                              "\"cancelled\":%lu,\"lanes\":[",
                              (unsigned)metrics.lane_count,
                              (unsigned)metrics.reported_lane_count,
                              metrics.default_concurrency,
                              metrics.default_timeout_ms,
                              metrics.total_in_flight,
                              metrics.total_waiting,
                              metrics.max_total_waiting,
                              metrics.acquired,
                              metrics.released,
                              metrics.cancelled));
    for (size_t i = 0; i < metrics.reported_lane_count; i++) {
        if (i > 0) {
            profile_json_result_discard(cc_string_builder_append_char(sb, ','));
        }
        profile_json_result_discard(cc_string_builder_append(sb, "{\"name\":"));
        json_escape_append(sb, lanes[i].name ? lanes[i].name : "");
        profile_json_result_discard(cc_string_builder_appendf(sb,
                                  ",\"concurrency\":%d,\"timeout_ms\":%d,"
                                  "\"in_flight\":%d,\"waiting\":%d,\"max_waiting\":%d,"
                                  "\"acquired\":%lu,\"released\":%lu,\"cancelled\":%lu}",
                                  lanes[i].concurrency,
                                  lanes[i].timeout_ms,
                                  lanes[i].in_flight,
                                  lanes[i].waiting,
                                  lanes[i].max_waiting,
                                  lanes[i].acquired,
                                  lanes[i].released,
                                  lanes[i].cancelled));
    }
    profile_json_result_discard(cc_string_builder_append(sb, "]}"));
    cc_runtime_generation_release(generation);
#else
    profile_json_result_discard(cc_string_builder_append(sb, "\"tool_pool\":{\"available\":false,\"reason\":\"CC_ENABLE_TOOL_POOL disabled\"}"));
#endif
}

/**
 * @brief 构造事件、堆、注册任务栈、CPU、总线和工具池的完整 profile 快照。
 * @param[in] event 同步借用事件名，NULL 使用 sample。
 * @param[in] phase 同步借用阶段名，NULL 使用空串。
 * @param[in] session_id 同步借用会话标识，NULL 使用空串。
 * @param[in] run_id 同步借用运行标识，NULL 使用空串。
 * @param[in] status 可选借用状态，NULL 省略成员。
 * @param[in] elapsed_us 已计算的耗时，单位微秒。
 * @param[in] sdk_event_type 可选 SDK 事件名，NULL 省略成员。
 * @param[in] step 非负值追加步骤与可选关联 ID，负值省略。
 * @param[in] attributes_json 可选同步借用的完整属性 JSON，非空时原样追加，内部调用者负责有效性。
 * @param[in] error_json 可选同步借用的完整错误 JSON，非空时原样追加。
 * @param[in] collect_cpu true 推进专用 CPU 窗口，其他快照复用缓存。
 * @return 调用者 free 的拥有正文，初始化或 sticky 追加失败返回 NULL，结果消息均清理。
 * @note 子模块各自查询，状态可来自相邻时刻，任务句柄水位读取持 profile 锁覆盖。
 */
static char *build_profile_json(const char *event,
                                const char *phase,
                                const char *session_id,
                                const char *run_id,
                                const char *status,
                                uint64_t elapsed_us,
                                const char *sdk_event_type,
                                int step,
                                const char *attributes_json,
                                const char *error_json,
                                bool collect_cpu)
{
    cc_string_builder_t sb;
    cc_result_t init_rc = cc_string_builder_init(&sb);
    bool initialized = init_rc.code == CC_OK;
    cc_result_free(&init_rc);
    if (!initialized) {
        return NULL;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    profile_json_result_discard(cc_string_builder_append(&sb, "{\"event\":"));
    json_escape_append(&sb, event ? event : "sample");
    profile_json_result_discard(cc_string_builder_append(&sb, ",\"phase\":"));
    json_escape_append(&sb, phase ? phase : "");
    profile_json_result_discard(cc_string_builder_append(&sb, ",\"run_id\":"));
    json_escape_append(&sb, run_id ? run_id : "");
    profile_json_result_discard(cc_string_builder_append(&sb, ",\"session_id\":"));
    json_escape_append(&sb, session_id ? session_id : "");
    if (step >= 0) {
        profile_json_result_discard(cc_string_builder_appendf(&sb, ",\"step\":%d", step));
        if (session_id && session_id[0]) {
            profile_json_result_discard(cc_string_builder_append(&sb, ",\"correlation_id\":"));
            profile_json_result_discard(cc_string_builder_append_char(&sb, '"'));
            for (const unsigned char *p = (const unsigned char *)session_id; *p; p++) {
                switch (*p) {
                case '\\': profile_json_result_discard(cc_string_builder_append(&sb, "\\\\")); break;
                case '"': profile_json_result_discard(cc_string_builder_append(&sb, "\\\"")); break;
                default:
                    if (*p < 0x20) profile_json_result_discard(cc_string_builder_appendf(&sb, "\\u%04x", (unsigned)*p));
                    else profile_json_result_discard(cc_string_builder_append_char(&sb, (char)*p));
                    break;
                }
            }
            profile_json_result_discard(cc_string_builder_appendf(&sb, ":%d\"", step));
        }
    }
    profile_json_result_discard(cc_string_builder_appendf(&sb,
                              ",\"timestamp_ms\":%" PRIu64 ",\"elapsed_us\":%" PRIu64,
                              now_us / 1000U,
                              elapsed_us));
    if (status) {
        profile_json_result_discard(cc_string_builder_append(&sb, ",\"status\":"));
        json_escape_append(&sb, status);
    }
    if (sdk_event_type) {
        profile_json_result_discard(cc_string_builder_append(&sb, ",\"sdk_event\":"));
        json_escape_append(&sb, sdk_event_type);
    }
    if (attributes_json && attributes_json[0]) {
        profile_json_result_discard(cc_string_builder_append(&sb, ",\"attributes\":"));
        profile_json_result_discard(cc_string_builder_append(&sb, attributes_json));
    }
    if (error_json && error_json[0]) {
        profile_json_result_discard(cc_string_builder_append(&sb, ",\"error\":"));
        profile_json_result_discard(cc_string_builder_append(&sb, error_json));
    }
    profile_json_result_discard(cc_string_builder_appendf(&sb,
                              ",\"heap\":{\"free\":%u,\"min_free\":%u,"
                              "\"internal_free\":%u,\"internal_total\":%u,\"internal_largest\":%u,\"internal_min_free\":%u,"
                              "\"psram_free\":%u,\"psram_total\":%u,\"psram_largest\":%u,\"psram_min_free\":%u},",
                              (unsigned)esp_get_free_heap_size(),
                              (unsigned)esp_get_minimum_free_heap_size(),
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                              (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                              (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
    profile_lock();
    append_stack_json_locked(&sb);
    profile_unlock();
    profile_json_result_discard(cc_string_builder_append_char(&sb, ','));
    append_cpu_json(&sb, collect_cpu);
    profile_json_result_discard(cc_string_builder_append_char(&sb, ','));
    append_event_bus_json(&sb);
    profile_json_result_discard(cc_string_builder_append_char(&sb, ','));
    append_tool_pool_json(&sb);
    profile_json_result_discard(cc_string_builder_append_char(&sb, '}'));
    return cc_string_builder_take(&sb);
}

/**
 * @brief 通过维护/启用准入后构造快照，更新最近状态，再锁外尽力发布 App profile 事件。
 * @param[in] event 同步借用事件名，NULL 按 sample。
 * @param[in] phase 同步借用阶段，NULL 按空串。
 * @param[in] session_id 可选同步借用会话标识。
 * @param[in] run_id 可选同步借用运行标识。
 * @param[in] status 可选同步借用状态。
 * @param[in] elapsed_us 耗时微秒。
 * @param[in] sdk_event_type 可选同步借用 SDK 事件名。
 * @param[in] step 非负步骤编号，负值省略。
 * @param[in] attributes_json 可选同步借用的有效 JSON 成员正文。
 * @param[in] error_json 可选同步借用的有效错误 JSON 正文。
 * @param[in] collect_cpu 是否由唯一周期 sampler 推进 CPU 窗口。
 * @param[in] force true 绕过维护和 enabled 检查。
 * @post 构造失败累计 dropped，成功累计 samples 并保管有限缓存，事件正文返回前释放。
 * @note 发布前的配置快照允许相邻时刻变化，App EventBus 发布在 profile 锁外执行。
 */
static void publish_profile(const char *event,
                            const char *phase,
                            const char *session_id,
                            const char *run_id,
                            const char *status,
                            uint64_t elapsed_us,
                            const char *sdk_event_type,
                            int step,
                            const char *attributes_json,
                            const char *error_json,
                            bool collect_cpu,
                            bool force)
{
    if (profile_drop_for_maintenance(force)) {
        return;
    }

    profile_lock();
    bool emit = force || profile_should_emit_locked();
    profile_unlock();
    if (!emit) {
        return;
    }

    char *json = build_profile_json(event,
                                    phase,
                                    session_id,
                                    run_id,
                                    status,
                                    elapsed_us,
                                    sdk_event_type,
                                    step,
                                    attributes_json,
                                    error_json,
                                    collect_cpu);
    if (!json) {
        profile_lock();
        s_profile.dropped++;
        profile_unlock();
        return;
    }

    profile_lock();
    s_profile.samples++;
    if (strlen(json) < sizeof(s_profile.last_snapshot)) {
        snprintf(s_profile.last_snapshot, sizeof(s_profile.last_snapshot), "%s", json);
    } else {
        snprintf(s_profile.last_snapshot,
                 sizeof(s_profile.last_snapshot),
                 "{\"truncated\":true,\"bytes\":%u}",
                 (unsigned)strlen(json));
    }
    profile_unlock();

    app_event_publish(APP_EVENT_KIND_PROFILE, APP_EVENT_SOURCE_SYSTEM, CC_LOG_INFO, json);
    free(json);
}

/**
 * @brief 按当前 enabled 快照记录跨度起点，按值借用事件身份。
 * @param[in] event 可选事件名借用，持续到 span_end 返回。
 * @param[in] phase 可选阶段名借用，持续到 span_end 返回。
 * @param[in] session_id 可选会话名借用，持续到 span_end 返回。
 * @param[in] run_id 可选运行名借用，持续到 span_end 返回。
 * @return 无动态资源的跨度值，时间以单调微秒计，保存 enabled 起点快照。
 */
app_profile_span_t app_profile_span_begin(const char *event,
                                          const char *phase,
                                          const char *session_id,
                                          const char *run_id)
{
    app_profile_span_t span = {
        .enabled = app_profile_enabled(),
        .start_us = (uint64_t)esp_timer_get_time(),
        .event = event,
        .phase = phase,
        .session_id = session_id,
        .run_id = run_id,
    };
    return span;
}

/**
 * @brief 计算跨度耗时并尝试发布，CPU 继续复用最近窗口。
 * @param[in] span 同步借用跨度，NULL 或起点未启用时跳过，不修改跨度。
 * @param[in] status 可选同步借用状态，NULL 省略字段。
 * @post 本次发布完成或按当前配置/维护状态跳过，重复调用可重复发布同一跨度。
 */
void app_profile_span_end(app_profile_span_t *span, const char *status)
{
    if (!span || !span->enabled) {
        return;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    publish_profile(span->event,
                    span->phase,
                    span->session_id,
                    span->run_id,
                    status,
                    now_us >= span->start_us ? now_us - span->start_us : 0,
                    NULL,
                    -1,
                    NULL,
                    NULL,
                    false,
                    false);
}

/**
 * @brief 发布无会话身份的采样点，保留普通准入或显式强制语义。
 * @param[in] event 同步借用事件名，NULL 使用 sample。
 * @param[in] phase 同步借用阶段名，NULL 使用 point。
 * @param[in] force true 绕过 enabled 与维护门禁。
 * @post 临时正文清理，只有精确 periodic/interval 身份推进 CPU 窗口。
 * @pre 外层保证该特殊身份仅由唯一周期 sampler 调用，并排空发布后才 destroy。
 */
void app_profile_sample(const char *event, const char *phase, bool force)
{
    const char *event_name = event ? event : "sample";
    const char *phase_name = phase ? phase : "point";
    /* 只有专用周期身份推进运行计数，手动点和事件复用最近的完整窗口。 */
    bool collect_cpu =
        strcmp(event_name, "periodic") == 0 && strcmp(phase_name, "interval") == 0;
    publish_profile(event_name,
                    phase_name,
                    NULL,
                    NULL,
                    "ok",
                    0,
                    NULL,
                    -1,
                    NULL,
                    NULL,
                    collect_cpu,
                    force);
}

/**
 * @brief 从事件树读取字符串字段。
 * @param[in] root 可选 JSON 树借用，NULL 返回 NULL。
 * @param[in] key 必填同步借用字段名，沿 JSON helper 的字段查找规则。
 * @return 字符串借用或 NULL，寿命覆盖到 root 销毁前。
 */
static const char *json_string_field(cc_json_value_t *root, const char *key)
{
    return root ? cc_json_string_value(cc_json_object_get(root, key)) : NULL;
}

/**
 * @brief 将事件树中的对象字段序列化为独立拥有正文。
 * @param[in] root 可选事件树借用，NULL 无结果。
 * @param[in] key 同步借用字段名。
 * @return 需 free 的拥有正文，缺失、非对象或序列化失败返回 NULL。
 */
static char *json_object_field_copy(cc_json_value_t *root, const char *key)
{
    cc_json_value_t *value = root ? cc_json_object_get(root, key) : NULL;
    if (!value || !cc_json_is_object(value)) {
        return NULL;
    }
    return cc_json_stringify_unformatted(value);
}

/**
 * @brief 查询 attributes.content 可表示为 C 字符串的字节长度。
 * @param[in] root 可选事件树借用，本次只读。
 * @return content 的 strlen 字节数，缺失或类型不符为零。
 */
static size_t sdk_event_content_len(cc_json_value_t *root)
{
    cc_json_value_t *attrs = root ? cc_json_object_get(root, "attributes") : NULL;
    if (!attrs || !cc_json_is_object(attrs)) {
        return 0;
    }
    const char *content = cc_json_string_value(cc_json_object_get(attrs, "content"));
    return content ? strlen(content) : 0;
}

/**
 * @brief 构造不含流式正文的计数摘要。
 * @param[in] chunks 当前累计 chunk 数。
 * @param[in] suppressed 当前累计节流次数。
 * @param[in] bytes 当前累计正文 strlen 字节数，按现有 unsigned 输出表示。
 * @param[in] content_bytes 当前 chunk 字节数，按现有 unsigned 输出表示。
 * @param[in] sampled true 表示节流抽样摘要，false 表示结束汇总。
 * @param[in] prefix 内部固定 JSON 键前缀借用，NULL 使用 stream，调用方保证无需转义。
 * @return 需 free 的拥有摘要，初始化或 sticky 追加失败返回 NULL，错误消息均清理。
 */
static char *build_stream_text_summary_json(uint32_t chunks,
                                            uint32_t suppressed,
                                            size_t bytes,
                                            size_t content_bytes,
                                            bool sampled,
                                            const char *prefix)
{
    cc_string_builder_t sb;
    cc_result_t init_rc = cc_string_builder_init(&sb);
    bool initialized = init_rc.code == CC_OK;
    cc_result_free(&init_rc);
    if (!initialized) {
        return NULL;
    }
    profile_json_result_discard(cc_string_builder_appendf(&sb,
                              "{\"sampled\":%s,\"content_bytes\":%u,"
                              "\"%s_chunks\":%u,\"%s_suppressed\":%u,"
                              "\"%s_bytes\":%u}",
                              sampled ? "true" : "false",
                              (unsigned)content_bytes,
                              prefix ? prefix : "stream",
                              (unsigned)chunks,
                              prefix ? prefix : "stream",
                              (unsigned)suppressed,
                              prefix ? prefix : "stream",
                              (unsigned)bytes));
    return cc_string_builder_take(&sb);
}

/**
 * @brief 锁内累计流式计数，按首块、块数或时间条件决定发布，锁外生成摘要。
 * @param[in] content_bytes 当前块字节数。
 * @param[in,out] chunks_counter 必填 profile owner 的累计块数槽。
 * @param[in,out] suppressed_counter 必填累计抑制次数槽。
 * @param[in,out] bytes_counter 必填累计字节槽，沿 size_t 加法表示。
 * @param[in,out] last_emit_us 必填最近发布微秒槽，发布时更新。
 * @param[in] every_chunks 必填正块数间隔，内部调用固定为二十。
 * @param[in] min_interval_us 无块数命中时的最小微秒间隔。
 * @param[in] summary_prefix 同步借用的固定键前缀。
 * @param[out] out_attributes_json 可选输出，先置 NULL，发布时尽力交付需 free 的摘要。
 * @return 应发布为 true，摘要 OOM 时仍可为 true 且输出 NULL，节流为 false。
 */
static bool sdk_stream_counter_should_emit(size_t content_bytes,
                                           uint32_t *chunks_counter,
                                           uint32_t *suppressed_counter,
                                           size_t *bytes_counter,
                                           uint64_t *last_emit_us,
                                           uint32_t every_chunks,
                                           uint64_t min_interval_us,
                                           const char *summary_prefix,
                                           char **out_attributes_json)
{
    if (out_attributes_json) {
        *out_attributes_json = NULL;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    uint32_t chunks = 0;
    uint32_t suppressed = 0;
    size_t bytes = 0;
    bool emit = false;

    profile_lock();
    (*chunks_counter)++;
    (*bytes_counter) += content_bytes;
    chunks = *chunks_counter;
    bytes = *bytes_counter;
    emit = chunks == 1 ||
           (chunks % every_chunks) == 0 ||
           *last_emit_us == 0 ||
           now_us - *last_emit_us >= min_interval_us;
    if (emit) {
        *last_emit_us = now_us;
    } else {
        (*suppressed_counter)++;
    }
    suppressed = *suppressed_counter;
    profile_unlock();

    if (!emit) {
        return false;
    }
    if (out_attributes_json) {
        *out_attributes_json = build_stream_text_summary_json(chunks,
                                                             suppressed,
                                                             bytes,
                                                             content_bytes,
                                                             true,
                                                             summary_prefix);
    }
    return true;
}

/**
 * @brief 使用 text 独立计数和一秒/二十块间隔进行节流。
 * @param[in] root 可选事件树借用，用于查询当前 content 字节数。
 * @param[out] out_attributes_json 可选拥有摘要输出，先置 NULL，成功正文由调用者 free。
 * @return 发布许可，摘要分配失败仍保留许可。
 */
static bool sdk_stream_text_should_emit(cc_json_value_t *root, char **out_attributes_json)
{
    size_t content_bytes = sdk_event_content_len(root);
    return sdk_stream_counter_should_emit(content_bytes,
                                          &s_profile.stream_text_chunks,
                                          &s_profile.stream_text_suppressed,
                                          &s_profile.stream_text_bytes,
                                          &s_profile.stream_text_last_emit_us,
                                          APP_PROFILE_STREAM_TEXT_EVERY,
                                          APP_PROFILE_STREAM_TEXT_MIN_INTERVAL_US,
                                          "stream_text",
                                          out_attributes_json);
}

/**
 * @brief 使用 thinking 独立计数和一秒/二十块间隔进行节流。
 * @param[in] root 可选事件树借用，用于读取当前内容字节数。
 * @param[out] out_attributes_json 可选拥有摘要输出，先置 NULL，成功正文由调用者 free。
 * @return 发布许可，摘要分配失败仍保留许可。
 */
static bool sdk_stream_thinking_should_emit(cc_json_value_t *root, char **out_attributes_json)
{
    size_t content_bytes = sdk_event_content_len(root);
    return sdk_stream_counter_should_emit(content_bytes,
                                          &s_profile.stream_thinking_chunks,
                                          &s_profile.stream_thinking_suppressed,
                                          &s_profile.stream_thinking_bytes,
                                          &s_profile.stream_thinking_last_emit_us,
                                          APP_PROFILE_STREAM_THINKING_EVERY,
                                          APP_PROFILE_STREAM_THINKING_MIN_INTERVAL_US,
                                          "stream_thinking",
                                          out_attributes_json);
}

/**
 * @brief 锁内取得 text 汇总并清空 text/thinking 两组计数，再锁外构造结束摘要。
 * @return 需 free 的 text 汇总正文，构造失败为 NULL，计数仍已复位。
 */
static char *sdk_stream_finished_summary_json(void)
{
    uint32_t chunks = 0;
    uint32_t suppressed = 0;
    size_t bytes = 0;
    profile_lock();
    chunks = s_profile.stream_text_chunks;
    suppressed = s_profile.stream_text_suppressed;
    bytes = s_profile.stream_text_bytes;
    s_profile.stream_text_chunks = 0;
    s_profile.stream_text_suppressed = 0;
    s_profile.stream_text_bytes = 0;
    s_profile.stream_text_last_emit_us = 0;
    s_profile.stream_thinking_chunks = 0;
    s_profile.stream_thinking_suppressed = 0;
    s_profile.stream_thinking_bytes = 0;
    s_profile.stream_thinking_last_emit_us = 0;
    profile_unlock();
    return build_stream_text_summary_json(chunks, suppressed, bytes, 0, false, "stream_text");
}

/**
 * @brief 识别 BASIC 级别保留的请求、用量、工具、恢复与运行完成事件。
 * @param[in] event_type 同步借用事件名，NULL 拒绝。
 * @return 命中当前精确白名单为 true，其余 false。
 */
static bool sdk_event_allowed_basic(const char *event_type)
{
    if (!event_type) {
        return false;
    }
    return strcmp(event_type, "llm.request.start") == 0 ||
           strcmp(event_type, "llm.request.config") == 0 ||
           strcmp(event_type, "llm.response.finish") == 0 ||
           strcmp(event_type, "tool.start") == 0 ||
           strcmp(event_type, "tool.finish") == 0 ||
           strcmp(event_type, "tool.error") == 0 ||
           strcmp(event_type, "llm.http.attempt") == 0 ||
           strcmp(event_type, "llm.context.build") == 0 ||
           strcmp(event_type, "llm.tools.schema_build") == 0 ||
           strcmp(event_type, "llm.provider.submit") == 0 ||
           strcmp(event_type, "llm.first_body") == 0 ||
           strcmp(event_type, "llm.stream.finish") == 0 ||
           strcmp(event_type, "llm.usage") == 0 ||
           strcmp(event_type, "llm.context.summary_cache") == 0 ||
           strcmp(event_type, "tool.recovery.required") == 0 ||
           strcmp(event_type, "tool.recovery.resolved") == 0 ||
           strcmp(event_type, "run.finished") == 0;
}

/**
 * @brief 更新缓存指标，再按 profile 配置过滤与节流 SDK 事件并发布快照。
 * @param[in] event_type 同步借用事件名，NULL 在 TRACE 路径使用 unknown。
 * @param[in] event_json 同步借用事件 JSON，NULL 按空对象，沿现有首值解析规则。
 * @post 事件树、复制的属性/错误正文与解析结果均释放，发布在 profile 锁外完成。
 * @note 缓存用量即使 profile 关闭仍更新，解析失败可发布无身份的诊断快照。
 */
void app_profile_sdk_event(const char *event_type, const char *event_json)
{
    /* 缓存计数独立于可选的详细 profile 开关，关闭详细采样仍观察用量。 */
    if (event_type && event_json && (strcmp(event_type, "llm.usage") == 0 ||
        strcmp(event_type, "llm.context.summary_cache") == 0)) {
        profile_lock();
        if (s_profile.cache_metrics) {
            cc_result_t metrics_rc = cc_cache_metrics_observe(s_profile.cache_metrics, event_type, event_json);
            cc_result_free(&metrics_rc);
        }
        profile_unlock();
    }
    if (!app_profile_enabled()) {
        return;
    }
    if (profile_drop_for_maintenance(false)) {
        return;
    }
    app_profile_level_t level = app_profile_level();
    if (level == APP_PROFILE_LEVEL_BASIC &&
        !sdk_event_allowed_basic(event_type)) {
        return;
    }

    const char *session_id = NULL;
    const char *run_id = NULL;
    const char *status = NULL;
    int step = -1;
    char *attributes_json = NULL;
    char *error_json = NULL;
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(event_json ? event_json : "{}", &root);
    if (rc.code == CC_OK && root && cc_json_is_object(root)) {
        session_id = json_string_field(root, "session_id");
        run_id = json_string_field(root, "run_id");
        status = json_string_field(root, "status");
        cc_json_value_t *step_value = cc_json_object_get(root, "step");
        if (step_value) {
            step = cc_json_int_value(step_value);
        }
        error_json = json_object_field_copy(root, "error");
        if (event_type && strcmp(event_type, "stream.text") == 0) {
            if (level == APP_PROFILE_LEVEL_TRACE &&
                !sdk_stream_text_should_emit(root, &attributes_json)) {
                if (error_json) {
                    free(error_json);
                }
                cc_result_free(&rc);
                cc_json_destroy(root);
                return;
            }
        } else if (event_type && strcmp(event_type, "stream.thinking") == 0) {
            if (level == APP_PROFILE_LEVEL_TRACE &&
                !sdk_stream_thinking_should_emit(root, &attributes_json)) {
                if (error_json) {
                    free(error_json);
                }
                cc_result_free(&rc);
                cc_json_destroy(root);
                return;
            }
        } else if (event_type && strcmp(event_type, "stream.finished") == 0) {
            attributes_json = sdk_stream_finished_summary_json();
        } else {
            attributes_json = json_object_field_copy(root, "attributes");
        }
    }
    cc_result_free(&rc);
    publish_profile("sdk_observability",
                    event_type ? event_type : "unknown",
                    session_id,
                    run_id,
                    status ? status : "ok",
                    0,
                    event_type,
                    step,
                    attributes_json,
                    error_json,
                    false,
                    false);
    if (attributes_json) {
        free(attributes_json);
    }
    if (error_json) {
        free(error_json);
    }
    if (root) {
        cc_json_destroy(root);
    }
}

/**
 * @brief 将 SDK 同步或异步订阅事件委托给 profile 处理。
 * @param[in] event_type 本次回调借用事件名。
 * @param[in] event_json 本次回调借用事件正文。
 * @param[in] user_data 预留上下文，当前忽略，允许 NULL。
 * @post 不保存传入正文借用，临时资源由处理入口清理。
 */
static void sdk_event_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)user_data;
    app_profile_sdk_event(event_type, event_json);
}

/**
 * @brief 为一个 builder 的长期事件总线建立唯一 profile 订阅。
 * @param[in] builder 可选 builder 借用，NULL 跳过，owner 保管到 sampler 和退订全部结束。
 * @post 成功记录订阅句柄，失败清空 builder/订阅标记并累计 dropped，结果消息清理。
 * @pre 生命周期 owner 串行协调 subscribe/unsubscribe/destroy，不在事件回调内交接订阅。
 * @note bus 在 builder 创建一次并供各 generation 共用，释放获取代际后由 builder 寿命保活。
 */
void app_profile_subscribe_runtime(cc_runtime_builder_t *builder)
{
    if (!builder) {
        return;
    }
    profile_lock();
    if (s_profile.runtime_subscribed) {
        profile_unlock();
        return;
    }
    s_profile.runtime_subscribed = true;
    s_profile.runtime_builder = builder;
    profile_unlock();

    cc_runtime_generation_t *generation = NULL;
    cc_result_t lease = cc_runtime_builder_acquire_generation(builder, &generation);
    cc_agent_runtime_t *runtime = lease.code == CC_OK ?
        cc_runtime_generation_runtime(generation) : NULL;
    cc_event_bus_t *bus = cc_agent_runtime_event_bus(runtime);
    if (generation) cc_runtime_generation_release(generation);
    cc_result_free(&lease);
    if (!bus) {
        profile_lock();
        s_profile.runtime_subscribed = false;
        s_profile.runtime_builder = NULL;
        s_profile.dropped++;
        profile_unlock();
        return;
    }
    cc_event_subscription_options_t options = {
        .size = sizeof(options),
        .overflow_policy = CC_EVENT_COALESCE_LATEST,
        .mailbox_capacity = 4,
        .block_timeout_ms = 0,
    };
    cc_event_subscription_t subscription = {0};
    cc_result_t rc = cc_event_bus_subscribe(
        bus, NULL, sdk_event_handler, NULL, &options, &subscription);
    if (rc.code != CC_OK) {
        profile_lock();
        s_profile.runtime_subscribed = false;
        s_profile.runtime_builder = NULL;
        s_profile.dropped++;
        profile_unlock();
    } else {
        profile_lock();
        s_profile.runtime_subscription = subscription;
        profile_unlock();
    }
    cc_result_free(&rc);
}

/**
 * @brief 借用 builder 当前代获取长期总线，撤销订阅并等待回调退出。
 * @param[in] timeout_ms 毫秒等待预算，超过 INT_MAX 饱和为 INT_MAX，零执行立即条件检查。
 * @return 未订阅或撤销/已不存在为 ESP_OK，代际不可取为 INVALID_STATE，超时为 TIMEOUT，其余 ESP_FAIL。
 * @pre owner 串行协调订阅变更，当前位于该订阅回调之外，builder 保持到返回。
 * @note 失败保留订阅与 builder 供重试，成功清空记录；sampler 的排空由 stop 独立完成。
 */
esp_err_t app_profile_unsubscribe_runtime(uint32_t timeout_ms)
{
    profile_lock();
    bool subscribed = s_profile.runtime_subscribed;
    cc_runtime_builder_t *builder = s_profile.runtime_builder;
    cc_event_subscription_t subscription = s_profile.runtime_subscription;
    profile_unlock();
    if (!subscribed) return ESP_OK;
    if (!builder) return ESP_ERR_INVALID_STATE;

    cc_runtime_generation_t *generation = NULL;
    cc_result_t lease = cc_runtime_builder_acquire_generation(builder, &generation);
    if (lease.code != CC_OK) {
        cc_result_free(&lease);
        return ESP_ERR_INVALID_STATE;
    }
    cc_event_bus_t *bus = cc_agent_runtime_event_bus(
        cc_runtime_generation_runtime(generation));
    int wait_ms = timeout_ms > (uint32_t)INT_MAX ? INT_MAX : (int)timeout_ms;
    cc_result_t rc = bus ? cc_event_bus_unsubscribe(bus, subscription, wait_ms) :
        cc_result_error(CC_ERR_INVALID_STATE, "Runtime event bus unavailable");
    cc_runtime_generation_release(generation);
    cc_result_free(&lease);
    if (rc.code != CC_OK && rc.code != CC_ERR_NOT_FOUND) {
        esp_err_t err = rc.code == CC_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        cc_result_free(&rc);
        return err;
    }
    cc_result_free(&rc);
    profile_lock();
    s_profile.runtime_subscribed = false;
    s_profile.runtime_builder = NULL;
    memset(&s_profile.runtime_subscription, 0, sizeof(s_profile.runtime_subscription));
    profile_unlock();
    return ESP_OK;
}

/**
 * @brief 在 profile 锁内追加配置、采样计数、最近快照和独立缓存指标正文。
 * @param[in,out] sb 调用者已初始化的 builder，NULL 跳过，保留 sticky 失败状态。
 * @post profile 成员追加完成或构造失败，缓存正文及全部拥有结果消息清理。
 * @note 锁覆盖状态一致性和 cache_metrics 寿命，外部事件发布不在此入口执行。
 */
void app_profile_append_status_json(cc_string_builder_t *sb)
{
    if (!sb) {
        return;
    }
    profile_lock();
    profile_json_result_discard(cc_string_builder_appendf(sb,
                              "\"profile\":{\"enabled\":%s,\"level\":",
                              s_profile.enabled ? "true" : "false"));
    json_escape_append(sb, app_profile_level_string(s_profile.level));
    profile_json_result_discard(cc_string_builder_appendf(sb,
                              ",\"interval_ms\":%d,\"samples\":%u,\"dropped\":%u,"
                              "\"last_snapshot\":",
                              s_profile.interval_ms,
                              (unsigned)s_profile.samples,
                              (unsigned)s_profile.dropped));
    if (s_profile.last_snapshot[0]) {
        profile_json_result_discard(cc_string_builder_append(sb, s_profile.last_snapshot));
    } else {
        profile_json_result_discard(cc_string_builder_append(sb, "{}"));
    }
    char *cache_json = NULL;
    cc_result_t cache_rc = s_profile.cache_metrics ?
        cc_cache_metrics_snapshot(s_profile.cache_metrics, &cache_json) :
        cc_result_error(CC_ERR_INVALID_STATE, "Cache metrics are unavailable");
    profile_json_result_discard(cc_string_builder_append(sb, ",\"cache_metrics\":"));
    profile_json_result_discard(cc_string_builder_append(sb, cache_rc.code == CC_OK && cache_json ? cache_json : "null"));
    free(cache_json);
    cc_result_free(&cache_rc);
    profile_json_result_discard(cc_string_builder_append_char(sb, '}'));
    profile_unlock();
}
