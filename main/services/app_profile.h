#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/util/cc_string_builder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t app_profile_init(void);
esp_err_t app_profile_stop(uint32_t timeout_ms);
esp_err_t app_profile_destroy(uint32_t timeout_ms);

typedef enum {
    APP_PROFILE_LEVEL_BASIC = 0,
    APP_PROFILE_LEVEL_TRACE,
} app_profile_level_t;

typedef struct {
    bool enabled;
    uint64_t start_us;
    const char *event;
    const char *phase;
    const char *session_id;
    const char *run_id;
} app_profile_span_t;

/*
 * 应用 profile 配置（启用状态、级别、间隔）。
 * 参数: config - profile 配置结构体指针
 */
esp_err_t app_profile_apply_config(const app_profile_config_t *config);
/*
 * 设置 profile 启用状态。
 * 参数: enabled - true 启用，false 停用
 */
void app_profile_set_enabled(bool enabled);
/*
 * 查询 profile 是否启用。
 * 返回: true 已启用，false 未启用
 */
bool app_profile_enabled(void);
/*
 * 设置 profile 采样级别（basic 或 trace）。
 * 参数: level - 采样级别枚举
 */
void app_profile_set_level(app_profile_level_t level);
/*
 * 查询当前 profile 采样级别。
 * 返回: 当前级别枚举值
 */
app_profile_level_t app_profile_level(void);
/*
 * 设置 profile 周期性采样间隔。
 * 参数: interval_ms - 间隔毫秒数（0 表示不启用周期性采样）
 */
void app_profile_set_interval_ms(int interval_ms);
/*
 * 将采样级别枚举转换为可读字符串。
 * 参数: level - 采样级别枚举
 * 返回: "basic" 或 "trace"
 */
const char *app_profile_level_string(app_profile_level_t level);

/*
 * 在 profile 中注册一个 FreeRTOS 任务，用于后续堆栈水位监控。
 * 参数: name   - 任务名称（最多 24 字节）
 * 参数: handle - 任务句柄
 */
void app_profile_register_task(const char *name, TaskHandle_t handle);
/*
 * 开始一个 profile 性能跨度（span），记录起始时间戳和元数据。
 * 参数: event       - 事件名
 * 参数: phase       - 阶段名
 * 参数: session_id  - 会话 ID（可为 NULL）
 * 参数: run_id      - 运行 ID（可为 NULL）
 * 返回: 初始化的 span 结构体
 */
app_profile_span_t app_profile_span_begin(const char *event,
                                          const char *phase,
                                          const char *session_id,
                                          const char *run_id);
/*
 * 结束一个 profile span，计算耗时并发布 profile JSON 事件。
 * 参数: span   - span 结构体指针
 * 参数: status - 结束状态字符串（如 "ok"/"error"）
 */
void app_profile_span_end(app_profile_span_t *span, const char *status);
/*
 * 发布一次性 profile 采样点（不含耗时计算）。
 * 参数: event - 事件名
 * 参数: phase - 阶段名
 * 参数: force - true 即使 profile 未启用也强制发布
 */
void app_profile_sample(const char *event, const char *phase, bool force);
/*
 * 处理来自 SDK cc_event_bus 的可观测性事件，按级别过滤并可能发布 profile 事件。
 * 参数: event_type - SDK 事件类型（如 "llm.request.start"）
 * 参数: event_json - 事件 JSON 字符串
 */
void app_profile_sdk_event(const char *event_type, const char *event_json);
/*
 * 订阅运行时 SDK 事件总线，将 SDK 事件转发给 app_profile_sdk_event。
 * 参数: runtime - Agent 运行时实例
 */
void app_profile_subscribe_runtime(cc_runtime_builder_t *builder);
esp_err_t app_profile_unsubscribe_runtime(uint32_t timeout_ms);
/*
 * 将 profile 状态 JSON 片段追加到 cc_string_builder。
 * 参数: sb - 字符串构建器指针
 */
void app_profile_append_status_json(cc_string_builder_t *sb);
