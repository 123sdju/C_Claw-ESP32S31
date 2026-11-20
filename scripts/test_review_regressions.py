"""Host execution of production C failure paths; no ESP-IDF build or board I/O."""
from pathlib import Path
import re
import unittest
from test_event_ring_ownership import run_c_test

ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8")


def function(path, signature):
    text = source(path)
    start = text.index(signature)
    return text[start:text.index("\n}\n", start) + 3]


COMMON = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { int code; const char *message; } cc_result_t;
#define CC_OK 0
#define CC_ERR_IO 1
#define CC_ERR_OUT_OF_MEMORY 2
#define CC_ERR_INVALID_ARGUMENT 3
#define CC_ERR_TIMEOUT 4
#define CC_ERR_STORAGE 5
#define CC_ERR_UNSUPPORTED 6
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NOT_FINISHED 3
#define ESP_ERR_INVALID_STATE 4
typedef int esp_err_t;
static cc_result_t cc_result_ok(void) { cc_result_t r = {0, NULL}; return r; }
static cc_result_t cc_result_error(int c, const char *m) { cc_result_t r = {c,m}; return r; }
static cc_result_t cc_result_errf(int c, const char *m, ...) { return cc_result_error(c,m); }
static void cc_result_free(cc_result_t *r) { r->code=0; r->message=NULL; }
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
'''


class ReviewRegressions(unittest.TestCase):
    def test_run_queue_failed_admission_notifies_collectors(self):
        """编译实际选取函数，检查失败完成作业在两条返回路径都唤醒等待者。"""
        prefix = r'''
#include <assert.h>
#include <stddef.h>
#define CC_RUN_JOB_PENDING 0
#define CC_RUN_JOB_RUNNING 1
#define CC_RUN_JOB_COMPLETED 2
typedef struct { int in_flight; } cc_run_queue_session_state_t;
typedef struct cc_run_queue_job {
    int state, lane;
    const char *session_key;
    struct cc_run_queue_job *next;
} cc_run_queue_job_t;
typedef struct {
    cc_run_queue_job_t *jobs;
    cc_run_queue_session_state_t sessions[1];
    int lane_in_flight[4], total_in_flight, total_pending, cond;
} cc_run_queue_t;
static int notifications;
static int lane_index(int lane) { return lane; }
static int lane_limit(const cc_run_queue_t *queue, int lane) {
    (void)queue; (void)lane; return 1;
}
static int session_limit(const cc_run_queue_t *queue) { (void)queue; return 1; }
static int find_session_locked(const cc_run_queue_t *queue, const char *key) {
    (void)queue; (void)key; return -1;
}
static int create_session_locked(cc_run_queue_t *queue, const char *key) {
    (void)queue; return key[0] == 'f' ? -1 : 0;
}
static void complete_pending_job_locked(cc_run_queue_t *queue,
    cc_run_queue_job_t *job, const char *message) {
    (void)message; job->state = CC_RUN_JOB_COMPLETED; queue->total_pending--;
}
static void cc_cond_broadcast(int condition) { (void)condition; notifications++; }
'''
        code = function("third_party/cclaw-sdk/cclaw/core/src/app/cc_run_queue.c",
                        "static cc_run_queue_job_t *find_runnable_job_locked(")
        tests = r'''
