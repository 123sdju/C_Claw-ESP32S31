#include "web_console.h"
#include "web_console_internal.h"
#include "ota_service.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(CC_TEST_WEB_LIMITS)
#include "app_profile.h"
#include "esp_timer.h"
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
/** @brief HTTPD模型拥有的静态路由描述副本和监听端口。 */
typedef struct fake_server { httpd_uri_t routes[20]; int count; unsigned port; } fake_server_t;
static int starts, stops, fail_start, fail_stop, fail_ota, fail_register_at;
static int protected_calls, denied_calls;
static pthread_mutex_t stop_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stop_cond=PTHREAD_COND_INITIALIZER;
static int block_stop, stop_entered, release_stop;
/** @brief 独立线程执行真实停止入口。 @param[out] arg 借用结果槽，覆盖线程执行期。 @return NULL。 */
static void *stop_thread(void *arg);

#if defined(CC_TEST_WEB_LIMITS)
static int observed_limit;
static int submitted_messages;
static int publisher_stop;

/** @brief 用libc堆模拟消息PSRAM缓冲。 @param[in] size 请求字节数。 @return 拥有对象或NULL，真实handler负责释放。 */
char *web_console_psram_alloc(size_t size) { return malloc(size); }

/**
 * @brief 为真实请求期限取得主机单调微秒时间。
 * @return CLOCK_MONOTONIC微秒，模型要求时钟读取成功。
 */
int64_t esp_timer_get_time(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now)!=0) abort();
    return (int64_t)now.tv_sec*1000000LL+now.tv_nsec/1000;
}

/**
 * @brief 生成禁用的性能模型span，真实handler仍执行其profile分支判断。
 * @param[in] category 借用分类，忽略。
 * @param[in] name 借用操作名，忽略。
 * @param[in] session 借用会话，允许NULL，忽略。
 * @param[in] detail 借用细节，允许NULL，忽略。
 * @return 无动态成员的禁用span。
 */
app_profile_span_t app_profile_span_begin(const char *category,const char *name,
    const char *session,const char *detail)
{
    (void)category;(void)name;(void)session;(void)detail;
    return (app_profile_span_t){0};
}

/**
 * @brief 接受真实handler的性能结束调用，模型不发布事件。
 * @param[in,out] span 借用span，忽略。
 * @param[in] status 借用状态，忽略。
 * @post 无资源或共享状态变化。
 */
void app_profile_span_end(app_profile_span_t *span,const char *status)
{ (void)span;(void)status; }

/**
 * @brief 返回拥有状态正文，交由真实状态handler释放。
 * @param[in] user_data 未使用，允许NULL。
 * @return strdup拥有对象或NULL。
 */
static char *status_json(void *user_data) { (void)user_data;return strdup("{}"); }

/**
 * @brief 记录真实事件handler传入的默认或显式限制。
 * @param[in] since 查询游标，模型忽略。
 * @param[in] limit 待验证限制，写入observed_limit。
 * @param[in] kind_mask 查询掩码，模型忽略。
 * @param[in] user_data 未使用，允许NULL。
 * @return strdup拥有JSON，真实handler负责free。
 */
static char *events_json(uint32_t since,int limit,uint32_t kind_mask,void *user_data)
{
    (void)since;(void)kind_mask;(void)user_data;
    observed_limit=limit;
    return strdup("{}");
}

/**
 * @brief 在真实输入handler的借用期验证完整消息并计数。
 * @param[in] text 必需借用的64字节NUL结尾正文，不在回调后保留。
 * @param[in] user_data 未使用，允许NULL。
 * @return 正文长度正确为ESP_OK，否则ESP_FAIL。
 */
static esp_err_t message_submitted(const char *text,void *user_data)
{
    (void)user_data;
    if (strlen(text)!=64) return ESP_FAIL;
    submitted_messages++;
    return ESP_OK;
}

/**
 * @brief 在独立配置发布线程交替写入两组有效限制。
 * @param[in] arg 未使用，允许NULL。
 * @return 停止标志原子发布后返回NULL，不访问任何配置对象或Web上下文。
 */
static void *limits_publisher(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&publisher_stop,__ATOMIC_ACQUIRE)) {
        web_console_publish_limits(17,32);
        web_console_publish_limits(43,96);
    }
    return NULL;
}

/**
 * @brief 验证原IO释放后真实handler、热发布及restart继续安全使用独立状态。
 * @return 全部检查满足为零，失败为一，成功路径join发布线程并destroy服务。
 * @note HTTPD、profile与PSRAM为主机替身，两个生产Web源参与执行，未验证真实socket与设备调度。
 */
