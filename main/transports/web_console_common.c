#include "web_console_internal.h"

#include <string.h>
#include <strings.h>

#include "app_event_bus.h"
#include "app_config.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

/**
 * @brief 申请八位可寻址的 PSRAM 缓冲，不初始化内容。
 * @param[in] size 请求字节数，零大小行为沿 heap_caps_malloc。
 * @return 独占缓冲交调用者 heap_caps_free，分配失败返回 NULL。
 */
char *web_console_psram_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/**
 * @brief 申请八位可寻址的内部 DRAM 缓冲，供文件接收等同步路径使用。
 * @param[in] size 请求字节数，零大小行为沿 heap_caps_malloc。
 * @return 独占缓冲交调用者 heap_caps_free，分配失败返回 NULL。
 */
char *web_console_internal_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/**
 * @brief 读取一个 ASCII 十六进制数字，兼容大小写。
 * @param[in] c 待解释字节，不读取其他内存。
 * @return 合法数字返回零至十五，其他字节返回负一。
 */
static int url_hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/**
 * @brief 在原缓冲内解码百分号字节与加号，拒绝百分号解码出的 NUL。
 * @param[in,out] text 可空的独占可写 NUL 字符串，成功后在压缩正文末尾补 NUL。
 * @return 成功为 true，NULL、残缺或非法转义为 false，失败可能已改写前缀，调用者丢弃本次内容。
 * @note 此处按字节处理，路径字符和权限由后续入口检查。
 */
static bool url_decode_in_place(char *text)
{
    if (!text) return false;
    char *read = text;
    char *write = text;
    while (*read) {
        if (*read == '%') {
            if (!read[1] || !read[2]) return false;
            int hi = url_hex_value(read[1]);
            int lo = url_hex_value(read[2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
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

/**
 * @brief 取得并解码查询值，区分缺键、无效输入和分配失败。
 * @param[in] req 调用期有效的 HTTPD 请求借用，NULL 为参数错误。
 * @param[in] key 调用期借用键名，NULL 为参数错误，键匹配语义沿 httpd_query_key_value。
 * @param[out] out_value 必填空资源槽，进入先置 NULL，成功发布独立 PSRAM 字符串，调用者 heap_caps_free。
 * @return 成功为 ESP_OK，缺 query/键为 NOT_FOUND，NULL 参数、容量或编码错误为 INVALID_ARG，分配失败为 NO_MEM。
 * @note 编码值须能放入 WEB_MAX_PATH 字节缓冲，临时完整 query 返回前释放。重复键沿 HTTPD 首项语义，不新增歧义规则。
 */
esp_err_t web_console_query_value_checked(httpd_req_t *req, const char *key, char **out_value)
{
    if (!out_value) return ESP_ERR_INVALID_ARG;
    *out_value = NULL;
    if (!req || !key) return ESP_ERR_INVALID_ARG;
    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len == 0) return ESP_ERR_NOT_FOUND;
    if (query_len == SIZE_MAX) return ESP_ERR_INVALID_ARG;
    char *query = web_console_psram_alloc(query_len + 1);
    if (!query) return ESP_ERR_NO_MEM;
    if (httpd_req_get_url_query_str(req, query, query_len + 1) != ESP_OK) {
        heap_caps_free(query);
        return ESP_ERR_INVALID_ARG;
    }
    char value[WEB_MAX_PATH];
    esp_err_t err = httpd_query_key_value(query, key, value, sizeof(value));
    if (err != ESP_OK) {
        heap_caps_free(query);
        return err == ESP_ERR_NOT_FOUND ? ESP_ERR_NOT_FOUND : ESP_ERR_INVALID_ARG;
    }
    heap_caps_free(query);
    if (!url_decode_in_place(value)) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(value);
    char *out = web_console_psram_alloc(len + 1);
    if (!out) return ESP_ERR_NO_MEM;
    memcpy(out, value, len + 1);
    *out_value = out;
    return ESP_OK;
}

/**
 * @brief 为事件和 OTA 的字符串型回调保留查询兼容入口。
 * @param[in] req 调用期 HTTPD 请求借用，NULL 返回 NULL。
 * @param[in] key 调用期 NUL 键名借用，NULL 返回 NULL。
 * @return 成功返回须 heap_caps_free 的独立 PSRAM 值，所有失败均为 NULL，具体分类由 checked 入口提供。
 */
char *web_console_query_value(httpd_req_t *req, const char *key)
{
    char *value = NULL;
    web_console_query_value_checked(req, key, &value);
    return value;
}

/**
 * @brief 检查通用文件 API 的 USB 根、完整长度和规范名称规则。
 * @param[in] path 调用期稳定的 NUL 路径借用，允许 NULL。
 * @return 精确 /usb0 根内且少于 WEB_MAX_PATH 字节，通过点段、连续分隔、控制字节及 FAT 别名规则为 true，其他为 false。
 * @note 不访问文件系统，不检查实际存在性、符号链接或当前业务保护区。
 */
bool web_console_path_is_safe(const char *path)
{
    size_t root_len = strlen(WEB_ROOT);
    if (!path || strncmp(path, WEB_ROOT, root_len) != 0) return false;
    if (path[root_len] != '\0' && path[root_len] != '/') return false;
    if (strlen(path) >= WEB_MAX_PATH) return false;
    if (strstr(path, "..")) return false;
    /* 只接收规范长文件名，拒绝 FAT 短名、点段和尾部空白等权限比较别名。 */
    const char *segment = path + root_len;
    for (const char *p = segment; ; ++p) {
        if (*p && ((unsigned char)*p < 0x20 || strchr("\\:~*?<>|\"", *p))) return false;
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - segment);
            if (len && (segment[0] == ' ' || segment[len - 1] == ' ' ||
                        segment[len - 1] == '.')) return false;
            if (!len && p != path + root_len && *p != '\0') return false;
            if (*p == '\0') break;
            segment = p + 1;
        }
    }
    return true;
}

/**
 * @brief 按忽略大小写的根前缀匹配路径及其点号名称族。
 * @param[in] path 必填的稳定 NUL 路径借用。
 * @param[in] root 必填的稳定保护前缀借用。
 * @return 前缀后为结尾、斜杠或点号时为 true，其他为 false，不执行路径归一。
 */
static bool web_path_matches(const char *path, const char *root)
{
    size_t len = strlen(root);
    return strncasecmp(path, root, len) == 0 &&
           (path[len] == '\0' || path[len] == '/' || path[len] == '.');
}

/**
 * @brief 结合静态保护路径和业务 owner 快照判定本次通用文件访问。
 * @param[in] path 调用期稳定路径借用，NULL 或路径语法不合法时拒绝。
 * @param[in] write 为 true 时再检查删除保护并调用修改回调，为 false 时调用读取回调。
 * @return 配置/恢复根、上传恢复 sidecar、缺失回调或 owner 拒绝均为 false，全部通过为 true。
 * @note HTTPD 运行期间上下文保持稳定，回调自行同步业务配置，此判定不为后续文件操作持有事务锁。
 */
bool web_console_file_access_allowed(const char *path, bool write)
{
    if (!web_console_path_is_safe(path)) return false;
    if (web_path_matches(path, APP_RECOVERY_ROOT)) return false;
    /* 保留 config.* 名称族，也阻断 FAT 对长扩展名可能生成的无 ~ 短名。 */
    if (web_path_matches(path, APP_CONFIG_FILE_PATH) || web_path_matches(path, WEB_ROOT "/config")) return false;
    /* 恢复文件保留给人工恢复，避免第二次 HTTP 请求覆盖或删除唯一旧副本。 */
    size_t len = strlen(path);
    if (len && path[len - 1] == '/') --len;
    const char *suffixes[] = {WEB_UPLOAD_TEMP_SUFFIX, WEB_UPLOAD_BACKUP_SUFFIX};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); ++i) {
        size_t suffix_len = strlen(suffixes[i]);
        if (len >= suffix_len && strncasecmp(path + len - suffix_len, suffixes[i], suffix_len) == 0) return false;
    }
    if (!write) {
        return g_web_console_context.file_read_allowed &&
               g_web_console_context.file_read_allowed(
                   path, g_web_console_context.user_data);
    }
    if (web_console_protected_delete_path(path)) return false;
    return g_web_console_context.file_mutation_allowed &&
           g_web_console_context.file_mutation_allowed(path, g_web_console_context.user_data);
}

