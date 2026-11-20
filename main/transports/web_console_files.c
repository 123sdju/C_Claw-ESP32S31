#include "web_console_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cc/util/cc_string_builder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/**
 * @brief 按 FAT 不覆盖目标的规则，将旧文件移到备份后安装本次上传。
 * @param[in] temporary 必填本次独占临时文件路径借用，完整写入及同步已结束。
 * @param[in] path 必填目标路径借用，存在时须普通文件。
 * @param[in] backup 必填且原本不存在的备份路径借用，外层协调同目录写入。
 * @param[out] backup_retained 必填输出，成功填写旧文件备份是否因删除失败而保留，失败保持原值。
 * @return 安装完成为 ESP_OK，stat/rename 失败为 ESP_FAIL，安装失败时尽力恢复旧文件，恢复失败保留备份。
 * @note 两次 rename 之间有中间状态，目录和介质持久性沿 VFS/FAT，调用者继续保管失败恢复材料。
 */
static esp_err_t web_console_commit_upload(const char *temporary, const char *path,
                                         const char *backup, bool *backup_retained)
{
    struct stat st;
    bool had_previous = stat(path, &st) == 0;
    if (!had_previous && errno != ENOENT) return ESP_FAIL;
    if (had_previous && (!S_ISREG(st.st_mode) || rename(path, backup) != 0)) return ESP_FAIL;
    if (rename(temporary, path) != 0) {
        if (had_previous && rename(backup, path) != 0) {
            ESP_LOGE("web_files", "upload commit and restore failed; keep backup: %s", backup);
        }
        return ESP_FAIL;
    }
    *backup_retained = had_previous && remove(backup) != 0;
    return ESP_OK;
}

/**
 * @brief 独占创建上传临时文件，完整接收并同步后复核权限、备份安装目标。
 * @param[in,out] req 必填 HTTPD 请求借用，正文最多八 MiB，零长度可创建空文件。
 * @param[in] path 必填合法且已获修改许可的稳定路径借用，至少保留 sidecar 后缀容量。
 * @return 返回成功或错误响应的发送结果，大小/路径预算为 HTTP 400，写入、同步、权限复核或安装失败为 500。
 * @note 本次拥有 fd/FILE 并清理临时文件，既有 tmp/bak 不覆盖，失败可能已移动目标或留下备份。
 * 生命周期在卸载 VFS 前停止 HTTPD 并排空本 handler，权限回调只提供当前快照，不长期持有配置锁。
 */
static esp_err_t web_console_upload_file(httpd_req_t *req, const char *path)
{
    if (!req) return ESP_ERR_INVALID_ARG;
    if (req->content_len > WEB_MAX_UPLOAD_BYTES) {
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST,
                                      "upload exceeds the 8 MiB limit");
    }
    char temporary[WEB_MAX_PATH];
    char backup[WEB_MAX_PATH];
    if (strlen(path) + strlen(WEB_UPLOAD_TEMP_SUFFIX) >= sizeof(temporary) ||
        strlen(path) + strlen(WEB_UPLOAD_BACKUP_SUFFIX) >= sizeof(backup)) {
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "path too long for upload recovery");
    }
    snprintf(temporary, sizeof(temporary), "%s%s", path, WEB_UPLOAD_TEMP_SUFFIX);
    snprintf(backup, sizeof(backup), "%s%s", path, WEB_UPLOAD_BACKUP_SUFFIX);
    struct stat st;
    if (stat(backup, &st) == 0 || errno != ENOENT) {
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "previous upload backup requires recovery");
    }
    /* O_EXCL 不覆盖上次崩溃遗留的临时文件；由用户离线核对后恢复。 */
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                            "cannot create upload temporary file");
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        remove(temporary);
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
    }
    esp_err_t err = web_console_read_request_to_file(req, f);
    if (err == ESP_OK && fflush(f) != 0) err = ESP_FAIL;
    if (err == ESP_OK && fsync(fileno(f)) != 0) err = ESP_FAIL;
    if (fclose(f) != 0) err = ESP_FAIL;
    bool backup_retained = false;
    /* 配置可能在上传期间变化，提交前再次检查运行文件保护。 */
    if (err == ESP_OK && !web_console_file_access_allowed(path, true)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = web_console_commit_upload(temporary, path, backup, &backup_retained);
    if (err != ESP_OK) {
        remove(temporary);
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                      "upload failed; inspect original and recovery files");
    }
    return web_console_send_text(req, backup_retained ?
        "{\"ok\":true,\"backup_retained\":true}" : "{\"ok\":true}", "application/json");
}