int main(void)
{
    app_runtime_io_config_t *io=calloc(1,sizeof(*io)); CHECK(io);
    io->web.enabled=1; io->web.port=8089; io->web.event_limit=71; io->max_input_bytes=512;
    web_console_context_t context={.io=io,.status_json=status_json,.events_json=events_json,
        .message_submitted=message_submitted};
    CHECK(web_console_start(&context)==ESP_OK && g_web_console_context.io!=io);
    memset(io,0xa5,sizeof(*io));free(io);context.io=NULL;
    httpd_req_t request={0};
    CHECK(web_console_status_handler(&request)==ESP_OK && request.response_status==200);
    CHECK(web_console_events_handler(&request)==ESP_OK && observed_limit==71);
    web_console_publish_limits(17,32);
    pthread_t publisher;CHECK(pthread_create(&publisher,NULL,limits_publisher,NULL)==0);
    char body[65];memset(body,'x',64);body[64]='\0';
    for (int i=0;i<1000;i++) {
        request=(httpd_req_t){0};
        CHECK(web_console_events_handler(&request)==ESP_OK && (observed_limit==17 || observed_limit==43));
        request=(httpd_req_t){.content_len=64,.body=body};
        esp_err_t result=web_console_input_handler(&request);
        CHECK((result==ESP_OK && request.response_status==200 && request.received==64) ||
              (result==ESP_FAIL && request.response_status==400 && request.received==0));
    }
    __atomic_store_n(&publisher_stop,1,__ATOMIC_RELEASE);
    CHECK(pthread_join(publisher,NULL)==0);
    web_console_publish_limits(43,96);
    request=(httpd_req_t){.limit_text="7"};
    CHECK(web_console_events_handler(&request)==ESP_OK && observed_limit==7);
    CHECK(web_console_stop()==ESP_OK && web_console_restart()==ESP_OK);
    CHECK(((fake_server_t *)g_web_console_server)->port==8089 &&
        web_console_event_limit()==43 && web_console_max_input_bytes()==96);
    request=(httpd_req_t){.content_len=64,.body=body};
    CHECK(web_console_input_handler(&request)==ESP_OK && submitted_messages>0);

    pthread_mutex_lock(&stop_mutex);block_stop=1;release_stop=0;stop_entered=0;pthread_mutex_unlock(&stop_mutex);
    pthread_t stopper;esp_err_t stop_result=ESP_FAIL;
    CHECK(pthread_create(&stopper,NULL,stop_thread,&stop_result)==0);
    pthread_mutex_lock(&stop_mutex);
    struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec+=5;
    while (!stop_entered && pthread_cond_timedwait(&stop_cond,&stop_mutex,&deadline)==0) {}
    int entered=stop_entered;pthread_mutex_unlock(&stop_mutex);CHECK(entered);
    web_console_publish_limits(29,77);
    CHECK(web_console_event_limit()==29 && web_console_max_input_bytes()==77);
    pthread_mutex_lock(&stop_mutex);release_stop=1;pthread_cond_broadcast(&stop_cond);pthread_mutex_unlock(&stop_mutex);
    CHECK(pthread_join(stopper,NULL)==0 && stop_result==ESP_OK);
    CHECK(web_console_restart()==ESP_OK && web_console_event_limit()==29 && web_console_max_input_bytes()==77);
    CHECK(web_console_destroy()==ESP_OK);block_stop=0;

    app_runtime_io_config_t startup_io={0};
    startup_io.web.enabled=1;startup_io.web.port=8077;startup_io.web.event_limit=71;startup_io.max_input_bytes=512;
    web_console_context_t startup={.io=&startup_io,.startup_only=true};
    CHECK(web_console_start(&startup)==ESP_OK);
    web_console_publish_limits(19,511);
    app_runtime_io_config_t normal_io={0};
    normal_io.web.enabled=1;normal_io.web.port=9999;normal_io.web.event_limit=91;normal_io.max_input_bytes=2048;
    web_console_context_t normal={.io=&normal_io};
    bool restored=false;fail_ota=1;
    CHECK(web_console_promote_startup(&normal,&restored)==ESP_FAIL && restored &&
        g_web_console_context.startup_only && g_web_console_context.io->web.port==8077 &&
        g_web_console_context.io->max_input_bytes==512 && web_console_event_limit()==19 &&
        web_console_max_input_bytes()==511);
    CHECK(web_console_destroy()==ESP_OK);
    return 0;
}
#endif