int main(void) {
    cc_run_queue_job_t failed = {CC_RUN_JOB_PENDING, 0, "failed", NULL};
    cc_run_queue_t queue = {0};
    queue.jobs = &failed; queue.total_pending = 1;
    assert(find_runnable_job_locked(&queue) == NULL);
    assert(failed.state == CC_RUN_JOB_COMPLETED && notifications == 1);

    cc_run_queue_job_t runnable = {CC_RUN_JOB_PENDING, 0, "ready", NULL};
    failed.state = CC_RUN_JOB_PENDING; failed.next = &runnable;
    queue = (cc_run_queue_t){0}; queue.jobs = &failed; queue.total_pending = 2;
    notifications = 0;
    assert(find_runnable_job_locked(&queue) == &runnable);
    assert(failed.state == CC_RUN_JOB_COMPLETED && notifications == 1);

    runnable.state = CC_RUN_JOB_PENDING;
    queue = (cc_run_queue_t){0}; queue.jobs = &runnable; queue.total_pending = 1;
    notifications = 0;
    assert(find_runnable_job_locked(&queue) == &runnable);
    assert(notifications == 0);
    return 0;
}
'''
        run_c_test(prefix + code + tests)

    def test_run_queue_worker_limits_do_not_overflow(self):
        """编译实际并发归一化函数，检查极大 int 配置仍得到有界 worker 数。"""
        prefix = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
typedef struct {
    int main_concurrency, subagent_concurrency, plugin_concurrency, mcp_concurrency;
} cc_run_queue_config_t;
'''
        queue_path = "third_party/cclaw-sdk/cclaw/core/src/app/cc_run_queue.c"
        code = function(queue_path, "static int normalized_limit(")
        code += function(queue_path, "static size_t desired_worker_count(")
        tests = r'''
int main(void) {
    cc_run_queue_config_t limits = {INT_MAX, INT_MAX, INT_MAX, INT_MAX};
    assert(desired_worker_count(&limits) == 32);
    limits = (cc_run_queue_config_t){INT_MIN, 0, -1, 1};
    assert(desired_worker_count(&limits) == 4);
    limits = (cc_run_queue_config_t){29, 1, 1, 1};
    assert(desired_worker_count(&limits) == 32);
    limits = (cc_run_queue_config_t){2, 3, 4, 5};
    assert(desired_worker_count(&limits) == 14);
    return 0;
}
'''
        run_c_test(prefix + code + tests)

    def test_runtime_clock_fallback_and_saturation(self):
        """编译生产时钟函数，核对单调零值回退、墙钟失败及两条饱和边界。"""
        prefix = r'''
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
static uint64_t monotonic_ms;
static time_t wall_seconds;
static int wall_reads;
/** 返回测试指定的单调毫秒值，覆盖零值及 uint64_t 上界。 */
static uint64_t cc_platform_monotonic_ms(void) { return monotonic_ms; }
/** 返回测试指定的墙钟秒值，并记录回退次数。 */
static time_t runtime_test_time(time_t *out) {
    wall_reads++;
    if (out) *out = wall_seconds;
    return wall_seconds;
}
#define time runtime_test_time
'''
        code = function("third_party/cclaw-sdk/cclaw/core/src/app/cc_agent_runtime.c",
                        "static long runtime_clock_ms(void)")
        tests = r'''
#undef time
/** 逐项断言时钟来源、失败值和乘法前的饱和判断。 */
int main(void) {
    monotonic_ms = 17; wall_seconds = (time_t)-1;
    assert(runtime_clock_ms() == 17 && wall_reads == 0);
    monotonic_ms = (uint64_t)LONG_MAX;
    assert(runtime_clock_ms() == LONG_MAX && wall_reads == 0);
    monotonic_ms = UINT64_MAX;
    assert(runtime_clock_ms() == LONG_MAX && wall_reads == 0);

    monotonic_ms = 0; wall_seconds = 42;
    assert(runtime_clock_ms() == 42000L && wall_reads == 1);
    wall_seconds = (time_t)-1;
    assert(runtime_clock_ms() == 0 && wall_reads == 2);
    wall_seconds = 0;
    assert(runtime_clock_ms() == 0 && wall_reads == 3);
    wall_seconds = (time_t)((uint64_t)LONG_MAX / 1000U);
    assert(runtime_clock_ms() == (long)wall_seconds * 1000L && wall_reads == 4);
    wall_seconds = (time_t)((uint64_t)LONG_MAX / 1000U + 1U);
    assert(runtime_clock_ms() == LONG_MAX && wall_reads == 5);
    return 0;
}
'''
        run_c_test(prefix + code + tests)

    def test_active_memory_append_error_messages_are_released(self):
        """提取实际摘要钩子和 sticky builder，检查追加失败消息释放且没有存储调用。"""
        prefix = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CC_OK 0
#define CC_ERR_INVALID_ARGUMENT 1
#define CC_ERR_INVALID_STATE 2
#define CC_ERR_OUT_OF_MEMORY 3
#define CC_ERR_LIMIT_EXCEEDED 4
#define CC_ENABLE_ACTIVE_MEMORY 1
#define SB_INITIAL_CAPACITY 256
typedef struct { int code; char *message; } cc_result_t;
typedef struct {
    char *data;
    size_t length, capacity;
    int error_code;
} cc_string_builder_t;
typedef struct { void *vtable; } cc_memory_store_t;
typedef struct {
    cc_memory_store_t *memory_store;
    struct {
        int active_memory_enabled, active_memory_write_summary;
        int active_memory_max_value_chars;
        const char *active_memory_category;
    } config;
} cc_agent_runtime_t;
static int messages_created, messages_released, grow_attempts, fail_growth, store_calls;
/** 构造无拥有字段的成功结果。 */
static cc_result_t cc_result_ok(void) { return (cc_result_t){CC_OK, NULL}; }
/** 复制真实拥有的错误消息，供回归计数其释放。 */
static cc_result_t cc_result_error(int code, const char *message) {
    size_t length = strlen(message) + 1U;
    char *owned = malloc(length);
    assert(owned);
    memcpy(owned, message, length);
    messages_created++;
    return (cc_result_t){code, owned};
}
/** 消费结果拥有的消息，成功结果允许空消息。 */
static void cc_result_free(cc_result_t *result) {
    if (result->message) {
        messages_released++;
        free(result->message);
    }
    *result = cc_result_ok();
}
/** 只对 builder 扩容注入失败，初始缓冲与错误消息仍使用正常分配器。 */
static void *runtime_test_realloc(void *buffer, size_t size) {
    grow_attempts++;
    return fail_growth ? NULL : realloc(buffer, size);
}
/** 成功对照核对完整问答，失败场景通过调用计数确认没有落库。 */
static cc_result_t cc_memory_store_set(cc_memory_store_t *store, const char *key,
    const char *value, const char *category, const char *session_id) {
    assert(store && strncmp(key, "active.", 7) == 0);
    assert(strcmp(value, "User: question\nAssistant: answer") == 0);
    assert(strcmp(category, "active_summary") == 0);
    assert(strcmp(session_id, "session") == 0);
    store_calls++;
    return cc_result_ok();
}
#define realloc runtime_test_realloc
'''
        builder_path = "third_party/cclaw-sdk/cclaw/core/src/util/cc_string_builder.c"
        builder = "".join(function(builder_path, signature) for signature in (
            "cc_result_t cc_string_builder_init(",
            "static cc_result_t ensure_capacity(",
            "cc_result_t cc_string_builder_append(",
            "char *cc_string_builder_take(",
            "void cc_string_builder_deinit("))
        runtime_path = "third_party/cclaw-sdk/cclaw/core/src/app/cc_agent_runtime.c"
        code = "\n#undef realloc\n" + function(runtime_path, "static uint64_t runtime_fnv1a64(")
        code += function(runtime_path, "static void active_memory_after_run(")
        tests = r'''
