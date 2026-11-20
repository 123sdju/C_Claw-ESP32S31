"""执行完整 FreeRTOS/lwIP HTTP 生产文件的 socket 与 TLS 原语模型。"""
from pathlib import Path
import re
import unittest

from test_event_ring_ownership import run_c_test


ROOT = Path(__file__).resolve().parents[1]
SOURCE = "third_party/cclaw-sdk/cclaw/platforms/freertos/src/cc_freertos_lwip_http_client.c"


def fixture(body, tls=False, nonstandard_timeout=False, tls_version=3):
    """组合真实公共头、完整生产实现和同步原语模型，返回独立可编译 C 文本。

    body 为调用者提供的 C 测试入口，tls 选择条件 TLS 分支，tls_version 指定
    模型的主版本，nonstandard_timeout 选择 lwIP 的 int 毫秒参数配置。
    本函数只读项目文件，返回文本交给 run_c_test 编译，失败由编译或断言向外报告。
    """
    result = (ROOT / "third_party/cclaw-sdk/cclaw/core/include/cc/core/cc_result.h").read_text(encoding="utf-8")
    header = (ROOT / "third_party/cclaw-sdk/cclaw/ports/include/cc/ports/cc_http_client.h").read_text(encoding="utf-8")
    header = header.replace('#include "cc/core/cc_result.h"', '')
    source = (ROOT / SOURCE).read_text(encoding="utf-8")
    source = re.sub(r"^#include[^\n]*", "", source, flags=re.M)
    flags = "\n#define CCLAW_FREERTOS_ENABLE_MBEDTLS 1\n" if tls else ""
    if tls:
        flags += f"\n#define MBEDTLS_VERSION_MAJOR {tls_version}\n"
    if nonstandard_timeout:
        flags += "\n#define LWIP_SO_SNDRCVTIMEO_NONSTANDARD 1\n"
    return PREFIX + result + header + flags + MODEL + r'''
#define malloc model_malloc
#define realloc model_realloc
#define calloc model_calloc
''' + source + r'''
#undef malloc
#undef realloc
#undef calloc
''' + body


PREFIX = r'''
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6
#define SOL_SOCKET 1
#define SO_RCVTIMEO 2
#define SO_SNDTIMEO 3
#define IPADDR_NONE UINT32_MAX
#define INET_ADDRSTRLEN 16
#define PP_HTONS(value) (value)
struct in_addr { uint32_t s_addr; };
typedef unsigned int socklen_t;
struct sockaddr { uint8_t sa_len; uint8_t sa_family; char sa_data[14]; };
struct sockaddr_in { uint8_t sin_len; uint8_t sin_family; uint16_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
struct addrinfo { int ai_flags; int ai_family; int ai_socktype; int ai_protocol; socklen_t ai_addrlen; struct sockaddr *ai_addr; char *ai_canonname; struct addrinfo *ai_next; };
struct timeval { long tv_sec; long tv_usec; };
'''


