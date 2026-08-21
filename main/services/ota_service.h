#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char *(*alloc_io)(size_t size);
    esp_err_t (*send_text)(void *request, const char *text, const char *type);
    esp_err_t (*send_error)(void *request, int code, const char *message);
    char *(*query_value)(void *request, const char *key);
    int (*recv)(void *request, char *buffer, int length);
    void (*json_escape_append)(void *builder, const char *text);
    esp_err_t (*stop_web)(void *user_data);
    esp_err_t (*start_web)(void *user_data);
    int request_timeout_code;
    int bad_request_code;
    int server_error_code;
    size_t io_chunk;
    void *user_data;
} ota_service_transport_t;

/*
 * 初始化 OTA 服务：保存传输层函数指针表，初始化互斥锁。
 * 参数: transport - 传输层接口（send_text/send_error/query_value/recv/stop_web 等回调）
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_ARG 缺少必需回调，ESP_ERR_NO_MEM 互斥锁创建失败
 */
esp_err_t ota_service_init(const ota_service_transport_t *transport);
/*
 * 返回 OTA 服务 JSON 状态（阶段、管理器状态、进度、分区信息、资源快照等）。
 * 参数: request - HTTP 请求指针（透传给传输层）
 * 返回: 传输层 send_text/send_error 返回值
 */
esp_err_t ota_service_status(void *request);
/*
 * 准备 OTA 上传：验证镜像大小、等待交互空闲、进入维护模式。
 * 参数: request - HTTP 请求指针（用于提取 name/size/time 参数）
 * 返回: 传输层 send_text/send_error 返回值
 */
esp_err_t ota_service_prepare(void *request);
/*
 * 执行 OTA 上传：接收固件二进制到 PSRAM、创建 flash 任务异步写入 flash。
 * 参数: request     - HTTP 请求指针
 * 参数: content_len - Content-Length（固件字节数）
 * 返回: 传输层 send_text/send_error 返回值
 */
esp_err_t ota_service_upload(void *request, int content_len);
/*
 * 选择指定标签的分区为下次启动分区。
 * 参数: request - HTTP 请求指针（提取 label 参数）
 * 返回: JSON 响应（含 pending_reboot/reboot_required 标志）
 */
esp_err_t ota_service_select(void *request);
/*
 * 触发设备重启。通过独立的 ota_reboot_task 执行，延迟约 1.2s 后调用 esp_restart。
 * 参数: request - HTTP 请求指针
 * 返回: JSON {"ok":true,"rebooting":true}
 */
esp_err_t ota_service_reboot(void *request);
/*
 * 取消当前 OTA 操作：调用 ota_manager_cancel、离开维护模式、恢复摄像头预览。
 * 参数: request - HTTP 请求指针
 * 返回: JSON {"ok":true}
 */
esp_err_t ota_service_cancel(void *request);
