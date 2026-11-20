"""编译 ESP32 HTTP 生产函数，验证请求隔离、正文拥有、来源及失败清理。"""
from pathlib import Path
import unittest

from test_event_ring_ownership import run_c_test
from test_review_regressions import function


ROOT = Path(__file__).resolve().parents[1]
HTTP = "third_party/cclaw-sdk/cclaw/platforms/esp32/src/cc_esp32_http_client.c"


def production(*signatures):
    """按完整签名提取本任务生产定义，返回可直接编译的 C 文本。"""
    return "\n".join(function(HTTP, signature) for signature in signatures)


def preamble():
    """组合真实公共类型和生产私有结构，返回带拥有诊断计数的独立替身前缀。"""
    result = (ROOT / "third_party/cclaw-sdk/cclaw/core/include/cc/core/cc_result.h").read_text(encoding="utf-8")
    header = (ROOT / "third_party/cclaw-sdk/cclaw/ports/include/cc/ports/cc_http_client.h").read_text(encoding="utf-8")
    header = header.replace('#include "cc/core/cc_result.h"', '')
    text = (ROOT / HTTP).read_text(encoding="utf-8")
    types = text[text.index("typedef enum cc_esp32_http_pool_id"):text.index("static unsigned http_next_attempt_id")]
    return r'''
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int *SemaphoreHandle_t;
typedef struct fake_client *esp_http_client_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 2
#define ESP_ERR_HTTP_READ_TIMEOUT 3
#define CC_HTTP_MAX_HOST_LENGTH 255
#define CC_HTTP_MAX_REDIRECT_URL 2048
#define CC_HTTP_MAX_RESPONSE_HEADERS 64U
#define CC_HTTP_MAX_RESPONSE_HEADER_BYTES (16U * 1024U)
#define CC_HTTP_LOG_DEBUG 1
#define CC_HTTP_LOG_WARN 3
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
/** @brief 返回原生错误的固定诊断。 @param[in] error 原生错误码。 @return 静态借用文本。 */
static const char *esp_err_to_name(esp_err_t error) { (void)error; return "fake IDF error"; }
''' + result + header + r'''
static unsigned result_messages;
/** @brief 复制测试文本。 @param[in] text 稳定输入。 @return 拥有副本，分配失败终止测试。 */
static char *cc_copy_string(const char *text) {
    size_t length = strlen(text) + 1;
    char *copy = malloc(length);
    assert(copy);
    memcpy(copy, text, length);
    return copy;
}
/** @brief 构造无诊断成功结果。 @return 全部动态字段为空的成功值。 */
cc_result_t cc_result_ok(void) {
    cc_result_t result = {0}; result.size = sizeof(result); return result;
}
/** @brief 构造可计数拥有诊断。 @param[in] code 结果码。 @param[in] message 输入诊断。 @return 接收者须释放的结果。 */
cc_result_t cc_result_error(cc_error_code_t code, const char *message) {
    cc_result_t result = cc_result_ok(); result.code = code;
    result.message = cc_copy_string(message); ++result_messages; return result;
}
/** @brief 为错误分类断言构造拥有诊断。 @param[in] code 结果码。 @param[in] format 借用格式。 @return 接收者清理的结果。 */
cc_result_t cc_result_errf(cc_error_code_t code, const char *format, ...) {
    return cc_result_error(code, format);
}
/** @brief 消费测试结果消息。 @param[in,out] result 非空结果。 @post 消息计数减少且结果清零。 */
void cc_result_free(cc_result_t *result) {
    if (result->message) { assert(result_messages); --result_messages; free(result->message); }
    memset(result, 0, sizeof(*result));
}
''' + types