/** @brief 满足真实server配置所需的模型匹配入口。 @param[in] a 借用模板，忽略。 @param[in] b 借用路径，忽略。 @param[in] n 长度，忽略。 @return 固定零。 */
int httpd_uri_match_wildcard(const char *a,const char *b,size_t n) { (void)a;(void)b;(void)n;return 0; }
/**
 * @brief 将借用正文分片送给真实请求接收循环。
 * @param[in,out] req 可选模型请求，更新 received。
 * @param[out] buffer 至少 length 字节的可写借用缓冲。
 * @param[in] length 本次最大接收字节数，非正数返回零。
 * @return 已复制字节数，无正文或已耗尽时返回零。
 */
int httpd_req_recv(httpd_req_t *req,char *buffer,int length)
{
    if (!req || !req->body || length <= 0 || req->received >= req->content_len) return 0;
    size_t count=req->content_len-req->received;
    if (count>(size_t)length) count=(size_t)length;
    memcpy(buffer,req->body+req->received,count);
    req->received+=count;
    return (int)count;
}
/**
 * @brief 创建模型server并复制监听端口，支持一次启动故障。
 * @param[out] out 成功交付拥有server，分配失败置 NULL，注入失败保持原槽。
 * @param[in] config 借用启动配置，仅复制端口。
 * @return 成功为 ESP_OK，故障为 ESP_FAIL，分配失败为 NO_MEM。
 */
esp_err_t httpd_start(httpd_handle_t *out,const httpd_config_t *config)
{
    starts++;
    if (fail_start) { fail_start--; return ESP_FAIL; }
    fake_server_t *server=calloc(1,sizeof(*server));
    if (server) server->port=config->server_port;
    *out=server; return *out ? ESP_OK : ESP_ERR_NO_MEM;
}
/**
 * @brief 在可控原生停止屏障后释放模型server，支持失败保留句柄。
 * @param[in,out] handle 拥有模型server，成功时消费，失败时保持。
 * @return ESP_OK 或一次注入的 ESP_FAIL。
 * @note block_stop 模拟 HTTPD 等待 handler 退场，控制标志以 stop_mutex 同步。
 */
esp_err_t httpd_stop(httpd_handle_t handle)
{
    stops++;
    if (fail_stop) { fail_stop--; return ESP_FAIL; }
    pthread_mutex_lock(&stop_mutex);
    if (block_stop) {
        stop_entered=1; pthread_cond_broadcast(&stop_cond);
        while (!release_stop) pthread_cond_wait(&stop_cond,&stop_mutex);
    }
    pthread_mutex_unlock(&stop_mutex);
    free(handle); return ESP_OK;
}
/**
 * @brief 在模型server保存路由，支持指定序号注册失败。
 * @param[in,out] handle 借用存活server。
 * @param[in] route 借用路由描述，按值保存字段，路径和入口继续借用。
 * @return 成功为 ESP_OK，注入失败为 FAIL，容量耗尽为 NO_MEM。
 */
esp_err_t httpd_register_uri_handler(httpd_handle_t handle,const httpd_uri_t *route)
{
    fake_server_t *server=handle;
    if (fail_register_at && server->count+1==fail_register_at) { fail_register_at=0; return ESP_FAIL; }
    if (server->count>=20) return ESP_ERR_NO_MEM;
    server->routes[server->count++]=*route; return ESP_OK;
}
/** @brief 模拟OTA传输初始化故障。 @param[in] transport 借用适配器，模型不保留。 @return ESP_OK 或一次注入的 ESP_FAIL。 */
esp_err_t ota_service_init(const ota_service_transport_t *transport)
{ (void)transport; if (fail_ota) { fail_ota--; return ESP_FAIL; } return ESP_OK; }
/** @brief 用libc堆模拟内部缓冲。 @param[in] size 字节数。 @return malloc拥有对象或 NULL，调用者释放。 */
char *web_console_internal_alloc(size_t size) { return malloc(size); }
/**
 * @brief 仅为模型请求提供可选的显式limit查询值。
 * @param[in] req 可选借用请求。
 * @param[in] key 借用键名。
 * @return limit命中时交付strdup拥有副本，其他键或无值返回 NULL。
 */
char *web_console_query_value(httpd_req_t *req,const char *key)
{ return req && req->limit_text && strcmp(key,"limit")==0 ? strdup(req->limit_text) : NULL; }
/**
 * @brief 记录成功响应，正文不保留。
 * @param[in,out] req 可选模型请求，非NULL写200。
 * @param[in] text 调用期间借用正文，模型忽略。
 * @param[in] type 调用期间借用类型，模型忽略。
 * @return ESP_OK。
 */