/** 在第二、第三及第四次追加处触发实际扩容失败，再执行成功对照。 */
int main(void) {
    cc_memory_store_t store = {(void *)1};
    cc_agent_runtime_t runtime = {0};
    runtime.memory_store = &store;
    runtime.config.active_memory_enabled = 1;
    runtime.config.active_memory_write_summary = 1;
    char large[301], near_capacity[246];
    memset(large, 'x', sizeof(large) - 1U); large[sizeof(large) - 1U] = '\0';
    memset(near_capacity, 'y', sizeof(near_capacity) - 1U);
    near_capacity[sizeof(near_capacity) - 1U] = '\0';
    const char *inputs[] = {large, near_capacity, "question"};
    const char *answers[] = {"answer", "answer", large};
    fail_growth = 1;
    for (size_t i = 0; i < 3; i++) {
        messages_created = messages_released = grow_attempts = 0;
        active_memory_after_run(&runtime, "session", inputs[i], answers[i]);
        assert(messages_released == messages_created);
        assert(store_calls == 0);
        assert(grow_attempts == 1 && messages_created == 1);
    }
    fail_growth = 0;
    active_memory_after_run(&runtime, "session", "question", "answer");
    assert(store_calls == 1 && messages_released == messages_created);
    return 0;
}
'''
        run_c_test(prefix + builder + code + tests)

    def test_controller_retries_failed_session_creation(self):
        prefix = COMMON + r'''
