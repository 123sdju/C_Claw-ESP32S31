"""编译实际 profile 函数，核对任务借用、退出屏障、CPU 身份与失败清理。"""
from pathlib import Path
import re

from test_event_ring_ownership import run_c_test
from test_ota_boot_confirmation import BUILDER_PREFIX, builder_source

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    """从输入源码提取完整定义，忽略注释、字符串和字符常量中的花括号。"""
    start = text.index(signature)
    body = text.index("{", start)
    depth = 0
    tokens = re.finditer(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*|[{}]',
                         text[body:], re.S)
    for token in tokens:
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return text[start:body + token.end()]
    raise ValueError("生产函数定义未闭合")


def test_tool_pool_borrowed_metrics(source):
    """输入生产源码，编译实际指标函数并检查名称序列化期间仍持有代际租约。

    返回 None，断言失败或 C 编译失败向外抛出异常。替身在最后释放时标记
    名称失效，覆盖成功、指标失败、无工具池及无 builder 四条退出路径。
    """
    prefix = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CC_ENABLE_TOOL_POOL 1
#define CC_OK 0
#define CC_ERR_INVALID_STATE 1
typedef struct { int code; } cc_result_t;
typedef struct { int unused; } cc_runtime_builder_t;
typedef struct { int unused; } cc_runtime_generation_t;
typedef struct { int unused; } cc_agent_runtime_t;
typedef struct { int unused; } cc_tool_executor_pool_t;
typedef struct { int unused; } cc_string_builder_t;
typedef struct {
    const char *name;
    int concurrency, timeout_ms, in_flight, waiting, max_waiting;
    unsigned long acquired, released, cancelled;
} cc_tool_executor_pool_lane_metrics_t;
typedef struct {
    size_t lane_count, reported_lane_count;
    int default_concurrency, default_timeout_ms, total_in_flight;
    int total_waiting, max_total_waiting;
    unsigned long acquired, released, cancelled;
} cc_tool_executor_pool_metrics_t;
static cc_runtime_builder_t builder_object;
static cc_runtime_generation_t generation_object;
static cc_agent_runtime_t runtime_object;
static cc_tool_executor_pool_t pool_object;
static struct { cc_runtime_builder_t *runtime_builder; } s_profile;
static int scenario, references, releases, names_serialized;
static void profile_lock(void) {}
static void profile_unlock(void) {}
static cc_result_t cc_result_error(int code, const char *message) {
    (void)message; return (cc_result_t){code};
}
static void cc_result_free(cc_result_t *result) { (void)result; }
static cc_result_t cc_runtime_builder_acquire_generation(
    cc_runtime_builder_t *builder, cc_runtime_generation_t **out) {
    assert(builder == &builder_object);
    references++; *out = &generation_object; return (cc_result_t){CC_OK};
}
static cc_agent_runtime_t *cc_runtime_generation_runtime(cc_runtime_generation_t *generation) {
    assert(generation == &generation_object && references == 1); return &runtime_object;
}
static cc_tool_executor_pool_t *cc_agent_runtime_tool_pool(cc_agent_runtime_t *runtime) {
    return runtime && scenario != 2 ? &pool_object : NULL;
}
static void cc_runtime_generation_release(cc_runtime_generation_t *generation) {
    assert(generation == &generation_object && references == 1);
    references--; releases++;
}
static cc_result_t cc_tool_executor_pool_get_metrics(
    cc_tool_executor_pool_t *pool, cc_tool_executor_pool_metrics_t *metrics,
    cc_tool_executor_pool_lane_metrics_t *lanes, size_t capacity) {
    assert(pool == &pool_object && references == 1 && capacity >= 1);
    if (scenario == 1) return (cc_result_t){CC_ERR_INVALID_STATE};
    memset(metrics, 0, sizeof(*metrics)); memset(lanes, 0, sizeof(*lanes));
    metrics->lane_count = metrics->reported_lane_count = 1;
    lanes[0].name = "borrowed-lane"; return (cc_result_t){CC_OK};
}
static cc_result_t cc_string_builder_append(cc_string_builder_t *builder, const char *text) {
    (void)builder; (void)text; return (cc_result_t){CC_OK};
}
static cc_result_t cc_string_builder_appendf(cc_string_builder_t *builder, const char *format, ...) {
    (void)builder; (void)format; return (cc_result_t){CC_OK};
}
static cc_result_t cc_string_builder_append_char(cc_string_builder_t *builder, char value) {
    (void)builder; (void)value; return (cc_result_t){CC_OK};
}
static void json_escape_append(cc_string_builder_t *builder, const char *name) {
    (void)builder;
    /* 代际最后释放后，旧池可能已销毁，借用的名称必须在此前使用完。 */
    assert(references == 1); assert(strcmp(name, "borrowed-lane") == 0);
    names_serialized++;
}
'''
    code = function(source, "static void profile_json_result_discard(")
    code += function(source, "static void append_tool_pool_json(")
    tests = r'''
