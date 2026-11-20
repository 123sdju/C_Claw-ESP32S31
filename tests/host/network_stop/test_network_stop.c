#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 私有包含真实服务，直接控制 owner 停止和销毁路径。 */
#ifndef NETWORK_TEST_SOURCE
#define NETWORK_TEST_SOURCE "../../../main/services/network_service.c"
#endif
#include NETWORK_TEST_SOURCE
#include "wifi_event_callback.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
typedef struct fake_group { pthread_mutex_t mutex; pthread_cond_t cond; EventBits_t bits; int deleted; } fake_group_t;
typedef struct fake_queue { int deleted, remove_calls; network_job_t job; } fake_queue_t;
typedef struct fixture { pthread_mutex_t network_mutex; fake_group_t events; fake_queue_t jobs, wifi, set; } fixture_t;
static pthread_mutex_t barrier=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int job_posted, event_published, release_event, block_event;
static int queue_entered, release_queue, block_queue, overwrites;
static int wait_entries, block_one_waiter, waiter_blocked, release_waiter, hold_owner, release_owner;
static int fail_sends;
static int block_give, give_published, release_give;
static _Thread_local int is_network_owner;
static _Thread_local TickType_t clock_offset;
static int probe_apply_budget;
static TickType_t config_lock_wait,state_lock_wait,config_event_wait;
static int real_owner_started,wifi_shutdown_error,wifi_shutdown_calls,http_results;
static int wifi_pending,fail_wifi_remove;
static int verify_diagnostic_borrow,diagnostic_borrow_checked;

/** @brief 借出当前 pthread 的稳定数值身份供自等待检查。
 * @return 本线程身份，仅在所属线程存活期间用于比较。
 */
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)(uintptr_t)pthread_self(); }

TickType_t xTaskGetTickCount(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (TickType_t)((uint64_t)t.tv_sec*1000U+(uint64_t)t.tv_nsec/1000000U)+clock_offset;
}
/** @brief 提供与 fixture 原生 tick 同单位的单调毫秒。
 * @return 当前线程的逻辑单调毫秒，预算场景可增加已耗时偏移。
 */