/**
 * @brief 检查目录读取许可，构造含 path 与 entries 的完整目录响应。
 * @param[in,out] req 必填 HTTPD 请求借用，仅缺少 path 查询时使用 /usb0，临时路径、DIR 与 JSON 均由本次调用清理。
 * @return 返回正文或错误响应的发送结果，路径无效为 HTTP 400，保护区为 403，无法打开目录为 404，构造 OOM 为 500。
 * @note 跳过点目录及拼接超容量项，stat 失败沿既有零元数据表示，枚举不提供目录一致性快照。
 * owner 在 VFS 卸载前排空 handler，g_web_console_context 在整个请求期保持稳定。
 */
esp_err_t web_console_files_handler(httpd_req_t *req)
{
    char *path = NULL;
    esp_err_t query_err = web_console_query_value_checked(req, "path", &path);
    if (query_err == ESP_ERR_NOT_FOUND) {
        path = web_console_psram_alloc(strlen(WEB_ROOT) + 1);
        if (!path) return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        strcpy(path, WEB_ROOT);
    } else if (query_err != ESP_OK) {
        return web_console_send_error(req, query_err == ESP_ERR_NO_MEM ?
            HTTPD_500_INTERNAL_SERVER_ERROR : HTTPD_400_BAD_REQUEST, "path query unavailable");
    }
    if (!web_console_path_is_safe(path)) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (!web_console_file_access_allowed(path, false)) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_403_FORBIDDEN, "protected directory");
    }
    DIR *dir = opendir(path);
    if (!dir) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_404_NOT_FOUND, "not a directory");
    }
    cc_string_builder_t sb;
    cc_result_t rc = cc_string_builder_init(&sb);
    if (rc.code != CC_OK) {
        cc_result_free(&rc);
        closedir(dir);
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
    }
    cc_result_free(&rc);
    rc = cc_string_builder_append(&sb, "{\"path\":");
    cc_result_free(&rc);
    web_console_json_escape_append(&sb, path);
    rc = cc_string_builder_append(&sb, ",\"entries\":[");
    cc_result_free(&rc);
    bool first = true;
    struct dirent *ent;
    while (cc_string_builder_error(&sb) == CC_OK && (ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[WEB_MAX_PATH];
        if (!web_console_join_path(full, sizeof(full), path, ent->d_name)) continue;
        struct stat st = {0};
        stat(full, &st);
        if (!first) {
            rc = cc_string_builder_append_char(&sb, ',');
            cc_result_free(&rc);
        }
        first = false;
        rc = cc_string_builder_append(&sb, "{\"name\":");
        cc_result_free(&rc);
        web_console_json_escape_append(&sb, ent->d_name);
        rc = cc_string_builder_appendf(&sb, ",\"dir\":%s,\"size\":%u}",
                                  S_ISDIR(st.st_mode) ? "true" : "false",
                                  (unsigned)st.st_size);
        cc_result_free(&rc);
    }
    closedir(dir);
    rc = cc_string_builder_append(&sb, "]}");
    cc_result_free(&rc);
    char *json = cc_string_builder_take(&sb);
    heap_caps_free(path);
    if (!json) return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                             "directory listing unavailable");
    esp_err_t err = web_console_send_text(req, json, "application/json");
    free(json);
    return err;
}

