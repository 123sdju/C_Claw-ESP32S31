#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "cc/util/cc_string_builder.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "web_console.h"

#define WEB_ROOT "/usb0"
#define WEB_MAX_PATH 256
#define WEB_IO_CHUNK 4096

extern httpd_handle_t g_web_console_server;
extern web_console_context_t g_web_console_context;

/*
 * 从 PSRAM 分配指定大小的内存块（8位可寻址）。
 * 参数: size - 字节数
 * 返回: 内存指针或 NULL
 */
char *web_console_psram_alloc(size_t size);
/*
 * 从内部 DRAM 分配指定大小的内存块（8位可寻址）。
 * 参数: size - 字节数
 * 返回: 内存指针或 NULL
 */
char *web_console_internal_alloc(size_t size);
/*
 * 提取 HTTP 请求 URL 查询参数的值（PSRAM 分配，调用方释放）。
 * 参数: req - HTTP 请求句柄  key - 参数名
 * 返回: 值字符串或 NULL
 */
char *web_console_query_value(httpd_req_t *req, const char *key);
/*
 * 校验路径是否安全（在 /usb0 下、无 ..、长度 < 256）。
 * 参数: path - 文件系统路径
 * 返回: true 表示安全
 */
bool web_console_path_is_safe(const char *path);
/*
 * 拼接目录和名称生成完整路径。
 * 参数: out - 输出缓冲  out_size - 输出大小  dir - 目录  name - 名称
 * 返回: true 表示成功
 */
bool web_console_join_path(char *out, size_t out_size, const char *dir, const char *name);
/*
 * 判断路径是否受保护不可删除（配置文件、关键目录、当前日志）。
 * 参数: path - 待检查路径
 * 返回: true 表示不可删除
 */
bool web_console_protected_delete_path(const char *path);
/*
 * 发送文本 HTTP 响应（含 Content-Type 和 keep-alive 头）。
 * 参数: req - 请求句柄  text - 响应文本  type - Content-Type
 * 返回: ESP_OK 或错误
 */
esp_err_t web_console_send_text(httpd_req_t *req, const char *text, const char *type);
/*
 * 发送 HTTP 错误响应（Connection: close）。
 * 参数: req - 请求句柄  code - 错误码  message - 错误描述
 * 返回: ESP_OK 或错误
 */
esp_err_t web_console_send_error(httpd_req_t *req, httpd_err_code_t code, const char *message);
/*
 * 将字符串以 JSON 转义格式追加到字符串构建器。
 * 参数: sb - 构建器  text - 原始字符串
 */
void web_console_json_escape_append(cc_string_builder_t *sb, const char *text);
/*
 * 将 HTTP 请求体流式写入文件。
 * 参数: req - 请求句柄  f - 已打开的文件
 * 返回: ESP_OK 或错误
 */
esp_err_t web_console_read_request_to_file(httpd_req_t *req, FILE *f);

/*
 * GET / - 返回内嵌 SPA HTML 页面。
 */
esp_err_t web_console_index_handler(httpd_req_t *req);
/*
 * GET /api/status - 返回设备运行时状态 JSON。
 */
esp_err_t web_console_status_handler(httpd_req_t *req);
/*
 * GET /api/events - 返回事件流 JSON（支持 since/limit/kind 查询参数）。
 */
esp_err_t web_console_events_handler(httpd_req_t *req);
/*
 * POST /api/messages - 提交消息文本到交互服务。
 */
esp_err_t web_console_input_handler(httpd_req_t *req);
/*
 * GET /api/config - 获取当前配置 JSON。
 */
esp_err_t web_console_config_get_handler(httpd_req_t *req);
/*
 * PUT /api/config - 更新配置 JSON。
 */
esp_err_t web_console_config_put_handler(httpd_req_t *req);
/*
 * GET /api/files?path=... - 列出目录内容。
 */
esp_err_t web_console_files_handler(httpd_req_t *req);
/*
 * GET/PUT/DELETE /api/file?path=... - 下载/上传/删除文件。
 */
esp_err_t web_console_file_handler(httpd_req_t *req);
/*
 * POST /api/mkdir?path=... - 创建目录。
 */
esp_err_t web_console_mkdir_handler(httpd_req_t *req);
/*
 * GET /api/ota/status - 获取 OTA 状态。
 */
esp_err_t web_console_ota_status_handler(httpd_req_t *req);
/*
 * POST /api/ota/prepare - 准备 OTA 会话。
 */
esp_err_t web_console_ota_prepare_handler(httpd_req_t *req);
/*
 * POST /api/ota/upload - 上传固件数据。
 */
esp_err_t web_console_ota_upload_handler(httpd_req_t *req);
/*
 * POST /api/ota/select - 选择 OTA 引导分区。
 */
esp_err_t web_console_ota_select_handler(httpd_req_t *req);
/*
 * POST /api/ota/reboot - 重启设备。
 */
esp_err_t web_console_ota_reboot_handler(httpd_req_t *req);
/*
 * POST /api/ota/cancel - 取消进行中的 OTA。
 */
esp_err_t web_console_ota_cancel_handler(httpd_req_t *req);