uint64_t cc_platform_monotonic_ms(void) { return xTaskGetTickCount(); }
void vTaskDelay(TickType_t ticks)
{
    struct timespec t={.tv_sec=ticks/1000U,.tv_nsec=(long)(ticks%1000U)*1000000L}; nanosleep(&t,NULL);
}
static struct timespec deadline(TickType_t wait)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME,&t);
    t.tv_sec+=wait/1000U; t.tv_nsec+=(long)(wait%1000U)*1000000L;
    if (t.tv_nsec>=1000000000L) { t.tv_nsec-=1000000000L; t.tv_sec++; }
    return t;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore,TickType_t wait)
{
#ifndef NETWORK_TEST_ORIGINAL
    if (verify_diagnostic_borrow && semaphore==s_network_mutex && !wait && s_last_stop_result==ESP_FAIL) {
        assert(s_stop_callers>0);diagnostic_borrow_checked++;verify_diagnostic_borrow=0;
    }
#endif
    if (probe_apply_budget) {
        if (semaphore==s_config_apply_mutex) config_lock_wait=wait;
        if (semaphore==s_network_mutex) state_lock_wait=wait;
    }
    if (probe_apply_budget && semaphore==s_config_apply_mutex) {
        BaseType_t result=pthread_mutex_trylock(semaphore)==0;
        if (result) clock_offset+=30;
        return result;
    }
    if (wait==portMAX_DELAY) return pthread_mutex_lock(semaphore)==0;
    if (!wait) return pthread_mutex_trylock(semaphore)==0;
    struct timespec until=deadline(wait); return pthread_mutex_timedlock(semaphore,&until)==0;
}
/** @brief 归还真实 pthread mutex，并可在 owner 原生 Give 的返回尾部暂停。
 * @param[in,out] semaphore 当前线程持有的 mutex 借用。
 * @return 原生解锁成功为真，尾部屏障只用于确定的退场交错。
 */
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    BaseType_t result = pthread_mutex_unlock(semaphore)==0;
    pthread_mutex_lock(&barrier);
    if (is_network_owner && block_give) {
        give_published=1; pthread_cond_broadcast(&changed);
        while (!release_give) pthread_cond_wait(&changed,&barrier);
    }
    pthread_mutex_unlock(&barrier);
    return result;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { (void)semaphore; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t handle,EventBits_t bits)
{
    fake_group_t *g=handle;
    pthread_mutex_lock(&g->mutex); g->bits|=bits; pthread_cond_broadcast(&g->cond); pthread_mutex_unlock(&g->mutex);
    if (bits & NETWORK_EVENT_STOPPED) {
        pthread_mutex_lock(&barrier); event_published=1; pthread_cond_broadcast(&changed);
        while (block_event && !release_event) pthread_cond_wait(&changed,&barrier);
        pthread_mutex_unlock(&barrier);
    }
    pthread_mutex_lock(&g->mutex); EventBits_t result=g->bits; pthread_mutex_unlock(&g->mutex);
    return result;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t handle,EventBits_t bits)
{
    fake_group_t *g=handle; pthread_mutex_lock(&g->mutex);
    EventBits_t old=g->bits; g->bits&=~bits; pthread_mutex_unlock(&g->mutex); return old;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t handle,EventBits_t bits,BaseType_t clear,BaseType_t all,TickType_t wait)
{
    if (probe_apply_budget && bits==NETWORK_EVENT_CONFIG_APPLIED) {
        config_event_wait=wait;
        return 0;
    }
    (void)all; fake_group_t *g=handle;
    pthread_mutex_lock(&barrier); wait_entries++; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&barrier);
    pthread_mutex_lock(&g->mutex); struct timespec until=deadline(wait);
    while (!(g->bits & bits) && wait) {
        if (pthread_cond_timedwait(&g->cond,&g->mutex,&until)==ETIMEDOUT) break;
    }
    EventBits_t result=g->bits; if (clear) g->bits&=~bits; pthread_mutex_unlock(&g->mutex);
    pthread_mutex_lock(&barrier);
    if (block_one_waiter && !waiter_blocked && (result & bits)) {
        waiter_blocked=1; pthread_cond_broadcast(&changed);
        while (!release_waiter) pthread_cond_wait(&changed,&barrier);
    }
    pthread_mutex_unlock(&barrier); return result;
}
void vEventGroupDelete(EventGroupHandle_t handle) { ((fake_group_t *)handle)->deleted=1; }
BaseType_t xQueueSend(QueueHandle_t queue,const void *item,TickType_t wait)
{
    (void)queue; (void)item; (void)wait;
    pthread_mutex_lock(&barrier);
    if (fail_sends) { fail_sends--; pthread_mutex_unlock(&barrier); return pdFALSE; }
    ((fake_queue_t *)queue)->job=*(const network_job_t *)item;
    job_posted=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&barrier);
    return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) { (void)queue; return 0; }
BaseType_t xQueueOverwrite(QueueHandle_t queue,const void *item)
{
    (void)queue; (void)item; pthread_mutex_lock(&barrier);
    overwrites++; queue_entered=1; pthread_cond_broadcast(&changed);
    while (block_queue && !release_queue) pthread_cond_wait(&changed,&barrier);
    pthread_mutex_unlock(&barrier); return pdTRUE;
}
/** @brief 检查原生 QueueSet 移除所需的空队列条件，并注入一次 Wi-Fi 移除失败。
 * @param[in,out] queue fixture 持有的成员队列。
 * @param[in] set 当前集合借用。
 * @return 非空或注入失败为 pdFAIL，其余 pdPASS，记录每个成员的移除次数。
 */
BaseType_t xQueueRemoveFromSet(QueueHandle_t queue,QueueSetHandle_t set)
{
    (void)set; ((fake_queue_t *)queue)->remove_calls++;
    if ((queue==s_network_queue && job_posted) || (queue==s_wifi_event_queue && wifi_pending)) return pdFALSE;
    if (queue==s_wifi_event_queue && fail_wifi_remove) { fail_wifi_remove--;return pdFALSE; }
    return pdPASS;
}
void vQueueDelete(QueueHandle_t queue) { ((fake_queue_t *)queue)->deleted=1; }

