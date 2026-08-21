#include "app_profile.h"

#include <inttypes.h>
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
#define APP_PROFILE_TASK_STACK 4096
#define APP_PROFILE_TASK_PRIORITY 2
#define APP_PROFILE_STREAM_TEXT_EVERY 20U
#define APP_PROFILE_STREAM_TEXT_MIN_INTERVAL_US 1000000ULL
#define APP_PROFILE_STREAM_THINKING_EVERY 20U
#define APP_PROFILE_STREAM_THINKING_MIN_INTERVAL_US 1000000ULL
#define APP_PROFILE_TASK_EXITED_BIT (1U << 0)

typedef struct {
    char name[24];
    TaskHandle_t handle;
} app_profile_task_t;

typedef struct {
    UBaseType_t task_number;
    uint32_t runtime_counter;
    bool seen;
} app_profile_cpu_prev_t;

typedef struct {
    const char *name;
    UBaseType_t state;
    UBaseType_t priority;
    uint32_t stack_high_water_words;
    int core_id;
    uint32_t runtime_counter;
    uint32_t runtime_delta;
    uint32_t cpu_pct_x100;
    uint32_t core_pct_x100;
} app_profile_cpu_task_sample_t;

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
    cc_runtime_builder_t *runtime_builder;
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

/*
 * 获取 profile 互斥锁（前向声明）。
 */
static void profile_lock(void);
/*
 * 释放 profile 互斥锁（前向声明）。
 */
static void profile_unlock(void);

/*
 * 周期性采样任务：按配置的 interval_ms 间隔调用 app_profile_sample。
 */
static void profile_sampler_task(void *arg)
{
    (void)arg;
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
    profile_lock();
    s_profile.sampler_task = NULL;
    profile_unlock();
    if (s_profile.events) xEventGroupSetBits(s_profile.events, APP_PROFILE_TASK_EXITED_BIT);
    vTaskDelete(NULL);
}

/*
 * 确保周期性采样任务已启动（按需创建）。
 */
static void ensure_sampler_task(void)
{
    profile_lock();
    bool need_task = s_profile.mutex && s_profile.events && !s_profile.stopping &&
                     s_profile.enabled && s_profile.interval_ms > 0 &&
                     !s_profile.sampler_task;
    profile_unlock();
    if (!need_task) {
        return;
    }
    TaskHandle_t handle = NULL;
    xEventGroupClearBits(s_profile.events, APP_PROFILE_TASK_EXITED_BIT);
    BaseType_t ok = xTaskCreate(profile_sampler_task,
                                "app_profile",
                                APP_PROFILE_TASK_STACK,
                                NULL,
                                APP_PROFILE_TASK_PRIORITY,
                                &handle);
    profile_lock();
    if (ok == pdPASS) {
        s_profile.sampler_task = handle;
    } else {
        s_profile.dropped++;
    }
    profile_unlock();
    if (ok == pdPASS) {
        app_profile_register_task("app_profile", handle);
    }
}

/*
 * 显式创建 profile 同步原语；必须在事件 sink 注册任务前调用。
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
    s_profile.stopping = false;
    xEventGroupSetBits(s_profile.events, APP_PROFILE_TASK_EXITED_BIT);
    return ESP_OK;
}

/*
 * 获取 profile 互斥锁（阻塞）。
 */
static void profile_lock(void)
{
    if (s_profile.mutex) {
        xSemaphoreTake(s_profile.mutex, portMAX_DELAY);
    }
}

/*
 * 释放 profile 互斥锁。
 */
static void profile_unlock(void)
{
    if (s_profile.mutex) {
        xSemaphoreGive(s_profile.mutex);
    }
}

/*
 * 将字符串 "trace" 解析为 APP_PROFILE_LEVEL_TRACE，其他均为 BASIC。
 * 参数: level - 级别字符串，NULL 时返回 BASIC
 * 返回: 对应的 app_profile_level_t
 */
