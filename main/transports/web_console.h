#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

typedef struct {
    const app_runtime_io_config_t *io;
    /* Returns an owned redacted JSON snapshot; the Web handler frees it. */
    char *(*config_json)(void *user_data);
    esp_err_t (*config_applied)(const char *json,
                                app_config_apply_result_t *out_result,
                                void *user_data);
    char *(*status_json)(void *user_data);
    char *(*events_json)(uint32_t since, int limit, uint32_t kind_mask, void *user_data);
    esp_err_t (*message_submitted)(const char *text, void *user_data);
    void *user_data;
} web_console_context_t;

/*
 * 启动 Web 控制台 HTTP 服务器，注册所有 API 路由并初始化 OTA 传输适配器。
 * 参数: context - 包含 IO 配置、config_json、config_applied、status_json、
 *                 events_json、message_submitted 回调及 user_data 的上下文
 * 返回: ESP_OK 表示成功或已运行；否则返回 ESP 错误码
 */
esp_err_t web_console_start(const web_console_context_t *context);
/*
 * 停止 Web 控制台 HTTP 服务器并清空全局上下文。
 * 返回: ESP_OK
 */
esp_err_t web_console_stop(void);
/* 使用最近一次 start 保存的上下文恢复 Web 服务。 */
esp_err_t web_console_restart(void);
/* 停止并清除保存的上下文；用于最终 destroy。 */
esp_err_t web_console_destroy(void);