/**
 * @brief 按需补一个斜杠，完整拼接目录与名称。
 * @param[out] out 必填可写数组，与两个输入不重叠，失败保持原内容。
 * @param[in] out_size 输出字节容量，含末尾 NUL，零返回 false。
 * @param[in] dir 必填 NUL 目录借用，允许空串。
 * @param[in] name 必填 NUL 名称借用，允许空串。
 * @return 完整结果可容纳时写入并返回 true，参数或容量错误为 false，不截断也不验证路径权限。
 */
bool web_console_join_path(char *out, size_t out_size, const char *dir, const char *name)
{
    if (!out || out_size == 0 || !dir || !name) return false;
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    bool needs_slash = dir_len > 0 && dir[dir_len - 1] != '/';
    if (dir_len >= out_size) return false;
    size_t remaining = out_size - dir_len;
    if (needs_slash) {
        if (remaining <= 1) return false;
        remaining--;
    }
    if (name_len >= remaining) return false;
    size_t total = dir_len + (needs_slash ? 1U : 0U) + name_len;
    memcpy(out, dir, dir_len);
    size_t pos = dir_len;
    if (needs_slash) out[pos++] = '/';
    memcpy(out + pos, name, name_len);
    out[total] = '\0';
    return true;
}

/**
 * @brief 检查固定运行根、配置与当前日志的通用修改保护。
 * @param[in] path 稳定 NUL 路径借用，NULL 保守拒绝。
 * @return USB/workspace 根本身、cclaw/config 名称族及当前日志名称族命中为 true，其他为 false。
 * @note 当前日志路径先复制到栈缓冲，不保留日志模块借用，其他运行路径由 owner 权限回调检查。
 */