MODEL = r'''
static uint64_t model_ms, io_advance, read_advance, dns_advance;
static uint64_t callback_advance;
static int cancelled, cancel_dns, cancel_connect, fail_socket_option, fail_connect, fail_receive;
static int fail_reallocation, fail_allocation, fail_callocation, fail_body_copy;
static int fail_body_callback, fail_tls_config;
static int short_dns_address, null_dns_address;
static unsigned opened, closed, body_calls, dns_calls, dns_freed, receive_calls, messages;
static const char *incoming;
static size_t incoming_size, incoming_offset, receive_piece;
static char outgoing[16384];
static size_t outgoing_size;
static long last_socket_timeout;
static int tls_live, config_live, handshake_wants, read_wants, write_wants, cancel_tls;
static int model_psa_initialized;
static unsigned handshake_calls;
static char sni[128];

/** @brief 复制模型诊断。 @param[in] text 稳定字符串。 @return 拥有副本。 */
static char *model_copy(const char *text) {
    size_t size = strlen(text) + 1;
    char *copy = malloc(size);
    assert(copy);
    memcpy(copy, text, size);
    return copy;
}
/** @brief 构造无拥有字段成功值。 @return 成功结果。 */
cc_result_t cc_result_ok(void) {
    cc_result_t result = {0};
    result.size = sizeof(result);
    return result;
}
/** @brief 构造拥有诊断并计数。 @param[in] code 结果码。 @param[in] text 诊断。 @return 接收者须释放的结果。 */
cc_result_t cc_result_error(cc_error_code_t code, const char *text) {
    cc_result_t result = cc_result_ok();
    result.code = code;
    result.message = model_copy(text);
    messages++;
    return result;
}
/** @brief 消费模型诊断。 @param[in,out] result 独占结果。 @post 动态消息释放并清零。 */
void cc_result_free(cc_result_t *result) {
    if (result->message) {
        assert(messages);
        messages--;
        free(result->message);
    }
    memset(result, 0, sizeof(*result));
}
/** @brief 对生产独立分配注入一次失败。 @param[in] size 字节数。 @return 拥有分配或 NULL。 */
static void *model_malloc(size_t size) {
    if (fail_allocation) { fail_allocation = 0; return NULL; }
    return malloc(size);
}
/** @brief 对生产扩容注入一次失败。 @param[in,out] pointer 旧拥有缓冲。 @param[in] size 容量。 @return 新缓冲或 NULL。 */
static void *model_realloc(void *pointer, size_t size) {
    if (fail_reallocation) { fail_reallocation = 0; return NULL; }
    return realloc(pointer, size);
}
/** @brief 对实例分配注入一次失败。 @param[in] count 项数。 @param[in] size 每项字节。 @return 拥有分配或 NULL。 */
static void *model_calloc(size_t count, size_t size) {
    if (fail_callocation) { fail_callocation = 0; return NULL; }
    return calloc(count, size);
}
/** @brief 比较有界 ASCII 字符串。 @param[in] left 第一串。 @param[in] right 第二串。 @param[in] count 最多字节数。 @return 相同为零，否则返回字符差。 */
static int model_ncasecmp(const char *left, const char *right, size_t count) {
    for (size_t i = 0; i < count; i++) {
        int a = tolower((unsigned char)left[i]);
        int b = tolower((unsigned char)right[i]);
        if (a != b || !a || !b) return a - b;
    }
    return 0;
}
/** @brief 比较完整 ASCII 字符串。 @param[in] left 第一串。 @param[in] right 第二串。 @return 相同为零。 */
static int model_casecmp(const char *left, const char *right) {
    size_t count = strlen(left);
    size_t other = strlen(right);
    return model_ncasecmp(left, right, (count > other ? count : other) + 1);
}
#define strcasecmp model_casecmp
#define strncasecmp model_ncasecmp
/** @brief 返回模型单调时钟。 @return 本次 fixture 的毫秒值。 */
uint64_t cc_platform_monotonic_ms(void) { return model_ms; }
/** @brief 读取模型取消状态。 @param[in] token 借用 token。 @return 当前取消位。 */
static int cc_cancel_token_is_cancelled(const cc_cancel_token_t *token) { (void)token; return cancelled; }
/** @brief 识别本模型数字回环地址。 @param[in] host 稳定主机。 @return 固定 IPv4 或 IPADDR_NONE。 */
static uint32_t inet_addr(const char *host) { return strcmp(host, "127.0.0.1") == 0 ? 0x0100007fU : IPADDR_NONE; }
/** @brief 渲染候选数字地址。 @param[in] family 地址族。 @param[in] address 地址。 @param[out] out 输出缓冲。 @param[in] size 容量。 @return 输出指针或 NULL。 */
static const char *inet_ntop(int family, const void *address, char *out, size_t size) {
    (void)address;
    assert(family == AF_INET);
    if (size < 10) return NULL;
    memcpy(out, "127.0.0.1", 10);
    return out;
}
/** @brief 交出固定 IPv4 候选并可推进取消。 @param[in] host 主机。 @param[in] service 服务。 @param[in] hints 查询约束。 @param[out] out 借用列表。 @return 零。 */
static int lwip_getaddrinfo(const char *host, const char *service, const struct addrinfo *hints, struct addrinfo **out) {
    static struct sockaddr_in address;
    static struct addrinfo result;
    (void)host; (void)service;
    assert(hints->ai_family == AF_INET);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    result.ai_family = AF_INET;
    result.ai_addrlen = (socklen_t)(sizeof(address) - (short_dns_address ? 1 : 0));
    result.ai_addr = null_dns_address ? NULL : (struct sockaddr *)&address;
    result.ai_next = NULL;
    *out = &result;
    dns_calls++;
    model_ms += dns_advance;
    if (cancel_dns) cancelled = 1;
    return 0;
}
/** @brief 结束静态候选借用。 @param[in] results 候选列表。 @post 无拥有分配。 */
static void lwip_freeaddrinfo(struct addrinfo *results) { assert(results); dns_freed++; }
/** @brief 发放本次请求 socket。 @param[in] family 地址族。 @param[in] type 类型。 @param[in] protocol 协议。 @return 固定有效 fd。 */
static int lwip_socket(int family, int type, int protocol) {
    assert(family == AF_INET && type == SOCK_STREAM && protocol == IPPROTO_TCP);
    opened++;
    return 7;
}
/** @brief 保存实际收发预算或注入失败。 @param[in] fd socket。 @param[in] level 级别。 @param[in] option 选项。 @param[in] value timeval。 @param[in] size 结构容量。 @return 成功零或失败负一。 */
static int lwip_setsockopt(int fd, int level, int option, const void *value, size_t size) {
    assert(fd == 7 && level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO));
#if defined(LWIP_SO_SNDRCVTIMEO_NONSTANDARD) && LWIP_SO_SNDRCVTIMEO_NONSTANDARD
    const int *timeout = value;
    assert(size == sizeof(*timeout));
    last_socket_timeout = *timeout;
#else
    const struct timeval *timeout = value;
    assert(size == sizeof(*timeout));
    last_socket_timeout = timeout->tv_sec * 1000 + timeout->tv_usec / 1000;
#endif
    assert(last_socket_timeout > 0);
    if (fail_socket_option) { errno = EINVAL; return -1; }
    return 0;
}
/** @brief 模拟阻塞 connect 的返回。 @param[in] fd socket。 @param[in] address IPv4 地址。 @param[in] size 地址容量。 @return 配置结果。 */
static int lwip_connect(int fd, const struct sockaddr *address, size_t size) {
    assert(fd == 7 && address && size == sizeof(struct sockaddr_in));
    model_ms += io_advance;
    if (cancel_connect) cancelled = 1;
    if (fail_connect) { errno = ETIMEDOUT; return -1; }
    return 0;
}
/** @brief 捕获实际发送字节并制造短写。 @param[in] fd socket。 @param[in] data 输入。 @param[in] size 字节数。 @param[in] flags 原生标志。 @return 最多七字节。 */
static int lwip_send(int fd, const void *data, size_t size, int flags) {
    assert(fd == 7); (void)flags;
    size_t take = size > 7 ? 7 : size;
    assert(outgoing_size + take < sizeof(outgoing));
    memcpy(outgoing + outgoing_size, data, take);
    outgoing_size += take;
    outgoing[outgoing_size] = '\0';
    model_ms += io_advance;
    return (int)take;
}
/** @brief 按片段返回原始响应并推进可控时钟。 @param[in] fd socket。 @param[out] out 缓冲。 @param[in] size 容量。 @param[in] flags 原生标志。 @return 字节数、EOF 或超时。 */
static int lwip_recv(int fd, void *out, size_t size, int flags) {
    assert(fd == 7); (void)flags;
    model_ms += read_advance;
    receive_calls++;
    if (fail_receive) { errno = EAGAIN; return -1; }
    if (incoming_offset == incoming_size) {
        if (fail_body_copy) fail_allocation = 1;
        return 0;
    }
    size_t take = incoming_size - incoming_offset;
    if (take > size) take = size;
    if (take > receive_piece) take = receive_piece;
    memcpy(out, incoming + incoming_offset, take);
    incoming_offset += take;
    return (int)take;
}
/** @brief 归还一次 socket。 @param[in] fd 当前 socket。 @return 零。 */
static int lwip_close(int fd) { assert(fd == 7); closed++; return 0; }

#if defined(CCLAW_FREERTOS_ENABLE_MBEDTLS) && CCLAW_FREERTOS_ENABLE_MBEDTLS
#define MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 1
#define MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 2
#define MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384 3
#define MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 4
#define MBEDTLS_SSL_IS_CLIENT 1
#define MBEDTLS_SSL_TRANSPORT_STREAM 2
#define MBEDTLS_SSL_PRESET_DEFAULT 3
#define MBEDTLS_SSL_VERIFY_NONE 0
#define MBEDTLS_ERR_SSL_WANT_READ -10
#define MBEDTLS_ERR_SSL_WANT_WRITE -11
#define MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY -12
typedef struct { int initialized; void *bio; int (*send)(void *, const unsigned char *, size_t); int (*recv)(void *, unsigned char *, size_t); } mbedtls_ssl_context;
typedef struct { int initialized; } mbedtls_ssl_config;
/** @brief 初始化 SSL 模型。 @param[out] ssl 上下文。 @post 增加存活计数。 */
static void mbedtls_ssl_init(mbedtls_ssl_context *ssl) { memset(ssl, 0, sizeof(*ssl)); ssl->initialized = 1; tls_live++; }
/** @brief 初始化配置模型。 @param[out] config 配置。 @post 增加存活计数。 */
static void mbedtls_ssl_config_init(mbedtls_ssl_config *config) { config->initialized = 1; config_live++; }
/** @brief 接受 TLS 默认配置。 @param[in,out] config 配置。 @param[in] role 角色。 @param[in] transport 传输。 @param[in] preset 默认项。 @return 零。 */
static int mbedtls_ssl_config_defaults(mbedtls_ssl_config *config, int role, int transport, int preset) { assert(config->initialized); (void)role; (void)transport; (void)preset; return fail_tls_config ? -99 : 0; }
/** @brief 核对示例校验模式。 @param[in,out] config 配置。 @param[in] mode 模式。 @post 保持 VERIFY_NONE。 */
static void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *config, int mode) { (void)config; assert(mode == MBEDTLS_SSL_VERIFY_NONE); }
/** @brief 接受示例 RNG 绑定。 @param[in,out] config 配置。 @param[in] callback RNG 回调。 @param[in,out] context 状态。 @post 不产生额外借用。 */
#if MBEDTLS_VERSION_MAJOR < 4
static void mbedtls_ssl_conf_rng(mbedtls_ssl_config *config, int (*callback)(void *, unsigned char *, size_t), void *context) { (void)config; assert(callback && context); }
#endif
/** @brief 接受套件列表。 @param[in,out] config 配置。 @param[in] suites 静态列表。 @post 无拥有变化。 */
static void mbedtls_ssl_conf_ciphersuites(mbedtls_ssl_config *config, const int *suites) { (void)config; assert(suites); }
/** @brief 接受 SSL 配置。 @param[in,out] ssl 上下文。 @param[in] config 配置。 @return 零。 */
static int mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *config) { assert(ssl->initialized && config->initialized); return 0; }
/** @brief 保存真实 SNI 输入。 @param[in,out] ssl 上下文。 @param[in] host 主机。 @return 零。 */
static int mbedtls_ssl_set_hostname(mbedtls_ssl_context *ssl, const char *host) { (void)ssl; assert(strlen(host) < sizeof(sni)); strcpy(sni, host); return 0; }
/** @brief 保存同步 BIO 回调。 @param[in,out] ssl 上下文。 @param[in,out] context transport。 @param[in] send 写回调。 @param[in] recv 读回调。 @param[in] timeout 未使用回调。 @post 借用保持至 SSL 清理。 */
static void mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *context, int (*send)(void *, const unsigned char *, size_t), int (*recv)(void *, unsigned char *, size_t), void *timeout) { (void)timeout; ssl->bio = context; ssl->send = send; ssl->recv = recv; }
/** @brief 制造 WANT 握手并推进时间。 @param[in,out] ssl 上下文。 @return WANT 或成功。 */
static int mbedtls_ssl_handshake(mbedtls_ssl_context *ssl) {
    (void)ssl;
#if MBEDTLS_VERSION_MAJOR >= 4
    assert(model_psa_initialized);
#endif
    handshake_calls++; model_ms++;
    if (cancel_tls) cancelled = 1;
    if (handshake_wants < 0) return MBEDTLS_ERR_SSL_WANT_READ;
    if (handshake_wants > 0) { handshake_wants--; return MBEDTLS_ERR_SSL_WANT_READ; }
    return 0;
}
/** @brief 通过真实 BIO 写或制造 WANT。 @param[in,out] ssl 上下文。 @param[in] data 输入。 @param[in] size 字节数。 @return 写入计数或 WANT。 */
static int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *data, size_t size) { if (write_wants) { model_ms++; return MBEDTLS_ERR_SSL_WANT_WRITE; } return ssl->send(ssl->bio, data, size); }
/** @brief 通过真实 BIO 读或制造 WANT。 @param[in,out] ssl 上下文。 @param[out] out 输出。 @param[in] size 容量。 @return 字节数或 WANT。 */
static int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *out, size_t size) { if (read_wants) { model_ms++; return MBEDTLS_ERR_SSL_WANT_READ; } return ssl->recv(ssl->bio, out, size); }
/** @brief 接受尽力通知。 @param[in,out] ssl 上下文。 @return 零。 */
static int mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl) { (void)ssl; return 0; }
/** @brief 清理已初始化 SSL 模型。 @param[in,out] ssl 上下文。 @post 存活计数回退。 */
static void mbedtls_ssl_free(mbedtls_ssl_context *ssl) { if (ssl->initialized) { tls_live--; ssl->initialized = 0; } }
/** @brief 清理已初始化配置模型。 @param[in,out] config 配置。 @post 存活计数回退。 */
static void mbedtls_ssl_config_free(mbedtls_ssl_config *config) { if (config->initialized) { config_live--; config->initialized = 0; } }
/** @brief 生成静态错误诊断。 @param[in] code 错误码。 @param[out] out 输出。 @param[in] size 容量。 @post 输出完整短消息。 */
static void mbedtls_strerror(int code, char *out, size_t size) { (void)code; assert(size > 5); memcpy(out, "fake", 5); }
#endif

/** @brief 开始下一个确定用例。 @param[in] response 稳定原始响应。 @post 计数和故障开关清零，前一用例拥有资源须归零。 */
static void reset_model(const char *response) {
    assert(opened == closed && !messages && !tls_live && !config_live);
    model_ms = 100;
    io_advance = read_advance = dns_advance = callback_advance = 0;
    cancelled = cancel_dns = cancel_connect = fail_socket_option = fail_connect = fail_receive = 0;
    fail_reallocation = fail_allocation = fail_callocation = fail_body_copy = 0;
    fail_body_callback = fail_tls_config = 0;
    short_dns_address = null_dns_address = 0;
    opened = closed = body_calls = dns_calls = dns_freed = receive_calls = 0;
    handshake_wants = read_wants = write_wants = cancel_tls = 0;
    handshake_calls = 0;
    model_psa_initialized = 0;
    incoming = response;
    incoming_size = strlen(response);
    incoming_offset = outgoing_size = 0;
    receive_piece = 512;
    outgoing[0] = sni[0] = '\0';
}
/** @brief 统计指定完整片段出现次数。 @param[in] text 完整文本。 @param[in] needle 非空片段。 @return 出现次数。 */
static unsigned count_text(const char *text, const char *needle) {
    unsigned count = 0;
    while ((text = strstr(text, needle)) != NULL) { count++; text += strlen(needle); }
    return count;
}
/** @brief 接收完整正文并返回可配置拥有结果。 @param[in] data 正文。 @param[in] size 字节数。 @param[in,out] context 备用参数。 @return 接收者须消费的诊断。 */
static cc_result_t on_body(const char *data, size_t size, void *context) { (void)context; assert(data && size); body_calls++; model_ms += callback_advance; return cc_result_error(fail_body_callback ? CC_ERR_TOOL : CC_OK, "owned callback result"); }
/** @brief 拒绝 URL 以检查输出初始化。 @param[in] url 借用 URL。 @param[in,out] context 备用上下文。 @return 零。 */
static int reject_url(const char *url, void *context) { (void)url; (void)context; return 0; }
'''


