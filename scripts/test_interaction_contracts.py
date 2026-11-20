"""执行交互服务真实生命周期入口，业务依赖和 FreeRTOS 由本地模型提供。

使用 pthread 运行真实 owner 循环，核对接纳登记、关闭重试和通知返回尾部。
此脚本验证宿主调度与拥有关系，目标板栈和原生内核另行验收。
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(text, signature):
    """跳过前置声明并按真实括号范围取得定义，保留原始注释及条件分支。"""
    search_from = 0
    while True:
        start = text.index(signature, search_from)
        depth = 0
        tokens = re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{};]', text[start:], re.S)
        for token in tokens:
            value = token[0]
            if value == ";" and depth == 0:
                search_from = start + len(signature)
                break
            if value == "{":
                depth += 1
            elif value == "}":
                depth -= 1
                if depth == 0:
                    return text[start:start + token.end()] + "\n"
        else:
            raise ValueError("Incomplete function " + signature)


PRELUDE = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_FAIL, ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE,
       ESP_ERR_INVALID_SIZE, ESP_ERR_TIMEOUT, ESP_ERR_NOT_FINISHED };
typedef uint32_t TickType_t;
typedef unsigned UBaseType_t;
typedef int BaseType_t;
typedef unsigned EventBits_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(mutex) assert(pthread_mutex_lock(mutex)==0)
#define portEXIT_CRITICAL(mutex) assert(pthread_mutex_unlock(mutex)==0)
#define configTICK_RATE_HZ 100U
#define configSUPPORT_STATIC_ALLOCATION TEST_WITH_CAPS
#define CONFIG_SPIRAM TEST_WITH_CAPS
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM TEST_WITH_CAPS
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms)*configTICK_RATE_HZ/1000U)
#define BIT0 1U
#define BIT1 2U
#define BIT2 4U
#define BIT3 8U
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_INTERNAL 2U
#define MALLOC_CAP_8BIT 4U
typedef struct queue_model {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    size_t capacity, item_bytes, count, head;
    unsigned char *items;
} *QueueHandle_t;
static void fixture_received_item(QueueHandle_t queue,void *item);
typedef struct group_model {
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    EventBits_t bits;
} *EventGroupHandle_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef struct task_model {
    pthread_t thread;
    void (*entry)(void *);
    void *argument;
} *TaskHandle_t;
static _Thread_local TaskHandle_t current_task;
static TaskHandle_t created_task;
static atomic_int queues_live, groups_live, semaphores_live, payloads_live, results_live;
static atomic_int controllers_live, menus_live, generations_live, task_create_failures;
static atomic_int shutdown_calls, text_calls, controller_failure, profile_self_probe;
static pthread_mutex_t test_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_changed=PTHREAD_COND_INITIALIZER;
static bool hold_exit_tail, exit_tail_entered, release_exit_tail;
static bool hold_reply_tail, reply_tail_entered, release_reply_tail;
static bool hold_status, status_entered, release_status;
static bool hold_start_reply, start_reply_entered, release_start_reply;
static bool verify_delayed_start;
static bool close_during_payload;
esp_err_t interaction_service_stop(uint32_t timeout_ms);

static struct timespec deadline(TickType_t ticks)
{
    struct timespec until;
    clock_gettime(CLOCK_REALTIME,&until);
    uint64_t milliseconds=(uint64_t)ticks*1000U/configTICK_RATE_HZ;
    until.tv_sec+=(time_t)(milliseconds/1000U);
    until.tv_nsec+=(long)(milliseconds%1000U)*1000000L;
    if(until.tv_nsec>=1000000000L) { until.tv_nsec-=1000000000L; until.tv_sec++; }
    return until;
}
static TickType_t xTaskGetTickCount(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (TickType_t)((uint64_t)now.tv_sec*configTICK_RATE_HZ+
                       (uint64_t)now.tv_nsec*configTICK_RATE_HZ/1000000000U);
}
static int64_t esp_timer_get_time(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (int64_t)now.tv_sec*1000000+(int64_t)now.tv_nsec/1000;
}
static void vTaskDelay(TickType_t ticks)
{
    struct timespec delay={.tv_sec=ticks/configTICK_RATE_HZ,
        .tv_nsec=(long)(ticks%configTICK_RATE_HZ)*1000000000L/configTICK_RATE_HZ};
    while(nanosleep(&delay,&delay)!=0) assert(errno==EINTR);
}
static TaskHandle_t xTaskGetCurrentTaskHandle(void)
{ return current_task ? current_task : (TaskHandle_t)(uintptr_t)pthread_self(); }
static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t mutex=malloc(sizeof(*mutex));
    assert(mutex && pthread_mutex_init(mutex,NULL)==0);
    atomic_fetch_add(&semaphores_live,1);
    return mutex;
}
static BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex,TickType_t wait)
{
    if(wait==portMAX_DELAY) return pthread_mutex_lock(mutex)==0;
    if(wait==0) return pthread_mutex_trylock(mutex)==0;
    struct timespec until=deadline(wait);
    return pthread_mutex_timedlock(mutex,&until)==0;
}
static BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{ return pthread_mutex_unlock(mutex)==0; }
static void vSemaphoreDelete(SemaphoreHandle_t mutex)
{
    assert(pthread_mutex_destroy(mutex)==0);
    free(mutex); atomic_fetch_sub(&semaphores_live,1);
}
static QueueHandle_t xQueueCreate(UBaseType_t capacity,UBaseType_t item_bytes)
{
    QueueHandle_t queue=calloc(1,sizeof(*queue));
    assert(queue && capacity && item_bytes);
    queue->items=calloc(capacity,item_bytes); assert(queue->items);
    queue->capacity=capacity; queue->item_bytes=item_bytes;
    assert(pthread_mutex_init(&queue->mutex,NULL)==0);
    assert(pthread_cond_init(&queue->changed,NULL)==0);
    atomic_fetch_add(&queues_live,1); return queue;
}
#if TEST_WITH_CAPS
static QueueHandle_t xQueueCreateWithCaps(UBaseType_t capacity,UBaseType_t bytes,unsigned caps)
{ (void)caps; return xQueueCreate(capacity,bytes); }
#endif
static BaseType_t xQueueSend(QueueHandle_t queue,const void *item,TickType_t wait)
{
    assert(wait==0);
    pthread_mutex_lock(&queue->mutex);
    if(queue->count==queue->capacity) { pthread_mutex_unlock(&queue->mutex); return pdFALSE; }
    size_t slot=(queue->head+queue->count)%queue->capacity;
    memcpy(queue->items+slot*queue->item_bytes,item,queue->item_bytes);
    queue->count++;
    pthread_cond_broadcast(&queue->changed);
    pthread_mutex_unlock(&queue->mutex); return pdTRUE;
}
static BaseType_t xQueueReceive(QueueHandle_t queue,void *item,TickType_t wait)
{
    pthread_mutex_lock(&queue->mutex);
    struct timespec until=deadline(wait);
    while(!queue->count && wait) {
        int result=wait==portMAX_DELAY ? pthread_cond_wait(&queue->changed,&queue->mutex) :
            pthread_cond_timedwait(&queue->changed,&queue->mutex,&until);
        if(result==ETIMEDOUT) break;
        assert(result==0);
    }
    bool available=queue->count>0;
    if(available) {
        memcpy(item,queue->items+queue->head*queue->item_bytes,queue->item_bytes);
        queue->head=(queue->head+1)%queue->capacity; queue->count--;
    }
    pthread_mutex_unlock(&queue->mutex);
    if(available) fixture_received_item(queue,item);
    return available;
}
static UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue)
{
    pthread_mutex_lock(&queue->mutex);
    UBaseType_t count=(UBaseType_t)queue->count;
    pthread_mutex_unlock(&queue->mutex); return count;
}
static void vQueueDelete(QueueHandle_t queue)
{
    assert(pthread_mutex_destroy(&queue->mutex)==0);
    assert(pthread_cond_destroy(&queue->changed)==0);
    free(queue->items); free(queue); atomic_fetch_sub(&queues_live,1);
}
#if TEST_WITH_CAPS
static void vQueueDeleteWithCaps(QueueHandle_t queue) { vQueueDelete(queue); }
#endif
static EventGroupHandle_t xEventGroupCreate(void)
{
    EventGroupHandle_t group=calloc(1,sizeof(*group)); assert(group);
    assert(pthread_mutex_init(&group->mutex,NULL)==0);
    assert(pthread_cond_init(&group->changed,NULL)==0);
    atomic_fetch_add(&groups_live,1); return group;
}
static EventBits_t xEventGroupSetBits(EventGroupHandle_t group,EventBits_t bits)
{
    pthread_mutex_lock(&group->mutex);
    group->bits|=bits; pthread_cond_broadcast(&group->changed);
    pthread_mutex_unlock(&group->mutex);
    pthread_mutex_lock(&test_mutex);
    if(current_task && (bits&BIT2) && hold_exit_tail) {
        exit_tail_entered=true; pthread_cond_broadcast(&test_changed);
        while(!release_exit_tail) pthread_cond_wait(&test_changed,&test_mutex);
    }
    if(current_task && (bits&BIT3) && hold_reply_tail) {
        reply_tail_entered=true; pthread_cond_broadcast(&test_changed);
        while(!release_reply_tail) pthread_cond_wait(&test_changed,&test_mutex);
    }
    pthread_mutex_unlock(&test_mutex);
    /* 模拟原生通知发布之后仍会访问事件组，提前销毁会被 sanitizer 发现。 */
    pthread_mutex_lock(&group->mutex);
    EventBits_t result=group->bits;
    pthread_mutex_unlock(&group->mutex); return result;
}
static EventBits_t xEventGroupClearBits(EventGroupHandle_t group,EventBits_t bits)
{
    pthread_mutex_lock(&group->mutex);
    EventBits_t old=group->bits; group->bits&=~bits;
    pthread_mutex_unlock(&group->mutex); return old;
}
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t group,EventBits_t bits,
                                      BaseType_t clear,BaseType_t all,TickType_t wait)
{
    pthread_mutex_lock(&group->mutex);
    struct timespec until=deadline(wait);
    while((all ? (group->bits&bits)!=bits : !(group->bits&bits)) && wait) {
        int result=pthread_cond_timedwait(&group->changed,&group->mutex,&until);
        if(result==ETIMEDOUT) break;
        assert(result==0);
    }
    EventBits_t result=group->bits;
    if(clear && (all ? (result&bits)==bits : (result&bits)!=0)) group->bits&=~bits;
    pthread_mutex_unlock(&group->mutex); return result;
}
static void vEventGroupDelete(EventGroupHandle_t group)
{
    assert(pthread_mutex_destroy(&group->mutex)==0);
    assert(pthread_cond_destroy(&group->changed)==0);
    free(group); atomic_fetch_sub(&groups_live,1);
}
static void *task_entry(void *argument)
{
    current_task=argument;
    current_task->entry(current_task->argument);
    return NULL;
}
static BaseType_t xTaskCreate(void (*entry)(void *),const char *name,uint32_t bytes,
                             void *argument,UBaseType_t priority,TaskHandle_t *output)
{
    (void)name; (void)bytes; (void)priority;
    if(atomic_load(&task_create_failures)>0) { atomic_fetch_sub(&task_create_failures,1); return pdFALSE; }
    TaskHandle_t task=calloc(1,sizeof(*task)); assert(task && !created_task);
    task->entry=entry; task->argument=argument; *output=task; created_task=task;
    assert(pthread_create(&task->thread,NULL,task_entry,task)==0); return pdPASS;
}
#if TEST_WITH_CAPS
static BaseType_t xTaskCreateWithCaps(void (*entry)(void *),const char *name,uint32_t bytes,
    void *argument,UBaseType_t priority,TaskHandle_t *output,unsigned caps)
{ (void)caps; return xTaskCreate(entry,name,bytes,argument,priority,output); }
#endif
static void vTaskDelete(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
#if TEST_WITH_CAPS
static void vTaskDeleteWithCaps(TaskHandle_t task) { vTaskDelete(task); }
#endif
static void join_worker(void)
{
    assert(created_task && pthread_join(created_task->thread,NULL)==0);
    free(created_task); created_task=NULL;
}
static void wait_flag(bool *flag)
{
    pthread_mutex_lock(&test_mutex);
    struct timespec until=deadline(500);
    while(!*flag) assert(pthread_cond_timedwait(&test_changed,&test_mutex,&until)==0);
    pthread_mutex_unlock(&test_mutex);
}
static void release_flag(bool *flag)
{
    pthread_mutex_lock(&test_mutex); *flag=true;
    pthread_cond_broadcast(&test_changed); pthread_mutex_unlock(&test_mutex);
}
typedef int cc_error_code_t;
enum { CC_OK, CC_ERR_INVALID_STATE, CC_ERR_TIMEOUT, CC_ERR_STORAGE, CC_ERR_IO,
       CC_ERR_LIMIT_EXCEEDED, CC_ERR_QUEUE_FULL };
typedef struct { cc_error_code_t code; char *message; } cc_result_t;
static cc_result_t result_with_message(int code,const char *message)
{
    char *copy=strdup(message); assert(copy); atomic_fetch_add(&results_live,1);
    return (cc_result_t){code,copy};
}
static cc_result_t cc_result_ok(void) { return (cc_result_t){0}; }
static cc_result_t cc_result_error(int code,const char *message) { return result_with_message(code,message); }
static const char *cc_error_string(int code) { (void)code; return "fixture error"; }
static void cc_result_free(cc_result_t *result)
{
    if(result->message) { free(result->message); atomic_fetch_sub(&results_live,1); }
    memset(result,0,sizeof(*result));
}
typedef int app_event_source_t;
enum { APP_EVENT_SOURCE_SYSTEM, APP_EVENT_SOURCE_RUNTIME, APP_EVENT_SOURCE_TOOL, APP_EVENT_SOURCE_SERIAL };
enum { APP_EVENT_KIND_STATUS, APP_EVENT_KIND_ERROR, APP_EVENT_KIND_RUN_STATE, APP_EVENT_KIND_TOOL,
       APP_EVENT_KIND_USER, APP_EVENT_KIND_ASSISTANT, APP_EVENT_KIND_THINKING };
enum { CC_LOG_INFO, CC_LOG_WARN, CC_LOG_ERROR, CC_LOG_DEBUG };
typedef struct { const char *session_id; int queue_depth,max_input_bytes; } app_runtime_io_config_t;
typedef struct { atomic_bool thinking; } cc_agent_runtime_t;
typedef struct { cc_agent_runtime_t runtime; } cc_runtime_builder_t;
typedef struct { cc_runtime_builder_t *builder; } cc_runtime_generation_t;
typedef struct { cc_runtime_builder_t *builder; } recovery_service_t;
typedef int manual_lua_policy_t;
enum { MANUAL_LUA_POLICY_PENDING, MANUAL_LUA_USE_MODEL_REGISTRY, MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY };
typedef struct { int unused; } cc_managed_memory_editor_t;
typedef struct { cc_managed_memory_editor_t *editor; } session_memory_turns_t;
typedef struct { int unused; } session_menu_t;
typedef struct { int unused; } recovery_console_t;
typedef struct { int unused; } memory_console_t;
typedef struct { int unused; } cc_cancel_source_t;
typedef struct { size_t size; const char *session_base; int drop_late_events,fresh_session_on_start; } cc_runtime_controller_config_t;
typedef struct { size_t size; uint64_t active_run_id; unsigned long runtime_generation; char session_id[96]; } cc_runtime_controller_status_t;
typedef struct { int unused; } cc_runtime_controller_t;
typedef struct { int unused; } app_profile_span_t;
enum { APP_PROFILE_LEVEL_BASIC, APP_PROFILE_LEVEL_TRACE };
static bool app_profile_enabled(void) { return true; }
static int app_profile_level(void) { return APP_PROFILE_LEVEL_BASIC; }
static const char *app_profile_level_string(int level) { (void)level; return "basic"; }
static void app_profile_set_enabled(bool enabled) { (void)enabled; }
static void app_profile_set_level(int level) { (void)level; }
static void app_profile_set_interval_ms(int interval) { (void)interval; }
static void app_profile_sample(const char *scope,const char *event,bool force) { (void)scope;(void)event;(void)force; }
static app_profile_span_t app_profile_span_begin(const char *scope,const char *event,const char *session,const char *run)
{ (void)scope;(void)event;(void)session;(void)run; return (app_profile_span_t){0}; }
static void app_profile_span_end(app_profile_span_t *span,const char *result) { (void)span;(void)result; }
static void app_profile_register_task(const char *name,TaskHandle_t task)
{
    (void)name; assert(task==current_task);
    if(atomic_exchange(&profile_self_probe,0)) assert(interaction_service_stop(1)==ESP_ERR_INVALID_STATE);
}
static void app_profile_unregister_task(TaskHandle_t task) { assert(task==current_task); }
static atomic_int lua_rejections;
static void app_event_publish(int kind,int source,int level,const char *text)
{
    (void)kind;(void)source;(void)level;
    if(text && strstr(text,"账本")) atomic_fetch_add(&lua_rejections,1);
}
static void app_event_publishf(int kind,int source,int level,const char *format,...)
{ (void)kind;(void)source;(void)level;(void)format; }
static void app_event_bus_clear_conversation(void) {}
static bool app_event_bus_maintenance_mode_enabled(void) { return false; }
static void app_event_bus_set_serial_debug(bool enabled) { (void)enabled; }
static bool app_event_bus_serial_debug_enabled(void) { return false; }
static uint32_t esp_get_free_heap_size(void) { return 100000; }
static uint32_t esp_get_minimum_free_heap_size(void) { return 90000; }
static size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 80000; }
static size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 70000; }
static bool usb_storage_service_is_mounted(void) { return true; }
static bool recovery_service_is_current(recovery_service_t *recovery) { return recovery!=NULL; }
static bool recovery_service_uses_builder(recovery_service_t *recovery,cc_runtime_builder_t *builder)
{ return recovery && recovery->builder==builder; }
static bool session_memory_turns_uses(session_memory_turns_t *turns,cc_runtime_builder_t *builder,recovery_service_t *recovery)
{ (void)builder;(void)recovery; return turns!=NULL; }
static cc_managed_memory_editor_t *session_memory_turns_editor(session_memory_turns_t *turns)
{ return turns ? turns->editor : NULL; }
static void cc_cancel_source_cancel(cc_cancel_source_t *source) { (void)source; }
static cc_result_t cc_runtime_controller_create(cc_runtime_builder_t *builder,
    const cc_runtime_controller_config_t *config,cc_runtime_controller_t **output)
{
    (void)builder; assert(config->session_base);
    *output=calloc(1,sizeof(**output)); assert(*output); atomic_fetch_add(&controllers_live,1);
    return result_with_message(CC_OK,"controller created");
}
static cc_result_t cc_runtime_controller_apply_config(cc_runtime_controller_t *controller,
    const cc_runtime_controller_config_t *config)
{ assert(controller && config); return result_with_message(CC_OK,"controller configured"); }
static void cc_runtime_controller_get_status(cc_runtime_controller_t *controller,
                                             cc_runtime_controller_status_t *status)
{
    memset(status,0,sizeof(*status));
    if(!controller) return;
    pthread_mutex_lock(&test_mutex);
    if(!current_task && hold_status) {
        hold_status=false; status_entered=true; pthread_cond_broadcast(&test_changed);
        while(!release_status) pthread_cond_wait(&test_changed,&test_mutex);
    }
    pthread_mutex_unlock(&test_mutex);
    status->runtime_generation=1; memcpy(status->session_id,"fixture-session",16);
}
static cc_result_t cc_runtime_controller_cancel(cc_runtime_controller_t *controller,uint64_t run)
{ assert(controller && run); return result_with_message(CC_OK,"cancelled"); }
static cc_result_t cc_runtime_controller_destroy(cc_runtime_controller_t *controller,int timeout)
{
    assert(controller && timeout>0); atomic_fetch_add(&shutdown_calls,1);
    if(atomic_load(&controller_failure)) return result_with_message(CC_ERR_TIMEOUT,"retry destroy");
    free(controller); atomic_fetch_sub(&controllers_live,1);
    return result_with_message(CC_OK,"destroyed");
}
static cc_result_t cc_runtime_builder_acquire_generation(cc_runtime_builder_t *builder,
                                                         cc_runtime_generation_t **output)
{
    *output=malloc(sizeof(**output)); assert(*output); (*output)->builder=builder;
    atomic_fetch_add(&generations_live,1); return result_with_message(CC_OK,"generation");
}
static cc_agent_runtime_t *cc_runtime_generation_runtime(cc_runtime_generation_t *generation)
{ return &generation->builder->runtime; }
static void cc_runtime_generation_release(cc_runtime_generation_t *generation)
{ free(generation); atomic_fetch_sub(&generations_live,1); }
static int cc_agent_runtime_get_thinking_mode(cc_agent_runtime_t *runtime) { return atomic_load(&runtime->thinking); }
static void cc_agent_runtime_set_thinking_mode(cc_agent_runtime_t *runtime,int enabled) { atomic_store(&runtime->thinking,enabled!=0); }
static int cc_agent_runtime_supports_stream(cc_agent_runtime_t *runtime) { (void)runtime; return 0; }
static void *allocate_menu(void)
{ void *menu=malloc(1); assert(menu); atomic_fetch_add(&menus_live,1); return menu; }
static void destroy_menu(void *menu) { if(menu) { free(menu); atomic_fetch_sub(&menus_live,1); } }
static cc_result_t session_menu_create(cc_runtime_controller_t *controller,cc_runtime_builder_t *builder,
                                      recovery_service_t *recovery,session_menu_t **output)
{ assert(controller && builder && recovery); *output=allocate_menu(); return result_with_message(CC_OK,"menu"); }
static cc_result_t session_menu_create_with_memory(cc_runtime_controller_t *controller,cc_runtime_builder_t *builder,
    recovery_service_t *recovery,session_memory_turns_t *turns,session_menu_t **output)
{ assert(turns); return session_menu_create(controller,builder,recovery,output); }
static cc_result_t recovery_console_create(recovery_service_t *recovery,recovery_console_t **output)
{ assert(recovery); *output=allocate_menu(); return result_with_message(CC_OK,"menu"); }
static cc_result_t memory_console_create(cc_managed_memory_editor_t *editor,recovery_service_t *recovery,memory_console_t **output)
{ assert(editor && recovery); *output=allocate_menu(); return result_with_message(CC_OK,"menu"); }
static cc_result_t memory_console_create_with_turns(cc_managed_memory_editor_t *editor,recovery_service_t *recovery,
    session_memory_turns_t *turns,memory_console_t **output)
{ assert(turns); return memory_console_create(editor,recovery,output); }
static esp_err_t session_menu_present(session_menu_t *menu) { assert(menu); return ESP_OK; }
static void memory_console_destroy(memory_console_t *menu) { destroy_menu(menu); }
static void recovery_console_destroy(recovery_console_t *menu) { destroy_menu(menu); }
static void session_menu_destroy(session_menu_t *menu) { destroy_menu(menu); }
static void session_menu_invalidate_view(session_menu_t *menu) { (void)menu; }
static void memory_console_invalidate_view(memory_console_t *menu) { (void)menu; }
static void recovery_console_invalidate_view(recovery_console_t *menu) { (void)menu; }
'''


BUSINESS_MODEL = r'''
/* 业务模型只处理本地命令，真实 owner 循环、队列、状态和生命周期函数由下文生产原文提供。 */
static void interaction_pump_memory(void) {}
static void interaction_handle_text(interaction_command_t *command)
{
    atomic_fetch_add(&text_calls,1);
    command_service_context_t context={.runtime=&s_interaction.runtime_builder->runtime,.source=command->source};
    (void)command_service_handle(&context,command->text);
    interaction_apply_state_after_work(true);
}
static void *heap_caps_malloc(size_t bytes,unsigned caps)
{
    (void)caps;
    if(close_during_payload) {
        close_during_payload=false;
        interaction_lock(); s_interaction.stopping=true; s_interaction.started=false;
        interaction_set_access_open(false); interaction_unlock();
    }
    void *result=malloc(bytes); if(result) atomic_fetch_add(&payloads_live,1); return result;
}
static void heap_caps_free(void *pointer)
{ if(pointer) { free(pointer); atomic_fetch_sub(&payloads_live,1); } }
'''


TESTS = r'''
static cc_runtime_builder_t builder;
static recovery_service_t recovery={.builder=&builder};
static cc_managed_memory_editor_t editor;
static app_runtime_io_config_t io={.session_id="fixture",.queue_depth=8,.max_input_bytes=1024};
static void fixture_received_item(QueueHandle_t queue,void *item)
{
    if(queue->capacity==1) return;
    interaction_command_t *command=item;
    pthread_mutex_lock(&test_mutex);
    if(command->type==INTERACTION_COMMAND_START && hold_start_reply) {
        start_reply_entered=true; pthread_cond_broadcast(&test_changed);
        while(!release_start_reply) pthread_cond_wait(&test_changed,&test_mutex);
    }
    if(command->type==INTERACTION_COMMAND_SHUTDOWN && verify_delayed_start) {
        assert(!s_interaction.started && !s_interaction_access_open);
        verify_delayed_start=false;
    }
    pthread_mutex_unlock(&test_mutex);
}
static void assert_clean(void)
{
    assert(!atomic_load(&queues_live) && !atomic_load(&groups_live) && !atomic_load(&semaphores_live));
    assert(!atomic_load(&payloads_live) && !atomic_load(&results_live) && !atomic_load(&controllers_live));
    assert(!atomic_load(&menus_live) && !atomic_load(&generations_live));
    assert(interaction_access_drained() && !s_interaction_access_open && interaction_task_completed());
}
static void start_service(void)
{
    atomic_store(&profile_self_probe,1);
    assert(interaction_service_start_with_recovery(&io,&builder,&recovery,
        MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY,&editor)==ESP_OK);
    assert(s_interaction.started && s_interaction_access_open && !interaction_task_completed());
    assert(!atomic_load(&results_live));
}
static void finish_service(void)
{
    assert(interaction_service_stop(5000)==ESP_OK); join_worker();
    assert(interaction_service_destroy()==ESP_OK); assert_clean();
}
static void *status_reader(void *argument)
{
    interaction_service_status_t *status=argument;
    interaction_service_get_status(status); return NULL;
}
static void test_exit_tail(void)
{
    start_service();
    pthread_mutex_lock(&test_mutex); hold_exit_tail=true; release_exit_tail=false; exit_tail_entered=false;
    pthread_mutex_unlock(&test_mutex);
    assert(interaction_service_stop(20)==ESP_ERR_TIMEOUT);
    wait_flag(&exit_tail_entered);
    assert(!interaction_task_completed() && interaction_service_destroy()==ESP_ERR_INVALID_STATE);
    assert(atomic_load(&groups_live)==1 && atomic_load(&semaphores_live)==1);
    release_flag(&release_exit_tail); finish_service();
    hold_exit_tail=false;
}
static void test_api_lease(void)
{
    start_service();
    pthread_mutex_lock(&test_mutex); hold_status=true; status_entered=false; release_status=false;
    pthread_mutex_unlock(&test_mutex);
    interaction_service_status_t status;
    pthread_t reader; assert(pthread_create(&reader,NULL,status_reader,&status)==0);
    wait_flag(&status_entered);
    int before=atomic_load(&shutdown_calls);
    assert(interaction_service_stop(20)==ESP_ERR_TIMEOUT);
    assert(atomic_load(&shutdown_calls)==before && atomic_load(&controllers_live)==1);
    assert(interaction_service_submit_text(APP_EVENT_SOURCE_SERIAL,"late")==ESP_ERR_INVALID_STATE);
    release_flag(&release_status); assert(pthread_join(reader,NULL)==0);
    assert(status.started && !strcmp(status.session_id,"fixture-session"));
    finish_service();
}
static void test_retry_and_stale_reply(void)
{
    start_service(); atomic_store(&controller_failure,1);
    pthread_mutex_lock(&test_mutex); hold_reply_tail=true; reply_tail_entered=false; release_reply_tail=false;
    pthread_mutex_unlock(&test_mutex);
    int before=atomic_load(&shutdown_calls);
    assert(interaction_service_stop(5000)==ESP_ERR_TIMEOUT); wait_flag(&reply_tail_entered);
    assert(s_interaction.shutdown_phase==INTERACTION_SHUTDOWN_RETRYABLE && !interaction_task_completed());
    atomic_store(&controller_failure,0);
    /* 第二次调用接纳新关闭命令，旧通知位即使仍在返回尾部也不能伪造本次完成。 */
    assert(interaction_service_stop(20)==ESP_ERR_TIMEOUT);
    assert(atomic_load(&shutdown_calls)==before+1 && s_interaction.shutdown_phase==INTERACTION_SHUTDOWN_REQUESTED);
    release_flag(&release_reply_tail);
    finish_service(); assert(atomic_load(&shutdown_calls)==before+2);
    hold_reply_tail=false;
}
static void test_submit_and_start_boundary(void)
{
    start_service(); close_during_payload=true;
    assert(interaction_service_submit_text(APP_EVENT_SOURCE_SERIAL,"closed during copy")==ESP_ERR_INVALID_STATE);
    assert(!atomic_load(&payloads_live) && !s_interaction_access_open && interaction_access_drained());
    finish_service();

    /* 实际 START 已交给 owner，暂停接收返回尾部，使同步等待先到期。 */
    pthread_mutex_lock(&test_mutex);
    hold_start_reply=true; start_reply_entered=false; release_start_reply=false; verify_delayed_start=true;
    pthread_mutex_unlock(&test_mutex);
    assert(ensure_interaction_primitives(8)==ESP_OK);
    interaction_command_t command={.type=INTERACTION_COMMAND_START,.runtime_builder=&builder,
        .recovery=&recovery,.memory_editor=&editor,.manual_lua_policy=MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY};
    assert(interaction_copy_config_payload(&io,&command.config)==ESP_OK);
    assert(interaction_enqueue_sync_timeout(&command,NULL,20)==ESP_ERR_TIMEOUT);
    interaction_command_cleanup(&command); wait_flag(&start_reply_entered);
    assert(interaction_service_stop(20)==ESP_ERR_TIMEOUT && !s_interaction_access_open);
    release_flag(&release_start_reply); finish_service();
    assert(!verify_delayed_start); hold_start_reply=false;
}
static void test_commands_waiter_and_creation(void)
{
    char lua[]="/lua return 1";
    assert(command_service_handle(NULL,lua)==COMMAND_SERVICE_HANDLED);
    assert(atomic_load(&lua_rejections)>0);
    char thinking[]="/thinking on";
    assert(command_service_handle(NULL,thinking)==COMMAND_SERVICE_HANDLED && !atomic_load(&builder.runtime.thinking));
    app_waiter_t *waiter=app_waiter_create(17); assert(waiter);
    assert(app_waiter_complete(waiter,ESP_FAIL,"business failure"));
    app_reply_t reply={.request_id=99};
    assert(app_waiter_wait(waiter,0,&reply)==ESP_FAIL && reply.request_id==17);
    app_waiter_release(waiter); assert_clean();

    atomic_store(&task_create_failures,TEST_WITH_CAPS ? 2 : 1);
    assert(interaction_service_start(&io,&builder)==ESP_FAIL);
    assert_clean();
    assert(interaction_timeout_ticks(0)==0 && interaction_timeout_ticks(1)==1 && interaction_timeout_ticks(2)==1);
    assert(interaction_timeout_ticks(UINT32_MAX)<portMAX_DELAY);
}
int main(void)
{
    test_exit_tail(); test_api_lease(); test_retry_and_stale_reply();
    test_submit_and_start_boundary(); test_commands_waiter_and_creation();
    puts("Interaction real owner/API admission, retry, tail and command contracts: PASS");
    return 0;
}
'''


class InteractionContracts(unittest.TestCase):
    def run_model(self, with_caps):
        """编译当前真实生命周期函数和 owner，以 pthread 替换内核原语。"""
        if os.name == "nt":
            raise RuntimeError("此 pthread 生命周期模型需在 Linux/WSL 运行")
        text = (ROOT / "main/services/interaction_service.c").read_text(encoding="utf-8")
        header = re.sub(r"^#(?:include|pragma).*$", "", (ROOT / "main/services/interaction_service.h").read_text(encoding="utf-8"), flags=re.M)
        ipc = re.sub(r"^#(?:include|pragma).*$", "", (ROOT / "main/services/app_ipc.h").read_text(encoding="utf-8"), flags=re.M)
        command_header = re.sub(r"^#(?:include|pragma).*$", "", (ROOT / "main/services/command_service.h").read_text(encoding="utf-8"), flags=re.M)
        command = re.sub(r"^#include.*$", "", (ROOT / "main/services/command_service.c").read_text(encoding="utf-8"), flags=re.M)
        start = text.index("#define INTERACTION_SERVICE_TASK_STACK ")
        end = text.index("/**", text.index("static bool s_interaction_task_completed"))
        state = text[start:end]
        before_business = [
            "static bool interaction_access_acquire(", "static void interaction_access_release(",
            "static void interaction_set_access_open(", "static bool interaction_access_drained(",
            "static bool interaction_task_completed(", "static TickType_t interaction_timeout_ticks(",
            "static TickType_t interaction_remaining_ticks(", "const char *interaction_service_state_string(",
            "static bool interaction_state_is_busy(", "static void interaction_lock(", "static void interaction_unlock(",
            "static void interaction_sync_event_bits(", "static char *interaction_strdup(",
            "static void interaction_set_state_locked(", "static void interaction_set_state(",
            "static app_request_id_t interaction_next_request_id(", "static void interaction_free_config_payload(",
            "static esp_err_t interaction_copy_config_payload(", "static void interaction_complete(",
            "static void interaction_command_cleanup(", "static esp_err_t interaction_enqueue_command(",
            "static esp_err_t interaction_enqueue_sync_timeout(", "static esp_err_t interaction_enqueue_sync(",
            "static cc_runtime_controller_t *interaction_controller_snapshot(",
            "static void interaction_apply_state_after_work(", "static esp_err_t interaction_apply_controller_config(",
            "static void interaction_handle_start(", "static void interaction_handle_apply_config(",
            "static esp_err_t interaction_handle_clear(", "static void interaction_handle_cancel(",
            "static void interaction_handle_maintenance(", "static esp_err_t interaction_handle_shutdown(",
            "static bool interaction_input_blocked_for_maintenance(",
        ]
        after_business = [
            "static void interaction_service_task(", "static BaseType_t create_interaction_task(",
            "static esp_err_t ensure_interaction_primitives(", "static esp_err_t interaction_service_start_internal(",
            "esp_err_t interaction_service_start(", "esp_err_t interaction_service_start_with_recovery(",
            "esp_err_t interaction_service_start_with_managed_memory(", "esp_err_t interaction_service_stop(",
            "esp_err_t interaction_service_destroy(", "esp_err_t interaction_service_apply_config(",
            "esp_err_t interaction_service_request_cancel(", "esp_err_t interaction_service_set_maintenance_paused(",
            "esp_err_t interaction_service_submit_text(", "esp_err_t interaction_service_submit_clear(",
            "bool interaction_service_wait_idle(", "void interaction_service_get_status(",
        ]
        overrides = os.environ.get("CC_TEST_INTERACTION_STOP_SOURCE")
        stop_text = Path(overrides).read_text(encoding="utf-8") if overrides else text
        declarations = "\nstatic void *heap_caps_malloc(size_t bytes,unsigned caps);\nstatic void heap_caps_free(void *pointer);\nstatic bool interaction_input_blocked_for_maintenance(app_event_source_t source);\n"
        model = "#define TEST_WITH_CAPS " + str(int(with_caps)) + "\n" + PRELUDE + ipc + header + command_header + state
        model += declarations + "\n" + "\n".join(function(text, signature) for signature in before_business)
        model += "\n" + command + "\n" + BUSINESS_MODEL
        model += "\n".join(function(stop_text if signature == "esp_err_t interaction_service_stop(" else text, signature)
                            for signature in after_business)
        model += TESTS
        with tempfile.TemporaryDirectory(prefix="cclaw-interaction-contract-") as folder:
            work = Path(folder)
            (work / "fixture.c").write_text(model, encoding="utf-8")
            arguments = ["cc", "-std=c11", "-Wall", "-Wextra", "-pthread", "-o", str(work / "fixture"), str(work / "fixture.c")]
            if os.environ.get("CCLAW_TEST_SANITIZERS") == "1":
                arguments[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
            subprocess.run(arguments, check=True)
            subprocess.run([str(work / "fixture")], check=True, timeout=25)

    def test_plain_task(self):
        """检查普通任务创建分支及完整生命周期交错。"""
        self.run_model(False)

    def test_capability_task(self):
        """检查 capability 创建、回退和配对删除分支。"""
        self.run_model(True)


if __name__ == "__main__":
    unittest.main()