/** @brief 等到真实 network_task 的模拟命令队列就绪。
 * @param[in] set 当前 fixture 的 QueueSet 借用。
 * @param[in] wait 原生等待值，本 fixture 只在有命令时返回。
 * @return 命令队列借用，字段内容由 receive 取走。
 */
QueueSetMemberHandle_t xQueueSelectFromSet(QueueSetHandle_t set,TickType_t wait)
{
    (void)set; (void)wait;
    pthread_mutex_lock(&barrier);
    while (!job_posted) pthread_cond_wait(&changed,&barrier);
    pthread_mutex_unlock(&barrier);
    return s_network_queue;
}

/** @brief 消费 fake 命令槽，供真实 owner 分支读取。
 * @param[in,out] queue 当前命令或 Wi-Fi 队列借用。
 * @param[out] item 与队列元素类型匹配的输出槽。
 * @param[in] wait 未使用的原生等待值。
 * @return 有命令为 pdTRUE，其余 pdFALSE，成功清空就绪状态。
 */
BaseType_t xQueueReceive(QueueHandle_t queue,void *item,TickType_t wait)
{
    (void)wait;
    pthread_mutex_lock(&barrier);
    bool available=queue==s_network_queue && job_posted;
    if (available) { *(network_job_t *)item=((fake_queue_t *)queue)->job; job_posted=0; }
    if (queue==s_wifi_event_queue && wifi_pending) {
        *(wifi_app_event_t *)item=WIFI_APP_EVENT_DISABLED;wifi_pending=0;available=true;
    }
    pthread_mutex_unlock(&barrier);
    return available;
}

/** @brief 结束 fixture owner 线程。 @param[in] task NULL 表示当前线程。
 * @post pthread_exit 后不再访问服务字段，主线程随后 join。
 */
void vTaskDelete(TaskHandle_t task) { assert(task==NULL); pthread_exit(NULL); }
/** @brief 模拟关闭 Wi-Fi 并记录重试。 @return 当前注入的原生结果。 */
esp_err_t wifi_app_shutdown_for_ota(void) { wifi_shutdown_calls++; return wifi_shutdown_error; }
/** @brief 模拟无失败通道的重启准备。 @post 不执行外部资源操作。 */
void wifi_app_prepare_for_reboot(void) {}
/** @brief 模拟成功配置。 @param[in] config 调用期借用配置。 @return true。 */
bool wifi_app_connect_from_config(const network_config_t *config) { (void)config; return true; }
/** @brief 返回空 Wi-Fi 值快照。 @param[out] status 有效输出槽。 @post 字段归零。 */
void wifi_app_get_status(wifi_app_status_t *status) { memset(status,0,sizeof(*status)); }
/** @brief 交付带动态成功消息的 HTTP 重置结果。 @return 诊断由真实服务消费。 */
cc_result_t runtime_service_reset_http_connections(void)
{
    cc_result_t result={.size=sizeof(result),.code=CC_OK};
    result.message=malloc(5); if (result.message) { memcpy(result.message,"http",5); http_results++; }
    return result;
}
/** @brief 消费 fixture 自己生成的结果。 @param[in,out] result message 为拥有分配，detail 恒 NULL。
 * @post 拥有消息归还并清空结果，统计递减。
 */
void cc_result_free(cc_result_t *result)
{ if (result->message) { free(result->message); http_results--; } memset(result,0,sizeof(*result)); }
/** @brief 模拟 mDNS 初始化。 @return ESP_OK。 */
esp_err_t mdns_init(void) { return ESP_OK; }
/** @brief 模拟 mDNS 释放。 @post 无拥有资源。 */
void mdns_free(void) {}
/** @brief 接收模拟主机名。 @param[in] name 借用文本。 @return ESP_OK。 */
esp_err_t mdns_hostname_set(const char *name) { (void)name; return ESP_OK; }
/** @brief 接收模拟实例名。 @param[in] name 借用文本。 @return ESP_OK。 */
esp_err_t mdns_instance_name_set(const char *name) { (void)name; return ESP_OK; }
/** @brief 模拟添加服务。
 * @param[in] instance 实例文本借用。 @param[in] service 服务名借用。 @param[in] protocol 协议借用。
 * @param[in] port 广告端口。 @param[in] txt 可空 TXT 借用。 @param[in] count TXT 项数。
 * @return ESP_OK，无资源接管。
 */