esp_err_t web_console_send_text(httpd_req_t *req,const char *text,const char *type)
{ if (req) req->response_status=200; (void)text;(void)type;return ESP_OK; }
/**
 * @brief 记录错误响应和拒绝次数。
 * @param[in,out] req 可选模型请求，非NULL记录code。
 * @param[in] code 响应状态。
 * @param[in] message 借用描述，模型忽略。
 * @return 固定ESP_FAIL，便于生命周期断言区分拒绝路径。
 */
esp_err_t web_console_send_error(httpd_req_t *req,httpd_err_code_t code,const char *message)
{ if (req) req->response_status=code; (void)message;denied_calls++;return ESP_FAIL; }
/**
 * @brief 为非被测业务路由生成计数占位handler。
 * @param[in] req 借用请求，模型忽略，允许NULL。
 * @return ESP_OK，并递增protected_calls以核对就绪门禁。
 */
#define HANDLER(name) esp_err_t web_console_##name##_handler(httpd_req_t *req) { (void)req;protected_calls++;return ESP_OK; }
#if !defined(CC_TEST_WEB_LIMITS)
HANDLER(status) HANDLER(events) HANDLER(input)
#endif
HANDLER(config_get) HANDLER(config_put)
HANDLER(files) HANDLER(file) HANDLER(mkdir) HANDLER(ota_status) HANDLER(ota_prepare)
HANDLER(ota_upload) HANDLER(ota_select) HANDLER(ota_reboot) HANDLER(ota_cancel)
#undef HANDLER

/**
 * @brief 从模型server查找已注册路由。
 * @param[in,out] server 必需存活借用server。
 * @param[in] path 借用完整路径。
 * @param[in] method 待匹配方法。
 * @return 指向server内部记录的借用指针，未命中为NULL，server释放后失效。
 */
static httpd_uri_t *find_route(fake_server_t *server,const char *path,httpd_method_t method)
{
    for (int i=0;i<server->count;i++)
        if (server->routes[i].method==method && strcmp(server->routes[i].uri,path)==0) return &server->routes[i];
    return NULL;
}
/**
 * @brief 从独立线程执行真实Web停止入口。
 * @param[out] arg 必需借用esp_err_t槽，主线程保管至join。
 * @return NULL，停止结果写输出。
 */
static void *stop_thread(void *arg)
{ *(esp_err_t *)arg=web_console_stop(); return NULL; }

#if !defined(CC_TEST_WEB_LIMITS)
/**
 * @brief 核对启动提升、部分路由失败、可重试停止和并发切换的资源寿命。
 * @return 断言全部满足为零，CHECK失败返回一，进程退出回收失败场景残留资源。
 * @note HTTPD与OTA为主机模型，真实Web生命周期实现参与执行，未覆盖设备socket调度。
 */
