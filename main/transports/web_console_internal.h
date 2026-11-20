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
#define WEB_MAX_UPLOAD_BYTES (8U * 1024U * 1024U)
/* 请求总期限；单次 socket recv 仍受 HTTPD 的 recv_wait_timeout 约束。 */
#define WEB_REQUEST_TIMEOUT_US (60LL * 1000LL * 1000LL)
#define WEB_UPLOAD_TEMP_SUFFIX ".web-upload.tmp"
#define WEB_UPLOAD_BACKUP_SUFFIX ".web-upload.bak"

/** @brief 生命周期切换入口拥有的server，原生stop成功后才清空。 */
extern httpd_handle_t g_web_console_server;
/** @brief handler借用的稳定上下文，替换或清空前先通过HTTPD停止排空请求，IO指向模块自有标量快照。 */
extern web_console_context_t g_web_console_context;

/** @brief 读取事件请求的独立热限制。 @return 最近原子发布的默认事件条数，初始化为 120。 */
int web_console_event_limit(void);
/** @brief 读取输入请求的独立热限制。 @return 最近原子发布的最大输入字节数，初始化为 1024。 */
int web_console_max_input_bytes(void);

/**
 * @brief 分配八位可寻址 PSRAM 缓冲。
 * @param[in] size 请求字节数。
 * @return 拥有缓冲供 heap_caps_free，分配失败为 NULL。
 */
char *web_console_psram_alloc(size_t size);
/**
 * @brief 分配八位可寻址内部 DRAM 缓冲。
 * @param[in] size 请求字节数。
 * @return 拥有缓冲供 heap_caps_free，分配失败为 NULL。
 */
char *web_console_internal_alloc(size_t size);
/**
 * @brief 提取并解码查询值，向文件入口区分缺省、非法和内存错误。
 * @param[in] req 调用期借用 HTTPD 请求，NULL 为参数错误。
 * @param[in] key 调用期借用 NUL 键名，NULL 为参数错误，重复键按 HTTPD 首项语义。
 * @param[out] out_value 必填无待释放资源槽，先置 NULL，成功得到须 heap_caps_free 的独立 PSRAM 值。
 * @return ESP_OK 成功，NOT_FOUND 表示无 query 或缺键，INVALID_ARG 表示参数、编码或编码值容量错误，NO_MEM 为分配失败。
 */
esp_err_t web_console_query_value_checked(httpd_req_t *req, const char *key, char **out_value);
/**
 * @brief 提取指定 URL 查询值并解码百分号和加号。
 * @param[in] req 必需借用 HTTP 请求。
 * @param[in] key 必需借用键名。
 * @return PSRAM 拥有字符串供 heap_caps_free，查询缺失、过长、非法编码或分配失败为 NULL，拒绝解码 NUL。
 */
char *web_console_query_value(httpd_req_t *req, const char *key);
/**
 * @brief 检查路径属于 /usb0 并符合通用文件 API 的规范名称规则。
 * @param[in] path 借用 NUL 结尾路径，NULL 返回 false。
 * @return 长度小于 WEB_MAX_PATH 且通过根边界、点段、FAT 别名和非法字符检查为 true，不查询文件系统。
 */
bool web_console_path_is_safe(const char *path);
/**
 * @brief 结合通用路径规则和当前业务 owner 回调判定访问权限。
 * @param[in] path 借用路径，NULL 或不规范时拒绝。
 * @param[in] write true 检查修改，false 检查读取。
 * @return true 允许，保留配置/恢复路径、上传 sidecar、owner 回调缺失或拒绝均为 false。
 */
bool web_console_file_access_allowed(const char *path, bool write);
/**
 * @brief 按需补斜杠拼接目录和名称，不执行路径权限检查。
 * @param[out] out 必需可写输出，与输入不重叠，失败保持原内容。
 * @param[in] out_size 输出容量，单位字节，包含末尾 NUL。
 * @param[in] dir 必需借用目录串，允许空串。
 * @param[in] name 必需借用名称串，允许空串。
 * @return 完整拼接为 true，参数错误或容量不足为 false。
 */
bool web_console_join_path(char *out, size_t out_size, const char *dir, const char *name);
/**
 * @brief 检查删除保护根及当前日志路径。
 * @param[in] path 借用规范路径，NULL 保守拒绝。
 * @return 配置名称族、cclaw 根、USB/workspace 根或当前日志名称族命中为 true，其余为 false。
 */
bool web_console_protected_delete_path(const char *path);
/**
 * @brief 设置 Content-Type 和 keep-alive 后同步发送文本。
 * @param[in,out] req 必需借用 HTTP 请求。
 * @param[in] text 借用 NUL 结尾正文，NULL 发送空串。
 * @param[in] type 借用 Content-Type，NULL 使用 text/plain。
 * @return 正文发送的原生结果，头设置结果当前不单独返回，不接管输入。
 */
esp_err_t web_console_send_text(httpd_req_t *req, const char *text, const char *type);
/**
 * @brief 设置关闭连接并同步发送 HTTP 错误响应。
 * @param[in,out] req 必需借用请求。
 * @param[in] code HTTPD 错误状态枚举。
 * @param[in] message 借用描述，NULL 使用 error。
 * @return 原生错误响应发送结果，不代表业务成功，不接管描述。
 */
esp_err_t web_console_send_error(httpd_req_t *req, httpd_err_code_t code, const char *message);
/**
 * @brief 追加带双引号的 JSON 字符串，转义控制字节和反斜杠。
 * @param[in,out] sb 已初始化借用构建器，分配失败沿其 sticky error 保存。
 * @param[in] text 借用 NUL 结尾输入，NULL 按空串，其他非控制字节原样追加。
 * @post 完整片段追加或构建器保存错误，调用者通过 take 等入口检查，不接管输入。
 */