#define CC_ERR_INVALID_STATE 10
#define CC_ERR_QUEUE_FULL 11
#define CC_ERR_CANCELLED 12
#define CC_RUNTIME_CONTROLLER_RUNNING 1
typedef int cc_runtime_builder_t;
typedef int cc_runtime_generation_t;
typedef int cc_agent_runtime_t;
typedef int cc_cancel_source_t;
typedef int cc_cancel_token_t;
typedef int cc_mutex_t;
typedef int cc_cond_t;
typedef int cc_runtime_controller_lifecycle_t;
typedef uint64_t cc_runtime_controller_run_id_t;
typedef void (*cc_runtime_controller_stream_fn)(void);
typedef struct cc_runtime_controller cc_runtime_controller_t;
typedef struct {size_t size;cc_cancel_token_t *cancel_token;void (*on_chunk)(void);void *user_data;} cc_agent_runtime_stream_options_t;
typedef cc_agent_runtime_stream_options_t cc_agent_runtime_run_options_t;
typedef struct { int unused; } cc_tool_call_t;
typedef struct { int unused; } cc_tool_t;
typedef struct { int unused; } cc_tool_result_t;
typedef struct { const char *session_id; } cc_run_checkpoint_t;
static int generation,token,runtime,create_attempts,fail_create=1,runs;
static cc_result_t cc_runtime_builder_acquire_generation(void *b,int **g) {(void)b;*g=&generation;return cc_result_ok();}
static void cc_runtime_generation_release(int *g) {(void)g;}
static unsigned long cc_runtime_generation_id(int *g) {(void)g;return 1;}
static cc_result_t cc_cancel_source_create(int **s) {*s=&token;return cc_result_ok();}
static void cc_cancel_source_destroy(int *s) {(void)s;}
static int *cc_cancel_source_token(int *s) {return s;}
static int cc_cancel_token_is_cancelled(int *t) {(void)t;return 0;}
static cc_result_t cc_id_generate_u64(uint64_t *id) {static uint64_t n=0;*id=++n;return cc_result_ok();}
static void cc_mutex_lock(int m) {(void)m;}
static void cc_mutex_unlock(int m) {(void)m;}
static void cc_cond_broadcast(int c) {(void)c;}
static int *cc_runtime_generation_runtime(int *g) {(void)g;return &runtime;}
static int cc_agent_runtime_supports_stream(int *r) {(void)r;return 0;}
static void controller_stream_cb(void) {}
static cc_result_t cc_agent_runtime_handle_message_stream_cb(int *r,const char *s,const char *i,void *o,char **out) {
 (void)r;(void)s;(void)i;(void)o;(void)out;assert(0);return cc_result_ok();
}
static cc_result_t cc_agent_runtime_handle_message_with_options(int *r,const char *s,const char *i,void *o,char **out) {
 (void)r;(void)s;(void)i;(void)o;(void)out;++runs;return cc_result_ok();
}
static cc_result_t cc_agent_runtime_load_interrupted_run(int *r,const char *id,cc_run_checkpoint_t *c) {
 (void)r;(void)id;(void)c;assert(0);return cc_result_ok();
}
static void cc_run_checkpoint_cleanup(cc_run_checkpoint_t *c) {(void)c;}
static cc_result_t cc_agent_runtime_execute_manual_tool_with_options(int *r,const char *s,
 const cc_tool_call_t *call,const cc_tool_t *tool,void *options,cc_tool_result_t *out) {
 (void)r;(void)s;(void)call;(void)tool;(void)options;(void)out;assert(0);return cc_result_ok();
}
static cc_result_t cc_agent_runtime_resume_run(int *r,const char *id,void *options,char **out) {
 (void)r;(void)id;(void)options;(void)out;assert(0);return cc_result_ok();
}
static cc_result_t cc_agent_runtime_handle_message_with_run_id(int *r,const char *s,const char *input,
 const char *id,void *options,char **out) {
 (void)r;(void)s;(void)input;(void)id;(void)options;(void)out;assert(0);return cc_result_ok();
}
'''
        path = "third_party/cclaw-sdk/cclaw/core/src/app/cc_runtime_controller.c"
        text = source(path)
        types = text[text.index("struct cc_runtime_controller {"):text.index("static const char *session_base_or_default")]
        stubs = r'''
static void update_session_id_locked(cc_runtime_controller_t *c) {(void)c;}
static cc_result_t create_session_on_generation(cc_runtime_controller_t *c,int *g,const char *s) {
 (void)c;(void)g;(void)s;++create_attempts;
 return fail_create?cc_result_error(CC_ERR_IO,"injected"):cc_result_ok();
}
int main(void);
'''
        test = r'''
int main(void) {
 cc_runtime_controller_t c={0};c.lifecycle=CC_RUNTIME_CONTROLLER_RUNNING;c.session_needs_create=1;
 char *reply=NULL;
 assert(cc_runtime_controller_submit_text(&c,"test",NULL,NULL,&reply).code==CC_ERR_IO);
 assert(create_attempts==1&&c.session_needs_create&&!c.active_run_id&&!runs);
 fail_create=0;
 assert(cc_runtime_controller_submit_text(&c,"test",NULL,NULL,&reply).code==CC_OK);
 assert(create_attempts==2&&!c.session_needs_create&&runs==1);
 assert(cc_runtime_controller_submit_text(&c,"test",NULL,NULL,&reply).code==CC_OK);
 assert(create_attempts==2&&runs==2);puts("Controller failed session creation retry: PASS");return 0;
}
'''
        code = function(path, "static cc_result_t controller_execute(")
        code += function(path, "cc_result_t cc_runtime_controller_submit_text(")
        run_c_test(prefix + types + stubs + code + test)

    def test_ota_health_requires_current_recovery_and_critical_services(self):
        prefix = COMMON + r'''
typedef struct {bool rollback_pending;} ota_manager_status_t;
typedef struct {bool started;} interaction_service_status_t;
static struct {struct {bool loaded;} config;void *runtime_builder;} s_app;
static int confirmations, rejections, recovery_ready=1;
static void *s_recovery=(void *)1;
static int recovery_service_is_current(void *recovery) {(void)recovery;return recovery_ready;}
static int app_reject_pending_ota(const char *reason) {(void)reason;rejections++;return ESP_OK;}
static void ota_manager_refresh_status(void) {}
static void ota_manager_get_status(ota_manager_status_t *s) {s->rollback_pending=true;}
static void interaction_service_get_status(interaction_service_status_t *s) {s->started=true;}
static int usb_storage_service_is_mounted(void) {return 1;}
static int ota_service_confirm_running_app(void) {++confirmations;return ESP_OK;}
#define APP_LOGW(...) ((void)0)
#define APP_LOGI(...) ((void)0)
'''
        test = r'''