esp_err_t mdns_service_add(const char *instance,const char *service,const char *protocol,unsigned port,void *txt,unsigned count)
{ (void)instance;(void)service;(void)protocol;(void)port;(void)txt;(void)count;return ESP_OK; }
/** @brief 模拟 SNTP 创建。 @param[in] config 稳定配置借用。 @return ESP_OK。 */
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) { (void)config;return ESP_OK; }
/** @brief 模拟 SNTP 启动。 @return ESP_OK。 */
esp_err_t esp_netif_sntp_start(void) { return ESP_OK; }
/** @brief 模拟 SNTP 等待。 @param[in] timeout 原生 tick。 @return ESP_OK。 */
esp_err_t esp_netif_sntp_sync_wait(unsigned timeout) { (void)timeout;return ESP_OK; }
/** @brief 接收模拟校时通知。 @post 无资源接管。 */
void app_event_bus_note_time_synced(void) {}

/** @brief 建立由 fixture 保管的动态原语和一个尚未完成的模拟 owner。
 * @param[out] f 未初始化的独占测试状态。
 * @post 当前服务字段借用本 fixture，所有控制屏障清零，测试结束后由 finish 清理。
 */
static void setup(fixture_t *f)
{
    memset(f,0,sizeof(*f)); pthread_mutex_init(&f->network_mutex,NULL);
    pthread_mutex_init(&f->events.mutex,NULL); pthread_cond_init(&f->events.cond,NULL);
    s_network_mutex=&f->network_mutex; s_network_events=&f->events; s_network_queue=&f->jobs;
    s_wifi_event_queue=&f->wifi; s_network_queue_set=NULL; s_config_apply_mutex=NULL;
    s_network_task=(void *)1; s_event_registered=false; s_stopping=false;
#ifndef NETWORK_TEST_ORIGINAL
    __atomic_store_n(&s_network_owner_exited,false,__ATOMIC_RELEASE);
    s_command_queue_attached=s_wifi_queue_attached=false;
#endif
    pthread_mutex_lock(&barrier);
    job_posted=event_published=release_event=block_event=queue_entered=release_queue=block_queue=overwrites=0;
    wait_entries=block_one_waiter=waiter_blocked=release_waiter=hold_owner=release_owner=0;
    fail_sends=0; __atomic_store_n(&s_stop_enqueue_failed,0,__ATOMIC_RELEASE);
    block_give=give_published=release_give=0;
    probe_apply_budget=0;clock_offset=0;real_owner_started=0;wifi_shutdown_error=wifi_shutdown_calls=http_results=0;
    wifi_pending=fail_wifi_remove=0;
    verify_diagnostic_borrow=diagnostic_borrow_checked=0;
    pthread_mutex_unlock(&barrier);
}
static void finish(fixture_t *f)
{
    pthread_cond_destroy(&f->events.cond); pthread_mutex_destroy(&f->events.mutex);
    pthread_mutex_destroy(&f->network_mutex);
}
/** @brief 等到 STOP 入队后执行真实 owner 最终交接函数。
 * @param[in] unused 未使用的可空线程参数。
 * @return NULL，主线程 join 后结束 fixture 借用。
 */
static void *owner(void *unused)
{
    is_network_owner=1;
    (void)unused; pthread_mutex_lock(&barrier);
    while (!job_posted) pthread_cond_wait(&changed,&barrier);
    while (hold_owner && !release_owner) pthread_cond_wait(&changed,&barrier);
    pthread_mutex_unlock(&barrier); network_owner_finish_stop(); return NULL;
}
/** @brief 在 pthread 中运行完整真实 network_task。
 * @param[in] unused 未使用的线程参数。
 * @return 正常由原生自删替身结束，返回 NULL 仅作签名配对。
 */