static app_profile_level_t parse_level(const char *level)
{
    return level && strcmp(level, "trace") == 0 ?
        APP_PROFILE_LEVEL_TRACE : APP_PROFILE_LEVEL_BASIC;
}

/*
 * 将 app_profile_level_t 转为字符串。
 * 参数: level - 级别枚举
 * 返回: "trace" 或 "basic"
 */
const char *app_profile_level_string(app_profile_level_t level)
{
    return level == APP_PROFILE_LEVEL_TRACE ? "trace" : "basic";
}

/*
 * 应用 profile 配置（enabled/level/interval_ms），确保采样任务启动。
 * 参数: config - 配置结构体指针，NULL 时禁用
 */
esp_err_t app_profile_apply_config(const app_profile_config_t *config)
{
    if (!s_profile.mutex || !s_profile.events) return ESP_ERR_INVALID_STATE;
    profile_lock();
    s_profile.enabled = config && config->enabled != 0;
    s_profile.level = parse_level(config ? config->level : NULL);
    s_profile.interval_ms = config ? config->interval_ms : 0;
    profile_unlock();
    ensure_sampler_task();
    return ESP_OK;
}

esp_err_t app_profile_stop(uint32_t timeout_ms)
{
    if (!s_profile.mutex || !s_profile.events) return ESP_OK;
    profile_lock();
    s_profile.enabled = false;
    s_profile.stopping = true;
    TaskHandle_t task = s_profile.sampler_task;
    profile_unlock();
    if (!task) {
        xEventGroupSetBits(s_profile.events, APP_PROFILE_TASK_EXITED_BIT);
        return ESP_OK;
    }
    xTaskNotifyGive(task);
    EventBits_t bits = xEventGroupWaitBits(s_profile.events,
                                           APP_PROFILE_TASK_EXITED_BIT,
                                           pdFALSE,
                                           pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & APP_PROFILE_TASK_EXITED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_profile_destroy(uint32_t timeout_ms)
{
    esp_err_t err = app_profile_stop(timeout_ms);
    if (err != ESP_OK) return err;
    if (s_profile.runtime_subscribed || s_profile.runtime_builder) {
        return ESP_ERR_INVALID_STATE;
    }
    EventGroupHandle_t events = s_profile.events;
    SemaphoreHandle_t mutex = s_profile.mutex;
    s_profile.events = NULL;
    s_profile.mutex = NULL;
    if (events) vEventGroupDelete(events);
    if (mutex) vSemaphoreDelete(mutex);
    memset(&s_profile, 0, sizeof(s_profile));
    s_profile.level = APP_PROFILE_LEVEL_BASIC;
    return ESP_OK;
}

/*
 * 设置 profile 启用/禁用状态（线程安全）。
 * 参数: enabled - true 启用
 */
void app_profile_set_enabled(bool enabled)
{
    profile_lock();
    s_profile.enabled = enabled;
    profile_unlock();
    ensure_sampler_task();
}

/*
 * 查询 profile 是否启用（线程安全）。
 * 返回: true 启用
 */
bool app_profile_enabled(void)
{
    bool enabled = false;
    profile_lock();
    enabled = s_profile.enabled;
    profile_unlock();
    return enabled;
}

/*
 * 设置 profile 级别（线程安全）。
 * 参数: level - TRACE 或 BASIC
 */
void app_profile_set_level(app_profile_level_t level)
{
    profile_lock();
    s_profile.level = level == APP_PROFILE_LEVEL_TRACE ?
        APP_PROFILE_LEVEL_TRACE : APP_PROFILE_LEVEL_BASIC;
    profile_unlock();
}

/*
 * 获取当前 profile 级别（线程安全）。
 * 返回: 当前级别
 */
app_profile_level_t app_profile_level(void)
{
    app_profile_level_t level;
    profile_lock();
    level = s_profile.level;
    profile_unlock();
    return level;
}

/*
 * 设置采样间隔（线程安全）。
 * 参数: interval_ms - 采样间隔毫秒，负值按0处理
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

/*
 * 注册 FreeRTOS 任务以便 profile 采集其栈高水位（最多 APP_PROFILE_TASK_MAX 个）。
 * 参数: name - 任务名称
 *       handle - 任务句柄
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

/*
 * 将字符串以 JSON 转义格式追加到字符串构建器（双引号包裹，特殊字符转义）。
 * 参数: sb - 字符串构建器指针
 *       text - 原始文本
 */
static void json_escape_append(cc_string_builder_t *sb, const char *text)
{
    cc_string_builder_append_char(sb, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '\\': cc_string_builder_append(sb, "\\\\"); break;
        case '"': cc_string_builder_append(sb, "\\\""); break;
        case '\n': cc_string_builder_append(sb, "\\n"); break;
        case '\r': cc_string_builder_append(sb, "\\r"); break;
        case '\t': cc_string_builder_append(sb, "\\t"); break;
        default:
            if (*p < 0x20) cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p);
            else cc_string_builder_append_char(sb, (char)*p);
            break;
        }
    }
    cc_string_builder_append_char(sb, '"');
}

