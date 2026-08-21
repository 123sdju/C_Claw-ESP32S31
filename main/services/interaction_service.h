#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "app_event_bus.h"
#include "cc/app/cc_runtime_builder.h"
#include "esp_err.h"

typedef enum {
    INTERACTION_SERVICE_STOPPED = 0,
    INTERACTION_SERVICE_STARTING,
    INTERACTION_SERVICE_IDLE,
    INTERACTION_SERVICE_QUEUED,
    INTERACTION_SERVICE_RUNNING,
    INTERACTION_SERVICE_CLEARING,
    INTERACTION_SERVICE_PAUSED_MAINTENANCE,
    INTERACTION_SERVICE_CANCELLING,
    INTERACTION_SERVICE_FAILED,
} interaction_service_lifecycle_t;

typedef struct {
    interaction_service_lifecycle_t state;
    bool started;
    bool busy;
    unsigned generation;
    int pending;
    int queue_depth;
    int max_input_bytes;
    bool thinking_enabled;
    bool stream_supported;
    uint32_t queue_high_water;
    uint32_t late_reply_count;
    uint32_t cancel_latency_ms;
    uint64_t owner_heartbeat_ms;
    char session_id[96];
} interaction_service_status_t;

/*
 * 启动交互服务：创建 FreeRTOS 主任务队列、本地命令队列、controller，初始化会话。
 * 参数: io       - IO 配置（队列深度、最大输入字节数、会话基础名等）
 * 参数: runtime  - 已启动的 Agent 运行时
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_ARG 参数无效，ESP_FAIL/ESP_ERR_NO_MEM 失败
 */
esp_err_t interaction_service_start(const app_runtime_io_config_t *io, cc_runtime_builder_t *builder);
esp_err_t interaction_service_stop(uint32_t timeout_ms);
esp_err_t interaction_service_destroy(void);
/*
 * 热应用最大输入长度和 controller/session 配置。queue_depth 是物理队列容量，
 * 只在下一次启动时生效，并由配置事务通过 restart_required 对外报告。
 */
esp_err_t interaction_service_apply_config(const app_runtime_io_config_t *io);
/* 立即触发线程安全 cancel source，并异步排队 owner 状态收敛。 */
esp_err_t interaction_service_request_cancel(app_event_source_t source);
/*
 * 设置交互服务维护暂停状态。OTA 等维护流程应通过此接口显式协调交互服务。
 */
esp_err_t interaction_service_set_maintenance_paused(bool paused, const char *reason);
/*
 * 将文本提交到交互队列。会先检查是否为本地命令（fast/slow），再决定直接处理还是入队。
 * 参数: source - 事件来源（串口/Web）
 * 参数: text   - 提交的文本，以 '\0' 结尾
 * 返回: ESP_OK 入队成功，ESP_ERR_INVALID_STATE/OA/挂起，ESP_ERR_TIMEOUT 队列满
 */
esp_err_t interaction_service_submit_text(app_event_source_t source, const char *text);
/*
 * 提交 /clear 清理请求：清除当前会话内容并通知事件总线。
 * 参数: source - 事件来源
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_STATE 运行时不可用
 */
esp_err_t interaction_service_submit_clear(app_event_source_t source);
/*
 * 阻塞等待交互服务 owner task 发布空闲状态。
 * 参数: timeout_ms - 最大等待时间（0 使用服务默认有限超时）
 * 返回: true 表示超时前进入空闲，false 表示超时
 */
bool interaction_service_wait_idle(uint32_t timeout_ms);
/*
 * 获取交互服务当前状态快照（生命周期状态、繁忙标志、队列深度、thinking/stream 支持等）。
 * 参数: out_status - 输出状态结构体指针
 */
void interaction_service_get_status(interaction_service_status_t *out_status);
/*
 * 将 lifecycle 枚举值转换为可读字符串。
 * 参数: state - 生命周期状态枚举
 * 返回: 对应的字符串（"stopped"/"ready"/"busy"/"clearing"/"failed"/"unknown"）
 */
const char *interaction_service_state_string(interaction_service_lifecycle_t state);