int main(void)
{
    app_runtime_io_config_t io={0}; io.web.enabled=1; io.web.port=8080;
    io.web.event_limit=71; io.max_input_bytes=512;
    app_runtime_io_config_t *stage_io=malloc(sizeof(*stage_io)); CHECK(stage_io);
    *stage_io=io; stage_io->session_id=(char *)"borrowed-stage";
    web_console_context_t startup={.io=stage_io,.startup_only=true};
    web_console_context_t normal={.io=&io,.startup_only=false};
    CHECK(web_console_start(&startup)==ESP_OK && g_web_console_server && g_web_console_context.startup_only);
    fake_server_t *server=g_web_console_server;
    CHECK(g_web_console_context.io && g_web_console_context.io!=stage_io && server->port==8080 &&
          g_web_console_context.io->web.event_limit==71 && g_web_console_context.io->max_input_bytes==512 &&
          !g_web_console_context.io->session_id);
    stage_io->web.port=9090; stage_io->max_input_bytes=1; free(stage_io); startup.io=&io;
    CHECK(g_web_console_context.io->web.port==8080 && g_web_console_context.io->max_input_bytes==512 &&
          web_console_start(&startup)==ESP_OK && starts==1);
    httpd_uri_t *input=find_route(server,"/api/messages",HTTP_POST);
    CHECK(server->count==4 && input && input->handler(NULL)==ESP_OK && !find_route(server,"/api/config",HTTP_GET));
    protected_calls=0;
    CHECK(web_console_start(&normal)==ESP_ERR_INVALID_STATE && g_web_console_server==server && starts==1);
    bool restored=false; fail_stop=1;
    CHECK(web_console_promote_startup(&normal,&restored)==ESP_FAIL && restored && g_web_console_server==server &&
          g_web_console_context.startup_only);
    fail_ota=1; restored=false;
    CHECK(web_console_promote_startup(&normal,&restored)==ESP_FAIL && restored && g_web_console_server &&
          g_web_console_context.startup_only && ((fake_server_t *)g_web_console_server)->count==4 &&
          ((fake_server_t *)g_web_console_server)->port==8080 && g_web_console_context.io->max_input_bytes==512);
    restored=true;
    CHECK(web_console_promote_startup(&normal,&restored)==ESP_OK && !restored && g_web_console_server &&
          !g_web_console_context.startup_only);
    server=g_web_console_server; httpd_uri_t *config=find_route(server,"/api/config",HTTP_GET);
    input=find_route(server,"/api/messages",HTTP_POST);
    CHECK(server->count==17 && config && input && config->handler(NULL)==ESP_OK && protected_calls==1);

    /* 停止失败保留句柄并关闭应用路由，相同自有上下文restart可重新放行完整server。 */
    fail_stop=1; CHECK(web_console_destroy()==ESP_FAIL && g_web_console_server==server && g_web_console_context.io!=&io);
    CHECK(config->handler(NULL)==ESP_FAIL && input->handler(NULL)==ESP_FAIL && denied_calls==2 && protected_calls==1);
    CHECK(web_console_restart()==ESP_OK && config->handler(NULL)==ESP_OK && protected_calls==2);

    /* 原生stop尚未退场时，其他生命周期入口立即失败，当前保存的上下文仍有效。 */
    pthread_mutex_lock(&stop_mutex); block_stop=1; stop_entered=0; release_stop=0; pthread_mutex_unlock(&stop_mutex);
    esp_err_t thread_result=ESP_FAIL; pthread_t thread; CHECK(pthread_create(&thread,NULL,stop_thread,&thread_result)==0);
    pthread_mutex_lock(&stop_mutex);
    struct timespec deadline; clock_gettime(CLOCK_REALTIME,&deadline); deadline.tv_sec+=5;
    while (!stop_entered) if (pthread_cond_timedwait(&stop_cond,&stop_mutex,&deadline)!=0) break;
    int entered=stop_entered; pthread_mutex_unlock(&stop_mutex); CHECK(entered);
    CHECK(web_console_start(&normal)==ESP_ERR_INVALID_STATE && web_console_restart()==ESP_ERR_INVALID_STATE &&
          web_console_destroy()==ESP_ERR_INVALID_STATE && g_web_console_server==server &&
          config->handler(NULL)==ESP_FAIL && input->handler(NULL)==ESP_FAIL && g_web_console_context.io!=&io);
    pthread_mutex_lock(&stop_mutex); release_stop=1; pthread_cond_broadcast(&stop_cond); pthread_mutex_unlock(&stop_mutex);
    CHECK(pthread_join(thread,NULL)==0 && thread_result==ESP_OK && !g_web_console_server);
    CHECK(web_console_destroy()==ESP_OK && !g_web_console_context.io);

    /* 注册和清理同时失败时，部分应用路由保持拒绝，再次destroy仍能清理保留句柄。 */
    block_stop=0; fail_register_at=6; fail_stop=1;
    CHECK(web_console_start(&normal)==ESP_FAIL && g_web_console_server);
    server=g_web_console_server; config=find_route(server,"/api/config",HTTP_GET);
    input=find_route(server,"/api/messages",HTTP_POST);
    CHECK(config && input && config->handler(NULL)==ESP_FAIL && input->handler(NULL)==ESP_FAIL &&
          web_console_restart()==ESP_ERR_INVALID_STATE);
    CHECK(web_console_destroy()==ESP_OK && !g_web_console_server && !g_web_console_context.io);

    /* 提升启动失败时恢复受限页面及其自有配置。 */
    CHECK(web_console_start(&startup)==ESP_OK); fail_start=1; restored=false;
    CHECK(web_console_promote_startup(&normal,&restored)==ESP_FAIL && restored &&
          g_web_console_context.startup_only && ((fake_server_t *)g_web_console_server)->count==4);
    CHECK(web_console_stop()==ESP_OK && web_console_restart()==ESP_OK && g_web_console_context.startup_only &&
          ((fake_server_t *)g_web_console_server)->port==8080 && g_web_console_context.io->max_input_bytes==512);
    CHECK(web_console_destroy()==ESP_OK); return 0;
}
#endif