/**
 * @brief 按请求方法执行下载、临时上传安装或删除，并先检查路径与 owner 许可。
 * @param[in,out] req 必填 HTTPD 请求借用，method 决定 GET/PUT/DELETE，path 查询须完整合法。
 * @return 返回响应发送结果，路径错误为 HTTP 400，许可拒绝为 403，读取打开失败为 404，其他操作失败为 500。
 * 下载已发出分块后的读取或发送失败直接返回错误，不发送正常结束块，可能已有部分正文到达客户端。
 * @note 文件、路径和 PSRAM 块只在本次 handler 内拥有。PUT 委托备份安装，DELETE 直接调用 remove，其他方法在权限通过后形成 405。
 * HTTPD 停止排空保护请求及 FILE 的寿命，真实拔盘和底层关闭失败仍可能返回不完整结果。
 */
esp_err_t web_console_file_handler(httpd_req_t *req)
{
    char *path = NULL;
    esp_err_t query_err = web_console_query_value_checked(req, "path", &path);
    if (query_err != ESP_OK) return web_console_send_error(req, query_err == ESP_ERR_NO_MEM ?
        HTTPD_500_INTERNAL_SERVER_ERROR : HTTPD_400_BAD_REQUEST, "path query unavailable");
    if (!path || !web_console_path_is_safe(path)) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (!web_console_file_access_allowed(path, req->method != HTTP_GET)) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_403_FORBIDDEN,
                                      "file access denied: protected path or configuration pending");
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
        esp_err_t send_err = ESP_OK;
        while ((n = fread(buf, 1, WEB_IO_CHUNK, f)) > 0) {
            send_err = httpd_resp_send_chunk(req, buf, n);
            if (send_err != ESP_OK) break;
        }
        if (ferror(f)) send_err = ESP_FAIL;
        heap_caps_free(buf);
        fclose(f);
        heap_caps_free(path);
        return send_err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : send_err;
    }
    if (req->method == HTTP_PUT) {
        esp_err_t err = web_console_upload_file(req, path);
        heap_caps_free(path);
        return err;
    }
    if (req->method == HTTP_DELETE) {
        int ret = remove(path);
        heap_caps_free(path);
        return ret == 0 ? web_console_send_text(req, "{\"ok\":true}", "application/json") :
            web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
    }
    heap_caps_free(path);
    return web_console_send_error(req, HTTPD_405_METHOD_NOT_ALLOWED, "method");
}

/**
 * @brief 检查修改许可后创建目录，已存在时确认实际对象也是目录。
 * @param[in,out] req 必填 HTTPD 请求借用，path 查询拥有副本在本次调用释放。
 * @return 新建或已存在目录发送成功正文，路径错误为 HTTP 400，许可拒绝为 403，其他情况发送 500，返回值为发送结果。
 * @note 原生 mkdir 使用 0755，权限含义由 VFS 决定，函数不递归创建父目录。
 */
esp_err_t web_console_mkdir_handler(httpd_req_t *req)
{
    char *path = NULL;
    esp_err_t query_err = web_console_query_value_checked(req, "path", &path);
    if (query_err != ESP_OK) return web_console_send_error(req, query_err == ESP_ERR_NO_MEM ?
        HTTPD_500_INTERNAL_SERVER_ERROR : HTTPD_400_BAD_REQUEST, "path query unavailable");
    if (!path || !web_console_path_is_safe(path)) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    if (!web_console_file_access_allowed(path, true)) {
        heap_caps_free(path);
        return web_console_send_error(req, HTTPD_403_FORBIDDEN, "protected directory");
    }
    int ret = mkdir(path, 0755);
    int error = errno;
    struct stat st;
    bool exists = ret != 0 && error == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    heap_caps_free(path);
    return (ret == 0 || exists) ?
        web_console_send_text(req, "{\"ok\":true}", "application/json") :
        web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mkdir failed");
}
