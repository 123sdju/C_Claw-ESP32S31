#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

static int overflow_warnings;

/** @brief 捕获实际串口超长提示。 @param[in] format 调用期格式借用。 @param[in] ... 未读取的显示参数。 @return 零，测试不模拟输出字节数。 */
static int capture_console_printf(const char *format, ...)
{
    if (strstr(format, "entire line rejected"))
        __atomic_add_fetch(&overflow_warnings, 1, __ATOMIC_RELEASE);
    return 0;
}

#define printf capture_console_printf
#include "../../../main/transports/serial_console.c"
#undef printf

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)

typedef struct fake_task { pthread_t thread; TaskFunction_t function; void *arg; } fake_task_t;
static fake_task_t tasks[32];
static unsigned task_count;
static _Thread_local fake_task_t *current_task;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned char input[4096];
static size_t input_head,input_tail;
static int callback_count,callback_blocked,release_callback,callback_bad_data;
static int fail_alloc,block_idle,idle_entered;
static int try_self_stop,self_stop_result;
static int gate_running;
static int fail_flush;
static unsigned flush_calls,flushed_bytes;
static int pause_creation,publication_wait_seen,ready_events;
static int failure_logs,failure_log_bad_handle,failure_log_stop_result;
static char last_text[serial_console_LINE_BYTES];

/**
 * @brief 在实际行缓冲OOM日志入口检查句柄已经发布，并尝试从该任务停止自身。
 * @param[in] tag 调用期标签借用，忽略。
 * @param[in] format 日志格式借用，只处理行缓冲分配失败分支。
 * @param[in] ... 未读取的显示参数。
 * @post 错误日志次数、句柄身份及self-stop结果原子发布，其他错误日志无副作用。
 */
void host_serial_log_error(const char *tag, const char *format, ...)
{
    (void)tag;
    if (strcmp(format, "serial line buffer alloc failed") != 0) return;
    TaskHandle_t published = __atomic_load_n(&s_serial_console_task, __ATOMIC_ACQUIRE);
    if (!published || published != current_task)
        __atomic_store_n(&failure_log_bad_handle, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&failure_log_stop_result, serial_console_stop(10), __ATOMIC_RELEASE);
    __atomic_add_fetch(&failure_logs, 1, __ATOMIC_RELEASE);
}

static void *task_main(void *arg)
{
    fake_task_t *task=arg; current_task=task; task->function(task->arg); return NULL;
}

/**
 * @brief 创建pthread任务，可在返回句柄前暂停以核对任务发布屏障。
 * @param[in] function 任务函数借用。
 * @param[in] name 忽略的名称。
 * @param[in] stack 忽略的栈字节数。
 * @param[in,out] arg 任务借用上下文。
 * @param[in] priority 忽略的优先级。
 * @param[out] out 成功写入稳定fixture句柄。
 * @return pdPASS成功，容量或pthread创建失败返回零，发布专项失败由assert终止。
 */
BaseType_t xTaskCreate(TaskFunction_t function,const char *name,uint32_t stack,void *arg,
                       UBaseType_t priority,TaskHandle_t *out)
{
    (void)name; (void)stack; (void)priority;
    if (task_count>=sizeof(tasks)/sizeof(tasks[0])) return 0;
    fake_task_t *task=&tasks[task_count++]; task->function=function; task->arg=arg;
    *out=task;
    if (pthread_create(&task->thread,NULL,task_main,task)!=0) return 0;
    pthread_detach(task->thread);
    if (pause_creation) {
        TickType_t start = xTaskGetTickCount();
        while (!__atomic_load_n(&publication_wait_seen, __ATOMIC_ACQUIRE) &&
               (TickType_t)(xTaskGetTickCount() - start) < 2000) {
            struct timespec wait = {0, 1000000L};
            nanosleep(&wait, NULL);
        }
        assert(__atomic_load_n(&publication_wait_seen, __ATOMIC_ACQUIRE));
        assert(!__atomic_load_n(&ready_events, __ATOMIC_ACQUIRE));
    }
    return pdPASS;
}
void vTaskDelete(TaskHandle_t task) { (void)task; pthread_exit(NULL); }
/** @brief 模拟tick等待并观察创建者尚未发布句柄时的任务等待。 @param[in] ticks 等待tick数，fixture为一千Hz。 @post 等待结束，任务发布窗口经过release标记。 */
void vTaskDelay(TickType_t ticks)
{
    if (current_task && !__atomic_load_n(&s_serial_console_task, __ATOMIC_ACQUIRE))
        __atomic_store_n(&publication_wait_seen, 1, __ATOMIC_RELEASE);
    struct timespec wait={ticks/1000U,(long)(ticks%1000U)*1000000L};
    nanosleep(&wait,NULL);
}
TickType_t xTaskGetTickCount(void)
{ struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now); return (TickType_t)((uint64_t)now.tv_sec*1000U+(unsigned long)now.tv_nsec/1000000U); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }

void *heap_caps_malloc(size_t size,int caps)
{ (void)caps; return __atomic_load_n(&fail_alloc,__ATOMIC_ACQUIRE) ? NULL : malloc(size); }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "error"; }
esp_err_t uart_param_config(uart_port_t port,const uart_config_t *config) { (void)port; (void)config; return ESP_OK; }
esp_err_t uart_driver_install(uart_port_t port,int rx,int tx,int queue,void *events,int flags)
{ (void)port; (void)rx; (void)tx; (void)queue; (void)events; (void)flags; return ESP_OK; }

/**
 * @brief 清空模拟RX队列或返回指定失败，不触碰已有回调。
 * @param[in] port 忽略的UART端口值。
 * @return 注入时返回ESP_FAIL并保留队列，成功清空并记录丢弃字节数。
 */
esp_err_t uart_flush_input(uart_port_t port)
{
    (void)port;
    pthread_mutex_lock(&lock);
    flush_calls++;
    if (fail_flush) {
        pthread_mutex_unlock(&lock);
        return ESP_FAIL;
    }
    flushed_bytes += (unsigned)(input_tail - input_head);
    input_head = input_tail = 0;
    pthread_mutex_unlock(&lock);
    return ESP_OK;
}

int uart_read_bytes(uart_port_t port,uint8_t *bytes,uint32_t length,TickType_t timeout)
{
    (void)port; (void)length;
    struct timespec deadline; clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_sec+=(time_t)(timeout/1000U);
    deadline.tv_nsec+=(long)(timeout%1000U)*1000000L;
    if (deadline.tv_nsec>=1000000000L) { deadline.tv_sec++; deadline.tv_nsec-=1000000000L; }
    pthread_mutex_lock(&lock);
    while (input_head==input_tail) {
        if (pthread_cond_timedwait(&changed,&lock,&deadline)!=0) break;
    }
    int count=0;
    if (input_head<input_tail) { *bytes=input[input_head++]; count=1; }
    pthread_mutex_unlock(&lock); return count;
}
/** @brief 在队列锁内附加 UART 字节并回收已消费前缀。 @param[in] text 借用 NUL 文本。 @post 容量内字节入队并通知接收者，已有待消费字节保持顺序。 */
static void feed(const char *text)
{
    pthread_mutex_lock(&lock);
    size_t length=strlen(text);
    if (input_head) {
        memmove(input, input + input_head, input_tail - input_head);
        input_tail -= input_head;
        input_head = 0;
    }
    if (input_tail+length<=sizeof(input)) { memcpy(input+input_tail,text,length); input_tail+=length; }
    pthread_cond_broadcast(&changed); pthread_mutex_unlock(&lock);
}
/** @brief 观察ready事件是否越过句柄发布。 @param[in] kind 忽略种类。 @param[in] source 忽略来源。 @param[in] level 忽略级别。 @param[in] message 调用期文本借用。 @post ready事件计数原子增加。 */
void app_event_publish(int kind,int source,int level,const char *message)
{ (void)kind; (void)source; (void)level; if (!strcmp(message,"Serial console ready")) __atomic_add_fetch(&ready_events,1,__ATOMIC_RELEASE); }
void app_event_publishf(int kind,int source,int level,const char *format,...)
{ (void)kind; (void)source; (void)level; (void)format; }
esp_err_t interaction_service_submit_clear(int source) { (void)source; return ESP_OK; }
esp_err_t interaction_service_submit_text(int source,const char *text) { (void)source; (void)text; return ESP_OK; }
bool interaction_service_wait_idle(uint32_t timeout_ms)
{
    if (!__atomic_load_n(&block_idle,__ATOMIC_ACQUIRE)) return true;
    __atomic_store_n(&idle_entered,1,__ATOMIC_RELEASE);
    vTaskDelay(timeout_ms); return false;
}
startup_gate_phase_t startup_gate_phase(void)
{ return __atomic_load_n(&gate_running,__ATOMIC_ACQUIRE) ? STARTUP_GATE_RUNNING : STARTUP_GATE_OFFLINE; }

