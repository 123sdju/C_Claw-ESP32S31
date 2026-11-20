#include "web_console_internal.h"

#include "ota_service.h"
#include <limits.h>

/**
 * @brief 同步转交 OTA 状态查询，状态快照和响应构造均由服务负责。
 * @param[in,out] req 必填 HTTPD 请求借用，覆盖服务传输回调及发送期间。
 * @return 原样返回 ota_service_status 结果，ESP_OK 表示响应发送成功，状态内容由正文表达。
 */
esp_err_t web_console_ota_status_handler(httpd_req_t *req)
{
    return ota_service_status(req);
}

/**
 * @brief 同步转交上传准备，服务检查 name/size/SHA 等查询并管理维护租约。
 * @param[in,out] req 必填 HTTPD 请求借用，查询文本只在服务调用期读取。
 * @return 原样返回准备入口结果，HTTP 错误响应也可能发送成功，调用者读取状态和正文。
 * @note 服务负责操作互斥、已准备会话替换与失败恢复，本包装器不持有请求或准备资源。
 */
esp_err_t web_console_ota_prepare_handler(httpd_req_t *req)
{
    return ota_service_prepare(req);
}

/**
 * @brief 检查正文长度可由 OTA 服务的 int 参数表示后转交上传。
 * @param[in,out] req HTTPD 请求借用，正常入口必填，NULL 沿旧路径传零长度给服务。
 * @return 过长正文发送 HTTP 400，其他原样返回上传入口结果，成功响应中的 flashing 表示后台已接管镜像。
 * @note 服务同步接收请求正文并独立拥有验证后的镜像，后台作业不保留本请求，Flash 完成和健康确认另读 OTA 状态。
 */
esp_err_t web_console_ota_upload_handler(httpd_req_t *req)
{
    if (req && req->content_len > INT_MAX)
        return web_console_send_error(req, HTTPD_400_BAD_REQUEST, "OTA request length exceeds service range");
    return ota_service_upload(req, req ? (int)req->content_len : 0);
}

/**
 * @brief 同步转交下次启动分区选择，实际安全检查和维护协调由 OTA 服务执行。
 * @param[in,out] req 必填 HTTPD 请求借用，包含 label 查询，寿命覆盖响应发送。
 * @return 原样返回选择入口结果，实际 boot 分区与是否需要重启由响应和状态表达。
 */
esp_err_t web_console_ota_select_handler(httpd_req_t *req)
{
    return ota_service_select(req);
}

/**
 * @brief 同步请求 OTA 服务安排重启，关闭与延迟重启由其后台任务负责。
 * @param[in,out] req 必填 HTTPD 请求借用，仅覆盖当前请求处理和响应发送。
 * @return 原样返回重启入口结果，已发送成功响应不表示设备已经完成重启或新镜像健康确认。
 */
esp_err_t web_console_ota_reboot_handler(httpd_req_t *req)
{
    return ota_service_reboot(req);
}

/**
 * @brief 同步转交 OTA 取消，操作状态、维护退出及恢复由服务管理。
 * @param[in,out] req 必填 HTTPD 请求借用，覆盖服务同步处理和发送期间。
 * @return 原样返回取消入口结果，服务可能因 Flash/重启进行中拒绝，具体状态读取 HTTP 响应。
 */
esp_err_t web_console_ota_cancel_handler(httpd_req_t *req)
{
    return ota_service_cancel(req);
}