bool web_console_protected_delete_path(const char *path)
{
    char current_log[192];
    app_event_bus_copy_current_log_path(current_log, sizeof(current_log));
    return !path ||
           web_path_matches(path, APP_CONFIG_FILE_PATH) ||
           web_path_matches(path, "/usb0/cclaw") ||
           strcasecmp(path, "/usb0") == 0 ||
           strcasecmp(path, "/usb0/") == 0 ||
           strcasecmp(path, "/usb0/workspace") == 0 ||
           strcasecmp(path, "/usb0/workspace/") == 0 ||
           (current_log[0] && web_path_matches(path, current_log));
}

/**
 * @brief 设置文本类型与 keep-alive 后同步发送完整 C 字符串。
 * @param[in,out] req 必填 HTTPD 请求借用，覆盖设置头和发送期间。
 * @param[in] text NUL 正文借用，NULL 发送空串，函数不接管。
 * @param[in] type 内容类型借用，NULL 使用 text/plain。
 * @return 返回正文发送的原生结果，头设置错误当前不单独传播，ESP_OK 只表示该响应发送成功。
 */
esp_err_t web_console_send_text(httpd_req_t *req, const char *text, const char *type)
{
    httpd_resp_set_type(req, type ? type : "text/plain");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    return httpd_resp_send(req, text ? text : "", HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief 要求关闭连接并发送 HTTPD 错误响应。
 * @param[in,out] req 必填 HTTPD 请求借用，覆盖同步发送期间。
 * @param[in] code HTTPD 错误状态枚举。
 * @param[in] message NUL 描述借用，NULL 使用 error，不接管文本。
 * @return 原生错误响应发送结果，ESP_OK 表示错误响应已发送，业务错误由 HTTP 状态表达。
 */
esp_err_t web_console_send_error(httpd_req_t *req, httpd_err_code_t code, const char *message)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send_err(req, code, message ? message : "error");
}

/**
 * @brief 向构造器追加带双引号的 JSON 字符串并消费各次追加诊断。
 * @param[in,out] sb 已初始化且由本次调用独占修改的构造器，NULL 沿 builder 参数错误处理。
 * @param[in] text 调用期稳定的 NUL 正文借用，NULL 按空串，非控制字节原样保留。
 * @post 完整字符串已追加或 builder 保留 sticky 错误，失败后停止追加，调用者用 take/error 检查结果。
 */
void web_console_json_escape_append(cc_string_builder_t *sb, const char *text)
{
    cc_result_t rc = cc_string_builder_append_char(sb, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : "");
         rc.code == CC_OK && *p; p++) {
        cc_result_free(&rc);
        switch (*p) {
        case '\\': rc = cc_string_builder_append(sb, "\\\\"); break;
        case '"': rc = cc_string_builder_append(sb, "\\\""); break;
        case '\n': rc = cc_string_builder_append(sb, "\\n"); break;
        case '\r': rc = cc_string_builder_append(sb, "\\r"); break;
        case '\t': rc = cc_string_builder_append(sb, "\\t"); break;
        default:
            if (*p < 0x20) rc = cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p);
            else rc = cc_string_builder_append_char(sb, (char)*p);
            break;
        }
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = cc_string_builder_append_char(sb, '"');
    }
    cc_result_free(&rc);
}

/**
 * @brief 在总接收期限内用内部 DRAM 分块缓冲将请求正文写入已有文件。
 * @param[in,out] req 必填 HTTPD 请求借用，content_len 为准确字节数且最多八 MiB，零长度可用。
 * @param[in,out] f 必填可写 FILE 借用，函数不关闭、flush 或回滚已写片段。
 * @return 参数或长度错误为 INVALID_SIZE，分配失败为 NO_MEM，总期限耗尽为 TIMEOUT，接收/短写为 FAIL，完整写入为 ESP_OK。
 * @note 所有出口释放本次缓冲，单次 socket 超时重试至六十秒总期限，底层一次阻塞仍受 HTTPD 自身超时控制。
 */
esp_err_t web_console_read_request_to_file(httpd_req_t *req, FILE *f)
{
    if (!req || !f || req->content_len > WEB_MAX_UPLOAD_BYTES) return ESP_ERR_INVALID_SIZE;
    char *buf = web_console_internal_alloc(WEB_IO_CHUNK);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t remaining = req->content_len;
    int64_t deadline = esp_timer_get_time() + WEB_REQUEST_TIMEOUT_US;
    while (remaining > 0) {
        if (esp_timer_get_time() >= deadline) {
            heap_caps_free(buf);
            return ESP_ERR_TIMEOUT;
        }
        size_t requested = remaining > WEB_IO_CHUNK ? WEB_IO_CHUNK : remaining;
        int n = httpd_req_recv(req, buf, requested);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || (size_t)n > requested) {
            heap_caps_free(buf);
            return ESP_FAIL;
        }
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            heap_caps_free(buf);
            return ESP_FAIL;
        }
        remaining -= (size_t)n;
    }
    heap_caps_free(buf);
    return ESP_OK;
}