static void *real_owner(void *unused)
{
    (void)unused;is_network_owner=1;
    pthread_mutex_lock(s_network_mutex);s_network_task=xTaskGetCurrentTaskHandle();pthread_mutex_unlock(s_network_mutex);
    pthread_mutex_lock(&barrier);real_owner_started=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&barrier);
    network_task(NULL);return NULL;
}
typedef struct stopper { uint32_t timeout; int done; bool result; } stopper_t;
static void *stopper(void *data)
{
    stopper_t *s=data; s->result=network_service_prepare_for_reboot(s->timeout);
    __atomic_store_n(&s->done,1,__ATOMIC_RELEASE); return NULL;
}
static void *dispatch_wifi(void *unused)
{
    (void)unused; wifi_app_status_t status={0};
    wifi_event_callback_dispatch(WIFI_APP_EVENT_CONNECTED,&status); return NULL;
}
static void wait_until(int *condition)
{
    pthread_mutex_lock(&barrier); while (!*condition) pthread_cond_wait(&changed,&barrier);
    pthread_mutex_unlock(&barrier);
}
static void release(int *condition)
{
    pthread_mutex_lock(&barrier); *condition=1; pthread_cond_broadcast(&changed); pthread_mutex_unlock(&barrier);
}
static void wait_for_two_stoppers(void)
{
    pthread_mutex_lock(&barrier); while (wait_entries<2) pthread_cond_wait(&changed,&barrier);
    pthread_mutex_unlock(&barrier);
}

static int test_stopped_signal(int timeout_case)
{
    fixture_t f; setup(&f); block_event=1;
    stopper_t stop={.timeout=timeout_case ? 25 : 1000}; pthread_t owner_thread, stop_thread;
    CHECK(pthread_create(&owner_thread,NULL,owner,NULL)==0 && pthread_create(&stop_thread,NULL,stopper,&stop)==0);
    wait_until(&event_published);
    if (timeout_case) {
        CHECK(pthread_join(stop_thread,NULL)==0 && !stop.result);
        CHECK(network_service_destroy(0)==ESP_ERR_TIMEOUT && !f.events.deleted && !f.jobs.deleted);
    } else {
        vTaskDelay(20);
        CHECK(!__atomic_load_n(&stop.done,__ATOMIC_ACQUIRE));
        CHECK(network_service_destroy(0)==ESP_ERR_TIMEOUT && !f.events.deleted && !f.jobs.deleted);
    }
    release(&release_event); CHECK(pthread_join(owner_thread,NULL)==0);
    if (!timeout_case) CHECK(pthread_join(stop_thread,NULL)==0 && stop.result);
    CHECK(network_service_prepare_for_reboot(100)==true && network_service_destroy(100)==ESP_OK &&
          f.events.deleted && f.jobs.deleted && f.wifi.deleted);
    finish(&f); return 0;
}

static int test_callback_drain(void)
{
    fixture_t f; setup(&f); s_network_task=NULL; block_queue=1;
#ifndef NETWORK_TEST_ORIGINAL
    __atomic_store_n(&s_network_owner_exited,true,__ATOMIC_RELEASE);
#endif
    CHECK(wifi_app_set_event_callback(network_wifi_event_cb,NULL)==ESP_OK); s_event_registered=true;
    pthread_t worker; CHECK(pthread_create(&worker,NULL,dispatch_wifi,NULL)==0); wait_until(&queue_entered);
    CHECK(network_service_destroy(5)==ESP_ERR_TIMEOUT && !f.jobs.deleted && !f.wifi.deleted && !f.events.deleted);
    CHECK(network_service_prepare_for_reboot(5)==true);
    wifi_app_status_t status={0}; wifi_event_callback_dispatch(WIFI_APP_EVENT_CONNECTED,&status);
    pthread_mutex_lock(&barrier); int count=overwrites; pthread_mutex_unlock(&barrier); CHECK(count==1);
    release(&release_queue); CHECK(pthread_join(worker,NULL)==0);
    CHECK(network_service_destroy(100)==ESP_OK && f.jobs.deleted && f.wifi.deleted && f.events.deleted);
    finish(&f); return 0;
}