int main(void) {
 s_app.config.loaded=true;s_app.runtime_builder=(void *)1;
 app_mark_ota_valid_after_health_check(true,false);assert(confirmations==1);
 app_mark_ota_valid_after_health_check(false,true);assert(confirmations==1 && rejections==1);
 recovery_ready=0;
 app_mark_ota_valid_after_health_check(true,true);assert(confirmations==1 && rejections==2);
 recovery_ready=1;
 app_mark_ota_valid_after_health_check(true,true);assert(confirmations==2);
 puts("OTA critical startup health and optional camera: PASS");return 0;
}
'''
        run_c_test(prefix + function("main/core/app_lifecycle.c", "static void app_mark_ota_valid_after_health_check(") + test)

    def test_event_stop_admission_and_destroy_join(self):
        """核对先登记 pending、停止后拒绝新消息，以及共享预算内的租约退场。"""
        prefix = COMMON + r'''
typedef unsigned TickType_t;
typedef unsigned UBaseType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) do { assert(!*(p)); *(p)=1; } while(0)
#define portEXIT_CRITICAL(p) do { assert(*(p)); *(p)=0; } while(0)
#define pdMS_TO_TICKS(x) (x)
#define configTICK_RATE_HZ 1000U
#define portMAX_DELAY ((TickType_t)-1)
#define pdTRUE 1
#define APP_EVENT_BUS_DRAINING 3
#define APP_EVENT_BUS_STOPPED 4
#define APP_EVENT_BUS_FAILED 5
static TickType_t tick;
static TickType_t xTaskGetTickCount(void) {return tick;}
static void vTaskDelay(TickType_t n) {tick+=n;}
static portMUX_TYPE s_access_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_access_open;
static unsigned s_access_users;
typedef int app_event_kind_t;
typedef struct {unsigned refs;} app_event_envelope_t;
typedef struct {int kind; app_event_envelope_t *envelope; bool clear,boot;} app_event_display_msg_t;
static struct {int lifecycle,display_queue; bool maintenance_mode;
 unsigned display_pending,max_display_queue_waiting,dropped,sink_dropped,display_queue_dropped;} g_app_event_bus;
static int bus_locked,sends,send_ok=1;
static void app_event_bus_lock(void) {assert(!bus_locked);bus_locked=1;}
static void app_event_bus_unlock(void) {assert(bus_locked);bus_locked=0;}
static void app_event_envelope_retain(app_event_envelope_t *e) {++e->refs;}
static void app_event_envelope_release(app_event_envelope_t *e) {assert(e->refs);--e->refs;}
static int is_stream_event(int kind) {(void)kind;return 1;}
static bool app_event_display_available(void) {return true;}
static unsigned uxQueueMessagesWaiting(int q) {assert(q==7);return 0;}
static void app_event_bus_mark_sink_busy(void) {}
static void app_event_bus_notify_idle_if_drained(void) {}
static int xQueueSend(int q, const app_event_display_msg_t *m, TickType_t t) {
 (void)m;(void)t;assert(q==7);assert(g_app_event_bus.display_pending==1);
 /* 停止发生在接纳之后、入队结束之前，必须观察到已登记工作。 */
 g_app_event_bus.lifecycle=APP_EVENT_BUS_DRAINING;++sends;return send_ok;
}
'''
        bus = "main/services/events/app_event_bus.c"
        sink = "main/services/events/app_event_display_sink.c"
        code = function("main/services/events/app_event_bus_internal.h",
                        "static inline void app_event_counter_increment(")
        code += "".join(function(bus, sig) for sig in (
            "TickType_t app_event_timeout_ticks(",
            "TickType_t app_event_remaining_ticks(",
            "bool app_event_bus_acquire(void)", "void app_event_bus_release(void)",
            "static bool app_event_bus_close_access("))
        code += "".join(function(sink, sig) for sig in (
            "static void display_complete_one(void)", "static void display_note_drop(void)",
            "static void app_event_display_emit_active(", "void app_event_display_emit("))
        test = r'''
int main(void) {
 s_access_open=true;g_app_event_bus.display_queue=7;
 app_event_envelope_t e={1}; app_event_display_emit(0,&e);
 assert(sends==1&&g_app_event_bus.display_pending==1&&e.refs==2&&!s_access_users);
 app_event_display_emit(0,&e); assert(sends==1&&e.refs==2);
 display_complete_one();app_event_envelope_release(&e);
 g_app_event_bus.lifecycle=0;send_ok=0;app_event_display_emit(0,&e);
 assert(sends==2&&!g_app_event_bus.display_pending&&e.refs==1);
 assert(app_event_bus_acquire());
 tick=101;assert(!app_event_bus_close_access(100,2)&&tick==102);
 assert(!app_event_bus_acquire()&&s_access_users==1);
 app_event_bus_release();assert(app_event_bus_close_access(100,2));
 g_app_event_bus.display_queue=0;app_event_display_emit(0,&e);assert(sends==2);
 puts("Event admission before pending, drain rejection, queue failure and destroy join: PASS");return 0;
}
'''
        run_c_test(prefix + code + test)

    def test_http_trickle_deadline_and_next_read_budget(self):
        path = "third_party/cclaw-sdk/cclaw/platforms/esp32/src/cc_esp32_http_client.c"
        prefix = COMMON + r'''