int main(void) {
    cc_string_builder_t output = {0};
    for (scenario = 0; scenario < 4; scenario++) {
        references = releases = names_serialized = 0;
        s_profile.runtime_builder = scenario == 3 ? NULL : &builder_object;
        append_tool_pool_json(&output);
        assert(references == 0);
        assert(releases == (scenario == 3 ? 0 : 1));
        assert(names_serialized == (scenario == 0 ? 1 : 0));
    }
    puts("Profile tool-pool borrowed names retain generation through serialization: PASS");
    return 0;
}
'''
    run_c_test(prefix + code + tests)


def test_profile_json_failure_cleanup(source):
    """用实际 sticky builder 注入分配失败，核对 profile 全部结果清理和关闭 trace 的格式分支。"""
    prefix = r'''
#include <inttypes.h>
#define configUSE_TRACE_FACILITY 0
#define configGENERATE_RUN_TIME_STATS 0
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
static struct {
    bool enabled;
    int level, interval_ms;
    unsigned samples, dropped, cpu_sample_seq;
    char last_cpu_json[4096], last_snapshot[4096];
    void *cache_metrics;
} s_profile;
static int locked;
static void profile_lock(void) { assert(!locked); locked = 1; }
static void profile_unlock(void) { assert(locked); locked = 0; }
static uint64_t esp_timer_get_time(void) { return 123000; }
static unsigned esp_get_free_heap_size(void) { return 0; }
static unsigned esp_get_minimum_free_heap_size(void) { return 0; }
static size_t heap_caps_get_free_size(int cap) { (void)cap; return 0; }
static size_t heap_caps_get_total_size(int cap) { (void)cap; return 0; }
static size_t heap_caps_get_largest_free_block(int cap) { (void)cap; return 0; }
static size_t heap_caps_get_minimum_free_size(int cap) { (void)cap; return 0; }
static const char *app_profile_level_string(int level) { (void)level; return "basic"; }
static cc_result_t cc_cache_metrics_snapshot(void *metrics, char **out) {
    (void)metrics; *out = malloc(3); assert(*out); memcpy(*out, "{}", 3); return cc_result_ok();
}
'''
    code = function(source, "static void profile_json_result_discard(")
    code += function(source, "static void json_escape_append(")
    code += function(source, "static void append_cpu_json_fresh(")
    code += function(source, "static void append_cpu_json(")
    code += r'''
/** 外部模块追加保持有效成员，并消费自己的结果，隔离本次生产函数责任。 */
static void append_stack_json_locked(cc_string_builder_t *sb) {
    assert(locked); profile_json_result_discard(cc_string_builder_append(sb, "\"stack\":{}"));
}
static void append_event_bus_json(cc_string_builder_t *sb) {
    profile_json_result_discard(cc_string_builder_append(sb, "\"event_bus\":{}"));
}
static void append_tool_pool_json(cc_string_builder_t *sb) {
    profile_json_result_discard(cc_string_builder_append(sb, "\"tool_pool\":{}"));
}
'''
    code += function(source, "static char *build_profile_json(")
    code += function(source, "static char *build_stream_text_summary_json(")
    code += function(source, "void app_profile_append_status_json(")
    tests = r'''