static int test_concurrent_stop_callers(void)
{
    fixture_t f; setup(&f); hold_owner=1; block_one_waiter=1;
    stopper_t a={.timeout=1000},b={.timeout=1000}; pthread_t owner_thread,first,second;
    CHECK(pthread_create(&owner_thread,NULL,owner,NULL)==0 && pthread_create(&first,NULL,stopper,&a)==0 &&
          pthread_create(&second,NULL,stopper,&b)==0);
    wait_for_two_stoppers(); release(&release_owner); wait_until(&waiter_blocked);
    for (unsigned i=0;i<500 && !__atomic_load_n(&a.done,__ATOMIC_ACQUIRE) &&
                              !__atomic_load_n(&b.done,__ATOMIC_ACQUIRE);i++) vTaskDelay(1);
    CHECK(__atomic_load_n(&a.done,__ATOMIC_ACQUIRE) || __atomic_load_n(&b.done,__ATOMIC_ACQUIRE));
    CHECK(pthread_join(owner_thread,NULL)==0);
    CHECK(network_service_destroy(5)==ESP_ERR_TIMEOUT && !f.events.deleted && !f.jobs.deleted);
    CHECK(network_service_prepare_for_reboot(5)==true);
    release(&release_waiter);
    CHECK(pthread_join(first,NULL)==0 && pthread_join(second,NULL)==0 && a.result && b.result);
    CHECK(network_service_destroy(100)==ESP_OK && f.events.deleted && f.jobs.deleted && f.wifi.deleted);
    finish(&f); return 0;
}

static int test_stop_enqueue_retry(void)
{
    fixture_t f; setup(&f); fail_sends=1;
    CHECK(!network_service_prepare_for_reboot(25) && !f.jobs.deleted && !f.events.deleted);
    pthread_t worker; CHECK(pthread_create(&worker,NULL,owner,NULL)==0);
    CHECK(network_service_prepare_for_reboot(1000));
    CHECK(pthread_join(worker,NULL)==0 && network_service_destroy(100)==ESP_OK);
    finish(&f); return 0;
}

/** @brief 在 owner 已解锁但 Give 尚未返回时验证 stop/destroy 保留全部资源。
 * @return 旧尾部未完成时超时、放行后可成功重试为零，其他为一。
 */
static int test_mutex_return_tail(void)
{
    fixture_t f; setup(&f); block_give=1;
    stopper_t stop={.timeout=25}; pthread_t owner_thread,stop_thread;
    CHECK(pthread_create(&owner_thread,NULL,owner,NULL)==0 &&
          pthread_create(&stop_thread,NULL,stopper,&stop)==0);
    wait_until(&give_published);
    CHECK(pthread_join(stop_thread,NULL)==0 && !stop.result);
    CHECK(network_service_destroy(0)==ESP_ERR_TIMEOUT && !f.events.deleted && !f.jobs.deleted);
    release(&release_give);
    CHECK(pthread_join(owner_thread,NULL)==0);
    CHECK(network_service_destroy(100)==ESP_OK && f.events.deleted && f.jobs.deleted && f.wifi.deleted);
    finish(&f); return 0;
}

/** @brief 验证 owner 线程发起同步 STOP 时立即拒绝自等待。
 * @return 未入队、未破坏状态且可由外层清理为零，其他为一。
 */
static int test_owner_self_stop(void)
{
    fixture_t f; setup(&f); s_network_task=xTaskGetCurrentTaskHandle();
    CHECK(!network_service_stop(100) && !job_posted && !s_stopping);
    s_network_task=NULL;
#ifndef NETWORK_TEST_ORIGINAL
    __atomic_store_n(&s_network_owner_exited,true,__ATOMIC_RELEASE);
#endif
    CHECK(network_service_destroy(100)==ESP_OK);
    finish(&f); return 0;
}

/** @brief 验证 STOP 准入、失败排队回滚、迟到配置副本及共享预算。
 * @return 全部断言通过为零，正常路径释放提交 mutex 和 fixture。
 */