typedef struct { int client; } esp_http_client_event_t;
typedef struct { int timeout_ms; } pool_t;
typedef struct { int total_timeout_ms; int64_t start_us; cc_result_t callback_error; pool_t *pool; } cc_esp32_http_ctx_t;
static int64_t now;
static int socket_timeout, aborted;
static int64_t http_now_us(void) { return now; }
static int esp_http_client_set_timeout_ms(int client, int ms) { (void)client; socket_timeout=ms; return 0; }
static esp_err_t http_abort_from_event(esp_http_client_event_t *e, cc_esp32_http_ctx_t *c, const char *reason) {
    (void)e; (void)c; assert(strcmp(reason,"total_timeout")==0); ++aborted; return ESP_FAIL;
}
'''
        test = r'''
int main(void) {
    pool_t pool={0}; esp_http_client_event_t e={1};
    cc_esp32_http_ctx_t c={1000,1000,{0,NULL},&pool};
    now=1000; assert(http_apply_remaining_timeout(&e,&c,90000)==0 && socket_timeout==1000);
    now=901000; assert(http_apply_remaining_timeout(&e,&c,90000)==0 && socket_timeout==100);
    now=951000; assert(http_apply_remaining_timeout(&e,&c,20)==0 && socket_timeout==20);
    now=1001000; assert(http_apply_remaining_timeout(&e,&c,90000)==ESP_FAIL);
    assert(aborted==1 && c.callback_error.code==CC_ERR_TIMEOUT);
    puts("HTTP trickle deadline and remaining socket timeout: PASS"); return 0;
}
'''
        run_c_test(prefix + function(path, "static esp_err_t http_apply_remaining_timeout(") + test)

    def test_fat_replace_cleanup_failure_keeps_committed_state(self):
        prefix = COMMON + r'''
#include <errno.h>
struct stat { int unused; };
static int renames, remove_fail, install_fail, installed;
static cc_result_t esp32_validate_path(const char *p) { (void)p; return cc_result_ok(); }
static int fake_stat(const char *p, struct stat *s) { (void)p; (void)s; errno=ENOENT; return -1; }
static int fake_rename(const char *a, const char *b) {
    (void)a; (void)b; ++renames;
    if(renames==1) { errno=EEXIST; return -1; }
    if(renames==3 && install_fail) { errno=EIO; return -1; }
    if(renames==3) installed=1;
    return 0;
}
static int fake_remove(const char *p) { (void)p; if(remove_fail) {errno=EIO; return -1;} return 0; }
#define stat fake_stat
#define rename fake_rename
#define remove fake_remove
'''
        # Keep the struct tag intact when replacing the function name.
        body = function("third_party/cclaw-sdk/cclaw/platforms/esp32/src/cc_esp32_filesystem.c", "static cc_result_t esp32_atomic_replace(")
        prefix = prefix.replace("#define stat fake_stat", "#define stat(...) fake_stat(__VA_ARGS__)")
        test = r'''
int main(void) {
    remove_fail=1; assert(esp32_atomic_replace(NULL,"new.tmp","config").code==0); assert(installed);
    renames=installed=0; remove_fail=0; install_fail=1;
    assert(esp32_atomic_replace(NULL,"new.tmp","config").code!=0); assert(!installed && renames==4);
    puts("FAT replacement commit vs cleanup/installation failure: PASS"); return 0;
}
'''
        run_c_test(prefix + body + test)

    def test_config_installed_but_unsynced_blocks_followup_writes(self):
        prefix = COMMON + r'''
#define APP_CONFIG_STAGE_MAX_FILE_BYTES (1024U * 1024U)
typedef struct { bool loaded; const char *raw_json; bool updates_blocked; } app_config_t;
static bool replace_ok, sync_ok;
static bool write_text_file_atomic(const char *p,const char *v,bool *installed,bool *uncertain) {
    (void)p; (void)v; *installed=replace_ok; *uncertain=replace_ok && !sync_ok;
    return replace_ok && sync_ok;
}
static const char *app_config_json(app_config_t *c) { return c->raw_json; }
static void app_config_destroy(app_config_t *c) { memset(c,0,sizeof(*c)); }
'''
        test = r'''
