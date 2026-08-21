#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "cc/ports/cc_logger.h"
#include "esp_err.h"

typedef enum {
    APP_EVENT_KIND_LOG = 0,
    APP_EVENT_KIND_USER,
    APP_EVENT_KIND_ASSISTANT,
    APP_EVENT_KIND_THINKING,
    APP_EVENT_KIND_TOOL,
    APP_EVENT_KIND_STATUS,
    APP_EVENT_KIND_ERROR,
    APP_EVENT_KIND_RUN_STATE,
    APP_EVENT_KIND_PROFILE,
    APP_EVENT_KIND_COUNT,
} app_event_kind_t;

typedef enum {
    APP_EVENT_SOURCE_SYSTEM = 0,
    APP_EVENT_SOURCE_SERIAL,
    APP_EVENT_SOURCE_WEB,
    APP_EVENT_SOURCE_RUNTIME,
    APP_EVENT_SOURCE_TOOL,
} app_event_source_t;

typedef enum {
    APP_EVENT_BUS_UNINIT = 0,
    APP_EVENT_BUS_FALLBACK_SERIAL,
    APP_EVENT_BUS_CONFIGURED,
    APP_EVENT_BUS_USB_LOG_READY,
    APP_EVENT_BUS_USB_LOG_FAILED,
    APP_EVENT_BUS_DRAINING,
    APP_EVENT_BUS_STOPPED,
    APP_EVENT_BUS_FAILED,
} app_event_bus_lifecycle_t;

#define APP_EVENT_KIND_MASK(kind) (1UL << (uint32_t)(kind))
#define APP_EVENT_KIND_MASK_ALL 0xffffffffUL

typedef struct {
    app_event_bus_lifecycle_t lifecycle;     /* 事件总线当前生命周期状态 */
    bool serial_debug_enabled;               /* 串口调试模式是否已启用 */
    bool maintenance_mode;                   /* 是否处于 OTA 维护模式 */
    bool usb_log_enabled;                    /* USB 日志功能是否已配置启用 */
    bool usb_log_started;                    /* USB 日志服务是否已实际启动 */
    bool usb_log_fallback_serial;            /* 是否因 USB 失败而回退到串口 */
    uint32_t dropped;                        /* 总丢弃事件计数 */
    uint32_t ring_overwrites;                /* 环形缓冲区覆盖覆盖计数 */
    uint32_t sink_dropped;                   /* sink 端丢弃事件计数 */
    uint32_t published;                      /* 总发布事件计数 */
    uint32_t maintenance_skipped;            /* 维护模式下跳过的日志行数 */
    uint32_t oom_dropped;                    /* 因内存不足丢弃的事件计数 */
    uint32_t usb_queue_dropped;              /* USB 队列满丢弃计数 */
    uint32_t display_queue_dropped;          /* 显示队列满丢弃计数 */
    uint32_t max_usb_queue_waiting;          /* USB 队列历史最大等待数 */
    uint32_t max_display_queue_waiting;      /* 显示队列历史最大等待数 */
    uint32_t oldest_seq;                     /* 缓冲区内最旧事件的序列号 */
    uint32_t latest_seq;                     /* 最近发布的事件序列号 */
    size_t capacity;                         /* 环形缓冲区容量 */
    size_t usb_bytes_written;                /* USB 日志累计写入字节数 */
    char maintenance_reason[64];             /* 进入维护模式的原因描述 */
    char usb_log_path[192];                  /* 当前 USB 日志文件完整路径 */
    char log_route[48];                      /* LOG 事件 sink 路由描述字符串 */
    char chat_route[48];                     /* 对话事件 sink 路由描述字符串 */
    char thinking_route[48];                 /* 思考事件 sink 路由描述字符串 */
    char tool_route[48];                     /* 工具事件 sink 路由描述字符串 */
    char status_route[48];                   /* 状态事件 sink 路由描述字符串 */
    char error_route[48];                    /* 错误事件 sink 路由描述字符串 */
    char profile_route[48];                  /* 性能事件 sink 路由描述字符串 */
} app_event_bus_status_t;

/** 初始化事件总线并应用 IO 配置，返回 ESP 错误码。 */
esp_err_t app_event_bus_init(const app_runtime_io_config_t *io);
/** 在运行时重新应用 IO 配置，返回 ESP 错误码。 */
esp_err_t app_event_bus_apply_config(const app_runtime_io_config_t *io);
/** 有限时间排空异步 sink 并停止 writer task；进入 draining 后不再向异步 sink 投递。 */
esp_err_t app_event_bus_stop(uint32_t timeout_ms);
/** stop 完成后卸载日志 hook 并释放总线自有资源。 */
esp_err_t app_event_bus_destroy(uint32_t timeout_ms);
/** 启动串口调试回退模式（USB 日志不可用时），强制串口输出 DEBUG 级别。 */
void app_event_bus_start_serial_fallback_debug(void);
/** 标记 NTP 时间已同步，触发日志文件重命名。 */
void app_event_bus_note_time_synced(void);
/** 设置串口调试开关并同步 ESP 日志级别。 */
void app_event_bus_set_serial_debug(bool enabled);
/** 启用/禁用维护模式，reason 为进入原因（可为 NULL）。 */
void app_event_bus_set_maintenance_mode(bool enabled, const char *reason);
/** 查询串口调试是否已启用。 */
bool app_event_bus_serial_debug_enabled(void);
/** 查询维护模式是否已启用。 */
bool app_event_bus_maintenance_mode_enabled(void);
/** 等待 USB 和显示队列排空，timeout_ms 为超时毫秒数，0 使用默认有限超时。 */
bool app_event_bus_wait_idle(uint32_t timeout_ms);
/** 获取事件总线运行状态快照，填充 out_status。 */
void app_event_bus_get_status(app_event_bus_status_t *out_status);
/** 把当前 USB 日志文件路径复制到调用方缓冲区。 */
void app_event_bus_copy_current_log_path(char *out_path, size_t out_size);

/** 发布事件：进行脱敏、复制、路由后分发到串口/显示/USB。 */
void app_event_publish(app_event_kind_t kind,
                       app_event_source_t source,
                       cc_log_level_t level,
                       const char *text);
/** 格式化字符串后发布事件，fmt 支持 printf 格式。 */
void app_event_publishf(app_event_kind_t kind,
                        app_event_source_t source,
                        cc_log_level_t level,
                        const char *fmt,
                        ...);
/** 清除 LCD 对话区和环形缓冲区。 */
void app_event_bus_clear_conversation(void);
/** 导出 JSON 格式的事件流快照，支持增量查询和种类过滤。返回堆上分配的字符串。 */
char *app_event_bus_json(uint32_t since_seq, int limit, uint32_t kind_mask);
/** 事件种类枚举转字符串。 */
const char *app_event_kind_string(app_event_kind_t kind);
/** 事件来源枚举转字符串。 */
const char *app_event_source_string(app_event_source_t source);
/** 事件总线生命周期状态转字符串。 */
const char *app_event_bus_lifecycle_string(app_event_bus_lifecycle_t lifecycle);
/** 显示队列消费泵：从队列取出事件并写入 LCD 对话区。 */
void app_event_display_pump(void);
