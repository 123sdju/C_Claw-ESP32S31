"""以真实 Wi-Fi/NVS 实现验证阶段重试、驱动串行及锁外通知，原生 API 使用本地模型。"""
from pathlib import Path
import re

from test_event_ring_ownership import run_c_test

ROOT = Path(__file__).resolve().parents[1]

BASE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef void *EventGroupHandle_t;
typedef void *cc_mutex_t;
typedef unsigned EventBits_t;
typedef const char *esp_event_base_t;
typedef int wifi_init_config_t;
typedef enum {WIFI_MODE_NULL,WIFI_MODE_STA,WIFI_MODE_AP,WIFI_MODE_APSTA} wifi_mode_t;
typedef struct {uint32_t addr;} esp_ip4_addr_t;
typedef struct {esp_ip4_addr_t ip,gw,netmask;} esp_netif_ip_info_t;
typedef struct {esp_netif_ip_info_t ip_info;} ip_event_got_ip_t;
typedef struct {uint16_t reason;} wifi_event_sta_disconnected_t;
typedef struct {int role;} esp_netif_t;
typedef struct {int role;} esp_netif_config_t;
typedef struct {
    struct {unsigned char ssid[32],password[64];int scan_method;uint8_t failure_retry_cnt;} sta;
    struct {unsigned char ssid[32],password[64];size_t ssid_len;int authmode;uint8_t channel,max_connection;} ap;
} wifi_config_t;
typedef struct {int code;char *message;} cc_result_t;
typedef struct {TickType_t period;bool active;} model_timer_t;
typedef model_timer_t *TimerHandle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_WIFI_NOT_INIT 3
#define ESP_ERR_WIFI_NOT_STARTED 4
#define ESP_ERR_WIFI_NOT_CONNECT 5
#define ESP_ERR_WIFI_CONN 6
#define ESP_ERR_WIFI_STATE 7
#define ESP_ERR_NVS_NO_FREE_PAGES 8
#define ESP_ERR_NVS_NEW_VERSION_FOUND 9
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED 10
#define ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED 11
#define CC_OK 0
#define CC_ERR_STORAGE 12
#define BIT0 1U
#define BIT1 2U
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define configTICK_RATE_HZ 100U
#define CONFIG_SPIRAM 1
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define WIFI_INIT_CONFIG_DEFAULT() 0
#define WIFI_STORAGE_RAM 1
#define WIFI_IF_STA 0
#define WIFI_IF_AP 1
#define WIFI_AUTH_WPA2_PSK 1
#define WIFI_AUTH_OPEN 0
#define WIFI_ALL_CHANNEL_SCAN 1
#define WIFI_PS_NONE 0
#define WIFI_EVENT ((const char *)1)
#define IP_EVENT ((const char *)2)
#define ESP_EVENT_ANY_ID -1
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_STOP 2
#define WIFI_EVENT_AP_START 3
#define WIFI_EVENT_AP_STOP 4
#define WIFI_EVENT_STA_DISCONNECTED 5
#define IP_EVENT_STA_GOT_IP 6
#define ESP_NETIF_DEFAULT_WIFI_STA() ((esp_netif_config_t){0})
#define ESP_NETIF_DEFAULT_WIFI_AP() ((esp_netif_config_t){1})
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (unsigned)((p)->addr>>24),(unsigned)(((p)->addr>>16)&255U),(unsigned)(((p)->addr>>8)&255U),(unsigned)((p)->addr&255U)
static mtx_t state_mutex,operation_mutex,model_mutex,event_mutex;
static cnd_t model_changed;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p),assert(mtx_lock(&state_mutex)==thrd_success))
#define portEXIT_CRITICAL(p) ((void)(p),assert(mtx_unlock(&state_mutex)==thrd_success))
static _Thread_local int operation_locked,thread_role;
static int failure_stage,nvs_error,deinit_error,set_config_errors,connect_error;
static int driver_live,driver_started,driver_connected,driver_configured;
static wifi_mode_t driver_mode;
static int netif_live,groups,timers,wifi_registrations,ip_registrations,inits,deinits,storage_sets;
static int set_configs,connects,erase_calls,diagnostics,warnings,reset_calls,reject_reset,reject_timer,reject_stop;
static int driver_active,driver_peak,block_config,config_entered,release_config;
static int shutdown_waiting,shutdown_done,timer_done,event_done;
static int callbacks,callback_reenter;
static model_timer_t timer_object;
static EventBits_t event_bits;
static void wifi_event_handler(void *,esp_event_base_t,int32_t,void *);