/** 每个独立场景前确认没有遗留拥有结果。 */
static void reset_results(void) {
    assert(messages_created == messages_released);
    messages_created = messages_released = 0;
    fail_builder_init = fail_builder_growth = 0;
}
int main(void) {
    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb); assert(rc.code == CC_OK); cc_result_free(&rc);
    append_cpu_json_fresh(&sb);
    char *text = cc_string_builder_take(&sb);
    assert(text && strstr(text, "\"sample_seq\":1") && strstr(text, "USE_TRACE_FACILITY"));
    free(text); reset_results();

    rc = cc_string_builder_init(&sb); cc_result_free(&rc);
    char long_text[301]; memset(long_text, 'x', 300); long_text[300] = 0;
    fail_builder_growth = 1;
    json_escape_append(&sb, long_text);
    assert(messages_created == 1 && messages_released == 1);
    assert(cc_string_builder_take(&sb) == NULL); reset_results();

    fail_builder_init = 1;
    assert(build_stream_text_summary_json(1, 0, 1, 1, true, "text") == NULL);
    assert(messages_created == 1 && messages_released == 1); reset_results();

    fail_builder_init = 1;
    assert(build_profile_json(NULL, NULL, NULL, NULL, NULL, 0, NULL, -1, NULL, NULL, false) == NULL);
    assert(messages_created == 1 && messages_released == 1); reset_results();

    rc = cc_string_builder_init(&sb); cc_result_free(&rc);
    fail_builder_init = 1;
    append_cpu_json(&sb, true);
    text = cc_string_builder_take(&sb);
    assert(text && strstr(text, "snapshot_builder_oom") && strstr(text, "\"available\":false") &&
           strstr(text, "\"runtime_stats\":false")); free(text);
    assert(messages_created == 1 && messages_released == 1); reset_results();

    rc = cc_string_builder_init(&sb); cc_result_free(&rc);
    memset(s_profile.last_snapshot, 'x', sizeof(s_profile.last_snapshot) - 1U);
    s_profile.cache_metrics = &sb;
    fail_builder_growth = 1;
    app_profile_append_status_json(&sb);
    assert(messages_created > 0 && messages_created == messages_released && !locked);
    assert(cc_string_builder_take(&sb) == NULL); reset_results();

    s_profile.last_snapshot[0] = 0;
    text = build_profile_json("event", "phase", "session", "run", "ok", 5,
                              "sdk", 1, "{}", "{}", false);
    assert(text && strstr(text, "session:1") && strstr(text, "\"event\":\"event\"")); free(text);
    reset_results();
    puts("Profile builder ownership and trace-disabled formatting: PASS");
    return 0;
}
'''
    run_c_test(BUILDER_PREFIX + builder_source() + prefix + code + tests)


def test_sampler_exit_handoff(source):
    """强制 mutex Give 尾部让出，核对 stop/destroy 保留原语及失败创建的并发交接。"""
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *EventGroupHandle_t;
typedef void *SemaphoreHandle_t;
typedef unsigned EventBits_t;
typedef uint16_t TickType_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_INVALID_STATE 2
#define APP_PROFILE_LEVEL_BASIC 0
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define configTICK_RATE_HZ 100U
#define portMAX_DELAY UINT16_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) * configTICK_RATE_HZ / 1000U))
#define APP_PROFILE_TASK_EXITED_BIT 1U
#define APP_PROFILE_TASK_STACK 4096
#define APP_PROFILE_TASK_PRIORITY 2
#define PROFILE_HANDLE ((void *)(uintptr_t)1)
#define __atomic_load_n(pointer, order) (*(pointer))
#define __atomic_store_n(pointer, value, order) (*(pointer)=(value))
#ifndef __ATOMIC_RELEASE
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQUIRE 2
#endif
typedef struct { int unused; } cc_cache_metrics_t;
static cc_cache_metrics_t cache;
static struct {
    SemaphoreHandle_t mutex;
    EventGroupHandle_t events;
    int interval_ms, level, dropped;
    TaskHandle_t sampler_task;
    bool enabled, stopping, runtime_subscribed;
    void *runtime_builder;
    cc_cache_metrics_t *cache_metrics;
} s_profile;
static int s_sampler_tail_complete=1;
static int locked, notifications, registered, deleted;
static int in_sampler, tail_hook_ran, tail_active, primitives_alive=1, destroys;
static int creation_attempts, creation_hook_ran, test_creation, delays, observed_event_ticks;
static int wait_mode, event_waits;
static uint64_t now_us;
static unsigned bits;
static void profile_lock(void) { assert(!locked && primitives_alive); locked=1; }
static void profile_unlock(void);
static esp_err_t app_profile_stop(uint32_t timeout_ms);
static esp_err_t app_profile_destroy(uint32_t timeout_ms);
static void ensure_sampler_task(void);
static uint64_t esp_timer_get_time(void) { return now_us; }
static void vTaskDelay(unsigned ticks) { assert(ticks==1); delays++; now_us+=1000; }
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)(uintptr_t)7; }
static void app_profile_register_task(const char *name, TaskHandle_t task) { (void)name; assert(task==xTaskGetCurrentTaskHandle()); registered++; }
static void app_profile_unregister_task(TaskHandle_t task) { assert(task==xTaskGetCurrentTaskHandle() && registered==1); registered--; }
static void app_profile_sample(const char *event, const char *phase, bool force) {
    (void)event; (void)phase; (void)force; assert(0);
}
static unsigned ulTaskNotifyTake(int clear, unsigned timeout) { (void)clear; (void)timeout; assert(0); return 0; }
static void xTaskNotifyGive(TaskHandle_t task) { assert(locked && task==xTaskGetCurrentTaskHandle()); notifications++; }
static void xEventGroupSetBits(EventGroupHandle_t events, unsigned value) {
    assert(events==PROFILE_HANDLE && locked && s_profile.sampler_task==0 && primitives_alive); bits |= value;
}
static void xEventGroupClearBits(EventGroupHandle_t events, unsigned value) { assert(events==PROFILE_HANDLE && locked); bits &= ~value; }
static void vTaskDelete(void *task) { assert(!task && !locked && !registered && !tail_active); deleted++; }
static void vEventGroupDelete(EventGroupHandle_t events) { assert(events==PROFILE_HANDLE && !tail_active); destroys++; }
static void vSemaphoreDelete(SemaphoreHandle_t mutex) { assert(mutex==PROFILE_HANDLE && !tail_active); primitives_alive=0; destroys++; }
static void cc_cache_metrics_destroy(cc_cache_metrics_t *metrics) { assert(metrics==&cache); destroys++; }
static BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                              void *arg, int priority, TaskHandle_t *out) {
    (void)entry; (void)name; (void)stack; (void)arg; (void)priority;
    assert(locked && !s_sampler_tail_complete);
    creation_attempts++;
    *out=creation_attempts==1 ? NULL : xTaskGetCurrentTaskHandle();
    return creation_attempts==1 ? 0 : pdPASS;
}
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t events, unsigned value, int clear, int all, unsigned ticks);
'''
    code = function(source, "static esp_err_t profile_wait_for_sampler_tail(")
    code += function(source, "static void profile_sampler_task(")
    code += function(source, "static void ensure_sampler_task(")
    code += function(source, "esp_err_t app_profile_stop(")
    code += function(source, "esp_err_t app_profile_destroy(")
    tests = r'''
/** 模拟 Give 已唤醒 owner、但旧 Give 仍将访问原语的真实交错。 */
static void profile_unlock(void) {
    assert(locked); locked=0;
    if (in_sampler && !s_profile.sampler_task && !tail_hook_ran) {
        tail_hook_ran=1; tail_active=1;
        assert(app_profile_stop(0)==ESP_ERR_TIMEOUT);
        assert(app_profile_destroy(0)==ESP_ERR_TIMEOUT);
        assert(primitives_alive && destroys==0);
        tail_active=0;
    }
    if (test_creation && creation_attempts==1 && !creation_hook_ran) {
        creation_hook_ran=1;
        ensure_sampler_task();
    }
    if (!in_sampler && !test_creation && now_us==0) now_us=3000;
}
/** 事件等待执行 sampler，并要求先前配置/等锁用时已从预算扣除。 */
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t events, unsigned value, int clear, int all, unsigned ticks) {
    (void)clear; (void)all; assert(events==PROFILE_HANDLE && !locked && value==1 && ticks<portMAX_DELAY);
    event_waits++;
    observed_event_ticks=(int)ticks;
    if (wait_mode==1) {
        assert(ticks==portMAX_DELAY-1U);
        now_us+=(uint64_t)ticks*1000000U/configTICK_RATE_HZ;
        if (event_waits==1) return 0;
        assert(event_waits==2);
        s_profile.sampler_task=NULL; s_sampler_tail_complete=1;
        return APP_PROFILE_TASK_EXITED_BIT;
    }
    if (wait_mode==2) {
        assert(ticks==1);
        now_us+=1000000U/configTICK_RATE_HZ;
        s_profile.sampler_task=NULL; s_sampler_tail_complete=1;
        return APP_PROFILE_TASK_EXITED_BIT;
    }
    if (wait_mode==3) { assert(ticks==0 && event_waits==1); return 0; }
    assert(ticks==1);
    in_sampler=1;
    profile_sampler_task(NULL);
    in_sampler=0;
    return bits;
}
int main(void) {
    s_profile.mutex=s_profile.events=PROFILE_HANDLE; s_profile.sampler_task=xTaskGetCurrentTaskHandle();
    s_profile.cache_metrics=&cache; s_sampler_tail_complete=0;
    assert(app_profile_stop(10)==ESP_OK);
    assert(observed_event_ticks==1);
    assert(notifications==1 && deleted==1 && !locked && !s_profile.sampler_task && tail_hook_ran);
    assert(app_profile_stop(0)==ESP_OK);
    assert(app_profile_destroy(0)==ESP_OK && destroys==3 && !primitives_alive);

    s_sampler_tail_complete=0;
    assert(profile_wait_for_sampler_tail(now_us,0)==ESP_ERR_TIMEOUT && delays==0);
    assert(profile_wait_for_sampler_tail(now_us,1)==ESP_ERR_TIMEOUT && delays==1);
    s_sampler_tail_complete=1;
    assert(profile_wait_for_sampler_tail(now_us,0)==ESP_OK);

    primitives_alive=1; test_creation=1;
    s_profile.mutex=s_profile.events=PROFILE_HANDLE;
    s_profile.sampler_task=xTaskGetCurrentTaskHandle(); s_sampler_tail_complete=0;
    wait_mode=1; event_waits=0;
    assert(app_profile_stop(UINT32_MAX)==ESP_OK && event_waits==2);
    s_profile.sampler_task=xTaskGetCurrentTaskHandle(); s_sampler_tail_complete=0;
    wait_mode=2; event_waits=0;
    assert(app_profile_stop(1)==ESP_OK && event_waits==1 && observed_event_ticks==1);
    s_profile.sampler_task=xTaskGetCurrentTaskHandle(); s_sampler_tail_complete=0;
    wait_mode=3; event_waits=0;
    assert(app_profile_stop(0)==ESP_ERR_TIMEOUT && event_waits==1);

    primitives_alive=1; test_creation=1;
    s_profile.sampler_task=NULL; s_sampler_tail_complete=1;
    s_profile.mutex=s_profile.events=PROFILE_HANDLE; s_profile.enabled=true; s_profile.interval_ms=10;
    s_profile.stopping=false;
    ensure_sampler_task();
    assert(creation_attempts==2 && s_profile.sampler_task && !s_sampler_tail_complete);
    puts("Profile sampler Give tail, bounded completion and failed-create handoff: PASS");
    return 0;
}
'''
    run_c_test(prefix + code + tests)