static esp_err_t submit(const char *text,void *user)
{
    pthread_mutex_lock(&lock);
    callback_count++;
    if (user!=(void *)1) callback_bad_data=1;
    snprintf(last_text,sizeof(last_text),"%s",text);
    if (try_self_stop) self_stop_result=serial_console_stop(30);
    pthread_cond_broadcast(&changed);
    while (callback_blocked && !release_callback) pthread_cond_wait(&changed,&lock);
    pthread_mutex_unlock(&lock); return ESP_OK;
}
static int wait_count(int expected,unsigned timeout_ms)
{
    TickType_t start=xTaskGetTickCount();
    while ((TickType_t)(xTaskGetTickCount()-start)<timeout_ms) {
        pthread_mutex_lock(&lock); int count=callback_count; pthread_mutex_unlock(&lock);
        if (count>=expected) return 1;
        vTaskDelay(1);
    }
    return 0;
}
static int wait_flag(int *flag,unsigned timeout_ms)
{
    TickType_t start=xTaskGetTickCount();
    while ((TickType_t)(xTaskGetTickCount()-start)<timeout_ms) {
        if (__atomic_load_n(flag,__ATOMIC_ACQUIRE)) return 1;
        vTaskDelay(1);
    }
    return 0;
}

/**
 * @brief 等待 UART fixture 已交出当前全部字节，供停止未结束超长行时建立确定边界。
 * @param[in] timeout_ms 有限主机毫秒预算。
 * @return 已无排队字节为一，到期为零，不消费输入也不等待八百毫秒行空闲。
 */
static int wait_input_empty(unsigned timeout_ms)
{
    TickType_t start = xTaskGetTickCount();
    while ((TickType_t)(xTaskGetTickCount() - start) < timeout_ms) {
        pthread_mutex_lock(&lock);
        int empty = input_head == input_tail;
        pthread_mutex_unlock(&lock);
        if (empty) return 1;
        vTaskDelay(1);
    }
    return 0;
}

/**
 * @brief 用实际任务验证满容量合法、超长命令和普通文本整行拒绝、后续恢复及停止中断。
 * @return 全部断言通过为零，失败为一，正常路径每次停止任务后才复用输入与上下文。
 * @note main 原用例已提交五次，当前回归从该固定计数继续，fixture 不执行真实 Lua。
 */
static int test_line_boundaries(void)
{
    char text[serial_console_LINE_BYTES + 2];
    memset(text, 'x', serial_console_LINE_BYTES - 1);
    text[serial_console_LINE_BYTES - 1] = '\n';
    text[serial_console_LINE_BYTES] = '\0';
    CHECK(serial_console_start_with_submit(submit, (void *)1));
    feed(text);
    CHECK(wait_count(6, 2000));
    CHECK(serial_console_stop(1000) == ESP_OK);
    CHECK(callback_count == 6 && strlen(last_text) == serial_console_LINE_BYTES - 1);

    memset(text, 'x', serial_console_LINE_BYTES);
    memcpy(text, "/lua ", 5);
    text[serial_console_LINE_BYTES] = '\0';
    __atomic_store_n(&overflow_warnings, 0, __ATOMIC_RELEASE);
    CHECK(serial_console_start_with_submit(submit, (void *)1));
    feed(text);
    feed("\r\nvalid after overlong\n");
    CHECK(wait_count(7, 2000));
    CHECK(serial_console_stop(1000) == ESP_OK);
    CHECK(callback_count == 7 && strcmp(last_text, "valid after overlong") == 0);
    CHECK(__atomic_load_n(&overflow_warnings, __ATOMIC_ACQUIRE) == 1);

    memset(text, 'y', serial_console_LINE_BYTES);
    __atomic_store_n(&overflow_warnings, 0, __ATOMIC_RELEASE);
    CHECK(serial_console_start_with_submit(submit, (void *)1));
    feed(text);
    CHECK(wait_flag(&overflow_warnings, 2000));
    feed("valid after idle rejection\n");
    CHECK(wait_count(8, 2000));
    CHECK(serial_console_stop(1000) == ESP_OK);
    CHECK(callback_count == 8 && strcmp(last_text, "valid after idle rejection") == 0);
    CHECK(__atomic_load_n(&overflow_warnings, __ATOMIC_ACQUIRE) == 1);

    __atomic_store_n(&overflow_warnings, 0, __ATOMIC_RELEASE);
    CHECK(serial_console_start_with_submit(submit, (void *)1));
    feed(text);
    CHECK(wait_input_empty(2000));
    CHECK(serial_console_stop(1000) == ESP_OK);
    CHECK(callback_count == 8 && !__atomic_load_n(&overflow_warnings, __ATOMIC_ACQUIRE));
    CHECK(serial_console_start_with_submit(submit, (void *)1));
    feed("valid after interrupted overflow\n");
    CHECK(wait_count(9, 2000));
    CHECK(serial_console_stop(1000) == ESP_OK);
    CHECK(callback_count == 9 && strcmp(last_text, "valid after interrupted overflow") == 0);
    char tiny[1] = {'x'};
    __atomic_store_n(&overflow_warnings, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_serial_stop_requested, 0, __ATOMIC_RELEASE);
    feed("a");
    CHECK(serial_read_line(tiny, sizeof(tiny)) && tiny[0] == '\0');
    CHECK(__atomic_load_n(&overflow_warnings, __ATOMIC_ACQUIRE) == 1);
    return 0;
}