void web_console_json_escape_append(cc_string_builder_t *sb, const char *text);
/**
 * @brief 用内部 DRAM 分块缓冲将请求正文写到已打开文件。
 * @param[in,out] req 借用请求，总长度最多 WEB_MAX_UPLOAD_BYTES。
 * @param[in,out] f 借用可写 FILE，函数不关闭、不回滚已写入片段。
 * @return 参数或长度错误为 INVALID_SIZE，分配失败为 NO_MEM，总期限为 TIMEOUT，接收/写入失败为 FAIL，写完为 ESP_OK。
 * @note 缓冲所有出口释放，单次 recv 超时重试至六十秒总期限。
 */
esp_err_t web_console_read_request_to_file(httpd_req_t *req, FILE *f);

/**
 * @brief 响应根路径，按模式发送内嵌启动页或完整控制台。
 * @param[in,out] req 借用 HTTP 请求，覆盖发送期。
 * @return 文本响应发送结果。
 */
esp_err_t web_console_index_handler(httpd_req_t *req);
/**
 * @brief 请求并发送拥有的设备状态 JSON，发送后 free。
 * @param[in,out] req 借用 HTTP 请求，覆盖业务回调和发送期。
 * @return 响应发送结果，缺失状态正文形成 HTTP 500。
 */
esp_err_t web_console_status_handler(httpd_req_t *req);
/**
 * @brief 按 since/limit/kind 请求事件 JSON，默认条数读取当前热限制。
 * @param[in,out] req 借用 HTTP 请求，覆盖查询、回调和发送期。
 * @return 响应发送结果，拥有查询值和正文在本次调用内清理。
 */
esp_err_t web_console_events_handler(httpd_req_t *req);
/**
 * @brief 按当前字节限制接收完整消息并同步提交业务回调。
 * @param[in,out] req 必需借用请求，正文缓冲仅在回调期间借用。
 * @return 响应发送结果，HTTP 状态和正文反映大小、接收、超时和提交结果。
 */
esp_err_t web_console_input_handler(httpd_req_t *req);
/**
 * @brief 调用 owner 生成脱敏配置 JSON 并发送，随后 free 正文。
 * @param[in,out] req 借用 HTTP 请求，覆盖回调与发送期。
 * @return 配置正文或错误响应的发送结果。
 */
esp_err_t web_console_config_get_handler(httpd_req_t *req);
/**
 * @brief 接收配置正文并同步交给事务 owner 应用，发送逐阶段结果。
 * @param[in,out] req 借用 HTTP 请求，所有正文和报告临时资源在本次调用内清理。
 * @return 请求处理或响应发送结果，HTTP 状态反映配置应用情况。
 */
esp_err_t web_console_config_put_handler(httpd_req_t *req);
/**
 * @brief 检查 path 的读取权限后列出目录。
 * @param[in,out] req 借用 HTTP 请求，覆盖查询、遍历与发送期。
 * @return 目录 JSON 或错误响应的发送结果，目录和临时路径由 handler 清理。
 */
esp_err_t web_console_files_handler(httpd_req_t *req);
/**
 * @brief 按请求方法与 owner 权限执行文件下载、上传或删除。
 * @param[in,out] req 借用请求，文件和临时缓冲由本次 handler 清理。
 * @return 操作响应结果，失败由 HTTP 状态和正文报告，上传遵循临时与备份安装流程。
 */
esp_err_t web_console_file_handler(httpd_req_t *req);
/**
 * @brief 检查 path 修改权限后创建目录。
 * @param[in,out] req 借用 HTTP 请求，路径副本在本次调用清理。
 * @return 成功正文或错误响应的发送结果。
 */
esp_err_t web_console_mkdir_handler(httpd_req_t *req);
/**
 * @brief 将 HTTP 请求转交 OTA 状态服务。
 * @param[in,out] req 借用 HTTP 请求，覆盖同步转交。
 * @return OTA 状态入口的响应结果。
 */
esp_err_t web_console_ota_status_handler(httpd_req_t *req);
/**
 * @brief 将 HTTP 请求转交 OTA 准备服务。
 * @param[in,out] req 借用 HTTP 请求，覆盖同步转交。
 * @return OTA 准备入口的响应结果。
 */
esp_err_t web_console_ota_prepare_handler(httpd_req_t *req);
/**
 * @brief 将 HTTP 请求体转交 OTA 上传服务。
 * @param[in,out] req 借用 HTTP 请求，覆盖服务接收与响应期。
 * @return OTA 上传入口的响应结果。
 */
esp_err_t web_console_ota_upload_handler(httpd_req_t *req);
/**
 * @brief 将分区选择请求转交 OTA 服务。
 * @param[in,out] req 借用 HTTP 请求，覆盖同步转交。
 * @return OTA 分区选择入口的响应结果。
 */
esp_err_t web_console_ota_select_handler(httpd_req_t *req);
/**
 * @brief 将重启请求转交 OTA 服务，实际重启由其调度。
 * @param[in,out] req 借用 HTTP 请求，覆盖同步响应期。
 * @return OTA 重启入口的响应结果。
 */
esp_err_t web_console_ota_reboot_handler(httpd_req_t *req);
/**
 * @brief 将取消请求转交 OTA 服务。
 * @param[in,out] req 借用 HTTP 请求，覆盖同步转交。
 * @return OTA 取消入口的响应结果。
 */
esp_err_t web_console_ota_cancel_handler(httpd_req_t *req);