def test_cpu_snapshot_owned_identity(source):
    """对当前双核、SMP、单核及按值核心字段分支编译真实身份复制函数。"""
    prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define configMAX_TASK_NAME_LEN 16
typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef struct { int alive; } task_t;
typedef struct { task_t *xHandle; const char *pcTaskName; int xCoreID; } TaskStatus_t;
typedef struct { char name[16]; int core_id; } app_profile_cpu_identity_t;
static task_t task={1};
static char borrowed_name[16]="worker";
static int depth, core_queries;
static void take_snapshot_lock(void) { assert(!depth); depth=1; }
/** 结束内核窗口立刻毒化旧 TCB 借用，后续序列化只能依赖拥有副本。 */
static void release_snapshot_lock(void) {
    assert(depth==1); depth=0; task.alive=0; strcpy(borrowed_name,"released");
}
#define prvTakeKernelLock take_snapshot_lock
#define prvReleaseKernelLock release_snapshot_lock
#define vTaskSuspendAll take_snapshot_lock
#if CONFIG_FREERTOS_SMP || APP_PROFILE_CPU_CORES == 1
static int xTaskResumeAll(void) { release_snapshot_lock(); return 0; }
#endif
static UBaseType_t uxTaskGetSystemState(TaskStatus_t *out, UBaseType_t count, uint32_t *total) {
    assert(depth==1 && count==1); *out=(TaskStatus_t){&task,borrowed_name,1};
    if (total) *total=42;
    return 1;
}
#if APP_PROFILE_CPU_CORES > 1 && !configTASKLIST_INCLUDE_COREID
static int xTaskGetCoreID(task_t *handle) { assert(depth==1 && handle->alive); core_queries++; return 1; }
#endif
'''
    code = function(source, "static UBaseType_t collect_cpu_snapshot(")
    tests = r'''