int main(void) {
    app_config_t current={true,"old"}, next={true,"new"};
    assert(app_config_commit_prepared(&current,&current,"config")==ESP_ERR_INVALID_ARG);
    assert(current.loaded && strcmp(current.raw_json,"old")==0);
    assert(app_config_commit_prepared(&current,&next,"config")==ESP_FAIL);
    assert(strcmp(current.raw_json,"old")==0 && next.loaded);
    replace_ok=true;
    assert(app_config_commit_prepared(&current,&next,"config")==ESP_ERR_NOT_FINISHED);
    assert(strcmp(current.raw_json,"new")==0 && !next.loaded && current.updates_blocked);
    next.loaded=true; next.raw_json="third"; sync_ok=true;
    assert(app_config_commit_prepared(&current,&next,"config")==ESP_ERR_INVALID_STATE);
    assert(strcmp(current.raw_json,"new")==0 && next.loaded);
    current.updates_blocked=false; /* The fixture simulates a successful owner reload. */
    assert(app_config_commit_prepared(&current,&next,"config")==ESP_OK);
    assert(strcmp(current.raw_json,"third")==0 && !next.loaded);
    puts("Config visible commit adoption and failed replacement: PASS"); return 0;
}
'''
        run_c_test(prefix + function("main/core/app_config.c", "esp_err_t app_config_commit_prepared(") + test)

    def test_config_root_file_syncs_root_directory(self):
        """正式文件位于根目录时实际同步根目录，保留相对路径的当前目录语义。"""
        prefix = "#define _POSIX_C_SOURCE 200809L\n" + COMMON + r'''
#define CC_FS_ENTRY_MISSING 0
typedef struct { size_t size; int type; } cc_filesystem_stat_t;
typedef struct {
    cc_result_t (*write_text)(void *,const char *,const char *);
    cc_result_t (*atomic_replace)(void *,const char *,const char *);
    cc_result_t (*sync_file)(void *,const char *);
    cc_result_t (*stat)(void *,const char *,cc_filesystem_stat_t *);
    cc_result_t (*read_text)(void *,const char *,char **);
    cc_result_t (*sync_dir)(void *,const char *);
    void (*destroy)(void *);
} cc_filesystem_vtable_t;
typedef struct { void *self; const cc_filesystem_vtable_t *vtable; } cc_filesystem_t;
static char synced_parent[64];
static cc_result_t fake_write(void *s,const char *p,const char *v)
{ (void)s;(void)p;(void)v;return cc_result_ok(); }
static cc_result_t fake_sync(void *s,const char *p)
{ (void)s;(void)p;return cc_result_ok(); }
static cc_result_t fake_stat(void *s,const char *p,cc_filesystem_stat_t *st)
{ (void)s;(void)p;st->type=CC_FS_ENTRY_MISSING;return cc_result_ok(); }
static cc_result_t fake_read(void *s,const char *p,char **v)
{ (void)s;(void)p;*v=NULL;return cc_result_ok(); }
static cc_result_t fake_sync_dir(void *s,const char *p)
{ (void)s;snprintf(synced_parent,sizeof(synced_parent),"%s",p);return cc_result_ok(); }
static cc_result_t cc_filesystem_get_default(cc_filesystem_t *out) {
    static const cc_filesystem_vtable_t v={fake_write,fake_write,fake_sync,fake_stat,fake_read,fake_sync_dir,NULL};
    out->vtable=&v;return cc_result_ok();
}
static void cc_secret_zero(void *p,size_t n) { memset(p,0,n); }
'''
        test = r'''
int main(void) {
    bool installed,uncertain;
    assert(write_text_file_atomic("/config.json","{}",&installed,&uncertain));
    assert(installed && !uncertain && strcmp(synced_parent,"/")==0);
    assert(write_text_file_atomic("config.json","{}",&installed,&uncertain));
    assert(strcmp(synced_parent,".")==0);
    assert(write_text_file_atomic("/usb0/config.json","{}",&installed,&uncertain));
    assert(strcmp(synced_parent,"/usb0")==0);
    puts("Config root and relative directory synchronization: PASS");return 0;
}
'''
        run_c_test(prefix + function("main/core/app_config.c", "static bool write_text_file_atomic(") + test)

    def test_config_partial_tool_cache_and_full_paths(self):
        """工具缓存的部分复制可清理，派生路径保留完整根路径与 lua 后缀。"""
        prefix = COMMON + r'''
typedef struct { char **items; size_t count; } cc_config_string_list_t;
typedef struct {
    char **enabled_tools; size_t enabled_tools_count;
    struct { cc_config_string_list_t enabled; } tools;
} cc_config_t;
static int copies;
static char *fail_second_copy(const char *text) {
    if (++copies==2) return NULL;
    size_t length=strlen(text)+1; char *copy=malloc(length);
    if(copy) memcpy(copy,text,length); return copy;
}
#define strdup fail_second_copy
'''
        bodies = function("main/core/app_config.c", "static bool sync_enabled_tools_cache(")
        bodies += function("main/core/app_config.c", "static const char *nonempty_or(")
        bodies += function("main/core/app_config.c", "static bool replace_joined_path(")
        test = r'''
