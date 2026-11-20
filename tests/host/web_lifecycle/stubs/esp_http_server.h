#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

typedef void *httpd_handle_t;
/** @brief 模型请求借用正文与查询值，记录接收进度和响应状态。 */
typedef struct httpd_req {
    size_t content_len;
    const char *body;
    size_t received;
    const char *limit_text;
    int response_status;
} httpd_req_t;
typedef enum { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE } httpd_method_t;
typedef enum { HTTPD_500_INTERNAL_SERVER_ERROR = 500, HTTPD_400_BAD_REQUEST = 400,
    HTTPD_408_REQ_TIMEOUT = 408 } httpd_err_code_t;
typedef struct { const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t *); } httpd_uri_t;
typedef struct {
    uint16_t server_port;
    int (*uri_match_fn)(const char *,const char *,size_t);
    int lru_purge_enable, max_open_sockets, recv_wait_timeout, send_wait_timeout, backlog_conn;
    int stack_size, max_uri_handlers, task_caps;
} httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define HTTPD_SOCK_ERR_TIMEOUT (-2)
#define HTTPD_400 HTTPD_400_BAD_REQUEST
#define HTTPD_500 HTTPD_500_INTERNAL_SERVER_ERROR
/** @brief 模型通配匹配占位。 @param[in] template_uri 借用模板。 @param[in] uri 借用路径。 @param[in] len 比较长度。 @return 固定零。 */
int httpd_uri_match_wildcard(const char *template_uri,const char *uri,size_t len);
/** @brief 创建模型server。 @param[out] handle 成功交付拥有对象。 @param[in] config 调用期间借用设置。 @return 成功或注入错误。 */
esp_err_t httpd_start(httpd_handle_t *handle,const httpd_config_t *config);
/** @brief 停止模型server，可在可控屏障等待。 @param[in,out] handle 拥有对象，成功消费，失败保留。 @return 停止结果。 */
esp_err_t httpd_stop(httpd_handle_t handle);
/** @brief 复制路由登记到模型server。 @param[in,out] handle 存活模型对象。 @param[in] config 借用路由，字段按值保存。 @return 登记结果。 */
esp_err_t httpd_register_uri_handler(httpd_handle_t handle,const httpd_uri_t *config);
/** @brief 从模型请求正文复制片段。 @param[in,out] req 借用请求，更新进度。 @param[out] buffer 可写借用缓冲。 @param[in] length 最大字节数。 @return 复制字节数，无正文为零。 */
int httpd_req_recv(httpd_req_t *req,char *buffer,int length);
