"""编译实际总线函数，确定性复现 USB 退出尾部与初始化失败交错。

此夹具只模拟 pthread 上的 FreeRTOS 调度和 FILE 接口边界，不替代真实内核、
USB 文件系统与设备断电验证。生产函数从当前文件逐段原样提取。
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    """返回指定生产定义的原文，定义以独占行右花括号结束。"""
    text = (ROOT / path).read_text(encoding="utf-8")
    start = text.index(signature)
    return text[start:text.index("\n}\n", start) + 3]


def run_fixture(code, name):
    """在独立临时目录编译并运行拥有本机 pthread 的模型。"""
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if os.name == "nt" or not compiler:
        raise RuntimeError("此 pthread 夹具需在 Linux/WSL 的本机 C 编译器运行")
    with tempfile.TemporaryDirectory(prefix="cclaw-event-lifecycle-") as folder:
        path = Path(folder)
        (path / "fixture.c").write_text(code, encoding="utf-8")
        command = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                   "-pthread", "-o", str(path / name), str(path / "fixture.c")]
        if os.environ.get("CCLAW_TEST_SANITIZERS") == "1":
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(path / name)], check=True, timeout=20)


USB_PREFIX = r'''
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t TickType_t;
typedef unsigned EventBits_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *EventGroupHandle_t;
typedef void *SemaphoreHandle_t;
typedef int esp_err_t;
typedef int app_event_kind_t;
typedef int cc_log_level_t;
typedef struct { unsigned refs; size_t len; char text[64]; } app_event_envelope_t;
typedef struct {
    app_event_envelope_t *envelope;
    uint32_t seq;
    app_event_kind_t kind;
    cc_log_level_t level;
    bool shutdown;
} app_event_usb_log_msg_t;
static struct {
    void *usb_queue;
    SemaphoreHandle_t file_mutex;
    SemaphoreHandle_t mutex;
    EventGroupHandle_t idle_events;
    TaskHandle_t usb_task;
    bool usb_task_completed, usb_stop_requested, usb_log_started, usb_log_enabled;
    bool rename_pending, display_enabled;
    FILE *usb_file;
    char usb_current_path[192];
    uint32_t usb_queue_dropped, sink_dropped, usb_pending;
    size_t usb_current_bytes, usb_bytes_written;
    int lifecycle;
} g_app_event_bus;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define APP_EVENT_TEXT_MAX 2048U
#define APP_EVENT_USB_TASK_STACK 6144U
#define APP_EVENT_USB_TASK_PRIORITY 4
#define APP_EVENT_USB_TASK_STACK_IN_PSRAM 0
#define APP_EVENT_USB_TASK_EXITED_BIT 2U
#define APP_EVENT_BUS_IDLE_BIT 1U
#define APP_EVENT_BUS_USB_LOG_READY 3
#define APP_EVENT_BUS_CONFIGURED 2
#define APP_EVENT_BUS_DRAINING 5
#define APP_EVENT_BUS_STOPPED 6
#define APP_EVENT_BUS_FAILED 7
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 1
#define ESP_ERR_INVALID_STATE 2
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tail_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tail_cond = PTHREAD_COND_INITIALIZER;
static _Thread_local TaskHandle_t caller;
static atomic_uint tick, event_bits;
static bool pause_tail, tail_entered, tail_released, fail_create;
static pthread_t writer;
static void (*task_entry)(void *);
static app_event_usb_log_msg_t messages[3];
static size_t next_message, message_count;
static unsigned released, deletes, starts;
/** @brief 模拟总线状态锁。 @post 当前线程取得互斥，须配对 unlock。 */
static void app_event_bus_lock(void) { assert(!pthread_mutex_lock(&state_mutex)); }
/** @brief 归还模拟状态锁。 @post 当前线程解除互斥。 */
static void app_event_bus_unlock(void) { assert(!pthread_mutex_unlock(&state_mutex)); }
/** @brief 模拟任务身份快照。 @return 当前线程的可复用句柄值。 */
static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return caller; }
/** @brief 读取确定性虚拟 tick。 @return 原子时钟值。 */
static TickType_t xTaskGetTickCount(void) { return atomic_load(&tick); }
/** @brief 推进虚拟预算并让出 pthread。 @param[in] ticks 有限 tick 数。 @post 时钟推进，其他线程可运行。 */
static void vTaskDelay(TickType_t ticks) { atomic_fetch_add(&tick, ticks); sched_yield(); }
/** @brief 模拟文件 mutex 获取。 @param[in] handle 必需非空句柄。 @param[in] wait 预算，仅测试无竞争获取。 @return pdTRUE。 */
static BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t wait)
{ (void)wait; assert(handle); assert(!pthread_mutex_lock(&file_mutex)); return pdTRUE; }
/** @brief 归还文件 mutex。 @param[in] handle 必需句柄。 @return pdTRUE。 */
static BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{ assert(handle); assert(!pthread_mutex_unlock(&file_mutex)); return pdTRUE; }
/** @brief 发布事件位并在 EXITED 原生尾部按需暂停。 @param[in] handle 事件组借用。 @param[in] bits 待置位集合。 @return 当前位集合，返回前对象仍可被本函数使用。 */
static EventBits_t xEventGroupSetBits(EventGroupHandle_t handle, EventBits_t bits)
{
    assert(handle);
    atomic_fetch_or(&event_bits, bits);
    if (bits & APP_EVENT_USB_TASK_EXITED_BIT) {
        assert(g_app_event_bus.usb_task == (void *)42);
        assert(!pthread_mutex_lock(&tail_mutex));
        tail_entered = true;
        assert(!pthread_cond_broadcast(&tail_cond));
        while (pause_tail && !tail_released)
            assert(!pthread_cond_wait(&tail_cond, &tail_mutex));
        assert(g_app_event_bus.idle_events == handle);
        assert(!pthread_mutex_unlock(&tail_mutex));
    }
    return atomic_load(&event_bits);
}
/** @brief 清除模拟事件位。 @param[in] handle 事件组借用。 @param[in] bits 清除位集合。 @return 清除前集合。 */
static EventBits_t xEventGroupClearBits(EventGroupHandle_t handle, EventBits_t bits)
{ assert(handle); return atomic_fetch_and(&event_bits, ~bits); }
/** @brief 读取已经发布的模拟事件位。 @param[in] handle 事件组借用。 @param[in] bits 目标位。 @param[in] clear 本夹具为 false。 @param[in] all 本夹具为 true。 @param[in] wait 当前预算。 @return 当前事件集合，未到位时模拟预算消耗。 */
static EventBits_t xEventGroupWaitBits(EventGroupHandle_t handle, EventBits_t bits,
                                      BaseType_t clear, BaseType_t all, TickType_t wait)
{
    (void)clear; (void)all; assert(handle);
    unsigned result = atomic_load(&event_bits);
    if (!(result & bits)) atomic_fetch_add(&tick, wait);
    return result;
}
/** @brief 消费启动前准备的队列消息。 @param[in] queue 固定队列身份。 @param[out] out 成功时复制消息。 @param[in] wait 等待 tick，未使用。 @return 有消息为 pdTRUE，空为 pdFALSE。 */
static BaseType_t xQueueReceive(void *queue, void *out, TickType_t wait)
{
    (void)wait; assert(queue);
    if (next_message == message_count) return pdFALSE;
    memcpy(out, &messages[next_message++], sizeof(messages[0])); return pdTRUE;
}
/** @brief 模拟停止哨兵成功发送。 @param[in] queue 队列借用。 @param[in] msg 消息借用。 @param[in] wait 有限预算。 @return pdTRUE，本夹具停止消息已预置。 */
static BaseType_t xQueueSend(void *queue, const void *msg, TickType_t wait)
{ (void)msg; (void)wait; assert(queue); return pdTRUE; }
/** @brief 模拟 profile 注册。 @param[in] name 调用期名称。 @param[in] task 当前句柄。 @post 校验 writer 身份。 */
static void app_profile_register_task(const char *name, TaskHandle_t task)
{ assert(name && task == (void *)42); }
/** @brief 模拟 profile 注销。 @param[in] task 当前句柄。 @post 校验 writer 身份。 */
static void app_profile_unregister_task(TaskHandle_t task) { assert(task == (void *)42); }
/** @brief 记录原生自删。 @param[in] task NULL 表示当前任务。 @post 本模型允许函数随后返回以便 pthread_join。 */
static void vTaskDelete(TaskHandle_t task) { assert(!task); deletes++; }
/** @brief 提供测试级别名称。 @param[in] level 未使用。 @return 静态借用。 */
static const char *app_event_log_level_string(cc_log_level_t level) { (void)level; return "info"; }
/** @brief 提供测试种类名称。 @param[in] kind 未使用。 @return 静态借用。 */
static const char *app_event_kind_string(app_event_kind_t kind) { (void)kind; return "status"; }
/** @brief 模拟已安装的日志路径重开。 @param[in] path 调用期路径。 @return 当前真实 FILE 可用性。 */
static bool open_usb_log_file(const char *path) { assert(path); return g_app_event_bus.usb_file != NULL; }
/** @brief 隔离命名操作。 @return true，模型专注生命周期和已接纳写入。 */
static bool rename_to_time_if_ready(void) { return true; }
/** @brief 隔离换卷。 @param[in] length 下一行字节。 @return true。 */
static bool rotate_if_needed(size_t length) { assert(length); return true; }
/** @brief 隔离目录清理。 @post 无操作。 */
static void cleanup_old_logs(void) {}
/** @brief 归还模型正文引用。 @param[in,out] envelope 必需拥有对象。 @post 引用减一并记录归还。 */
static void app_event_envelope_release(app_event_envelope_t *envelope)
{ assert(envelope && envelope->refs); envelope->refs--; released++; }
/** @brief 隔离 idle 通知。 @post 无操作。 */
static void app_event_bus_notify_idle_if_drained(void) {}
/** @brief 模拟前置 drain 消耗同一预算的两 tick。 @param[in] start 原起点。 @param[in] budget 原预算。 @return true，给停止下一阶段核对剩余额度。 */
static bool app_event_bus_wait_idle_until(TickType_t start, TickType_t budget)
{ assert(start == 0 && budget == 3); atomic_fetch_add(&tick, 2); return true; }
/** @brief 为 start 创建真实临时 FILE。 @return 成功为 true。 @post FILE 归生产 stop 消费。 */
static bool start_usb_file_output(void)
{ starts++; g_app_event_bus.usb_file = tmpfile(); return g_app_event_bus.usb_file != NULL; }
/** @brief 独立 pthread 入口。 @param[in] arg 未使用。 @return NULL，任务函数已返回。 */
static void *run_writer(void *arg) { (void)arg; caller = (void *)42; task_entry(NULL); return NULL; }
/** @brief 模拟真实异步任务创建。 @param[in] entry 用户入口借用。 @param[in] name 名称借用。 @param[in] stack 栈预算。 @param[in] arg 参数，此夹具 NULL。 @param[in] priority 优先级。 @param[out] out 成功发布任务句柄。 @return pdPASS 或失败零。 */
static BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned stack,
                             void *arg, unsigned priority, TaskHandle_t *out)
{
    (void)stack; (void)arg; (void)priority; assert(name && out);
    if (fail_create) return 0;
    task_entry = entry; assert(!pthread_create(&writer, NULL, run_writer, NULL));
    *out = (void *)42; return pdPASS;
}
/** @brief 声明后续原样提取的生产停止包装。 @param[in] timeout_ms 有限毫秒预算，零即时尝试。 @return 生产停止结果。 */
esp_err_t app_event_usb_stop(uint32_t timeout_ms);
'''

USB_TESTS = r'''
/** @brief 准备唯一 owner 的停止模型。 @param[in] pause 是否暂停 SetBits 尾部。 @post 所有旧 worker 已 join，新配置和队列在创建前完整发布。 */
static void reset(bool pause)
{
    memset(&g_app_event_bus, 0, sizeof(g_app_event_bus));
    g_app_event_bus.usb_queue = (void *)1;
    g_app_event_bus.file_mutex = (void *)2;
    g_app_event_bus.mutex = (void *)2;
    g_app_event_bus.idle_events = (void *)3;
    g_app_event_bus.lifecycle = APP_EVENT_BUS_CONFIGURED;
    g_app_event_bus.usb_stop_requested = true;
    strcpy(g_app_event_bus.usb_current_path, "model.log");
    pause_tail = pause; tail_entered = false; tail_released = false;
    next_message = 0; message_count = 1; messages[0] = (app_event_usb_log_msg_t){.shutdown = true};
    atomic_store(&tick, 0); atomic_store(&event_bits, 0);
    caller = (void *)43; fail_create = false;
}
/** @brief 验证 SetBits 唤醒尾部仍保留所有资源且可有限重试。 @post worker 最终发布后复用句柄地址不会被误判自停，FILE 仅最终停止消费。 */
static void test_exit_tail(void)
{
    reset(true);
    assert(app_event_usb_start() == ESP_OK);
    assert(!pthread_mutex_lock(&tail_mutex));
    while (!tail_entered) assert(!pthread_cond_wait(&tail_cond, &tail_mutex));
    assert(!pthread_mutex_unlock(&tail_mutex));
    assert(!__atomic_load_n(&g_app_event_bus.usb_task_completed, __ATOMIC_ACQUIRE));
    FILE *owned_file = g_app_event_bus.usb_file;
    assert(app_event_usb_stop(3) == ESP_ERR_TIMEOUT);
    assert(g_app_event_bus.usb_task == (void *)42 && g_app_event_bus.usb_file == owned_file &&
           g_app_event_bus.idle_events == (void *)3);
    assert(!pthread_mutex_lock(&tail_mutex)); tail_released = true;
    assert(!pthread_cond_broadcast(&tail_cond)); assert(!pthread_mutex_unlock(&tail_mutex));
    assert(!pthread_join(writer, NULL));
    assert(__atomic_load_n(&g_app_event_bus.usb_task_completed, __ATOMIC_ACQUIRE));
    caller = (void *)42;
    assert(app_event_usb_stop(3) == ESP_OK);
    assert(!g_app_event_bus.usb_task && !g_app_event_bus.usb_file);
}
/** @brief 验证配置关闭后仍处理已接纳正文且字节统计饱和。 @post 正文引用和 pending 归还，随后停止哨兵完成退场。 */
static void test_accepted_after_disable(void)
{
    reset(false);
    app_event_envelope_t envelope = {.refs = 1, .len = 8, .text = "accepted"};
    messages[0] = (app_event_usb_log_msg_t){.envelope = &envelope, .seq = 7};
    messages[1] = (app_event_usb_log_msg_t){.shutdown = true}; message_count = 2;
    g_app_event_bus.usb_pending = 1;
    g_app_event_bus.usb_current_bytes = SIZE_MAX - 1U;
    g_app_event_bus.usb_bytes_written = SIZE_MAX - 1U;
    assert(!g_app_event_bus.usb_log_enabled);
    assert(app_event_usb_start() == ESP_OK);
    assert(!pthread_join(writer, NULL));
    assert(g_app_event_bus.usb_pending == 0 && envelope.refs == 0 && released == 1);
    assert(g_app_event_bus.usb_current_bytes == SIZE_MAX && g_app_event_bus.usb_bytes_written == SIZE_MAX);
    assert(!fseek(g_app_event_bus.usb_file, 0, SEEK_SET));
    char text[128]; size_t n = fread(text, 1, sizeof(text) - 1U, g_app_event_bus.usb_file);
    text[n] = '\0'; assert(strstr(text, "accepted"));
    assert(app_event_usb_stop(3) == ESP_OK);
}
/** @brief 验证 writer 自停不会关闭自身接纳，创建失败不发布句柄。 @post 失败 FILE 消费，状态可重试。 */
static void test_create_and_self_stop(void)
{
    reset(false); g_app_event_bus.usb_task = caller;
    g_app_event_bus.usb_log_started = true;
    assert(app_event_usb_stop(3) == ESP_ERR_INVALID_STATE && g_app_event_bus.usb_log_started);
    g_app_event_bus.usb_task = NULL; fail_create = true;
    assert(app_event_usb_start() == ESP_FAIL);
    assert(!g_app_event_bus.usb_task && !g_app_event_bus.usb_file);
}
/** @brief 验证 drain 和 writer 等待共用总预算。 @post 三 tick 到期即返回，未重新发放三 tick，失败句柄保留。 */
static void test_shared_stop_budget(void)
{
    reset(false); g_app_event_bus.usb_task = (void *)42;
    assert(app_event_bus_stop_active(0, 3) == ESP_ERR_TIMEOUT);
    assert(atomic_load(&tick) == 3 && g_app_event_bus.usb_task == (void *)42 &&
           g_app_event_bus.lifecycle == APP_EVENT_BUS_FAILED);
}
/** @brief 运行实际生产 writer/start/stop 模型。 @return 所有断言通过为零。 */
int main(void)
{
    test_exit_tail(); test_accepted_after_disable(); test_create_and_self_stop();
    test_shared_stop_budget();
    assert(deletes == 2 && starts == 3);
    puts("app event USB lifecycle: PASS"); return 0;
}
'''

SCALAR_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t TickType_t;
typedef void *SemaphoreHandle_t;
typedef void *EventGroupHandle_t;
#define configTICK_RATE_HZ 2000U
#define portMAX_DELAY UINT32_MAX
#define APP_EVENT_BUS_IDLE_BIT 1U
static struct { SemaphoreHandle_t mutex; EventGroupHandle_t idle_events; } g_app_event_bus;
static SemaphoreHandle_t s_publish_order_mutex;
static unsigned attempt, fail_at, active;
static TickType_t now;
static int s_access_mux;
static bool s_access_open;
static unsigned s_access_users;
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
/** @brief 模拟单次原语分配。 @return 指定失败点 NULL，其余拥有字节块。 */
static void *allocate(void) { if (++attempt == fail_at) return NULL; void *p = malloc(1); assert(p); active++; return p; }
/** @brief 释放模型原语。 @param[in] p 拥有指针。 @post 活跃计数归还。 */
static void release(void *p) { assert(p && active); active--; free(p); }
/** @brief 创建模型 mutex。 @return 拥有句柄或 NULL。 */
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return allocate(); }
/** @brief 创建模型事件组。 @return 拥有句柄或 NULL。 */
static EventGroupHandle_t xEventGroupCreate(void) { return allocate(); }
/** @brief 删除模型 mutex。 @param[in] p 拥有句柄。 @post 对象释放。 */
static void vSemaphoreDelete(SemaphoreHandle_t p) { release(p); }
/** @brief 删除模型事件组。 @param[in] p 拥有句柄。 @post 对象释放。 */
static void vEventGroupDelete(EventGroupHandle_t p) { release(p); }
/** @brief 模拟初始 idle 标志发布。 @param[in] p 有效事件组。 @param[in] bits 待置位。 @return 原集合。 */
static unsigned xEventGroupSetBits(EventGroupHandle_t p, unsigned bits) { assert(p); return bits; }
/** @brief 取得确定性时钟。 @return 当前 tick。 */
static TickType_t xTaskGetTickCount(void) { return now; }
/** @brief 推进单线程退出等待预算。 @param[in] ticks tick 数。 @post 虚拟时钟推进。 */
static void vTaskDelay(TickType_t ticks) { now += ticks; }
'''

SCALAR_TESTS = r'''
/** @brief 验证每个候选分配失败都完整撤销，以及已有句柄保持。 @post 全部临时对象回收且成功路径可重试。 */
static void test_primitives(void)
{
    for (unsigned failure = 1; failure <= 3; failure++) {
        attempt = 0; fail_at = failure;
        assert(!initialize_primitives());
        assert(!g_app_event_bus.mutex && !g_app_event_bus.idle_events &&
               !s_publish_order_mutex && !active);
    }
    attempt = 0; fail_at = 0; assert(initialize_primitives() && active == 3);
    void *old_mutex = g_app_event_bus.mutex;
    vEventGroupDelete(g_app_event_bus.idle_events); g_app_event_bus.idle_events = NULL;
    attempt = 0; fail_at = 1; assert(!initialize_primitives() && active == 2);
    assert(g_app_event_bus.mutex == old_mutex && s_publish_order_mutex);
    attempt = 0; fail_at = 0; assert(initialize_primitives() && active == 3);
    vSemaphoreDelete(g_app_event_bus.mutex); vEventGroupDelete(g_app_event_bus.idle_events);
    vSemaphoreDelete(s_publish_order_mutex); assert(!active);
}
/** @brief 验证有限预算向上取整和时钟回绕。 @post 无分配或全局无限等待哨兵。 */
static void test_budget(void)
{
    assert(app_event_timeout_ticks(0) == 0 && app_event_timeout_ticks(1) == 2);
    assert(app_event_timeout_ticks(UINT32_MAX) == portMAX_DELAY - 1U);
    now = 3; assert(app_event_remaining_ticks(UINT32_MAX - 1U, 7) == 2);
    now = 5; assert(app_event_remaining_ticks(UINT32_MAX - 1U, 7) == 0);
}
/** @brief 验证关闭门后只等待已登记租约，超时保留并可归还后重试。 @post 无新租约通过，旧借用全部归还。 */
static void test_admission(void)
{
    s_access_open = true; s_access_users = 0; now = 0;
    assert(app_event_bus_acquire() && app_event_bus_acquire() && s_access_users == 2);
    assert(!app_event_bus_close_access(0, 2) && now == 2 && !s_access_open);
    assert(!app_event_bus_acquire() && s_access_users == 2);
    app_event_bus_release(); app_event_bus_release();
    assert(app_event_bus_close_access(now, 0) && !s_access_users);
}
/** @brief 运行初始化与预算生产函数。 @return 全部断言通过为零。 */
int main(void) { test_primitives(); test_budget(); test_admission(); puts("app event primitives/budget: PASS"); return 0; }
'''


JSON_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef int cc_error_code_t;
typedef struct { cc_error_code_t code; char *message; } cc_result_t;
typedef struct { char *data; size_t length, capacity; cc_error_code_t error_code; } cc_string_builder_t;
typedef struct { unsigned refs; size_t len; char text[4096]; } app_event_envelope_t;
typedef struct { uint32_t seq; int64_t timestamp_ms; int kind, source, level; app_event_envelope_t *envelope; } app_event_record_t;
static struct {
    app_event_record_t *recent, view[16];
    size_t recent_capacity, recent_count, recent_start, recent_bytes, view_count;
    uint32_t next_seq, clear_generation, dropped, ring_overwrites, sink_dropped, published;
    uint32_t maintenance_skipped, oom_dropped, usb_queue_dropped, display_queue_dropped;
    uint32_t max_usb_queue_waiting, max_display_queue_waiting, view_latest_seq;
    bool view_active;
} g_app_event_bus;
#define CC_OK 0
#define CC_ERR_INVALID_ARGUMENT 1
#define CC_ERR_OUT_OF_MEMORY 2
#define CC_ERR_INVALID_STATE 3
#define CC_ERR_LIMIT_EXCEEDED 4
#define CC_ERR_UNKNOWN 5
#define SB_INITIAL_CAPACITY 256
#define APP_EVENT_RECENT_MAX 2U
#define APP_EVENT_VIEW_MAX_ITEMS 16U
#define APP_EVENT_KIND_COUNT 9
#define APP_EVENT_KIND_MASK(kind) (1UL << (uint32_t)(kind))
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
static size_t diagnostics, buffers, snapshots, allocation_call, fail_allocation;
/** @brief 构造无动态字段的成功结果。 @return 按值成功。 */
static cc_result_t cc_result_ok(void) { return (cc_result_t){CC_OK, NULL}; }
/** @brief 让失败诊断真实拥有堆字符串。 @param[in] code 错误码。 @param[in] message 调用期文本借用。 @return 必须消费的拥有结果。 */
static cc_result_t cc_result_error(int code, const char *message)
{ char *copy = strdup(message); assert(copy); diagnostics++; return (cc_result_t){code, copy}; }
/** @brief 消费诊断并清零。 @param[in,out] result 必需拥有结果槽。 @post 动态诊断释放。 */
static void cc_result_free(cc_result_t *result)
{ if (result->message) { assert(diagnostics); diagnostics--; free(result->message); } *result = (cc_result_t){0}; }
/** @brief 注入构造器首次分配失败。 @param[in] bytes 字节数。 @return 拥有缓冲或 NULL。 */
static void *builder_malloc(size_t bytes)
{ if (++allocation_call == fail_allocation) return NULL; void *p = malloc(bytes); assert(p); buffers++; return p; }
/** @brief 注入构造器扩容失败并保留旧块。 @param[in,out] old 独占旧块。 @param[in] bytes 新字节数。 @return 新拥有缓冲或 NULL。 */
static void *builder_realloc(void *old, size_t bytes)
{ assert(old); if (++allocation_call == fail_allocation) return NULL; void *p = realloc(old, bytes); assert(p); return p; }
/** @brief 归还构造器拥有缓冲。 @param[in] p 可空拥有指针。 @post 活跃缓冲归还。 */
static void builder_free(void *p) { if (p) { assert(buffers); buffers--; free(p); } }
/** @brief 分配真实快照数组。 @param[in] count 项数。 @param[in] bytes 每项字节。 @param[in] caps 未使用能力。 @return 拥有数组。 */
static void *heap_caps_calloc(size_t count, size_t bytes, int caps)
{ (void)caps; void *p = calloc(count, bytes); assert(p); snapshots++; return p; }
/** @brief 释放快照数组。 @param[in] p 拥有数组。 @post 快照计数归还。 */
static void heap_caps_free(void *p) { if (p) { assert(snapshots); snapshots--; free(p); } }
/** @brief 模拟已在 fixture 固定的 ring。 @return true。 */
static bool app_event_ring_ensure_locked(void) { assert(g_app_event_bus.recent); return true; }
/** @brief 固定快照正文。 @param[in,out] envelope 活跃对象。 @post 引用增加。 */
static void app_event_envelope_retain(app_event_envelope_t *envelope) { assert(envelope->refs); envelope->refs++; }
/** @brief 归还快照引用。 @param[in,out] envelope 可空拥有引用。 @post 静态 ring 引用仍存活。 */
static void app_event_envelope_release(app_event_envelope_t *envelope) { if (envelope) { assert(envelope->refs > 1); envelope->refs--; } }
/** @brief 模拟单线程状态锁。 @post 无操作。 */
static void app_event_bus_lock(void) {}
/** @brief 模拟单线程状态解锁。 @post 无操作。 */
static void app_event_bus_unlock(void) {}
/** @brief 提供静态种类名。 @param[in] kind 未使用。 @return 静态借用。 */
static const char *app_event_kind_string(int kind) { (void)kind; return "status"; }
/** @brief 提供静态来源名。 @param[in] source 未使用。 @return 静态借用。 */
static const char *app_event_source_string(int source) { (void)source; return "system"; }
/** @brief 提供静态级别名。 @param[in] level 未使用。 @return 静态借用。 */
static const char *app_event_log_level_string(int level) { (void)level; return "info"; }
'''

JSON_TESTS = r'''
/** @brief 在每个真实构造器分配点失败，验证无半成品、诊断或引用泄漏。 @post 成功对照和 OOM 矩阵都释放快照与构造器资源。 */
static void test_json_failures(void)
{
    app_event_envelope_t envelope = {.refs = 1, .len = 4095};
    memset(envelope.text, '\n', sizeof(envelope.text) - 1U);
    envelope.text[sizeof(envelope.text) - 1U] = '\0';
    app_event_record_t record = {.seq = 1, .kind = 5, .envelope = &envelope};
    g_app_event_bus.recent = &record; g_app_event_bus.recent_capacity = 1;
    g_app_event_bus.recent_count = 1; g_app_event_bus.next_seq = 1;
    char *json = app_event_bus_json_active(0, 1, 0);
    assert(json && strstr(json, "\\n") && envelope.refs == 1 && !diagnostics && !snapshots);
    size_t total = allocation_call; assert(total > 2); builder_free(json);
    for (size_t failure = 1; failure <= total; failure++) {
        allocation_call = 0; fail_allocation = failure;
        json = app_event_bus_json_active(0, 1, 0);
        assert(!json && envelope.refs == 1 && !diagnostics && !snapshots && !buffers);
    }
    allocation_call = 0; fail_allocation = 0;
    record.kind = -1;
    json = app_event_bus_json_active(0, 1, UINT32_MAX);
    assert(json && strstr(json, "\"events\":[]")); builder_free(json);
    assert(envelope.refs == 1 && !diagnostics && !snapshots && !buffers);
}
/** @brief 运行实际 ring JSON 与实际 builder 故障矩阵。 @return 全部断言通过为零。 */
int main(void) { test_json_failures(); puts("app event JSON allocation/ownership: PASS"); return 0; }
'''


FILE_PREFIX = r'''
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
typedef uint32_t TickType_t;
static struct {
    FILE *usb_file;
    size_t usb_current_bytes, usb_max_bytes;
    int usb_part, usb_max_files;
    bool renamed_with_time, rename_pending;
    char usb_dir[128], usb_current_path[192], usb_base_path[176];
} g_app_event_bus;
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define pdTICKS_TO_MS(tick) (tick)
static bool fail_close, fail_rename;
static int stat_error;
static time_t fixture_wall_time = 1704068434;
/** @brief 单线程文件模型状态锁。 @post 无操作。 */
static void app_event_bus_lock(void) {}
/** @brief 单线程文件模型解锁。 @post 无操作。 */
static void app_event_bus_unlock(void) {}
/** @brief 返回稳定启动 tick。 @return 1。 */
static TickType_t xTaskGetTickCount(void) { return 1; }
/** @brief 透传真实 fclose 并可报告关闭失败。 @param[in] file 拥有 FILE。 @return 原生结果或注入 EOF，FILE 始终已消费。 */
static int fixture_fclose(FILE *file)
{ int rc = fclose(file); if (fail_close) { fail_close = false; errno = EIO; return EOF; } return rc; }
/** @brief 对真实 rename 注入无副作用失败。 @param[in] from 源借用。 @param[in] to 目标借用。 @return 原生返回或 -1。 */
static int fixture_rename(const char *from, const char *to)
{ if (fail_rename) { errno = EIO; return -1; } return rename(from, to); }
/** @brief 注入目标存在探测错误。 @param[in] path 路径借用。 @param[out] info 成功时写元数据。 @return 原生 stat 或指定失败，不把权限错误当成目标不存在。 */
static int fixture_stat(const char *path, struct stat *info)
{ if (stat_error) { errno = stat_error; return -1; } return stat(path, info); }
/** @brief 提供可推进的墙钟。 @param[out] out 可空秒值槽。 @return 当前模型 Unix 秒。 */
static time_t fixture_time(time_t *out) { if (out) *out = fixture_wall_time; return fixture_wall_time; }
/** @brief 分配真实清理候选缓存。 @param[in] count 项数。 @param[in] bytes 单项字节。 @param[in] caps 未使用。 @return 拥有缓存或 NULL。 */
static void *heap_caps_calloc(size_t count, size_t bytes, int caps) { (void)caps; return calloc(count, bytes); }
/** @brief 释放清理缓存。 @param[in] p 可空拥有块。 @post 对象释放。 */
static void heap_caps_free(void *p) { free(p); }
'''

FILE_TESTS = r'''
/** @brief 创建内容已存在的模型日志。 @param[in] path 目标路径借用。 @param[in] text NUL 正文借用。 @post 文件关闭且正文完整写入。 */
static void put_file(const char *path, const char *text)
{ FILE *file = fopen(path, "w"); assert(file); assert(fwrite(text, 1, strlen(text), file) == strlen(text)); assert(!fclose(file)); }
/** @brief 验证实际文件尺寸、换卷溢出和命名/关闭错误。 @post 全部文件与目录清理，所有 FILE 已消费。 */
static void test_files(void)
{
    char directory[] = "/tmp/cclaw-usb-log-XXXXXX"; assert(mkdtemp(directory));
    strcpy(g_app_event_bus.usb_dir, directory);
    char boot[192]; build_boot_base_path(boot, sizeof(boot), directory);
    assert(strstr(boot, "/cclaw-boot-1.log"));
    assert(app_event_join_path(g_app_event_bus.usb_base_path, sizeof(g_app_event_bus.usb_base_path), directory, "cclaw-existing.log"));
    put_file(g_app_event_bus.usb_base_path, "12345");
    assert(open_usb_log_file(g_app_event_bus.usb_base_path) && g_app_event_bus.usb_current_bytes == 5);
    g_app_event_bus.usb_part = INT_MAX; g_app_event_bus.usb_max_bytes = 1;
    g_app_event_bus.usb_current_bytes = SIZE_MAX;
    FILE *old = g_app_event_bus.usb_file;
    assert(!rotate_if_needed(SIZE_MAX) && g_app_event_bus.usb_file == old && g_app_event_bus.usb_part == INT_MAX);
    char small[3] = "ok";
    build_part_path(small, sizeof(small), "cclaw-existing.log", 2); assert(!small[0]);
    fail_close = true;
    assert(!open_usb_log_file(g_app_event_bus.usb_base_path) && !g_app_event_bus.usb_file);
    assert(open_usb_log_file(g_app_event_bus.usb_base_path));
    g_app_event_bus.usb_part = 1; g_app_event_bus.rename_pending = true;
    old = g_app_event_bus.usb_file; stat_error = EACCES;
    assert(!rename_to_time_if_ready() && g_app_event_bus.usb_file == old && g_app_event_bus.rename_pending);
    stat_error = 0;
    fail_rename = true;
    assert(rename_to_time_if_ready() && g_app_event_bus.usb_file &&
           !g_app_event_bus.renamed_with_time && g_app_event_bus.rename_pending);
    fail_rename = false; fail_close = true;
    assert(!rename_to_time_if_ready() && !g_app_event_bus.usb_file &&
           !g_app_event_bus.renamed_with_time && g_app_event_bus.rename_pending);
    assert(open_usb_log_file(g_app_event_bus.usb_base_path));
    assert(rename_to_time_if_ready() && g_app_event_bus.renamed_with_time && !g_app_event_bus.rename_pending);
    char first_volume[192]; strcpy(first_volume, g_app_event_bus.usb_current_path);
    assert(app_event_join_path(g_app_event_bus.usb_base_path, sizeof(g_app_event_bus.usb_base_path), directory, "cclaw-second.log"));
    put_file(g_app_event_bus.usb_base_path, "second volume");
    assert(open_usb_log_file(g_app_event_bus.usb_base_path));
    g_app_event_bus.renamed_with_time = false; g_app_event_bus.rename_pending = true;
    assert(rename_to_time_if_ready() && !g_app_event_bus.renamed_with_time && g_app_event_bus.rename_pending);
    assert(strcmp(g_app_event_bus.usb_current_path, g_app_event_bus.usb_base_path) == 0);
    FILE *first = fopen(first_volume, "r"); assert(first);
    char saved[32] = {0}; assert(fread(saved, 1, sizeof(saved) - 1U, first) == 5 && !strcmp(saved, "12345"));
    assert(!fclose(first));
    fixture_wall_time++;
    assert(rename_to_time_if_ready() && g_app_event_bus.renamed_with_time && !g_app_event_bus.rename_pending);
    assert(strcmp(g_app_event_bus.usb_current_path, first_volume) != 0 && access(first_volume, F_OK) == 0);
    g_app_event_bus.renamed_with_time = false; g_app_event_bus.rename_pending = true;
    old = g_app_event_bus.usb_file; stat_error = EACCES;
    assert(rename_to_time_if_ready() && g_app_event_bus.usb_file == old &&
           g_app_event_bus.renamed_with_time && !g_app_event_bus.rename_pending);
    stat_error = 0;
    char ignored[192]; assert(app_event_join_path(ignored, sizeof(ignored), directory, "cclaw-old.log.bak"));
    put_file(ignored, "preserve");
    char old_path[192]; assert(app_event_join_path(old_path, sizeof(old_path), directory, "cclaw-old.log"));
    put_file(old_path, "remove");
    g_app_event_bus.usb_max_files = 1; cleanup_old_logs();
    assert(access(g_app_event_bus.usb_current_path, F_OK) == 0 && access(ignored, F_OK) == 0 &&
           access(old_path, F_OK) != 0);
    assert(!fclose(g_app_event_bus.usb_file)); g_app_event_bus.usb_file = NULL;
    assert(!remove(g_app_event_bus.usb_current_path)); assert(!remove(ignored)); assert(!rmdir(directory));
}
/** @brief 运行真实日志路径、文件状态与轮转算术。 @return 全部断言通过为零。 */
int main(void) { test_files(); puts("app event USB file state: PASS"); return 0; }
'''


def main():
    """运行 writer 交错、初始化预算与实际构造器 OOM 模型。"""
    bus = "main/services/events/app_event_bus.c"
    usb = "main/services/events/app_event_usb_log_sink.c"
    internal = "main/services/events/app_event_bus_internal.h"
    code = function(internal, "static inline void app_event_counter_increment(")
    code += function(bus, "TickType_t app_event_timeout_ticks(")
    code += function(bus, "TickType_t app_event_remaining_ticks(")
    code += function(usb, "static bool usb_log_enabled(")
    code += function(usb, "static void usb_log_task(")
    code += function(usb, "static BaseType_t create_usb_log_task(")
    code += function(usb, "esp_err_t app_event_usb_start(")
    code += function(usb, "esp_err_t app_event_usb_stop_until(")
    code += function(usb, "esp_err_t app_event_usb_stop(")
    code += function(bus, "static esp_err_t app_event_bus_stop_active(")
    run_fixture(USB_PREFIX + code + USB_TESTS, "usb-lifecycle")
    scalar = function(bus, "TickType_t app_event_timeout_ticks(")
    scalar += function(bus, "TickType_t app_event_remaining_ticks(")
    scalar += function(bus, "static bool initialize_primitives(")
    scalar += function(bus, "bool app_event_bus_acquire(")
    scalar += function(bus, "void app_event_bus_release(")
    scalar += function(bus, "static bool app_event_bus_close_access(")
    run_fixture(SCALAR_PREFIX + scalar + SCALAR_TESTS, "event-primitives")
    builder = "third_party/cclaw-sdk/cclaw/core/src/util/cc_string_builder.c"
    json_code = "#define malloc builder_malloc\n#define realloc builder_realloc\n#define free builder_free\n"
    for signature in ("cc_result_t cc_string_builder_init(", "static cc_result_t ensure_capacity(",
                      "cc_result_t cc_string_builder_append(", "cc_result_t cc_string_builder_appendf(",
                      "cc_result_t cc_string_builder_append_char(", "char *cc_string_builder_take("):
        json_code += function(builder, signature)
    json_code += "#undef malloc\n#undef realloc\n#undef free\n"
    json_code += function(internal, "static inline void app_event_discard_result(")
    ring = "main/services/events/app_event_ring.c"
    for signature in ("void app_event_json_escape_append(", "static void app_event_json_append_record(",
                      "static char *app_event_bus_json_active("):
        json_code += function(ring, signature)
    run_fixture(JSON_PREFIX + json_code + JSON_TESTS, "event-json")
    files = function(bus, "static size_t app_event_complete_utf8_prefix(")
    for signature in ("void app_event_copy_text(", "bool app_event_append_text(",
                      "bool app_event_join_path(", "bool app_event_append_uint32("):
        files += function(bus, signature)
    files += "#define fclose fixture_fclose\n#define rename fixture_rename\n#define stat(path,info) fixture_stat(path,info)\n#define time(out) fixture_time(out)\n"
    for signature in ("static bool time_is_valid(", "static void build_time_base_path(",
                      "static void build_boot_base_path(", "static void build_part_path(",
                      "static bool open_usb_log_file(", "static bool rotate_if_needed(",
                      "static bool rename_to_time_if_ready(", "static bool is_log_name(",
                      "static void cleanup_old_logs("):
        files += function(usb, signature)
    files += "#undef fclose\n#undef rename\n#undef stat\n#undef time\n"
    run_fixture(FILE_PREFIX + files + FILE_TESTS, "event-files")


class EventLifecycleTests(unittest.TestCase):
    def test_lifecycle(self):
        """在统一发现入口执行四组实际生产函数模型。"""
        main()


if __name__ == "__main__":
    unittest.main()