class Esp32HttpClientTests(unittest.TestCase):
    """用实际端口定义和原生 API 替身执行有界、无网络回归。"""

    def test_origin_redirect_and_body_capacity(self):
        """核对完整来源、重定向头剥离、相对地址及正文加法溢出保留旧状态。"""
        code = preamble() + production(
            "static int http_ascii_equal(", "static int http_origins_equal(",
            "static char *http_redirect_url(", "static int http_sensitive_redirect_header(",
            "static int http_entity_header(", "static cc_result_t http_redirect_headers(",
            "static int response_append(", "void cc_http_response_free(")
        run_c_test(code + r'''
/** @brief 执行来源及容量断言。 @return 所有断言通过时返回零。 */
int main(void) {
    char left[300], right[300];
    memset(left, 'a', sizeof(left)); memcpy(left, "https://", 8); left[250] = '\0';
    memcpy(right, left, sizeof(left)); right[240] = 'b';
    assert(!http_origins_equal(left, right));
    assert(http_origins_equal("https://Example:443/path?a", "HTTPS://example:443/other"));
    assert(!http_origins_equal("https://example", "https://example:443"));
    assert(http_origins_equal("http://[::1]:8080/a", "HTTP://[::1]:8080/b"));
    cc_http_header_t headers[] = {{"Authorization", "secret"}, {"Cookie", "private"},
        {"Content-Type", "application/json"}, {"Accept", "text/plain"}};
    cc_http_request_t request = {0}; request.headers = headers; request.header_count = 4;
    cc_http_header_t *filtered = NULL; size_t count = 99;
    cc_result_t rc = http_redirect_headers(&request, 0, 1, &filtered, &count);
    assert(rc.code == CC_OK && count == 1 && filtered[0].name == headers[3].name);
    free(filtered); cc_result_free(&rc);
    const char *locations[] = {"?new", "#new", "/next", "child"};
    const char *expected[] = {"https://host/a/file?new", "https://host/a/file?old#new",
        "https://host/next", "https://host/a/child"};
    for (size_t i = 0; i < 4; i++) {
        char *url = http_redirect_url("https://host/a/file?old#old", locations[i]);
        assert(url && strcmp(url, expected[i]) == 0); free(url);
    }
    char *url = http_redirect_url("https://host?old", "child");
    assert(url && strcmp(url, "https://host/child") == 0); free(url);
    char long_location[2048]; memset(long_location, 'x', sizeof(long_location) - 1);
    long_location[sizeof(long_location) - 1] = '\0';
    assert(http_redirect_url("https://host", long_location) == NULL);
    cc_http_response_t response = {0}; response.body = cc_copy_string("old"); response.body_size = 3;
    assert(!response_append(&response, "x", SIZE_MAX, 0));
    assert(response.body_size == 3 && strcmp(response.body, "old") == 0);
    assert(!response_append(&response, "xx", 2, 4));
    assert(response_append(&response, "x", 1, 4));
    assert(response.body_size == 4 && strcmp(response.body, "oldx") == 0);
    cc_http_response_free(&response); assert(!result_messages);
    puts("ESP HTTP complete origin, redirect and capacity: PASS"); return 0;
}
''')

    def test_reused_handle_replaces_all_headers(self):
        """两次真实请求配置共用替身 handle，检查旧认证消失及 Host/端口/IPv6 完整。"""
        code = preamble() + r'''
#define HTTP_METHOD_POST 1
#define HTTP_METHOD_GET 2
struct fake_client { cc_http_header_t headers[16]; size_t count; int method; };
/** @brief 归还替身全部头字段。 @param[in,out] client 当前 handle。 @return ESP_OK。 */
static esp_err_t esp_http_client_delete_all_headers(esp_http_client_handle_t client) {
    for (size_t i = 0; i < client->count; i++) {
        free((char *)client->headers[i].name); free((char *)client->headers[i].value);
    }
    client->count = 0; return ESP_OK;
}
/** @brief 同步复制一个请求头。 @param[in,out] client 当前 handle。 @param[in] name 头名。 @param[in] value 头值。 @return ESP_OK。 */
static esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value) {
    for (size_t i = 0; i < client->count; i++) {
        if (strcmp(client->headers[i].name, name) == 0) {
            free((char *)client->headers[i].value);
            client->headers[i].value = cc_copy_string(value); return ESP_OK;
        }
    }
    assert(client->count < 16); cc_http_header_t *header = &client->headers[client->count++];
    header->name = cc_copy_string(name); header->value = cc_copy_string(value); return ESP_OK;
}
/** @brief 模拟同来源 set_url 不重装 Host。 @param[in,out] client 当前 handle。 @param[in] url 借用 URL。 @return ESP_OK。 */
static esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url) {
    (void)client; (void)url; return ESP_OK;
}
/** @brief 接受已清理列表上的单头删除。 @param[in,out] client 当前 handle。 @param[in] name 借用头名。 @return ESP_OK。 */
static esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *name) {
    (void)client; (void)name; return ESP_OK;
}
/** @brief 保存方法。 @param[in,out] client 当前 handle。 @param[in] method 原生方法。 @return ESP_OK。 */
static esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, int method) {
    client->method = method; return ESP_OK;
}
/** @brief 接受调用内正文借用。 @param[in,out] client 当前 handle。 @param[in] body 正文。 @param[in] length 字节数。 @return ESP_OK。 */
static esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *body, int length) {
    (void)client; assert(body && length >= 0); return ESP_OK;
}
''' + production("static cc_result_t http_client_apply_request_locked(")
        run_c_test(code + r'''
/** @brief 执行复用字段隔离断言。 @return 全部通过时返回零。 */
int main(void) {
    struct fake_client client = {0};
    esp_http_client_set_header(&client, "Host", "[::1]:8080");
    cc_http_header_t headers[] = {{"Authorization", "first-secret"}, {"X-Old", "first"},
        {"User-Agent", "caller-agent"}};
    cc_http_request_t request = {0}; request.url = "http://[::1]:8080/first";
    request.headers = headers; request.header_count = 3;
    cc_result_t rc = http_client_apply_request_locked(&client, &request);
    assert(rc.code == CC_OK && client.count == 5); cc_result_free(&rc);
    assert(strcmp(client.headers[0].name, "Host") == 0);
    assert(strcmp(client.headers[0].value, "[::1]:8080") == 0);
    int saw_caller_agent = 0;
    for (size_t i = 0; i < client.count; i++) {
        if (strcmp(client.headers[i].name, "User-Agent") == 0)
            saw_caller_agent = strcmp(client.headers[i].value, "caller-agent") == 0;
    }
    assert(saw_caller_agent);
    request.url = "http://[::1]:8080/next"; request.headers = NULL; request.header_count = 0;
    rc = http_client_apply_request_locked(&client, &request);
    assert(rc.code == CC_OK && client.count == 3); cc_result_free(&rc);
    int saw_default_agent = 0;
    for (size_t i = 0; i < client.count; i++) {
        assert(strcmp(client.headers[i].name, "Authorization") != 0);
        assert(strcmp(client.headers[i].name, "X-Old") != 0);
        if (strcmp(client.headers[i].name, "User-Agent") == 0)
            saw_default_agent = strcmp(client.headers[i].value, "ESP32 HTTP Client/1.0") == 0;
    }
    assert(saw_default_agent);
    esp_http_client_delete_all_headers(&client); assert(!result_messages);
    puts("ESP HTTP reused request header replacement: PASS"); return 0;
}
''')

    def test_callback_owned_success_and_first_error(self):
        """成功消息逐次释放，首个回调错误锁存，后续 IDF 数据不再交付用户。"""
        code = preamble() + r'''
enum { HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH, HTTP_EVENT_DISCONNECTED };
typedef struct { int event_id; void *user_data; esp_http_client_handle_t client;
    void *data; int data_len; char *header_key; char *header_value; } esp_http_client_event_t;
struct fake_client { int status; int closed; };
static int64_t clock_us = 1000;
static unsigned body_calls;
static int fail_body, cancelled;
/** @brief 返回可控微秒时钟。 @return 当前替身时间。 */
static int64_t http_now_us(void) { return clock_us; }
/** @brief 抑制 fixture 日志。 @param[in] state 实例。 @param[in] level 级别。 @return 零。 */
static int http_log_enabled(const cc_esp32_http_client_state_t *state, int level) { (void)state; (void)level; return 0; }
/** @brief 读取当前取消状态。 @param[in] token 借用 token。 @return 替身取消位。 */
static int cc_cancel_token_is_cancelled(const cc_cancel_token_t *token) { (void)token; return cancelled; }
/** @brief 接受下一读预算。 @param[in,out] client 当前 handle。 @param[in] timeout 毫秒。 @return ESP_OK。 */
static int esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout) { (void)client; assert(timeout > 0); return ESP_OK; }
/** @brief 标记同步关闭。 @param[in,out] client 当前 handle。 @return ESP_OK。 */
static int esp_http_client_close(esp_http_client_handle_t client) { client->closed++; return ESP_OK; }
/** @brief 返回当前状态码。 @param[in] client 当前 handle。 @return 状态码。 */
static int esp_http_client_get_status_code(esp_http_client_handle_t client) { return client->status; }
/** @brief 模拟完整接收。 @param[in] client 当前 handle。 @return 一。 */
static int esp_http_client_is_complete_data_received(esp_http_client_handle_t client) { (void)client; return 1; }
/** @brief 接受已连接 peer。 @param[in] client 当前 handle。 @param[in,out] ctx 当前上下文。 @return 无拥有诊断的成功。 */
static cc_result_t http_validate_connected_peer(esp_http_client_handle_t client, cc_esp32_http_ctx_t *ctx) { (void)client; (void)ctx; return cc_result_ok(); }
/** @brief 返回可计数的回调诊断。 @param[in] data 借用正文。 @param[in] length 字节数。 @param[in,out] user 备用上下文。 @return 成功和错误均带拥有消息。 */
static cc_result_t body_callback(const char *data, size_t length, void *user) {
    (void)user; assert(data && length); body_calls++;
    return cc_result_error(fail_body ? CC_ERR_TOOL : CC_OK, fail_body ? "first failure" : "owned success");
}
''' + production("static int elapsed_ms(", "static void http_pool_record_discard(",
            "static int response_append(", "static int response_append_preview(",
            "static cc_result_t response_header_append(", "static esp_err_t http_abort_from_event(",
            "static esp_err_t http_apply_remaining_timeout(", "static esp_err_t http_event_handler(",
            "void cc_http_response_free(")
        run_c_test(code + r'''
/** @brief 执行回调拥有和首错保留断言。 @return 所有断言通过时返回零。 */
int main(void) {
    cc_esp32_http_client_state_t owner = {0}; cc_esp32_http_pool_t *pool = &owner.pools[0];
    cc_http_response_t response = {0}; cc_esp32_http_ctx_t ctx = {0};
    struct fake_client client = {200, 0};
    ctx.owner = &owner; ctx.pool = pool; ctx.response = &response; ctx.on_body = body_callback;
    ctx.total_timeout_ms = 1000; ctx.start_us = 1000; ctx.idle_timeout_ms = 100;
    ctx.first_byte_timeout_ms = 100; pool->active_ctx = &ctx;
    esp_http_client_event_t event = {HTTP_EVENT_ON_DATA, pool, &client, "abc", 3, NULL, NULL};
    assert(http_event_handler(&event) == ESP_OK && body_calls == 1 && !result_messages);
    assert(http_event_handler(&event) == ESP_OK && body_calls == 2 && !result_messages);
    client.status = 302;
    assert(http_event_handler(&event) == ESP_OK && body_calls == 2);
    client.status = 200; fail_body = 1;
    assert(http_event_handler(&event) == ESP_FAIL && body_calls == 3 && result_messages == 1);
    char *first_message = ctx.callback_error.message;
    event.event_id = HTTP_EVENT_ON_HEADER; event.header_key = "Retry-After"; event.header_value = "1";
    assert(http_event_handler(&event) == ESP_FAIL && ctx.callback_error.message == first_message);
    event.event_id = HTTP_EVENT_ON_DATA;
    assert(http_event_handler(&event) == ESP_FAIL && body_calls == 3 && result_messages == 1);
    cc_result_free(&ctx.callback_error); cancelled = 1; event.event_id = HTTP_EVENT_ON_CONNECTED;
    assert(http_event_handler(&event) == ESP_FAIL && ctx.callback_error.code == CC_ERR_CANCELLED);
    cc_result_free(&ctx.callback_error); cc_http_response_free(&response); assert(!result_messages);
    puts("ESP HTTP owned callback and first-error latch: PASS"); return 0;
}
''')

    def test_destroy_does_not_allocate_unused_pool_mutexes(self):
        """析构真实函数直接清理已建 handle，未用池锁保持空且无新分配。"""
        code = preamble() + r'''
static int cleaned, deleted;
/** @brief 抑制同步清理日志。 @param[in] state 实例。 @param[in] level 级别。 @return 零。 */
static int http_log_enabled(const cc_esp32_http_client_state_t *state, int level) { (void)state; (void)level; return 0; }
/** @brief 消费替身 handle。 @param[in,out] client 唯一拥有句柄。 @return ESP_OK。 */
static int esp_http_client_cleanup(esp_http_client_handle_t client) { assert(client); cleaned++; free(client); return ESP_OK; }
/** @brief 消费已有互斥锁。 @param[in,out] mutex 唯一拥有锁。 @post 计数增加并释放。 */
static void vSemaphoreDelete(SemaphoreHandle_t mutex) { assert(mutex); deleted++; free(mutex); }
''' + production("static void http_client_cleanup_locked(", "static void esp32_http_destroy(")
        run_c_test(code + r'''
/** @brief 执行部分懒建实例的无分配析构。 @return 所有断言通过返回零。 */
int main(void) {
    cc_esp32_http_client_state_t *state = calloc(1, sizeof(*state)); assert(state);
    state->mutex = malloc(sizeof(int)); state->pools[0].mutex = malloc(sizeof(int));
    state->pools[0].client = malloc(1); state->pools[0].origin = cc_copy_string("https://host");
    assert(state->mutex && state->pools[0].mutex && state->pools[0].client);
    esp32_http_destroy(state); assert(cleaned == 1 && deleted == 2);
    esp32_http_destroy(NULL); assert(!result_messages);
    puts("ESP HTTP partial-pool allocation-free destroy: PASS"); return 0;
}
''')

    def test_complete_origin_cache_reuse_limit_and_oom(self):
        """完整长来源可复用，诊断代际有界，来源变化、超限及 OOM 正确清理。"""
        code = preamble() + r'''
#define CC_HTTP_POOL_IDLE_TTL_MS 4000
#define CC_HTTP_POOL_MAX_REUSE 12
#define CC_ESP32_HTTP_BUFFER_SIZE 4096
#define CC_ESP32_HTTP_BUFFER_SIZE_TX 4096
#define CC_ESP32_HTTP_KEEPALIVE_IDLE_SEC 5
#define CC_ESP32_HTTP_KEEPALIVE_INTERVAL_SEC 5
#define CC_ESP32_HTTP_KEEPALIVE_COUNT 3
typedef struct {
    const char *url; int timeout_ms; void *event_handler; void *user_data;
    void *crt_bundle_attach; bool skip_cert_common_name_check, disable_auto_redirect;
    int buffer_size, buffer_size_tx; bool keep_alive_enable;
    int keep_alive_idle, keep_alive_interval, keep_alive_count;
} esp_http_client_config_t;
static unsigned initialized, cleaned;
static int fail_origin_allocation;
/** @brief 返回固定空闲计时。 @return 单调毫秒值。 */
static int64_t http_now_ms(void) { return 1000; }
/** @brief 抑制日志。 @param[in] state 实例。 @param[in] level 级别。 @return 零。 */
static int http_log_enabled(const cc_esp32_http_client_state_t *state, int level) { (void)state; (void)level; return 0; }
/** @brief 返回固定初始预算。 @param[in] state 实例。 @param[in] request 请求。 @param[in] reused 是否复用。 @return 十五毫秒。 */
static int request_initial_timeout_ms(const cc_esp32_http_client_state_t *state, const cc_http_request_t *request, int reused) { (void)state; (void)request; (void)reused; return 15; }
/** @brief 路由到固定池。 @param[in,out] state 实例。 @param[in] request 请求。 @param[in] origin 诊断来源。 @return 借用首池。 */
static cc_esp32_http_pool_t *http_pool_for_request(cc_esp32_http_client_state_t *state, const cc_http_request_t *request, const char *origin) { (void)request; (void)origin; return &state->pools[0]; }
/** @brief 接受超时更新。 @param[in,out] client 当前 handle。 @param[in] timeout 毫秒。 @return ESP_OK。 */
static int esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout) { (void)client; (void)timeout; return ESP_OK; }
/** @brief 提供配置函数地址。 @post 无状态变化。 */
static void http_event_handler(void) {}
/** @brief 提供证书绑定函数地址。 @post 无状态变化。 */
static void esp_crt_bundle_attach(void) {}
/** @brief 分配一个可释放替身 handle。 @param[in] config 本次配置。 @return 拥有句柄。 */
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    assert(config->disable_auto_redirect); initialized++; return (esp_http_client_handle_t)malloc(1);
}
/** @brief 归还替身 handle。 @param[in,out] client 唯一拥有句柄。 @return ESP_OK。 */
static int esp_http_client_cleanup(esp_http_client_handle_t client) { cleaned++; free(client); return ESP_OK; }
/** @brief 只对本用例来源分配注入失败。 @param[in] bytes 容量。 @return 拥有分配或 NULL。 */
static void *origin_malloc(size_t bytes) { return fail_origin_allocation ? NULL : malloc(bytes); }
''' + production("static void http_origin_from_url(", "static int http_origins_equal(",
            "static const char *request_header_profile(", "static void http_pool_record_discard(",
            "static void http_pool_cleanup_locked(") + "\n#define malloc origin_malloc\n" + production(
            "static cc_result_t http_client_ensure_pool_locked(") + "\n#undef malloc\n"
        run_c_test(code + r'''
/** @brief 执行缓存上限、完整来源及 OOM 断言。 @return 所有断言通过返回零。 */
int main(void) {
    cc_esp32_http_client_state_t state = {0}; state.pools[0].owner = &state;
    cc_esp32_http_ctx_t ctx = {0}; ctx.owner = &state;
    cc_http_request_t request = {0}; char url[2300]; state.pools[0].generation = INT_MAX;
    memset(url, 'a', sizeof(url)); memcpy(url, "https://", 8); url[250] = '\0'; request.url = url;
    cc_result_t rc = http_client_ensure_pool_locked(&request, &ctx);
    assert(rc.code == CC_OK && initialized == 1 && strlen(state.pools[0].origin) == 250);
    assert(state.pools[0].generation == 1 && ctx.connection_generation == 1);
    cc_result_free(&rc); state.pools[0].healthy = 1;
    rc = http_client_ensure_pool_locked(&request, &ctx);
    assert(rc.code == CC_OK && initialized == 1 && ctx.reused_connection); cc_result_free(&rc);
    url[240] = 'b'; rc = http_client_ensure_pool_locked(&request, &ctx);
    assert(rc.code == CC_OK && initialized == 2 && cleaned == 1); cc_result_free(&rc);
    http_pool_cleanup_locked(&state.pools[0], "test"); assert(cleaned == 2);
    fail_origin_allocation = 1; rc = http_client_ensure_pool_locked(&request, &ctx);
    assert(rc.code == CC_ERR_OUT_OF_MEMORY && initialized == 2 && !state.pools[0].client);
    cc_result_free(&rc); fail_origin_allocation = 0;
    memset(url + 8, 'a', 2040); url[2048] = '\0';
    rc = http_client_ensure_pool_locked(&request, &ctx);
    assert(rc.code == CC_OK && initialized == 3 && !state.pools[0].origin); cc_result_free(&rc);
    http_pool_cleanup_locked(&state.pools[0], "uncached");
    assert(cleaned == initialized && !result_messages);
    puts("ESP HTTP complete origin cache, limit and OOM: PASS"); return 0;
}
''')

    def test_pool_threshold_counters_saturate(self):
        """阈值计数达到上限后停止累积，长运行及已有大值不产生有符号溢出。"""
        code = preamble() + r'''
#define CC_HTTP_POOL_STALE_THRESHOLD 2
#define CC_HTTP_POOL_COOLDOWN_MS 3000
#define CC_HTTP_POOL_RECOVER_SUCCESS 3
/** @brief 提供稳定单调时间。 @return 毫秒数。 */
static int64_t http_now_ms(void) { return 10000; }
/** @brief 记录借用丢弃原因。 @param[in,out] ctx 测试上下文。 @param[in] reason 静态原因。 @post 原因保存在上下文。 */
static void http_pool_record_discard(cc_esp32_http_ctx_t *ctx, const char *reason) {
    ctx->connection_discard_reason = reason;
}
/** @brief 模拟句柄清理。 @param[in,out] pool 测试池。 @param[in] reason 借用原因。 @post 句柄为空。 */
static void http_pool_cleanup_locked(cc_esp32_http_pool_t *pool, const char *reason) {
    (void)reason; pool->client = NULL;
}
''' + production("static void http_pool_note_failure_locked(",
                 "static void http_pool_note_success_locked(")
        run_c_test(code + r'''
/** @brief 检查冷却与恢复计数的饱和。 @return 断言通过时返回零。 */
int main(void) {
    cc_esp32_http_pool_t pool = {0};
    cc_esp32_http_ctx_t ctx = {0}; ctx.pool = &pool; ctx.reused_connection = 1;
    for (int i = 0; i < 8; i++) http_pool_note_failure_locked(&ctx, ESP_FAIL);
    assert(pool.stale_reused_failures == CC_HTTP_POOL_STALE_THRESHOLD);
    assert(pool.cooldown_until_ms == 13000 && pool.recover_successes == 0);
    pool.stale_reused_failures = INT_MAX;
    http_pool_note_failure_locked(&ctx, ESP_FAIL);
    assert(pool.stale_reused_failures == INT_MAX);
    ctx.reused_connection = 0;
    for (int i = 0; i < 8; i++) http_pool_note_success_locked(&ctx);
    assert(pool.recover_successes == CC_HTTP_POOL_RECOVER_SUCCESS);
    assert(pool.stale_reused_failures == 0 && pool.cooldown_until_ms == 0);
    pool.recover_successes = INT_MAX;
    http_pool_note_success_locked(&ctx);
    assert(pool.recover_successes == INT_MAX);
    assert(!result_messages);
    puts("ESP HTTP threshold counters saturate: PASS"); return 0;
}
''')

    def test_monotonic_deadline_and_initial_timeout(self):
        """期限使用六十四位公共单调时钟，已到期初始预算保持错误并限制 long 输入。"""
        code = preamble() + r'''
#define CC_HTTP_DEFAULT_CONNECT_TIMEOUT_MS 15000
#define CC_HTTP_DEFAULT_FIRST_BYTE_TIMEOUT_MS 30000
static uint64_t monotonic_ms;
/** @brief 返回可控公共单调时钟。 @return 毫秒值。 */
static uint64_t cc_platform_monotonic_ms(void) { return monotonic_ms; }
''' + production("static int64_t http_now_ms(", "static int positive_or(",
            "static int min_positive(", "static int request_total_timeout_ms(",
            "static int request_connect_timeout_ms(", "static int request_first_byte_timeout_ms(",
            "static int request_initial_timeout_ms(", "static int request_retry_count(",
            "static const char *http_failure_stage(", "static cc_result_t http_error_result(")
        run_c_test(code + r'''
/** @brief 执行时钟及预算边界。 @return 所有断言通过时返回零。 */
int main(void) {
    cc_esp32_http_client_state_t state = {0}; state.connect_timeout_ms = 15; state.first_byte_timeout_ms = 30;
    cc_http_request_t request = {0};
    monotonic_ms = (uint64_t)INT_MAX + 1000; request.deadline_ms = monotonic_ms + 7;
    assert(http_now_ms() == (int64_t)monotonic_ms);
    assert(request_total_timeout_ms(&state, &request) == 7);
    assert(request_initial_timeout_ms(&state, &request, 0) == 7);
    monotonic_ms += 7;
    assert(request_initial_timeout_ms(&state, &request, 0) == -1);
    request.deadline_ms = 0; request.timeout_ms = LONG_MAX;
    assert(request_total_timeout_ms(&state, &request) == INT_MAX);
    request.connect_timeout_ms = LONG_MAX; request.first_byte_timeout_ms = LONG_MAX;
    assert(request_connect_timeout_ms(&state, &request) == INT_MAX);
    assert(request_first_byte_timeout_ms(&state, &request) == INT_MAX);
    state.retry_count = 2; request.retry_count = -1;
    assert(request_retry_count(&state, &request) == 0);
    request.retry_count = 0; assert(request_retry_count(&state, &request) == 2);
    request.retry_count = 3; assert(request_retry_count(&state, &request) == 3);
    cc_esp32_http_ctx_t ctx = {0}; ctx.failure_stage = "body_io_error";
    cc_result_t rc = http_error_result(&ctx, ESP_ERR_HTTP_READ_TIMEOUT);
    assert(rc.code == CC_ERR_TIMEOUT); cc_result_free(&rc);
    assert(!result_messages); puts("ESP HTTP monotonic deadline and timeout bounds: PASS"); return 0;
}
''')

    def test_explicit_deadline_does_not_reset_total_budget(self):
        """编译真实重定向入口，检查显式远期限仍被总预算收紧且跨跳保持。"""
        code = preamble() + r'''
static unsigned hops;
static uint64_t expected_deadline;
/** @brief 返回可控单调毫秒值。 @return 固定一百。 */
static int64_t http_now_ms(void) { return 100; }
#define portTICK_PERIOD_MS 1
/** @brief 为原文负对照提供同一起点的 tick。 @return 固定一百。 */
static unsigned xTaskGetTickCount(void) { return 100; }
/** @brief 模拟一跳重定向后成功。 @param[in,out] self 备用实例。 @param[in] request 当前跳。 @param[out] response 拥有响应。 @return 成功值。 */
static cc_result_t esp32_http_perform_single(void *self, const cc_http_request_t *request, cc_http_response_t *response) {
    (void)self; assert(request->deadline_ms == expected_deadline); hops++;
    memset(response, 0, sizeof(*response)); response->status_code = hops % 2 ? 302 : 200;
    if (response->status_code == 302) {
        response->headers = calloc(1, sizeof(*response->headers)); assert(response->headers);
        response->headers[0].name = cc_copy_string("Location");
        response->headers[0].value = cc_copy_string("/next"); response->header_count = 1;
    }
    return cc_result_ok();
}
''' + production("static int http_ascii_equal(", "static int http_origins_equal(",
            "static int http_is_redirect_status(", "static const char *http_response_header(",
            "static char *http_redirect_url(", "static int http_redirect_drops_body(",
            "static int http_sensitive_redirect_header(", "static int http_entity_header(",
            "static cc_result_t http_redirect_headers(", "void cc_http_response_free(",
            "static cc_result_t esp32_http_perform(")
        run_c_test(code + r'''
/** @brief 执行三种绝对期限组合。 @return 所有断言通过时返回零。 */
int main(void) {
    cc_http_request_t request = {0}; request.url = "https://host/start";
    request.timeout_ms = 100; request.deadline_ms = 1000; request.max_redirects = 1;
    cc_http_response_t response = {0}; expected_deadline = 200;
    cc_result_t rc = esp32_http_perform(NULL, &request, &response);
    assert(rc.code == CC_OK && hops == 2); cc_result_free(&rc); cc_http_response_free(&response);
    request.deadline_ms = 150; expected_deadline = 150;
    rc = esp32_http_perform(NULL, &request, &response);
    assert(rc.code == CC_OK && hops == 4); cc_result_free(&rc); cc_http_response_free(&response);
    request.deadline_ms = 0; expected_deadline = 200;
    rc = esp32_http_perform(NULL, &request, &response);
    assert(rc.code == CC_OK && hops == 6); cc_result_free(&rc); cc_http_response_free(&response);
    assert(!result_messages); puts("ESP HTTP shared total deadline across redirects: PASS"); return 0;
}
''')

    def test_max_retry_count_still_enters_first_attempt(self):
        """INT_MAX 重试配置不发生有符号加一溢出，首次失败诊断正常返回并归还两把锁。"""
        code = preamble() + r'''
#define portMAX_DELAY UINT_MAX
#define pdMS_TO_TICKS(value) (value)
static unsigned attempted, released;
static int config_lock, pool_lock;
/** @brief 接受前置主机校验。 @param[in] request 请求。 @param[out] host 输出主机。 @param[in] size 容量。 @return 成功值。 */
static cc_result_t http_validate_resolved_candidates(const cc_http_request_t *request, char *host, size_t size) { (void)request; assert(size > 4); memcpy(host, "host", 5); return cc_result_ok(); }
/** @brief 维持未取消状态。 @param[in] token 借用 token。 @return 零。 */
static int cc_cancel_token_is_cancelled(const cc_cancel_token_t *token) { (void)token; return 0; }
/** @brief 取得固定配置锁。 @param[in,out] state 实例。 @return 借用锁。 */
static SemaphoreHandle_t http_mutex_get(cc_esp32_http_client_state_t *state) { (void)state; return &config_lock; }
/** @brief 取得固定池锁。 @param[in,out] pool 当前池。 @return 借用锁。 */
static SemaphoreHandle_t http_pool_mutex_get_locked(cc_esp32_http_pool_t *pool) { (void)pool; return &pool_lock; }
/** @brief 接受原生锁等待。 @param[in,out] lock 锁。 @param[in] ticks 等待值。 @return 一。 */
static int xSemaphoreTake(SemaphoreHandle_t lock, unsigned ticks) { assert(lock); (void)ticks; return 1; }
/** @brief 记录归还锁。 @param[in,out] lock 锁。 @return 一。 */
static int xSemaphoreGive(SemaphoreHandle_t lock) { assert(lock); released++; return 1; }
/** @brief 固定选择首池。 @param[in,out] state 实例。 @param[in] request 请求。 @param[in] origin 来源。 @return 借用首池。 */
static cc_esp32_http_pool_t *http_pool_for_request(cc_esp32_http_client_state_t *state, const cc_http_request_t *request, const char *origin) { (void)request; (void)origin; return &state->pools[0]; }
/** @brief 交付最大配置值。 @param[in] state 实例。 @param[in] request 请求。 @return INT_MAX。 */
static int request_retry_count(const cc_esp32_http_client_state_t *state, const cc_http_request_t *request) { (void)state; return request->retry_count; }
/** @brief 发放固定观测序号。 @param[in,out] state 实例。 @return 一。 */
static unsigned http_next_attempt_id(cc_esp32_http_client_state_t *state) { (void)state; return 1; }
/** @brief 模拟首次设置即失败。 @param[in] request 请求。 @param[in,out] ctx 当前尝试。 @param[in,out] response 当前响应。 @param[out] error 原生结果。 @return 拥有取消诊断。 */
static cc_result_t http_perform_once_locked(const cc_http_request_t *request, cc_esp32_http_ctx_t *ctx, cc_http_response_t *response, esp_err_t *error) { (void)request; (void)response; *error = ESP_OK; ctx->pool = &ctx->owner->pools[0]; attempted++; return cc_result_error(CC_ERR_CANCELLED, "setup cancelled"); }
/** @brief 提供可用重试剩余量。 @param[in] state 实例。 @param[in] request 请求。 @return 一。 */
static int request_total_timeout_ms(const cc_esp32_http_client_state_t *state, const cc_http_request_t *request) { (void)state; (void)request; return 1; }
/** @brief 接受本次诊断。 @param[in,out] ctx 上下文。 @param[in] error 原生错误。 @param[in] status 状态码。 @param[in] final 是否末次。 @post 无额外拥有状态。 */
static void http_log_attempt_summary(cc_esp32_http_ctx_t *ctx, esp_err_t error, long status, int final) { (void)ctx; (void)error; (void)status; (void)final; }
/** @brief 接受连接失败记录。 @param[in,out] ctx 上下文。 @param[in] error 原生错误。 @post 无额外拥有状态。 */
static void http_pool_note_failure_locked(cc_esp32_http_ctx_t *ctx, esp_err_t error) { (void)ctx; (void)error; }
/** @brief 接受连接成功记录。 @param[in,out] ctx 上下文。 @post 无额外拥有状态。 */
static void http_pool_note_success_locked(cc_esp32_http_ctx_t *ctx) { (void)ctx; }
/** @brief 提供分支中的拥有传输错误。 @param[in] ctx 上下文。 @param[in] error 原生错误。 @return 拥有网络诊断。 */
static cc_result_t http_error_result(const cc_esp32_http_ctx_t *ctx, esp_err_t error) { (void)ctx; (void)error; return cc_result_error(CC_ERR_NETWORK, "transport"); }
/** @brief 抑制日志。 @param[in] state 实例。 @param[in] level 阈值。 @return 零。 */
static int http_log_enabled(const cc_esp32_http_client_state_t *state, int level) { (void)state; (void)level; return 0; }
/** @brief 接受未进入的退避分支。 @param[in] ticks 等待值。 @post 无状态变化。 */
static void vTaskDelay(unsigned ticks) { (void)ticks; }
/** @brief 接受未缓存来源的收尾。 @param[in,out] pool 当前池。 @param[in] reason 原因。 @post 无状态变化。 */
static void http_pool_cleanup_locked(cc_esp32_http_pool_t *pool, const char *reason) { (void)pool; (void)reason; }
''' + production("static void http_origin_from_url(", "static void http_pool_record_discard(",
            "void cc_http_response_free(", "static cc_result_t esp32_http_perform_single(")
        run_c_test(code + r'''
/** @brief 执行最大重试配置的首次尝试。 @return 所有断言通过时返回零。 */
int main(void) {
    cc_esp32_http_client_state_t state = {0}; cc_http_request_t request = {0};
    request.url = "https://host/path"; request.retry_count = INT_MAX;
    cc_http_response_t response = {0};
    cc_result_t rc = esp32_http_perform_single(&state, &request, &response);
    assert(rc.code == CC_ERR_CANCELLED && attempted == 1 && released == 2);
    assert(result_messages == 1); cc_result_free(&rc); cc_http_response_free(&response);
    assert(!result_messages); puts("ESP HTTP INT_MAX retry count and setup cleanup: PASS"); return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