/**
 * @brief 验证恢复前旧RX尾部清空，以及start/stop清空失败时的关闭状态和重试。
 * @return 全部检查通过为零，失败为一，正常收尾停止最后一代任务。
 * @note 仅在此前任务已停止时调用，软件队列边界后的新输入继续正常交付。
 */
static int test_rx_flush_boundaries(void)
{
    int before = callback_count;
    unsigned previous_tasks = task_count;
    unsigned previous_dropped = flushed_bytes;
    feed("queued before restart\n");
    fail_flush = 1;
    CHECK(!serial_console_start_with_submit(submit, (void *)1));
    CHECK(task_count == previous_tasks && !serial_console_running());
    fail_flush = 0;
    __atomic_store_n(&publication_wait_seen, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ready_events, 0, __ATOMIC_RELEASE);
    pause_creation = 1;
    CHECK(serial_console_start_with_submit(submit, (void *)1));
    pause_creation = 0;
    CHECK(flushed_bytes > previous_dropped);
    feed("fresh after restart\n");
    CHECK(wait_count(before + 1, 2000));
    fail_flush = 1;
    CHECK(serial_console_stop(1000) == ESP_FAIL && !serial_console_running());
    CHECK(s_serial_console_task && s_serial_submit == submit && s_serial_submit_data == (void *)1);
    CHECK(callback_count == before + 1 && strcmp(last_text, "fresh after restart") == 0);
    feed("queued while stopped with flush failure\n");
    previous_tasks = task_count;
    CHECK(!serial_console_start_with_submit(submit, (void *)1));
    CHECK(task_count == previous_tasks && serial_console_stopping());
    fail_flush = 0;
    CHECK(serial_console_stop(1000) == ESP_OK && !s_serial_console_task && !s_serial_submit);
    CHECK(serial_console_start_with_submit(submit, (void *)1));
    feed("fresh after stop retry\n");
    CHECK(wait_count(before + 2, 2000));
    CHECK(serial_console_stop(1000) == ESP_OK);
    CHECK(callback_count == before + 2 && strcmp(last_text, "fresh after stop retry") == 0);
    return 0;
}