int main(void) {
    char *names[]={"one","two","three"};
    cc_config_t config={0}; config.tools.enabled.items=names; config.tools.enabled.count=3;
    assert(!sync_enabled_tools_cache(&config));
    assert(config.enabled_tools_count==1 && strcmp(config.enabled_tools[0],"one")==0);
    for(size_t i=0;i<config.enabled_tools_count;i++) free(config.enabled_tools[i]);
    free(config.enabled_tools);
    char root[321]; root[0]='/'; memset(root+1,'r',319); root[320]='\0';
    char *path=NULL;
    assert(replace_joined_path(&path,root,"lua"));
    assert(strlen(path)==324 && memcmp(path,root,320)==0 && strcmp(path+320,"/lua")==0);
    assert(replace_joined_path(&path,"/short/","sessions"));
    assert(strcmp(path,"/short/sessions")==0); free(path);
    puts("Config partial cache cleanup and complete derived paths: PASS"); return 0;
}
'''
        run_c_test(prefix + bodies + test)

    def test_config_size_conversion_rejects_nonfinite_and_overflow(self):
        """size_t 转换在缺失、非数字、非有限及上界输入下保留正确输出。"""
        prefix = COMMON + r'''
#include <math.h>
#include <limits.h>
typedef struct { bool number; double value; } cc_json_value_t;
static cc_json_value_t *value_at_alias(cc_json_value_t *o,const char *s,const char *c)
{ (void)s; (void)c; return o; }
static bool cc_json_is_number(cc_json_value_t *v) { return v && v->number; }
static double cc_json_number_value(cc_json_value_t *v) { return v ? v->value : 0; }
'''
        test = r'''
int main(void) {
    size_t output=17; cc_json_value_t value={true,3.75};
    assert(size_at_alias(NULL,"s",NULL,&output).code==CC_OK && output==17);
    assert(size_at_alias(&value,"s",NULL,&output).code==CC_OK && output==3);
    double bad[]={-1,NAN,INFINITY,-INFINITY,ldexp(1.0,sizeof(size_t)*CHAR_BIT)};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        output=17; value.value=bad[i];
        assert(size_at_alias(&value,"s",NULL,&output).code==CC_ERR_INVALID_ARGUMENT && output==17);
    }
    output=17; value.number=false; value.value=4;
    assert(size_at_alias(&value,"s",NULL,&output).code==CC_ERR_INVALID_ARGUMENT && output==17);
    value.number=true; value.value=0;
    assert(size_at_alias(&value,"s",NULL,&output).code==CC_OK && output==0);
    puts("Config size_t finite range and retained output: PASS"); return 0;
}
'''
        run_c_test(prefix + function("main/core/app_config.c", "static cc_result_t size_at_alias(") + test)

    def test_nvs_remove_never_commits_invalid_handle(self):
        prefix = COMMON + r'''
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define ESP_ERR_NVS_NOT_FOUND 20
static int open_error, erase_error, commits, closes;
static cc_result_t secret_key(const char *r,const char **key) { *key=r; return cc_result_ok(); }
static int nvs_open(const char *n,int m,int *h) {(void)n;(void)m;if(!open_error)*h=7;return open_error;}
static int nvs_erase_key(int h,const char *k) {(void)k;assert(h==7);return erase_error;}
static int nvs_commit(int h) {assert(h==7);++commits;return 0;}
static void nvs_close(int h) {assert(h==7);++closes;}
'''
        test = r'''
int main(void) {
    open_error=ESP_ERR_NVS_NOT_FOUND; assert(nvs_secret_remove(NULL,"x").code==0); assert(!commits&&!closes);
    open_error=99; assert(nvs_secret_remove(NULL,"x").code!=0); assert(!commits&&!closes);
    open_error=0; erase_error=ESP_ERR_NVS_NOT_FOUND; assert(nvs_secret_remove(NULL,"x").code==0);
    assert(commits==1&&closes==1); puts("NVS missing namespace/key and invalid handle: PASS");return 0;
}
'''
        run_c_test(prefix + function("third_party/cclaw-sdk/cclaw/platforms/esp32/src/cc_esp32_secret_store.c", "static cc_result_t nvs_secret_remove(") + test)

    def test_every_visible_ui_field_is_registered(self):
        text = source("main/ui/board_ui.c")
        capacity = int(re.search(r"#define UI_CONFIG_FIELD_MAX (\d+)", text)[1])
        editor = function("main/ui/board_ui.c", "void board_ui_show_config_editor(void)")
        self.assertGreaterEqual(capacity, len(re.findall(r"\badd_field\(", editor)))


if __name__ == "__main__":
    unittest.main()