/*
 * 判断当前是否应该发送 profile 数据（锁定状态下调用）。
 * 返回: true 允许发送
 */
static bool profile_should_emit_locked(void)
{
    if (!s_profile.enabled) {
        return false;
    }
    return true;
}

/*
 * 检查维护模式，若非 force 且处于维护模式则丢弃本次 profile 数据。
 * 参数: force - true 时即使维护模式也不丢弃
 * 返回: true 应丢弃
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

/*
 * 将已注册任务的栈高水位以 JSON 格式追加到 sb（"stack": {"task_name": watermark, ...}）。
 * 参数: sb - 字符串构建器指针
 */
static void append_stack_json_locked(cc_string_builder_t *sb)
{
    cc_string_builder_append(sb, "\"stack\":{");
    bool first = true;
    for (int i = 0; i < APP_PROFILE_TASK_MAX; i++) {
        if (!s_profile.tasks[i].handle || !s_profile.tasks[i].name[0]) {
            continue;
        }
        if (!first) {
            cc_string_builder_append_char(sb, ',');
        }
        first = false;
        json_escape_append(sb, s_profile.tasks[i].name);
        cc_string_builder_appendf(sb,
                                  ":%u",
                                  (unsigned)uxTaskGetStackHighWaterMark(s_profile.tasks[i].handle));
    }
    cc_string_builder_append_char(sb, '}');
}

/*
 * 查询任务上次运行时计数器值（用于计算差值），并更新当前值。
 * 参数: task_number - FreeRTOS 任务编号
 *       current - 当前运行时计数器值
 *       known - 输出：true 表示找到历史记录
 * 返回: 上次的计数器值（无历史记录时返回 current）
 */
#if (configGENERATE_RUN_TIME_STATS == 1)
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

/*
 * 判断任务名是否为 IDLE（空闲任务）。
 * 参数: name - 任务名
 * 返回: true 是空闲任务
 */
static bool app_profile_is_idle_task_name(const char *name)
{
    if (!name) {
        return false;
    }
    return strcmp(name, "IDLE") == 0 || strncmp(name, "IDLE", 4) == 0;
}

/*
 * 从任务名末尾数字推断 CPU 核心编号（IDLE0→core0, IDLE1→core1）。
 * 参数: name - 任务名
 * 返回: 核心编号，无法推断返回 -1
 */
