#define _POSIX_C_SOURCE 200809L
#include "wifi_event_callback.h"
#include "freertos/task.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
TickType_t xTaskGetTickCount(void)
{
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    return (TickType_t)((uint64_t)now.tv_sec*1000U+(uint64_t)now.tv_nsec/1000000U);
}
void vTaskDelay(TickType_t ticks)
{
    struct timespec delay={.tv_sec=ticks/1000U,.tv_nsec=(long)(ticks%1000U)*1000000L};
    nanosleep(&delay,NULL);
}

typedef struct fixture {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int entered, released, calls, wrong_input, self_detach;
    esp_err_t self_result;
} fixture_t;

static void callback(wifi_app_event_t event,const wifi_app_status_t *status,void *data)
{
    fixture_t *f=data;
    pthread_mutex_lock(&f->mutex);
    f->calls++;
    if (event!=WIFI_APP_EVENT_CONNECTED || !status || !status->connected) f->wrong_input=1;
    if (f->self_detach) {
        pthread_mutex_unlock(&f->mutex);
        f->self_result=wifi_app_clear_event_callback(0);
        return;
    }
    f->entered=1; pthread_cond_broadcast(&f->condition);
    while (!f->released) pthread_cond_wait(&f->condition,&f->mutex);
    pthread_mutex_unlock(&f->mutex);
}
static void *dispatch(void *unused)
{
    (void)unused; wifi_app_status_t status={.connected=true};
    wifi_event_callback_dispatch(WIFI_APP_EVENT_CONNECTED,&status);
    return NULL;
}
typedef struct waiter { esp_err_t result; } waiter_t;
static void *detach(void *data)
{ waiter_t *waiter=data; waiter->result=wifi_app_clear_event_callback(1000); return NULL; }

int main(void)
{
    fixture_t f={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
    CHECK(wifi_app_set_event_callback(NULL,&f)==ESP_ERR_INVALID_ARG);
    CHECK(wifi_app_set_event_callback(callback,&f)==ESP_OK);
    CHECK(wifi_app_set_event_callback(callback,&f)==ESP_ERR_INVALID_STATE);
    pthread_t running; CHECK(pthread_create(&running,NULL,dispatch,NULL)==0);
    pthread_mutex_lock(&f.mutex);
    while (!f.entered) pthread_cond_wait(&f.condition,&f.mutex);
    pthread_mutex_unlock(&f.mutex);

    CHECK(wifi_app_clear_event_callback(5)==ESP_ERR_TIMEOUT);
    wifi_app_status_t status={.connected=true};
    wifi_event_callback_dispatch(WIFI_APP_EVENT_CONNECTED,&status);
    CHECK(f.calls==1 && wifi_app_set_event_callback(callback,&f)==ESP_ERR_INVALID_STATE);
    waiter_t a={0},b={0}; pthread_t first,second;
    CHECK(pthread_create(&first,NULL,detach,&a)==0 && pthread_create(&second,NULL,detach,&b)==0);
    vTaskDelay(10);
    CHECK(wifi_app_set_event_callback(callback,&f)==ESP_ERR_INVALID_STATE);
    pthread_mutex_lock(&f.mutex); f.released=1; pthread_cond_broadcast(&f.condition); pthread_mutex_unlock(&f.mutex);
    CHECK(pthread_join(running,NULL)==0 && pthread_join(first,NULL)==0 && pthread_join(second,NULL)==0);
    CHECK(a.result==ESP_OK && b.result==ESP_OK && f.calls==1 && !f.wrong_input);

    /* A callback cannot drain itself. Its slot becomes reusable on return. */
    f.self_detach=1;
    CHECK(wifi_app_set_event_callback(callback,&f)==ESP_OK);
    wifi_event_callback_dispatch(WIFI_APP_EVENT_CONNECTED,&status);
    CHECK(f.self_result==ESP_ERR_TIMEOUT && f.calls==2);
    CHECK(wifi_app_set_event_callback(callback,&f)==ESP_OK);
    CHECK(wifi_app_clear_event_callback(0)==ESP_OK);
    pthread_cond_destroy(&f.condition); pthread_mutex_destroy(&f.mutex);
    puts("Wi-Fi callback shutdown tests passed");
    return 0;
}
