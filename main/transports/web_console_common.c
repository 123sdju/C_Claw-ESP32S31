#include "web_console_internal.h"

#include <string.h>

#include "app_event_bus.h"
#include "app_config.h"
#include "esp_heap_caps.h"

/*
 * 从 PSRAM 分配指定大小的内存块。使用 ESP-IDF heap_caps API，
 * 标志为 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT，确保分配 8 位可寻址的 PSRAM。
 * 参数: size - 需要分配的字节数
 * 返回: 成功返回内存指针，失败返回 NULL
 */
char *web_console_psram_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/*
 * 从内部 DRAM 分配指定大小的内存块。与 web_console_psram_alloc 对应，
 * 使用 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT 标志分配内部 RAM。
 * 参数: size - 需要分配的字节数
 * 返回: 成功返回内存指针，失败返回 NULL
 */
char *web_console_internal_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/*
 * 将 URL 编码中的单个十六进制字符转换为对应的整数值。
 * 支持 '0'-'9'、'a'-'f'、'A'-'F'。
 * 参数: c - 要解析的十六进制字符
 * 返回: 0~15 的有效值，若字符不是合法十六进制则返回 -1
 */
static int url_hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * 对字符串进行原地 URL 解码。将 %XX 转义序列还原为对应字节，将 '+' 转为空格。
 * 解码直接写入原缓冲区（原地操作，压缩写入），末尾补 '\0'。
 * 参数: text - 待解码的字符串（原地修改）
 * 返回: true 表示解码成功，false 表示参数为空或遇到非法转义序列
 */
static bool url_decode_in_place(char *text)
{
    if (!text) return false;
    char *read = text;
    char *write = text;
    while (*read) {
        if (*read == '%') {
            int hi = url_hex_value(read[1]);
            int lo = url_hex_value(read[2]);
            if (hi < 0 || lo < 0) return false;
            *write++ = (char)((hi << 4) | lo);
            read += 3;
        } else if (*read == '+') {
            *write++ = ' ';
            read++;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
    return true;
}

/*
 * 从 HTTP 请求的 URL 查询字符串中提取指定键的值。解析问号之后的 query string，
 * 对值进行 URL 解码后返回 PSRAM 分配的副本。调用方负责用 heap_caps_free 释放。
 * 参数: req - HTTP 请求句柄
 *       key - 要查询的参数名
 * 返回: 成功返回 PSRAM 中分配的值字符串，失败或键不存在返回 NULL
 */
char *web_console_query_value(httpd_req_t *req, const char *key)
{
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) return NULL;
    char *query = web_console_psram_alloc(query_len + 1);
    if (!query) return NULL;
    if (httpd_req_get_url_query_str(req, query, query_len + 1) != ESP_OK) {
        heap_caps_free(query);
        return NULL;
    }
    char value[WEB_MAX_PATH];
    if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK) {
        heap_caps_free(query);
        return NULL;
    }
    heap_caps_free(query);
    if (!url_decode_in_place(value)) return NULL;
    size_t len = strlen(value);
    char *out = web_console_psram_alloc(len + 1);
    if (!out) return NULL;
    memcpy(out, value, len + 1);
    return out;
}

/*
 * 校验文件路径是否安全合法。检查路径以 WEB_ROOT ("/usb0") 开头、
 * 不包含 ".." 目录穿越、长度不超过 WEB_MAX_PATH (256)。
 * 参数: path - 待校验的文件系统路径
 * 返回: true 表示路径安全可用，false 表示路径非法
 */
bool web_console_path_is_safe(const char *path)
{
    size_t root_len = strlen(WEB_ROOT);
    if (!path || strncmp(path, WEB_ROOT, root_len) != 0) return false;
    if (path[root_len] != '\0' && path[root_len] != '/') return false;
    if (strlen(path) >= WEB_MAX_PATH) return false;
    return strstr(path, "..") == NULL;
}

/*
 * 拼接目录路径和文件/子目录名，生成完整路径。自动补充分隔斜杠（若 dir 不以 '/' 结尾），
 * 确保最终路径长度不超过 out_size - 1。
 * 参数: out      - 输出缓冲区
 *       out_size - 输出缓冲区大小（字节）
 *       dir      - 目录路径
 *       name     - 文件或子目录名称
 * 返回: true 表示拼接成功，false 表示参数无效或缓冲区不足
 */