int main(void) {
    TaskStatus_t status;
    app_profile_cpu_identity_t identity;
    uint32_t total=0;
    assert(collect_cpu_snapshot(&status,&identity,1,&total)==1);
    assert(!depth && !task.alive && total==42 && status.xHandle==NULL);
    assert(status.pcTaskName==identity.name && strcmp(identity.name,"worker")==0);
    assert(identity.core_id==(APP_PROFILE_CPU_CORES==1 ? 0 : 1));
    assert(core_queries==((APP_PROFILE_CPU_CORES>1 && !configTASKLIST_INCLUDE_COREID) ? 1 : 0));
    puts("Profile CPU identity survives snapshot unlock: PASS");
    return 0;
}
'''
    for smp, cores, copied_core in [(0, 2, 0), (1, 2, 0), (0, 1, 0), (0, 2, 1)]:
        macros = (f'#define CONFIG_FREERTOS_SMP {smp}\n#define APP_PROFILE_CPU_CORES {cores}\n'
                  f'#define configTASKLIST_INCLUDE_COREID {copied_core}\n')
        run_c_test(macros + prefix + code + tests)


def test_unsubscribe_timeout_bounds(source):
    """执行实际退订函数，核对极大预算不转负、超时保留状态且成功后清理借用。"""
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int cc_event_subscription_t;
typedef struct { int code; } cc_result_t;
typedef struct { int unused; } cc_runtime_builder_t;
typedef struct { int unused; } cc_runtime_generation_t;
typedef struct { int unused; } cc_agent_runtime_t;
typedef struct { int unused; } cc_event_bus_t;
#define CC_OK 0
#define CC_ERR_NOT_FOUND 1
#define CC_ERR_TIMEOUT 2
#define CC_ERR_INVALID_STATE 3
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define ESP_FAIL 3
static cc_runtime_builder_t builder;
static cc_runtime_generation_t generation;
static cc_agent_runtime_t runtime;
static cc_event_bus_t bus;
static struct { bool runtime_subscribed; cc_runtime_builder_t *runtime_builder; int runtime_subscription; } s_profile;
static int locked, observed_timeout, releases, failure;
static void profile_lock(void) { assert(!locked); locked=1; }
static void profile_unlock(void) { assert(locked); locked=0; }
static cc_result_t cc_result_error(int code, const char *message) { (void)message; return (cc_result_t){code}; }
static void cc_result_free(cc_result_t *result) { (void)result; }
static cc_result_t cc_runtime_builder_acquire_generation(cc_runtime_builder_t *input, cc_runtime_generation_t **out) {
    assert(input==&builder); *out=&generation; return (cc_result_t){CC_OK};
}
static cc_agent_runtime_t *cc_runtime_generation_runtime(cc_runtime_generation_t *input) {
    assert(input==&generation); return &runtime;
}
static cc_event_bus_t *cc_agent_runtime_event_bus(cc_agent_runtime_t *input) { assert(input==&runtime); return &bus; }
static void cc_runtime_generation_release(cc_runtime_generation_t *input) { assert(input==&generation); releases++; }
static cc_result_t cc_event_bus_unsubscribe(cc_event_bus_t *input, int subscription, int timeout) {
    assert(input==&bus && subscription==17); observed_timeout=timeout;
    return (cc_result_t){failure ? CC_ERR_TIMEOUT : CC_OK};
}
'''
    code = function(source, "esp_err_t app_profile_unsubscribe_runtime(")
    tests = r'''
int main(void) {
    s_profile.runtime_subscribed=true; s_profile.runtime_builder=&builder; s_profile.runtime_subscription=17;
    failure=1;
    assert(app_profile_unsubscribe_runtime(UINT32_MAX)==ESP_ERR_TIMEOUT);
    assert(observed_timeout==INT_MAX && releases==1 && s_profile.runtime_subscribed && s_profile.runtime_builder==&builder);
    failure=0;
    assert(app_profile_unsubscribe_runtime(0)==ESP_OK);
    assert(observed_timeout==0 && releases==2 && !s_profile.runtime_subscribed && !s_profile.runtime_builder && !locked);
    puts("Profile unsubscribe bounded timeout and retry: PASS");
    return 0;
}
'''
    run_c_test(prefix + code + tests)


