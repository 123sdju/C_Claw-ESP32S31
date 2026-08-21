#include "web_console_internal.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cc/util/cc_string_builder.h"
#include "esp_heap_caps.h"

/*
 * 处理 GET /api/files?path=... 请求。列出指定目录下的文件和子目录。
 * 返回 JSON 数组，每项包含 name（名称）、dir（是否目录）和 size（文件大小）。
 * 参数: req - HTTP 请求句柄（查询参数 path 指定目录，默认 "/usb0"）
 * 返回: ESP_OK 且目录存在返回列表 JSON；路径不安全返回 400；目录不存在返回 404
 */
esp_err_t web_console_files_handler(httpd_req_t *req)
{
    char *path = web_console_query_value(req, "path");
    if (!path) {
        path = web_console_psram_alloc(strlen(WEB_ROOT) + 1);
        if (path) strcpy(path, WEB_ROOT);
    }
    if (!web_console_path_is_safe(path)) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    DIR *dir = opendir(path);
    if (!dir) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_404_NOT_FOUND, "not a directory");
    }
    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) {
        closedir(dir);
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    cc_string_builder_append(&sb, "{\"path\":");
    web_console_json_escape_append(&sb, path);
    cc_string_builder_append(&sb, ",\"entries\":[");
    bool first = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[WEB_MAX_PATH];
        if (!web_console_join_path(full, sizeof(full), path, ent->d_name)) continue;
        struct stat st = {0};
        stat(full, &st);
        if (!first) cc_string_builder_append_char(&sb, ',');
        first = false;
        cc_string_builder_append(&sb, "{\"name\":");
        web_console_json_escape_append(&sb, ent->d_name);
        cc_string_builder_appendf(&sb, ",\"dir\":%s,\"size\":%u}",
                                  S_ISDIR(st.st_mode) ? "true" : "false",
                                  (unsigned)st.st_size);
    }
    closedir(dir);
    cc_string_builder_append(&sb, "]}");
    char *json = cc_string_builder_take(&sb);
    heap_caps_free(path);
    esp_err_t err = web_console_send_text(req, json, "application/json");
    free(json);
    return err;
}

/*
 * 处理 /api/file?path=... 的多方法端点：
 *   GET    - 以 application/octet-stream 流式下载文件（分块传输）
 *   PUT    - 将请求体覆盖写入文件
 *   DELETE - 删除文件（保护性路径拒绝删除，返回 403）
 * 参数: req - HTTP 请求句柄（method 决定操作，查询参数 path 指定文件路径）
 * 返回: 根据操作结果返回 ESP_OK 或相应错误码；不支持的方法返回 405
 */
esp_err_t web_console_file_handler(httpd_req_t *req)
{
    char *path = web_console_query_value(req, "path");
    if (!path || !web_console_path_is_safe(path)) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (req->method == HTTP_GET) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            heap_caps_free(path);
            return web_console_send_error(req, HTTPD_404_NOT_FOUND, "file not found");
        }
        httpd_resp_set_type(req, "application/octet-stream");
        char *buf = web_console_psram_alloc(WEB_IO_CHUNK);
        if (!buf) {
            fclose(f);
            heap_caps_free(path);
            return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        }
        size_t n;
        while ((n = fread(buf, 1, WEB_IO_CHUNK, f)) > 0) {
            if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
        }
        heap_caps_free(buf);
        fclose(f);
        heap_caps_free(path);
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    if (req->method == HTTP_PUT) {
        FILE *f = fopen(path, "wb");
        if (!f) {
            heap_caps_free(path);
            return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        }
        esp_err_t err = web_console_read_request_to_file(req, f);
        fclose(f);
        heap_caps_free(path);
        return err == ESP_OK ? web_console_send_text(req, "{\"ok\":true}", "application/json") :
            web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    if (req->method == HTTP_DELETE) {
        if (web_console_protected_delete_path(path)) {
            heap_caps_free(path);
            return web_console_send_error(req, HTTPD_403_FORBIDDEN, "protected path");
        }
        int ret = remove(path);
        heap_caps_free(path);
        return ret == 0 ? web_console_send_text(req, "{\"ok\":true}", "application/json") :
            web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
    }
    heap_caps_free(path);
    return web_console_send_error(req, HTTPD_405_METHOD_NOT_ALLOWED, "method");
}

/*
 * 处理 POST /api/mkdir?path=... 请求。在指定路径创建目录（权限 0755）。
 * 若目录已存在（EEXIST）视为成功。
 * 参数: req - HTTP 请求句柄（查询参数 path 指定要创建的目录路径）
 * 返回: 成功返回 {"ok":true}；路径不安全返回 400；创建失败返回 500
 */
esp_err_t web_console_mkdir_handler(httpd_req_t *req)
{
    char *path = web_console_query_value(req, "path");
    if (!path || !web_console_path_is_safe(path)) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    int ret = mkdir(path, 0755);
    heap_caps_free(path);
    return (ret == 0 || errno == EEXIST) ?
        web_console_send_text(req, "{\"ok\":true}", "application/json") :
        web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
}