/** @brief 顺序验证任务拥有、停止与行边界，主机 UART 和任务均使用替身。 @return 成功为零，任一 CHECK 失败为一。 */
int main(void)
{
    CHECK(serial_console_ticks(0) == 1 && serial_console_ticks(1) == 1);
    CHECK(serial_console_ticks(UINT32_MAX) == portMAX_DELAY - 1);
    CHECK(serial_console_stop(100)==ESP_OK);
    CHECK(serial_console_start_with_submit(submit,(void *)1));
    CHECK(serial_console_start_with_submit(submit,(void *)1));
    CHECK(!serial_console_start_with_submit(submit,(void *)2));
    CHECK(serial_console_stop(1000)==ESP_OK && !serial_console_running());

    pthread_mutex_lock(&lock); callback_blocked=1; release_callback=0; pthread_mutex_unlock(&lock);
    CHECK(serial_console_start_with_submit(submit,(void *)1));
    feed("blocked\n"); CHECK(wait_count(1,2000));
    feed("queued tail after blocked input\n");
    unsigned flush_before_timeout = flush_calls;
    CHECK(serial_console_stop(40)==ESP_ERR_TIMEOUT && serial_console_running());
    CHECK(flush_calls == flush_before_timeout);
    CHECK(!serial_console_start_with_submit(submit,(void *)1));
    pthread_mutex_lock(&lock); release_callback=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&lock);
    CHECK(serial_console_stop(1000)==ESP_OK && !serial_console_running());
    CHECK(!callback_bad_data && callback_count == 1 && strcmp(last_text,"blocked")==0);
    CHECK(input_head == input_tail);

    pthread_mutex_lock(&lock); callback_blocked=0; pthread_mutex_unlock(&lock);
    __atomic_store_n(&block_idle,1,__ATOMIC_RELEASE);
    __atomic_store_n(&idle_entered,0,__ATOMIC_RELEASE);
    CHECK(serial_console_start_with_submit(submit,(void *)1));
    feed("startup\n"); CHECK(wait_count(2,2000));
    vTaskDelay(150);
    CHECK(!__atomic_load_n(&idle_entered,__ATOMIC_ACQUIRE) && serial_console_stop(1000)==ESP_OK);
    __atomic_store_n(&block_idle,0,__ATOMIC_RELEASE);

    CHECK(serial_console_start_with_submit(submit,(void *)1));
    feed("idle input"); CHECK(wait_count(3,2000));
    CHECK(strcmp(last_text,"idle input")==0 && serial_console_stop(1000)==ESP_OK);

    pthread_mutex_lock(&lock); try_self_stop=1; self_stop_result=ESP_OK; pthread_mutex_unlock(&lock);
    CHECK(serial_console_start_with_submit(submit,(void *)1));
    feed("self stop\n"); CHECK(wait_count(4,2000));
    CHECK(serial_console_stop(1000)==ESP_OK && self_stop_result==ESP_ERR_INVALID_STATE);
    pthread_mutex_lock(&lock); try_self_stop=0; pthread_mutex_unlock(&lock);

    __atomic_store_n(&block_idle,1,__ATOMIC_RELEASE);
    __atomic_store_n(&idle_entered,0,__ATOMIC_RELEASE);
    __atomic_store_n(&gate_running,1,__ATOMIC_RELEASE);
    CHECK(serial_console_start_with_submit(submit,(void *)1));
    feed("waiting\n"); CHECK(wait_count(5,2000) && wait_flag(&idle_entered,2000));
    CHECK(serial_console_stop(1000)==ESP_OK && !serial_console_running());
    __atomic_store_n(&block_idle,0,__ATOMIC_RELEASE);

    CHECK(test_line_boundaries() == 0);

    __atomic_store_n(&fail_alloc,1,__ATOMIC_RELEASE);
    __atomic_store_n(&failure_logs,0,__ATOMIC_RELEASE);
    __atomic_store_n(&failure_log_bad_handle,0,__ATOMIC_RELEASE);
    __atomic_store_n(&failure_log_stop_result,ESP_OK,__ATOMIC_RELEASE);
    (void)serial_console_start_with_submit(submit,(void *)1);
    CHECK(wait_flag(&s_serial_finished,2000) && serial_console_stop(1000)==ESP_OK);
    CHECK(__atomic_load_n(&failure_logs,__ATOMIC_ACQUIRE)==1);
    CHECK(!__atomic_load_n(&failure_log_bad_handle,__ATOMIC_ACQUIRE));
    CHECK(__atomic_load_n(&failure_log_stop_result,__ATOMIC_ACQUIRE)==ESP_ERR_INVALID_STATE);
    __atomic_store_n(&fail_alloc,0,__ATOMIC_RELEASE);
    CHECK(serial_console_start_with_submit(submit,(void *)1));
    CHECK(serial_console_stop(1000)==ESP_OK && !serial_console_running());
    CHECK(test_rx_flush_boundaries() == 0);
    return 0;
}