def main():
    """运行注册/注销、调用点顺序与生产函数提取回归，任何失败向外抛出。"""
    source = (ROOT / "main/services/app_profile.c").read_text(encoding="utf-8")
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define APP_PROFILE_TASK_MAX 8
typedef struct { bool alive; unsigned samples; } task_t;
typedef task_t *TaskHandle_t;
static struct { struct { char name[25]; TaskHandle_t handle; } tasks[8]; } s_profile;
static bool locked;
static void profile_lock(void) { assert(!locked); locked=true; }
static void profile_unlock(void) { assert(locked); locked=false; }
static void sample(void) {
    profile_lock();
    for (int i=0; i<APP_PROFILE_TASK_MAX; i++) {
        TaskHandle_t task=s_profile.tasks[i].handle;
        if (task) { assert(task->alive); task->samples++; }
    }
    profile_unlock();
}
'''
    code = function(source, "void app_profile_register_task(")
    code += function(source, "void app_profile_unregister_task(")
    tests = r'''
int main(void) {
    task_t first={true,0}, second={true,0};
    app_profile_register_task("usb", &first);
    sample(); assert(first.samples==1);
    app_profile_unregister_task(&first);
    first.alive=false;
    sample(); assert(first.samples==1);
    app_profile_register_task("usb", &second);
    app_profile_unregister_task(&first);
    sample(); assert(second.samples==1);
    app_profile_unregister_task(&second);
    app_profile_unregister_task(&second);
    second.alive=false;
    sample();
    puts("Profile task register, unregister and handle replacement: PASS");
    return 0;
}
'''
    run_c_test(prefix + code + tests)
    for path, signature in [
        ("main/services/app_profile.c", "static void profile_sampler_task("),
        ("main/services/events/app_event_usb_log_sink.c", "static void usb_log_task("),
        ("main/services/interaction_service.c", "static void interaction_service_task("),
    ]:
        task = function((ROOT / path).read_text(encoding="utf-8"), signature)
        assert task.index("app_profile_register_task(") < task.index("app_profile_unregister_task(")
        assert task.index("app_profile_unregister_task(") < task.index("vTaskDelete")
    ensure = function(source, "static void ensure_sampler_task(void)")
    creation = ensure.index("xTaskCreate(")
    creation_prefix = ensure[ensure.index("xEventGroupClearBits"):creation]
    assert "profile_unlock();" not in creation_prefix
    stop = function(source, "esp_err_t app_profile_stop(")
    assert stop.index("xTaskNotifyGive(") < stop.index("profile_unlock();")
    sampler = function(source, "static void append_stack_json_locked(")
    assert "uxTaskGetStackHighWaterMark" in sampler
    emit = function(source, "static char *build_profile_json(")
    assert emit.index("profile_lock();", emit.index("psram_largest")) < emit.index("append_stack_json_locked(")
    test_tool_pool_borrowed_metrics(source)
    test_profile_json_failure_cleanup(source)
    test_sampler_exit_handoff(source)
    test_cpu_snapshot_owned_identity(source)
    test_unsubscribe_timeout_bounds(source)


if __name__ == "__main__":
    main()