/* 每个模型进程独占这些原语，线程仅在明确屏障之后启动。 */
static void model_init(void) {
    assert(mtx_init(&state_mutex,mtx_plain)==thrd_success);
    assert(mtx_init(&operation_mutex,mtx_plain)==thrd_success);
    assert(mtx_init(&model_mutex,mtx_plain)==thrd_success);
    assert(mtx_init(&event_mutex,mtx_plain)==thrd_success);
    assert(cnd_init(&model_changed)==thrd_success);
}
static int fails(int stage) {return failure_stage==stage;}
static void warning(void) {mtx_lock(&model_mutex);warnings++;mtx_unlock(&model_mutex);}
#define ESP_LOGW(...) warning()
#define ESP_LOGE(...) warning()
#define ESP_LOGI(...) ((void)0)
static const char *esp_err_to_name(int error) {(void)error;return "model error";}
static cc_result_t result_with_message(int code) {
    char *text=malloc(11);assert(text);memcpy(text,"diagnostic",11);
    mtx_lock(&model_mutex);diagnostics++;mtx_unlock(&model_mutex);
    return (cc_result_t){code,text};
}
static cc_result_t cc_result_ok(void) {return (cc_result_t){0,NULL};}
static cc_result_t cc_result_errf(int code,const char *format,...) {(void)format;return result_with_message(code);}
static void cc_result_free(cc_result_t *result) {
    if(result->message) {mtx_lock(&model_mutex);diagnostics--;mtx_unlock(&model_mutex);free(result->message);}
    result->message=NULL;result->code=0;
}
static cc_result_t cc_mutex_init_once(cc_mutex_t *slot) {
    mtx_lock(&model_mutex);if(!*slot) *slot=&operation_mutex;mtx_unlock(&model_mutex);
    return result_with_message(CC_OK);
}
static BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex,TickType_t wait) {
    assert(mutex==&operation_mutex);
    if(thread_role==2 && wait==portMAX_DELAY) {
        mtx_lock(&model_mutex);shutdown_waiting=1;cnd_broadcast(&model_changed);mtx_unlock(&model_mutex);
    }
    int result=wait ? mtx_lock(mutex) : mtx_trylock(mutex);
    if(result==thrd_success) operation_locked=1;
    return result==thrd_success;
}
static BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
    assert(operation_locked);operation_locked=0;return mtx_unlock(mutex)==thrd_success;
}
static TaskHandle_t xTaskGetCurrentTaskHandle(void) {return (void *)(uintptr_t)thrd_current();}
static void driver_begin(void) {
    assert(operation_locked);
    mtx_lock(&model_mutex);driver_active++;if(driver_active>driver_peak)driver_peak=driver_active;
    assert(driver_active==1);mtx_unlock(&model_mutex);
}
static void driver_end(void) {mtx_lock(&model_mutex);driver_active--;mtx_unlock(&model_mutex);}
static void *heap_caps_calloc(size_t count,size_t size,int caps) {(void)caps;return calloc(count,size);}
static bool esp_ptr_external_ram(const void *pointer) {(void)pointer;return false;}
static int nvs_flash_init(void) {return nvs_error ? nvs_error : fails(1) ? ESP_ERR_NVS_NO_FREE_PAGES : ESP_OK;}
static int nvs_flash_erase(void) {erase_calls++;return ESP_OK;}
static int esp_netif_init(void) {return fails(2) ? ESP_ERR_NO_MEM : ESP_OK;}
static int esp_event_loop_create_default(void) {return fails(3) ? ESP_ERR_NO_MEM : ESP_OK;}
static esp_netif_t *esp_netif_new(const esp_netif_config_t *configuration) {
    if(fails(configuration->role ? 7 : 4)) return NULL;
    esp_netif_t *netif=malloc(sizeof(*netif));assert(netif);netif->role=configuration->role;netif_live++;return netif;
}
static int esp_netif_attach_wifi_station(esp_netif_t *netif) {(void)netif;return fails(5) ? ESP_FAIL : ESP_OK;}
static int esp_netif_attach_wifi_ap(esp_netif_t *netif) {(void)netif;return fails(8) ? ESP_FAIL : ESP_OK;}
static int esp_wifi_set_default_wifi_sta_handlers(void) {return fails(6) ? ESP_ERR_NO_MEM : ESP_OK;}
static int esp_wifi_set_default_wifi_ap_handlers(void) {return fails(9) ? ESP_ERR_NO_MEM : ESP_OK;}
static void esp_netif_destroy_default_wifi(esp_netif_t *netif) {assert(netif);netif_live--;free(netif);}
static int esp_netif_str_to_ip4(const char *text,esp_ip4_addr_t *ip) {(void)text;ip->addr=0xc0a80401U;return ESP_OK;}
static int esp_netif_dhcps_stop(esp_netif_t *netif) {(void)netif;return ESP_OK;}
static int esp_netif_dhcps_start(esp_netif_t *netif) {(void)netif;return ESP_OK;}
static int esp_netif_set_ip_info(esp_netif_t *netif,const esp_netif_ip_info_t *info) {(void)netif;(void)info;return ESP_OK;}
static int esp_wifi_init(wifi_init_config_t *configuration) {
    (void)configuration;driver_begin();inits++;
    int result=fails(10) ? ESP_ERR_NO_MEM : ESP_OK;
    if(!result){assert(!driver_live);driver_live=1;}driver_end();return result;
}
static int esp_wifi_set_storage(int storage) {
    assert(storage==WIFI_STORAGE_RAM);driver_begin();assert(driver_live);storage_sets++;
    int result=fails(11) ? ESP_FAIL : ESP_OK;driver_end();return result;
}
static int esp_event_handler_instance_register(esp_event_base_t base,int32_t id,
    void (*callback)(void *,esp_event_base_t,int32_t,void *),void *arg,void *instance) {
    (void)id;(void)callback;(void)arg;(void)instance;
    if(fails(base==WIFI_EVENT ? 14 : 15))return ESP_ERR_NO_MEM;
    if(base==WIFI_EVENT)wifi_registrations++;else ip_registrations++;return ESP_OK;
}
static void *xEventGroupCreate(void) {if(fails(12))return NULL;groups++;return &event_bits;}
static EventBits_t xEventGroupClearBits(void *group,EventBits_t bits) {
    assert(group==&event_bits);mtx_lock(&event_mutex);EventBits_t old=event_bits;event_bits&=~bits;mtx_unlock(&event_mutex);return old;
}
static EventBits_t xEventGroupSetBits(void *group,EventBits_t bits) {
    assert(group==&event_bits);mtx_lock(&event_mutex);event_bits|=bits;EventBits_t result=event_bits;mtx_unlock(&event_mutex);return result;
}
static EventBits_t xEventGroupWaitBits(void *group,EventBits_t bits,int clear,int all,TickType_t wait) {
    (void)bits;(void)clear;(void)all;assert(group==&event_bits && wait<portMAX_DELAY);
    mtx_lock(&event_mutex);EventBits_t result=event_bits;mtx_unlock(&event_mutex);return result;
}
static TimerHandle_t xTimerCreate(const char *name,TickType_t period,int repeat,void *data,void (*callback)(TimerHandle_t)) {
    (void)name;(void)repeat;(void)data;(void)callback;if(fails(13))return NULL;
    timers++;timer_object.period=period;return &timer_object;
}
static int xTimerStop(TimerHandle_t timer,TickType_t wait) {
    assert(timer==&timer_object && !wait);mtx_lock(&model_mutex);
    int result=!reject_stop;if(result)timer->active=false;mtx_unlock(&model_mutex);return result;
}
static int xTimerChangePeriod(TimerHandle_t timer,TickType_t period,TickType_t wait) {
    assert(timer==&timer_object && !wait && period>0 && period<portMAX_DELAY);
    mtx_lock(&model_mutex);timer->period=period;timer->active=!reject_timer;int result=!reject_timer;mtx_unlock(&model_mutex);return result;
}
static int xTimerReset(TimerHandle_t timer,TickType_t wait) {
    assert(timer==&timer_object && !wait);mtx_lock(&model_mutex);reset_calls++;
    int result=!reject_reset;timer->active=result;mtx_unlock(&model_mutex);return result;
}
static int esp_wifi_get_mac(int interface,uint8_t mac[6]) {
    (void)interface;driver_begin();memset(mac,0x12,6);driver_end();return ESP_OK;
}
static int esp_wifi_get_mode(wifi_mode_t *mode) {driver_begin();*mode=driver_mode;driver_end();return ESP_OK;}
static int esp_wifi_set_mode(wifi_mode_t mode) {driver_begin();driver_mode=mode;driver_end();return ESP_OK;}
static int esp_wifi_set_config(int interface,const wifi_config_t *configuration) {
    (void)configuration;driver_begin();set_configs++;
    if(thread_role==1 && block_config && interface==WIFI_IF_AP) {
        mtx_lock(&model_mutex);config_entered=1;cnd_broadcast(&model_changed);
        while(!release_config)cnd_wait(&model_changed,&model_mutex);
        mtx_unlock(&model_mutex);
    }
    int result=ESP_OK;
    if(set_config_errors){set_config_errors--;result=ESP_FAIL;}else if(interface==WIFI_IF_STA)driver_configured=1;
    driver_end();return result;
}
static int esp_wifi_start(void) {
    driver_begin();assert(driver_live);int result=driver_started ? ESP_ERR_WIFI_STATE : ESP_OK;driver_started=1;
    wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_START,NULL);driver_end();return result;
}
static int esp_wifi_connect(void) {
    driver_begin();connects++;int result=connect_error ? ESP_FAIL : driver_connected ? ESP_ERR_WIFI_CONN : ESP_OK;
    if(!result){assert(driver_live && driver_started && driver_configured);driver_connected=1;
        ip_event_got_ip_t event={.ip_info.ip.addr=0x0a000001U};wifi_event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&event);}
    driver_end();return result;
}
static int esp_wifi_disconnect(void) {
    driver_begin();int result=driver_live ? ESP_OK : ESP_ERR_WIFI_NOT_INIT;driver_connected=0;
    if(driver_live){wifi_event_sta_disconnected_t event={.reason=7};wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);}
    driver_end();return result;
}
static int esp_wifi_stop(void) {
    driver_begin();int result=driver_live ? ESP_OK : ESP_ERR_WIFI_NOT_INIT;driver_started=driver_connected=0;
    if(driver_live)wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_STOP,NULL);
    driver_end();return result;
}
static int esp_wifi_deinit(void) {
    driver_begin();deinits++;int result=deinit_error ? ESP_FAIL : driver_live ? ESP_OK : ESP_ERR_WIFI_NOT_INIT;
    if(result!=ESP_FAIL){driver_live=driver_started=driver_connected=driver_configured=0;}driver_end();return result;
}
static int esp_wifi_set_ps(int mode) {(void)mode;driver_begin();driver_end();return ESP_OK;}
static void wifi_event_callback_dispatch(wifi_app_event_t event,const wifi_app_status_t *status) {
    assert(!operation_locked && status);
    mtx_lock(&model_mutex);callbacks++;
    bool reenter=callback_reenter && event==WIFI_APP_EVENT_CONNECTED;
    if(reenter)callback_reenter=0;
    mtx_unlock(&model_mutex);
    if(reenter){wifi_app_status_t snapshot;wifi_app_get_status(&snapshot);wifi_app_prepare_for_reboot();}
}
'''

INITIALIZATION = r'''
int main(void) {
    model_init();
    for(int stage=1;stage<=15;stage++) {
        failure_stage=stage;assert(wifi_operation_enter(true));
        assert(init_wifi_once()!=ESP_OK);wifi_operation_leave();
        assert(!s_wifi_ready && netif_live<=2 && groups<=1 && timers<=1 && wifi_registrations<=1 && ip_registrations<=1);
    }
    failure_stage=0;assert(wifi_operation_enter(true));assert(init_wifi_once()==ESP_OK);wifi_operation_leave();
    assert(netif_live==2 && groups==1 && timers==1 && wifi_registrations==1 && ip_registrations==1 && inits==2);
    network_config_t configuration={.enabled=true};memcpy(configuration.ssid,"station",8);
    assert(wifi_app_connect_from_config(&configuration));
    for(int cycle=0;cycle<3;cycle++) {
        int previous=set_configs;
        assert(wifi_app_shutdown_for_ota()==ESP_OK);
        assert(!driver_live && !s_wifi_driver_initialized && !s_wifi_config_valid);
        assert(wifi_app_connect_from_config(&configuration) && set_configs>previous);
        assert(netif_live==2 && groups==1 && timers==1 && wifi_registrations==1 && ip_registrations==1);
    }
    deinit_error=1;assert(wifi_app_shutdown_for_ota()==ESP_FAIL);
    assert(s_wifi_driver_initialized && s_wifi_config_valid);
    deinit_error=0;assert(wifi_app_shutdown_for_ota()==ESP_OK);
    assert(wifi_app_connect_from_config(&configuration));driver_live=0;
    assert(wifi_app_shutdown_for_ota()==ESP_OK && !s_wifi_driver_initialized && !s_wifi_config_valid);
    assert(wifi_app_connect_from_config(&configuration));assert(wifi_app_shutdown_for_ota()==ESP_OK);
    nvs_error=ESP_ERR_NVS_NEW_VERSION_FOUND;
    cc_result_t result=storage_nvs_service_check();assert(result.code==CC_ERR_STORAGE);cc_result_free(&result);
    nvs_error=0;result=storage_nvs_service_check();assert(result.code==CC_OK);cc_result_free(&result);
    assert(!erase_calls && !diagnostics && !driver_active);
    puts("Wi-Fi staged init, retained NVS and repeated OTA recovery: PASS");return 0;
}
'''

CONFIGURATION = r'''
int main(void) {
    model_init();network_config_t configuration={.enabled=true};memcpy(configuration.ssid,"station",8);
    set_config_errors=1;assert(!wifi_app_connect_from_config(&configuration));
    assert(!s_wifi_config_valid);int previous=set_configs;
    callback_reenter=1;
    assert(wifi_app_connect_from_config(&configuration) && set_configs>previous && !callback_reenter);
    assert(!s_wifi_keepalive && callbacks>0 && diagnostics==0);
    assert(wifi_timeout_ticks(1)==1 && wifi_timeout_ticks(UINT32_MAX)==429496730U);
    configuration.max_retry=INT_MAX;
    assert(wifi_app_connect_from_config(&configuration));
    portENTER_CRITICAL(&s_wifi_state_lock);s_wifi_retry_count=INT_MAX;portEXIT_CRITICAL(&s_wifi_state_lock);
    wifi_event_sta_disconnected_t disconnected={.reason=8};
    wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&disconnected);
    assert(s_wifi_retry_count==INT_MAX);
    reject_timer=1;int previous_warnings=warnings;wifi_schedule_reconnect("queue-full");assert(warnings>previous_warnings);
    reject_stop=1;previous_warnings=warnings;int previous_connects=connects;
    wifi_app_prepare_for_reboot();assert(warnings>previous_warnings && !s_wifi_keepalive);
    wifi_reconnect_timer_cb(&timer_object);assert(connects==previous_connects);reject_stop=0;
    configuration.ssid[32]='x';memset(configuration.ssid,'x',32);
    assert(!wifi_app_connect_from_config(&configuration));
    assert(wifi_app_shutdown_for_ota()==ESP_OK && !diagnostics);
    puts("Wi-Fi configuration retry, callback reentry and finite ticks: PASS");return 0;
}
'''

CONCURRENCY = r'''
static void wait_for(int *flag) {
    struct timespec deadline;timespec_get(&deadline,TIME_UTC);deadline.tv_sec+=2;
    mtx_lock(&model_mutex);while(!*flag)assert(cnd_timedwait(&model_changed,&model_mutex,&deadline)==thrd_success);mtx_unlock(&model_mutex);
}
static int configure_thread(void *data) {thread_role=1;assert(wifi_app_connect_from_config(data));return 0;}
static int shutdown_thread(void *data) {
    (void)data;thread_role=2;assert(wifi_app_shutdown_for_ota()==ESP_OK);
    mtx_lock(&model_mutex);shutdown_done=1;cnd_broadcast(&model_changed);mtx_unlock(&model_mutex);return 0;
}
static int timer_thread(void *data) {
    (void)data;wifi_reconnect_timer_cb(&timer_object);
    mtx_lock(&model_mutex);timer_done=1;cnd_broadcast(&model_changed);mtx_unlock(&model_mutex);return 0;
}
static int event_thread(void *data) {
    (void)data;wifi_event_sta_disconnected_t event={.reason=9};
    wifi_event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);
    wifi_app_status_t snapshot;wifi_app_get_status(&snapshot);assert(strcmp(snapshot.ssid,"station")==0);
    mtx_lock(&model_mutex);event_done=1;cnd_broadcast(&model_changed);mtx_unlock(&model_mutex);return 0;
}
int main(void) {
    model_init();network_config_t configuration={.enabled=true,.softap_enabled=true};
    memcpy(configuration.ssid,"station",8);memcpy(configuration.softap_password,"password",9);
    block_config=1;thrd_t configure,shutdown,timer,event;
    assert(thrd_create(&configure,configure_thread,&configuration)==thrd_success);wait_for(&config_entered);
    reject_reset=1;
    assert(thrd_create(&timer,timer_thread,NULL)==thrd_success);wait_for(&timer_done);
    assert(thrd_join(timer,NULL)==thrd_success && reset_calls==1 && warnings>0);
    assert(thrd_create(&event,event_thread,NULL)==thrd_success);wait_for(&event_done);assert(thrd_join(event,NULL)==thrd_success);
    assert(thrd_create(&shutdown,shutdown_thread,NULL)==thrd_success);wait_for(&shutdown_waiting);
    mtx_lock(&model_mutex);assert(!shutdown_done && driver_peak==1);release_config=1;cnd_broadcast(&model_changed);mtx_unlock(&model_mutex);
    assert(thrd_join(configure,NULL)==thrd_success && thrd_join(shutdown,NULL)==thrd_success);
    assert(shutdown_done && !driver_live && driver_peak==1 && !driver_active && !diagnostics);
    puts("Wi-Fi timer nonblocking admission, event snapshots and serialized shutdown: PASS");return 0;
}
'''


def production_model():
    """组合实际配置/公共头及两份完整生产 C，模型只替换平台 API。"""
    def without_includes(relative):
        """保留文件正文及条件编译，仅去掉由模型提供的本地头引用。"""
        text = (ROOT / relative).read_text(encoding="utf-8")
        return re.sub(r"^#(?:include|pragma).*?$", "", text, flags=re.M)

    configuration = without_includes("main/services/network_config.h")
    public = without_includes("main/services/wifi_app.h")
    service = without_includes("main/services/wifi_app.c")
    nvs = without_includes("main/services/storage_nvs_service.c")
    split = BASE.index("/* 每个模型进程")
    return BASE[:split] + configuration + public + BASE[split:] + service + nvs


def main():
    """分别运行阶段失败/OTA、同参重试/回调重入和确定并发三个生产模型。"""
    model = production_model()
    for scenario in (INITIALIZATION, CONFIGURATION, CONCURRENCY):
        run_c_test(model + scenario)


if __name__ == "__main__":
    main()