bool web_console_join_path(char *out, size_t out_size, const char *dir, const char *name)
{
    if (!out || out_size == 0 || !dir || !name) return false;
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    bool needs_slash = dir_len > 0 && dir[dir_len - 1] != '/';
    size_t total = dir_len + (needs_slash ? 1U : 0U) + name_len;
    if (total >= out_size) return false;
    memcpy(out, dir, dir_len);
    size_t pos = dir_len;
    if (needs_slash) out[pos++] = '/';
    memcpy(out + pos, name, name_len);
    out[total] = '\0';
    return true;
}

/*
 * 判断路径是否受保护、不允许删除。保护对象包括：应用配置文件路径、
 * "/usb0/cclaw"、"/usb0/workspace" 以及当前正在写入的日志文件路径。
 * 参数: path - 待检查的路径
 * 返回: true 表示路径受保护不可删除，false 表示可删除
 */
bool web_console_protected_delete_path(const char *path)
{
    char current_log[192];
    app_event_bus_copy_current_log_path(current_log, sizeof(current_log));
    return !path ||
           strcmp(path, APP_CONFIG_FILE_PATH) == 0 ||
           strcmp(path, "/usb0/cclaw") == 0 ||
           strcmp(path, "/usb0/workspace") == 0 ||
           (current_log[0] && strcmp(path, current_log) == 0);
}

/*
 * 发送纯文本（或指定 Content-Type）HTTP 响应。设置 Content-Type 头
 * 和 Connection: keep-alive 头，然后发送响应体。
 * 参数: req  - HTTP 请求句柄
 *       text - 响应体文本（NULL 时发送空字符串）
 *       type - Content-Type 值（NULL 时默认 "text/plain"）
 * 返回: ESP_OK 表示发送成功，其他 ESP 错误码表示失败
 */
esp_err_t web_console_send_text(httpd_req_t *req, const char *text, const char *type)
{
    httpd_resp_set_type(req, type ? type : "text/plain");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    return httpd_resp_send(req, text ? text : "", HTTPD_RESP_USE_STRLEN);
}

/*
 * 发送 HTTP 错误响应。设置 Connection: close 头，使用 ESP HTTP Server
 * 内置错误响应格式发送指定状态码和错误消息。
 * 参数: req     - HTTP 请求句柄
 *       code    - HTTP 错误码
 *       message - 错误描述文本（NULL 时发送 "error"）
 * 返回: ESP_OK 表示发送成功，其他 ESP 错误码表示失败
 */
esp_err_t web_console_send_error(httpd_req_t *req, httpd_err_code_t code, const char *message)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send_err(req, code, message ? message : "error");
}

/*
 * 将字符串以 JSON 双引号字符串格式追加到字符串构建器。对 \、"、\n、\r、\t
 * 等特殊字符进行转义，控制字符（&lt;0x20）转为 \u00XX 形式，其余字符原样输出。
 * 参数: sb   - 字符串构建器指针
 *       text - 需要转义并追加的原始字符串（NULL 视为空字符串）
 */
void web_console_json_escape_append(cc_string_builder_t *sb, const char *text)
{
    cc_string_builder_append_char(sb, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '\\': cc_string_builder_append(sb, "\\\\"); break;
        case '"': cc_string_builder_append(sb, "\\\""); break;
        case '\n': cc_string_builder_append(sb, "\\n"); break;
        case '\r': cc_string_builder_append(sb, "\\r"); break;
        case '\t': cc_string_builder_append(sb, "\\t"); break;
        default:
            if (*p < 0x20) cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p);
            else cc_string_builder_append_char(sb, (char)*p);
            break;
        }
    }
    cc_string_builder_append_char(sb, '"');
}

/*
 * 将 HTTP 请求体内容流式写入已打开的文件。内部使用 4096 字节缓冲区逐块接收
 * 请求体数据并写入文件，超时时重试，接收错误时返回失败。
 * 参数: req - HTTP 请求句柄（含 content_len 指定总字节数）
 *       f   - 已以写入模式打开的文件指针
 * 返回: ESP_OK 表示全部写入成功，ESP_ERR_NO_MEM 表示缓冲区分配失败，
 *       ESP_FAIL 表示接收数据时出错
 */
esp_err_t web_console_read_request_to_file(httpd_req_t *req, FILE *f)
{
    char *buf = web_console_internal_alloc(WEB_IO_CHUNK);
    if (!buf) return ESP_ERR_NO_MEM;
    int remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining > WEB_IO_CHUNK ? WEB_IO_CHUNK : remaining);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) {
            heap_caps_free(buf);
            return ESP_FAIL;
        }
        fwrite(buf, 1, (size_t)n, f);
        remaining -= n;
    }
    heap_caps_free(buf);
    return ESP_OK;
}