static int app_profile_core_from_task_name(const char *name)
{
    if (!name) {
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

/*
 * 采集 CPU 使用率数据并以 JSON 格式追加到 sb。
 * 包含各核心使用率、按运行时差值排序的任务列表（Top-N）。
 * 参数: sb - 字符串构建器指针
 */
static void append_cpu_json(cc_string_builder_t *sb)
{
#if (configUSE_TRACE_FACILITY == 1)
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count == 0) {
        cc_string_builder_append(sb, "\"cpu\":{\"available\":false,\"reason\":\"no_tasks\"}");
        return;
    }
    TaskStatus_t *tasks = heap_caps_calloc((size_t)task_count,
                                           sizeof(TaskStatus_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tasks) {
        cc_string_builder_append(sb, "\"cpu\":{\"available\":false,\"reason\":\"oom\"}");
        return;
    }
#if (configGENERATE_RUN_TIME_STATS == 1)
    uint32_t total_runtime = 0;
    UBaseType_t filled = uxTaskGetSystemState(tasks, task_count, &total_runtime);
    uint32_t previous_total = 0;
    bool have_previous_total = false;
    profile_lock();
    previous_total = s_profile.cpu_prev_total_runtime;
    have_previous_total = previous_total > 0 && total_runtime >= previous_total;
    s_profile.cpu_prev_total_runtime = total_runtime;
    profile_unlock();
    uint32_t total_delta = have_previous_total ? total_runtime - previous_total : 0;
#else
    UBaseType_t filled = uxTaskGetSystemState(tasks, task_count, NULL);
#endif
    app_profile_cpu_task_sample_t samples[APP_PROFILE_CPU_TASK_MAX];
    UBaseType_t sample_count = 0;
#if (configGENERATE_RUN_TIME_STATS == 1)
#ifndef portNUM_PROCESSORS
#define APP_PROFILE_CPU_CORES 1U
#else
#define APP_PROFILE_CPU_CORES portNUM_PROCESSORS
#endif
    uint32_t idle_delta_by_core[APP_PROFILE_CPU_CORES] = {0};
    uint32_t core_pct_x100[APP_PROFILE_CPU_CORES] = {0};
#endif
    for (UBaseType_t i = 0; i < filled; i++) {
        app_profile_cpu_task_sample_t sample = {
            .name = tasks[i].pcTaskName ? tasks[i].pcTaskName : "",
            .state = tasks[i].eCurrentState,
            .priority = tasks[i].uxCurrentPriority,
            .stack_high_water_words = (uint32_t)tasks[i].usStackHighWaterMark,
            .core_id = app_profile_core_from_task_name(tasks[i].pcTaskName),
            .runtime_counter = 0,
            .runtime_delta = 0,
            .cpu_pct_x100 = 0,
            .core_pct_x100 = 0,
        };
#if (configNUMBER_OF_CORES > 1)
        if (tasks[i].xHandle) {
            BaseType_t core_id = xTaskGetCoreID(tasks[i].xHandle);
#ifdef tskNO_AFFINITY
            if (core_id != tskNO_AFFINITY)
#endif
            {
                if (core_id >= 0 && core_id < (BaseType_t)APP_PROFILE_CPU_CORES) {
                    sample.core_id = (int)core_id;
                }
            }
        }
#endif
#if (configGENERATE_RUN_TIME_STATS == 1)
        uint32_t current = (uint32_t)tasks[i].ulRunTimeCounter;
        bool known = false;
        profile_lock();
        uint32_t previous = cpu_previous_counter_locked(tasks[i].xTaskNumber, current, &known);
        profile_unlock();
        sample.runtime_counter = current;
        sample.runtime_delta = known && current >= previous ? current - previous : 0;
        sample.cpu_pct_x100 = total_delta > 0 ?
            (uint32_t)(((uint64_t)sample.runtime_delta * 10000ULL) / total_delta) : 0;
        sample.core_pct_x100 = sample.cpu_pct_x100 * APP_PROFILE_CPU_CORES;
        if (sample.core_pct_x100 > 10000U) {
            sample.core_pct_x100 = 10000U;
        }
        if (app_profile_is_idle_task_name(sample.name) &&
            sample.core_id >= 0 && sample.core_id < (int)APP_PROFILE_CPU_CORES) {
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
    uint32_t core_total_delta = APP_PROFILE_CPU_CORES > 0 ? total_delta / APP_PROFILE_CPU_CORES : total_delta;
    if (core_total_delta > 0) {
        for (UBaseType_t core = 0; core < APP_PROFILE_CPU_CORES; core++) {
            uint64_t idle_pct_x100 = ((uint64_t)idle_delta_by_core[core] * 10000ULL) / core_total_delta;
            core_pct_x100[core] = idle_pct_x100 >= 10000ULL ? 0U : (uint32_t)(10000ULL - idle_pct_x100);
        }
    }
    cc_string_builder_appendf(sb,
                              "\"cpu\":{\"available\":true,\"runtime_stats\":true,"
                              "\"task_count\":%u,\"cores\":%u,\"tr\":%u,"
                              "\"td\":%u,\"core_pct\":[",
                              (unsigned)filled,
                              (unsigned)APP_PROFILE_CPU_CORES,
                              (unsigned)total_runtime,
                              (unsigned)total_delta);
    for (UBaseType_t core = 0; core < APP_PROFILE_CPU_CORES; core++) {
        if (core > 0) {
            cc_string_builder_append_char(sb, ',');
        }
        cc_string_builder_appendf(sb, "%u.%02u",
                                  (unsigned)(core_pct_x100[core] / 100U),
                                  (unsigned)(core_pct_x100[core] % 100U));
    }
    cc_string_builder_append(sb, "],\"tasks\":[");
#else
    cc_string_builder_appendf(sb,
                              "\"cpu\":{\"available\":true,\"runtime_stats\":false,"
                              "\"task_count\":%u,\"tasks\":[",
                              (unsigned)filled);
#endif
    for (UBaseType_t i = 0; i < sample_count; i++) {
        if (i > 0) {
            cc_string_builder_append_char(sb, ',');
        }
        cc_string_builder_append(sb, "{\"n\":");
        json_escape_append(sb, samples[i].name);
        cc_string_builder_appendf(sb,
                                  ",\"s\":%u,\"pri\":%u,\"stk\":%u,\"c\":%d",
                                  (unsigned)samples[i].state,
                                  (unsigned)samples[i].priority,
                                  (unsigned)samples[i].stack_high_water_words,
                                  samples[i].core_id);
#if (configGENERATE_RUN_TIME_STATS == 1)
        cc_string_builder_appendf(sb,
                                  ",\"r\":%u,\"d\":%u,\"p\":%u.%02u,\"cp\":%u.%02u",
                                  (unsigned)samples[i].runtime_counter,
                                  (unsigned)samples[i].runtime_delta,
                                  (unsigned)(samples[i].cpu_pct_x100 / 100U),
                                  (unsigned)(samples[i].cpu_pct_x100 % 100U),
                                  (unsigned)(samples[i].core_pct_x100 / 100U),
                                  (unsigned)(samples[i].core_pct_x100 % 100U));
#endif
        cc_string_builder_append_char(sb, '}');
    }
    if (filled > sample_count) {
        cc_string_builder_appendf(sb, "],\"truncated\":true,\"reported_tasks\":%u}",
                                  (unsigned)sample_count);
    } else {
        cc_string_builder_append(sb, "],\"truncated\":false}");
    }
    heap_caps_free(tasks);
#else
    cc_string_builder_append(sb,
                             "\"cpu\":{\"available\":false,"
                             "\"reason\":\"CONFIG_FREERTOS_USE_TRACE_FACILITY disabled\"}");
#endif
}

/*
 * 将事件总线的运行时统计以 JSON 格式追加到 sb。
 * 参数: sb - 字符串构建器指针
 */
static void append_event_bus_json(cc_string_builder_t *sb)
{
    app_event_bus_status_t events;
    app_event_bus_get_status(&events);
    cc_string_builder_appendf(sb,
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
                              (unsigned)events.capacity);
}

/*
 * 将工具执行池的性能指标以 JSON 格式追加到 sb（需 CC_ENABLE_TOOL_POOL）。
 * 参数: sb - 字符串构建器指针
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
        cc_string_builder_append(sb, "\"tool_pool\":{\"available\":false}");
        return;
    }

    cc_tool_executor_pool_lane_metrics_t lanes[12];
    cc_tool_executor_pool_metrics_t metrics;
    cc_result_t rc = cc_tool_executor_pool_get_metrics(pool,
                                                       &metrics,
                                                       lanes,
                                                       sizeof(lanes) / sizeof(lanes[0]));
    cc_runtime_generation_release(generation);
    cc_result_free(&lease);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        cc_string_builder_append(sb, "\"tool_pool\":{\"available\":false,\"reason\":\"metrics_error\"}");
        return;
    }
    cc_result_free(&rc);

    cc_string_builder_appendf(sb,
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
                              metrics.cancelled);
    for (size_t i = 0; i < metrics.reported_lane_count; i++) {
        if (i > 0) {
            cc_string_builder_append_char(sb, ',');
        }
        cc_string_builder_append(sb, "{\"name\":");
        json_escape_append(sb, lanes[i].name ? lanes[i].name : "");
        cc_string_builder_appendf(sb,
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
                                  lanes[i].cancelled);
    }
    cc_string_builder_append(sb, "]}");
#else
    cc_string_builder_append(sb, "\"tool_pool\":{\"available\":false,\"reason\":\"CC_ENABLE_TOOL_POOL disabled\"}");
#endif
}

/*
 * 构建完整的 profile JSON 快照：事件元数据、堆内存、任务栈、CPU、事件总线、工具池。
 * 参数: event, phase, session_id, run_id - 事件标识
 *       status - 状态字符串
 *       elapsed_us - 耗时微秒
 *       sdk_event_type - SDK 事件类型
 *       step - 步骤编号（-1 表示无）
 *       attributes_json - 附加属性 JSON
 *       error_json - 错误信息 JSON
 * 返回: 分配的 JSON 字符串（调用者需 free），失败返回 NULL
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
                                const char *error_json)
{
    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) {
        return NULL;
    }
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    cc_string_builder_append(&sb, "{\"event\":");
    json_escape_append(&sb, event ? event : "sample");
    cc_string_builder_append(&sb, ",\"phase\":");
    json_escape_append(&sb, phase ? phase : "");
    cc_string_builder_append(&sb, ",\"run_id\":");
    json_escape_append(&sb, run_id ? run_id : "");
    cc_string_builder_append(&sb, ",\"session_id\":");
    json_escape_append(&sb, session_id ? session_id : "");
    if (step >= 0) {
        cc_string_builder_appendf(&sb, ",\"step\":%d", step);
        if (session_id && session_id[0]) {
            cc_string_builder_append(&sb, ",\"correlation_id\":");
            cc_string_builder_append_char(&sb, '"');
            for (const unsigned char *p = (const unsigned char *)session_id; *p; p++) {
                switch (*p) {
                case '\\': cc_string_builder_append(&sb, "\\\\"); break;
                case '"': cc_string_builder_append(&sb, "\\\""); break;
                default:
                    if (*p < 0x20) cc_string_builder_appendf(&sb, "\\u%04x", (unsigned)*p);
                    else cc_string_builder_append_char(&sb, (char)*p);
                    break;
                }
            }
            cc_string_builder_appendf(&sb, ":%d\"", step);
        }
    }
    cc_string_builder_appendf(&sb,
                              ",\"timestamp_ms\":%" PRIu64 ",\"elapsed_us\":%" PRIu64,
                              now_us / 1000U,
                              elapsed_us);
    if (status) {
        cc_string_builder_append(&sb, ",\"status\":");
        json_escape_append(&sb, status);
    }
    if (sdk_event_type) {
        cc_string_builder_append(&sb, ",\"sdk_event\":");
        json_escape_append(&sb, sdk_event_type);
    }
    if (attributes_json && attributes_json[0]) {
        cc_string_builder_append(&sb, ",\"attributes\":");
        cc_string_builder_append(&sb, attributes_json);
    }
    if (error_json && error_json[0]) {
        cc_string_builder_append(&sb, ",\"error\":");
        cc_string_builder_append(&sb, error_json);
    }
    cc_string_builder_appendf(&sb,
                              ",\"heap\":{\"free\":%u,\"min_free\":%u,"
                              "\"internal_free\":%u,\"internal_total\":%u,\"internal_largest\":%u,"
                              "\"psram_free\":%u,\"psram_total\":%u,\"psram_largest\":%u},",
                              (unsigned)esp_get_free_heap_size(),
                              (unsigned)esp_get_minimum_free_heap_size(),
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                              (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
                              (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    profile_lock();
    append_stack_json_locked(&sb);
    profile_unlock();
    cc_string_builder_append_char(&sb, ',');
    append_cpu_json(&sb);
    cc_string_builder_append_char(&sb, ',');
    append_event_bus_json(&sb);
    cc_string_builder_append_char(&sb, ',');
    append_tool_pool_json(&sb);
    cc_string_builder_append_char(&sb, '}');
    return cc_string_builder_take(&sb);
}

/*
 * 构建 profile JSON 并通过事件总线发布。
 * 参数: event, phase, session_id, run_id, status - 事件标识
 *       elapsed_us - 耗时
 *       sdk_event_type - SDK 事件类型
 *       step - 步骤编号
 *       attributes_json - 附加属性
 *       error_json - 错误信息
 *       force - 强制发送（绕过维护模式和启用状态检查）
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
                                    error_json);
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

/*
 * 开始一个 profile span：记录开始时间戳和事件标识。
 * 参数: event, phase, session_id, run_id - 事件标识
 * 返回: 填充的 app_profile_span_t
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

/*
 * 结束一个 profile span：计算耗时并发布 profile 数据。
 * 参数: span - span 指针（enabled=false 时直接返回）
 *       status - 结束状态
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
                    false);
}

/*
 * 发布一个即时 profile 采样点（无耗时、无 session/run 标识）。
 * 参数: event - 事件名
 *       phase - 阶段名
 *       force - 强制发送
 */
void app_profile_sample(const char *event, const char *phase, bool force)
{
    publish_profile(event ? event : "sample",
                    phase ? phase : "point",
                    NULL,
                    NULL,
                    "ok",
                    0,
                    NULL,
                    -1,
                    NULL,
                    NULL,
                    force);
}

/*
 * 从 JSON 对象中按 key 获取字符串值。
 * 参数: root - JSON 根对象
 *       key - 字段名
 * 返回: 字符串值或 NULL
 */
static const char *json_string_field(cc_json_value_t *root, const char *key)
{
    return root ? cc_json_string_value(cc_json_object_get(root, key)) : NULL;
}

/*
 * 从 JSON 对象中按 key 获取嵌套对象并序列化为 JSON 字符串。
 * 参数: root - JSON 根对象
 *       key - 字段名
 * 返回: 分配的 JSON 字符串（调用者需 free），失败返回 NULL
 */
static char *json_object_field_copy(cc_json_value_t *root, const char *key)
{
    cc_json_value_t *value = root ? cc_json_object_get(root, key) : NULL;
    if (!value || !cc_json_is_object(value)) {
        return NULL;
    }
    return cc_json_stringify_unformatted(value);
}

/*
 * 获取 SDK 事件的 attributes.content 字符串长度。
 * 参数: root - SDK 事件 JSON 对象
 * 返回: content 字符串长度，0 表示不存在或非字符串
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

/*
 * 构建流式文本的摘要 JSON（chunks/suppressed/bytes 统计）。
 * 参数: chunks - 累计 chunk 数
 *       suppressed - 抑制计数
 *       bytes - 累计字节数
 *       content_bytes - 当前内容字节数
 *       sampled - 是否为采样数据
 *       prefix - 统计项前缀（如 "stream_text"）
 * 返回: JSON 字符串（调用者需 free）
 */
static char *build_stream_text_summary_json(uint32_t chunks,
                                            uint32_t suppressed,
                                            size_t bytes,
                                            size_t content_bytes,
                                            bool sampled,
                                            const char *prefix)
{
    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) {
        return NULL;
    }
    cc_string_builder_appendf(&sb,
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
                              (unsigned)bytes);
    return cc_string_builder_take(&sb);
}

/*
 * 通用流式计数器：判断当前 chunk 是否应触发 profile 发送。
 * 按 every_chunks 间隔或 min_interval_us 时间间隔发一次。
 * 参数: content_bytes - 当前内容字节数
 *       chunks_counter, suppressed_counter, bytes_counter - 全局计数器指针
 *       last_emit_us - 上次发送时间戳
 *       every_chunks - 每隔多少 chunk 发送
 *       min_interval_us - 最小发送间隔微秒
 *       summary_prefix - 摘要前缀
 *       out_attributes_json - 输出摘要 JSON
 * 返回: true 应发送
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

/*
 * stream.text 事件的节流判断：使用 stream_text 计数器。
 * 参数: root - 事件 JSON
 *       out_attributes_json - 输出摘要 JSON
 * 返回: true 应发送
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

/*
 * stream.thinking 事件的节流判断：使用 stream_thinking 计数器。
 * 参数: root - 事件 JSON
 *       out_attributes_json - 输出摘要 JSON
 * 返回: true 应发送
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

/*
 * 构建流式传输结束时的汇总 JSON，并重置所有流式计数器。
 * 返回: JSON 字符串（调用者需 free）
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

/*
 * BASIC 级别允许的 SDK 事件白名单过滤。
 * 参数: event_type - SDK 事件类型
 * 返回: true 允许通过
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
           strcmp(event_type, "run.finished") == 0;
}

/*
 * 处理 SDK 可观测事件：解析事件 JSON，按级别过滤，节流流式事件，发布 profile。
 * 参数: event_type - SDK 事件类型
 *       event_json - 事件 JSON 字符串
 */
void app_profile_sdk_event(const char *event_type, const char *event_json)
{
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

/*
 * SDK 事件回调——委托给 app_profile_sdk_event。
 */
static void sdk_event_handler(const char *event_type, const char *event_json, void *user_data)
{
    (void)user_data;
    app_profile_sdk_event(event_type, event_json);
}

/*
 * 订阅 agent runtime 的事件总线（SDK 事件），防重复订阅。
 * 参数: runtime - cc_agent_runtime_t 指针，NULL 时无操作
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
    cc_result_t rc = bus ? cc_event_bus_unsubscribe(bus, subscription, (int)timeout_ms) :
        cc_result_error(CC_ERR_INVALID_STATE, "Runtime event bus unavailable");
    cc_runtime_generation_release(generation);
    cc_result_free(&lease);
    if (rc.code != CC_OK) {
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

/*
 * 将 profile 状态摘要以 JSON 格式追加到字符串构建器（供全局状态查询使用）。
 * 参数: sb - 字符串构建器指针，NULL 时无操作
 */
void app_profile_append_status_json(cc_string_builder_t *sb)
{
    if (!sb) {
        return;
    }
    profile_lock();
    cc_string_builder_appendf(sb,
                              "\"profile\":{\"enabled\":%s,\"level\":",
                              s_profile.enabled ? "true" : "false");
    json_escape_append(sb, app_profile_level_string(s_profile.level));
    cc_string_builder_appendf(sb,
                              ",\"interval_ms\":%d,\"samples\":%u,\"dropped\":%u,"
                              "\"last_snapshot\":",
                              s_profile.interval_ms,
                              (unsigned)s_profile.samples,
                              (unsigned)s_profile.dropped);
    if (s_profile.last_snapshot[0]) {
        cc_string_builder_append(sb, s_profile.last_snapshot);
    } else {
        cc_string_builder_append(sb, "{}");
    }
    cc_string_builder_append_char(sb, '}');
    profile_unlock();
}