class FreeRtosLwipHttpTests(unittest.TestCase):
    """完整生产文件的确定时钟、分配和 socket/TLS 模型断言。"""

    def test_plain_framing_and_callback_ownership(self):
        """验证非默认端口、查询/fragment、唯一真实长度及末次回调结果的拥有权转交。"""
        run_c_test(fixture(r'''
/** @brief 执行明文请求的实际编码。 @return 所有断言通过时返回零。 */
int main(void) {
    reset_model("HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nabc");
    cc_http_client_t client = {0}; cc_result_t rc = cc_http_client_create_default(NULL, &client);
    assert(rc.code == CC_OK); cc_result_free(&rc);
    cc_http_header_t headers[] = {{"Content-Length", "999"}, {"content-length", "7"}, {"X-Test", "yes"}};
    cc_http_request_t request = {0}; request.url = "http://model.test:8080?x=1#fragment";
    request.method = "POST"; request.body = "hi"; request.headers = headers; request.header_count = 3;
    request.on_body = on_body;
    cc_http_response_t response = {0};
    rc = cc_http_client_perform(&client, &request, &response);
    assert(rc.code == CC_OK && rc.size == sizeof(rc) && response.status_code == 200 && response.body_size == 3);
    assert(strcmp(response.body, "abc") == 0 && body_calls == 1 && messages == 1);
    assert(strstr(outgoing, "POST /?x=1 HTTP/1.0\r\n"));
    assert(strstr(outgoing, "Host: model.test:8080\r\n"));
    assert(count_text(outgoing, "Content-Length:") == 1 && strstr(outgoing, "Content-Length: 2\r\n\r\nhi"));
    assert(!strstr(outgoing, "fragment"));
    cc_result_free(&rc); cc_http_response_free(&response);
    for (int mode = 0; mode < 2; mode++) {
        reset_model("HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nabc");
        request.timeout_ms = 5;
        if (mode == 0) fail_body_callback = 1;
        else callback_advance = 10;
        rc = cc_http_client_perform(&client, &request, &response);
        assert(rc.code == (mode == 0 ? CC_ERR_TOOL : CC_ERR_TIMEOUT));
        assert(body_calls == 1 && response.body_size == 3);
        cc_result_free(&rc); cc_http_response_free(&response);
    }
    cc_http_client_destroy(&client);
    assert(opened == closed && !messages);
    puts("FreeRTOS HTTP framing and callback ownership: PASS"); return 0;
}
'''))

    def test_response_status_length_and_no_body_rules(self):
        """坏状态/残缺头/短正文拒绝，相同长度可重复，HEAD 与无正文状态保留合法元数据。"""
        run_c_test(fixture(r'''
/** @brief 遍历有界响应契约样例。 @return 全部断言通过时返回零。 */
int main(void) {
    const char *responses[] = {
        "HTTP/1.0 200 OK\r\nContent-Length: 3\r\nContent-Length: 003, 3\r\n\r\nabc",
        "HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\na",
        "HTTP/1.0 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\na",
        "HTTP/1.0 200 OK\r\nContent-Length: 1x\r\n\r\na",
        "HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n0\r\n\r\n",
        "HTTP/1.0 200 OK\r\nContent-Length: 10000000000000000000000000000000000\r\n\r\n",
        "HTTP/1.0 204 No Content\r\n\r\n",
        "HTTP/1.0 304 Not Modified\r\nContent-Length: 3000\r\n\r\n",
        "HTTP/1.0 100 Continue\r\n\r\nHTTP/1.0 200 OK\r\nContent-Length: 1\r\n\r\nz",
        "HTTP/1.0 200 OK\r\nX-Test: incomplete",
        "garbage\r\n\r\na",
        ""
    };
    const cc_error_code_t expected[] = {CC_OK, CC_ERR_NETWORK, CC_ERR_NETWORK, CC_ERR_NETWORK,
        CC_ERR_UNSUPPORTED, CC_OK, CC_OK, CC_OK, CC_OK, CC_ERR_NETWORK, CC_ERR_NETWORK, CC_ERR_NETWORK};
    cc_http_client_t client = {0}; cc_result_t rc = cc_http_client_create_default(NULL, &client);
    cc_result_free(&rc);
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        reset_model(responses[i]);
        cc_http_request_t request = {0}; request.url = "http://127.0.0.1/";
        request.method = i == 5 ? "HEAD" : "GET"; request.on_body = on_body;
        cc_http_response_t response = {0};
        rc = cc_http_client_perform(&client, &request, &response);
        assert(rc.code == expected[i]);
        if (rc.code != CC_OK) assert(body_calls == 0 && response.body == NULL && response.body_size == 0);
        if (i == 5 || i == 6 || i == 7) assert(body_calls == 0 && response.body_size == 0);
        if (i == 8) assert(response.status_code == 200 && response.body_size == 1 && response.body[0] == 'z');
        cc_result_free(&rc); cc_http_response_free(&response);
        assert(opened == closed && !messages);
    }
    cc_http_client_destroy(&client);
    puts("FreeRTOS HTTP complete response and no-body metadata: PASS"); return 0;
}
'''))

    def test_deadline_cancel_and_socket_setup_failures(self):
        """DNS/connect 返回后检查取消，持续读仍受总预算，原生设置/等待错误返回真实分类。"""
        run_c_test(fixture(r'''
/** @brief 执行确定原生失败与总期限交错。 @return 所有断言通过时返回零。 */
int main(void) {
    cc_http_client_t client = {0}; cc_result_t rc = cc_http_client_create_default(NULL, &client); cc_result_free(&rc);
    for (int mode = 0; mode < 9; mode++) {
        reset_model("HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nabc");
        cc_http_request_t request = {0}; request.url = "http://model.test/"; request.on_body = on_body;
        if (mode == 0) cancel_dns = 1;
        if (mode == 1) cancel_connect = 1;
        if (mode == 2) fail_socket_option = 1;
        if (mode == 3) { request.timeout_ms = 5; request.deadline_ms = 1000; receive_piece = 1; read_advance = 1; }
        if (mode == 4) fail_connect = 1;
        if (mode == 5) fail_receive = 1;
        if (mode == 6) { request.timeout_ms = 5; dns_advance = 6; }
        if (mode == 7) short_dns_address = 1;
        if (mode == 8) null_dns_address = 1;
        cc_http_response_t response = {0};
        rc = cc_http_client_perform(&client, &request, &response);
        cc_error_code_t expected = mode < 2 ? CC_ERR_CANCELLED : mode == 2 || mode >= 7 ? CC_ERR_NETWORK : CC_ERR_TIMEOUT;
        assert(rc.code == expected && !body_calls && !response.body);
        if (mode == 0) assert(!outgoing_size && dns_calls == 1);
        if (mode == 1 || mode == 2) assert(!outgoing_size);
        if (mode == 3) assert(receive_calls > 1);
        if (mode == 6) assert(!outgoing_size && dns_calls == 1);
        if (mode >= 7) assert(!outgoing_size && dns_calls == 1 && dns_freed == 1);
        cc_result_free(&rc); cc_http_response_free(&response); assert(opened == closed && !messages);
    }
    cc_http_client_destroy(&client);
    puts("FreeRTOS HTTP shared deadline, cancellation and socket failures: PASS"); return 0;
}
''', nonstandard_timeout=True))

    def test_allocation_capacity_and_request_failures(self):
        """区分缓冲 OOM 与超限，拒绝坏数组/编码并清空有效输出，复制失败保留可清理状态。"""
        run_c_test(fixture(r'''
/** @brief 执行容量、输入和分配故障断言。 @return 全部通过时返回零。 */
int main(void) {
    reset_model("HTTP/1.0 200 OK\r\nContent-Length: 1\r\n\r\na");
    assert(copy_string_len("x", SIZE_MAX) == NULL);
    char *buffer = model_copy("old"); size_t length = 3;
    cc_result_t rc = append_bytes(&buffer, &length, 0, "x", SIZE_MAX);
    assert(rc.code == CC_ERR_LIMIT_EXCEEDED && length == 3 && strcmp(buffer, "old") == 0); cc_result_free(&rc); free(buffer);
    cc_http_client_t client = {0}; rc = cc_http_client_create_default(NULL, &client); cc_result_free(&rc);
    for (int mode = 0; mode < 6; mode++) {
        reset_model("HTTP/1.0 200 OK\r\nContent-Length: 1\r\n\r\na");
        cc_http_request_t request = {0}; request.url = "http://127.0.0.1/"; request.max_response_bytes = 1000;
        cc_http_header_t encoding = {"Transfer-Encoding", "chunked"};
        if (mode == 0) fail_reallocation = 1;
        if (mode == 1) request.max_response_bytes = 5;
        if (mode == 2) fail_body_copy = 1;
        if (mode == 3) { request.headers = &encoding; request.header_count = 1; }
        if (mode == 4) { request.headers = NULL; request.header_count = 1; }
        if (mode == 5) request.validate_url = reject_url;
        cc_http_response_t response = {0};
        if (mode == 5) { response.status_code = 418; response.body_size = 99; }
        rc = cc_http_client_perform(&client, &request, &response);
        cc_error_code_t expected = mode == 0 || mode == 2 ? CC_ERR_OUT_OF_MEMORY : mode == 1 ? CC_ERR_LIMIT_EXCEEDED : mode == 3 ? CC_ERR_UNSUPPORTED : mode == 4 ? CC_ERR_INVALID_ARGUMENT : CC_ERR_PERMISSION_DENIED;
        assert(rc.code == expected && !response.body && response.body_size == 0);
        if (mode == 5) assert(response.status_code == 0);
        cc_result_free(&rc); cc_http_response_free(&response); assert(opened == closed && !messages);
    }
    cc_http_client_destroy(&client); fail_callocation = 1;
    rc = cc_http_client_create_default(NULL, &client);
    assert(rc.code == CC_ERR_OUT_OF_MEMORY && !client.self && !client.vtable); cc_result_free(&rc);
    puts("FreeRTOS HTTP allocation, budget and request failures: PASS"); return 0;
}
'''))

    def test_tls_sni_lifetime_and_want_budget(self):
        """TLS 保留示例验证边界，纯 URL 主机用于 SNI，所有 WANT 循环可按预算或取消退出。"""
        run_c_test(fixture(r'''
/** @brief 遍历完整 TLS 模型的成功和中止路径。 @return 所有断言通过时返回零。 */
int main(void) {
    cc_http_client_options_t options = {0}; options.tls_verify = 0;
    cc_http_client_t client = {0}; cc_result_t rc = cc_http_client_create_default(&options, &client); cc_result_free(&rc);
    for (int mode = 0; mode < 6; mode++) {
        reset_model("HTTP/1.0 200 OK\r\nContent-Length: 1\r\n\r\na");
        cc_http_header_t host = {"Host", "override.test:9999"};
        cc_http_request_t request = {0}; request.url = "https://model.test:8443/";
        request.headers = &host; request.header_count = 1; request.timeout_ms = 20; request.connect_timeout_ms = 5;
        if (mode == 0) handshake_wants = 2;
        if (mode == 1) handshake_wants = -1;
        if (mode == 2) read_wants = 1;
        if (mode == 3) write_wants = 1;
        if (mode == 4) cancel_tls = 1;
        if (mode == 5) fail_tls_config = 1;
        cc_http_response_t response = {0};
        rc = cc_http_client_perform(&client, &request, &response);
        if (mode != 5) assert(strcmp(sni, "model.test") == 0);
        if (mode == 0) {
            assert(rc.code == CC_OK && handshake_calls == 3 && response.body_size == 1);
            assert(strstr(outgoing, "Host: override.test:9999\r\n"));
        } else assert(rc.code == (mode == 4 ? CC_ERR_CANCELLED : mode == 5 ? CC_ERR_NETWORK : CC_ERR_TIMEOUT));
        if (mode == 1) assert(model_ms == 105);
        cc_result_free(&rc); cc_http_response_free(&response);
        assert(opened == closed && !messages && !tls_live && !config_live);
    }
    cc_http_client_destroy(&client);
    reset_model("HTTP/1.0 200 OK\r\n\r\n");
    rc = cc_http_client_create_default(NULL, &client); cc_result_free(&rc);
    cc_http_request_t request = {0}; request.url = "https://model.test/";
    cc_http_response_t response = {0};
    rc = cc_http_client_perform(&client, &request, &response);
    assert(rc.code == CC_ERR_UNSUPPORTED && !tls_live && !config_live);
    cc_result_free(&rc); cc_http_response_free(&response); cc_http_client_destroy(&client);
    assert(opened == closed && !messages);
    puts("FreeRTOS HTTP TLS SNI, initialized lifetime and WANT bounds: PASS"); return 0;
}
''', tls=True))

    def test_tls4_uses_bsp_psa_rng_without_legacy_api(self):
        """4.x 模型不声明 conf_rng，BSP 先初始化 PSA 后完成握手并验证 WANT 超时清理。"""
        run_c_test(fixture(r'''
/** @brief 以 BSP 初始化的 PSA 状态执行 4.x 路径。 @return 全部断言通过时返回零。 */
int main(void) {
    cc_http_client_options_t options = {0}; options.tls_verify = 0;
    cc_http_client_t client = {0};
    cc_result_t rc = cc_http_client_create_default(&options, &client); cc_result_free(&rc);
    for (int mode = 0; mode < 2; mode++) {
        reset_model("HTTP/1.0 200 OK\r\nContent-Length: 1\r\n\r\na");
        model_psa_initialized = 1;
        handshake_wants = mode == 0 ? 1 : -1;
        cc_http_request_t request = {0}; request.url = "https://model.test:8443/";
        request.timeout_ms = 20; request.connect_timeout_ms = 5;
        cc_http_response_t response = {0};
        rc = cc_http_client_perform(&client, &request, &response);
        assert(rc.code == (mode == 0 ? CC_OK : CC_ERR_TIMEOUT));
        assert(strcmp(sni, "model.test") == 0);
        if (mode == 0) assert(response.body_size == 1 && handshake_calls == 2);
        cc_result_free(&rc); cc_http_response_free(&response);
        assert(opened == closed && !tls_live && !config_live && !messages);
    }
    cc_http_client_destroy(&client);
    puts("FreeRTOS HTTP Mbed TLS 4.x PSA lifecycle: PASS"); return 0;
}
''', tls=True, tls_version=4))


if __name__ == "__main__":
    unittest.main()
