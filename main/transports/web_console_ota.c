#include "web_console_internal.h"

#include "ota_service.h"

/*
 * 处理 GET /api/ota/status 请求。直接委托 ota_service_status 获取 OTA 状态。
 * 参数: req - HTTP 请求句柄
 * 返回: 委托 ota_service_status 的返回值
 */
esp_err_t web_console_ota_status_handler(httpd_req_t *req)
{
    return ota_service_status(req);
}

/*
 * 处理 POST /api/ota/prepare 请求。委托 ota_service_prepare 准备 OTA 会话。
 * 参数: req - HTTP 请求句柄
 * 返回: 委托 ota_service_prepare 的返回值
 */
esp_err_t web_console_ota_prepare_handler(httpd_req_t *req)
{
    return ota_service_prepare(req);
}

/*
 * 处理 POST /api/ota/upload 请求。委托 ota_service_upload 上传固件数据。
 * 参数: req - HTTP 请求句柄（content_len 为固件文件大小）
 * 返回: 委托 ota_service_upload 的返回值
 */
esp_err_t web_console_ota_upload_handler(httpd_req_t *req)
{
    return ota_service_upload(req, req ? req->content_len : 0);
}

/*
 * 处理 POST /api/ota/select 请求。委托 ota_service_select 选择 OTA 引导分区。
 * 参数: req - HTTP 请求句柄
 * 返回: 委托 ota_service_select 的返回值
 */
esp_err_t web_console_ota_select_handler(httpd_req_t *req)
{
    return ota_service_select(req);
}

/*
 * 处理 POST /api/ota/reboot 请求。委托 ota_service_reboot 重启设备。
 * 参数: req - HTTP 请求句柄
 * 返回: 委托 ota_service_reboot 的返回值
 */
esp_err_t web_console_ota_reboot_handler(httpd_req_t *req)
{
    return ota_service_reboot(req);
}

/*
 * 处理 POST /api/ota/cancel 请求。委托 ota_service_cancel 取消进行中的 OTA。
 * 参数: req - HTTP 请求句柄
 * 返回: 委托 ota_service_cancel 的返回值
 */
esp_err_t web_console_ota_cancel_handler(httpd_req_t *req)
{
    return ota_service_cancel(req);
}