static int test_configuration_admission_and_budget(void)
{
    fixture_t f;setup(&f);pthread_mutex_t apply;pthread_mutex_init(&apply,NULL);s_config_apply_mutex=&apply;
    network_config_t configuration={0};memcpy(configuration.ssid,"original",9);
    s_stopping=true;
    CHECK(network_service_apply_wifi_config(&configuration,0)==ESP_ERR_INVALID_STATE && !job_posted && !s_pending_config_dirty);
    s_stopping=false;fail_sends=1;
    CHECK(network_service_apply_wifi_config(&configuration,0)==ESP_ERR_NO_MEM && !s_pending_config_dirty && !s_config_job_enqueued);
    CHECK(network_service_apply_wifi_config(&configuration,0)==ESP_ERR_TIMEOUT && s_pending_config_dirty);
    configuration.ssid[0]='x';CHECK(strcmp(s_pending_config.ssid,"original")==0);
    probe_apply_budget=1;
    CHECK(network_service_apply_wifi_config(&configuration,100)==ESP_ERR_TIMEOUT);
    CHECK(config_lock_wait<=100 && state_lock_wait<=70 && config_event_wait<=70);
    probe_apply_budget=0;clock_offset=0;s_network_task=NULL;
#ifndef NETWORK_TEST_ORIGINAL
    __atomic_store_n(&s_network_owner_exited,true,__ATOMIC_RELEASE);
#endif
    CHECK(network_service_destroy(100)==ESP_OK);pthread_mutex_destroy(&apply);finish(&f);return 0;
}

#ifndef NETWORK_TEST_ORIGINAL
/** @brief 运行真实 owner 的关闭失败分支，确认保留任务并在后续 STOP 成功退出。
 * @return 首个原生错误可诊断、未提前删除且重试成功为零，否则为一。
 */
static int test_shutdown_failure_keeps_owner(void)
{
    fixture_t f;setup(&f);s_network_queue_set=&f.set;wifi_shutdown_error=ESP_FAIL;verify_diagnostic_borrow=1;
    pthread_t worker;CHECK(pthread_create(&worker,NULL,real_owner,NULL)==0);wait_until(&real_owner_started);
    CHECK(network_service_shutdown_for_ota()==ESP_FAIL && s_network_task && !f.events.deleted && wifi_shutdown_calls==1);
    CHECK(diagnostic_borrow_checked==1 && s_last_stop_result==ESP_FAIL && network_service_destroy(0)==ESP_ERR_INVALID_STATE);
    wifi_shutdown_error=ESP_OK;
    CHECK(network_service_shutdown_for_ota()==ESP_OK && wifi_shutdown_calls==2);
    CHECK(pthread_join(worker,NULL)==0 && !http_results);
    CHECK(network_service_destroy(100)==ESP_OK && f.events.deleted && f.jobs.deleted && f.set.deleted);
    finish(&f);return 0;
}

/** @brief 验证排空末次队列事件后移除成员，第二阶段失败可保留集合并独立重试。
 * @return 没有非空移除、重复移除或提前删除为零，否则为一。
 */
static int test_queue_set_cleanup_retry(void)
{
    fixture_t f;setup(&f);s_network_task=NULL;s_network_queue_set=&f.set;
    __atomic_store_n(&s_network_owner_exited,true,__ATOMIC_RELEASE);
    s_command_queue_attached=s_wifi_queue_attached=true;
    job_posted=wifi_pending=fail_wifi_remove=1;
    CHECK(network_service_destroy(100)==ESP_ERR_INVALID_STATE);
    CHECK(!job_posted && !wifi_pending && !f.set.deleted && !f.jobs.deleted && !f.wifi.deleted);
    CHECK(!s_command_queue_attached && s_wifi_queue_attached && f.jobs.remove_calls==1 && f.wifi.remove_calls==1);
    CHECK(network_service_destroy(100)==ESP_OK && f.set.deleted && f.jobs.deleted && f.wifi.deleted);
    CHECK(f.jobs.remove_calls==1 && f.wifi.remove_calls==2);
    finish(&f);return 0;
}
#endif

/** @brief 执行事件/Give 尾部、并发等待者、回调排空和停止重试回归。
 * @return 所有断言通过为零，失败为一，正常路径释放全部 fixture 原语。
 */
int main(void)
{
    CHECK(test_stopped_signal(0)==0);
    CHECK(test_stopped_signal(1)==0);
    CHECK(test_callback_drain()==0);
    CHECK(test_concurrent_stop_callers()==0);
    CHECK(test_stop_enqueue_retry()==0);
    CHECK(test_mutex_return_tail()==0);
    CHECK(test_owner_self_stop()==0);
    CHECK(test_configuration_admission_and_budget()==0);
#ifndef NETWORK_TEST_ORIGINAL
    CHECK(test_shutdown_failure_keeps_owner()==0);
    CHECK(test_queue_set_cleanup_retry()==0);
#endif
    puts("network owner shutdown tests passed"); return 0;
}
