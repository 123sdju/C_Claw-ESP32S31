#include "app_shutdown_sequence.h"
#include "app_usb_owner_guard.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
typedef struct fixture { int fail_at, count, trace[13]; uint32_t timeout; } fixture_t;
static esp_err_t step(fixture_t *fixture,int n,uint32_t timeout)
{
    fixture->trace[fixture->count++]=n; fixture->timeout=timeout;
    return fixture->fail_at==n ? ESP_ERR_TIMEOUT : ESP_OK;
}
#define STEP(name,n) static esp_err_t name(void *context,uint32_t timeout) { return step(context,n,timeout); }
STEP(usb_owner,1) STEP(serial,2) STEP(config_drain,3) STEP(ui_callbacks,4) STEP(web,5) STEP(camera,6) STEP(interaction,7)
STEP(profile_stop,8) STEP(network,9) STEP(runtime,10) STEP(ui,11) STEP(events,12) STEP(profile_destroy,13)

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *mutex=malloc(sizeof(*mutex));
    if (!mutex) return NULL;
    if (pthread_mutex_init(mutex,NULL)!=0) { free(mutex); return NULL; }
    return mutex;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t timeout)
{
    pthread_mutex_t *mutex=handle;
    if (!timeout) return pthread_mutex_trylock(mutex)==0 ? pdTRUE : pdFALSE;
    struct timespec until;
    if (clock_gettime(CLOCK_REALTIME,&until)!=0) return pdFALSE;
    until.tv_sec+=(time_t)(timeout/1000U);
    until.tv_nsec+=(long)(timeout%1000U)*1000000L;
    if (until.tv_nsec>=1000000000L) { until.tv_sec++; until.tv_nsec-=1000000000L; }
    return pthread_mutex_timedlock(mutex,&until)==0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{ return pthread_mutex_unlock((pthread_mutex_t *)handle)==0 ? pdTRUE : pdFALSE; }
void vSemaphoreDelete(SemaphoreHandle_t handle)
{ pthread_mutex_t *mutex=handle; pthread_mutex_destroy(mutex); free(mutex); }

typedef struct guard_fixture {
    app_usb_owner_guard_t guard;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int entered, release;
} guard_fixture_t;
static void *guard_owner(void *context)
{
    guard_fixture_t *fixture=context;
    if (!app_usb_owner_guard_enter(&fixture->guard,1000)) return NULL;
    pthread_mutex_lock(&fixture->mutex);
    fixture->entered=1; pthread_cond_broadcast(&fixture->cond);
    while (!fixture->release) pthread_cond_wait(&fixture->cond,&fixture->mutex);
    pthread_mutex_unlock(&fixture->mutex);
    app_usb_owner_guard_leave(&fixture->guard);
    return NULL;
}

int main(void)
{
    const app_shutdown_ops_t ops={usb_owner,serial,config_drain,ui_callbacks,web,camera,interaction,profile_stop,network,runtime,ui,events,profile_destroy};
    app_shutdown_stage_t stage=APP_SHUTDOWN_NONE;
    for (int failure=1;failure<=13;failure++) {
        fixture_t fixture={.fail_at=failure};
        CHECK(app_shutdown_sequence_run(&ops,&fixture,1234,&stage)==ESP_ERR_TIMEOUT &&
              stage==(app_shutdown_stage_t)failure && fixture.count==failure && fixture.timeout==1234);
        for (int i=0;i<fixture.count;i++) CHECK(fixture.trace[i]==i+1);
    }
    fixture_t fixture={0};
    CHECK(app_shutdown_sequence_run(&ops,&fixture,777,&stage)==ESP_OK && stage==APP_SHUTDOWN_NONE && fixture.count==13);
    for (int i=0;i<fixture.count;i++) CHECK(fixture.trace[i]==i+1);
    app_shutdown_ops_t incomplete=ops; incomplete.runtime=NULL; memset(&fixture,0,sizeof(fixture));
    CHECK(app_shutdown_sequence_run(&incomplete,&fixture,777,&stage)==ESP_ERR_INVALID_ARG &&
          stage==APP_SHUTDOWN_NONE && fixture.count==0);

    guard_fixture_t state={0};
    CHECK(pthread_mutex_init(&state.mutex,NULL)==0 && pthread_cond_init(&state.cond,NULL)==0 &&
          app_usb_owner_guard_init(&state.guard)==ESP_OK);
    pthread_t owner; CHECK(pthread_create(&owner,NULL,guard_owner,&state)==0);
    pthread_mutex_lock(&state.mutex);
    while (!state.entered) pthread_cond_wait(&state.cond,&state.mutex);
    pthread_mutex_unlock(&state.mutex);
    CHECK(app_usb_owner_guard_close(&state.guard,30)==ESP_ERR_TIMEOUT &&
          !app_usb_owner_guard_enter(&state.guard,0));
    pthread_mutex_lock(&state.mutex); state.release=1; pthread_cond_signal(&state.cond);
    pthread_mutex_unlock(&state.mutex);
    CHECK(pthread_join(owner,NULL)==0 && app_usb_owner_guard_close(&state.guard,1000)==ESP_OK &&
          !app_usb_owner_guard_enter(&state.guard,0));
    app_usb_owner_guard_destroy(&state.guard);
    pthread_cond_destroy(&state.cond); pthread_mutex_destroy(&state.mutex);
    return 0;
}
