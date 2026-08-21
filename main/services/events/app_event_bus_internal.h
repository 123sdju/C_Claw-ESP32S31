#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "app_event_bus.h"
#include "cc/util/cc_string_builder.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define APP_EVENT_RECENT_MAX 1024U
#define APP_EVENT_TEXT_MAX 2048U
#define APP_EVENT_USB_QUEUE_DEPTH 32U
#define APP_EVENT_USB_TASK_STACK 6144U
#define APP_EVENT_USB_TASK_PRIORITY 4
#define APP_EVENT_DISPLAY_QUEUE_DEPTH 24U
#define APP_EVENT_DISPLAY_PUMP_MAX 8U
#define APP_EVENT_BUS_IDLE_BIT (1U << 0)
#define APP_EVENT_USB_TASK_EXITED_BIT (1U << 1)

typedef enum {
    APP_EVENT_SINK_SERIAL = 1U << 0,
    APP_EVENT_SINK_WEB = 1U << 1,
    APP_EVENT_SINK_DISPLAY = 1U << 2,
    APP_EVENT_SINK_USB_LOG = 1U << 3,
} app_event_sink_mask_t;

typedef struct app_event_envelope {
    unsigned refs;
    size_t len;
    char text[];
} app_event_envelope_t;

typedef struct {
    uint32_t seq;                /* 事件序列号，单调递增 */
    int64_t timestamp_ms;        /* 事件时间戳（毫秒），基于 FreeRTOS tick */
    app_event_kind_t kind;       /* 事件种类 */
    app_event_source_t source;   /* 事件来源 */
    cc_log_level_t level;        /* 日志级别 */
    app_event_envelope_t *envelope;
} app_event_record_t;

typedef struct {
    app_event_envelope_t *envelope;
    uint32_t seq;
    app_event_kind_t kind;
    cc_log_level_t level;
    bool shutdown;
} app_event_usb_log_msg_t;

typedef struct {
    app_event_kind_t kind;       /* 事件种类 */
    app_event_envelope_t *envelope;
    bool clear;                  /* 清空消息标志，消费时触发 board_conversation_clear */
} app_event_display_msg_t;

typedef struct {
    app_event_bus_lifecycle_t lifecycle;     /* 事件总线当前生命周期状态 */
    bool config_applied;                     /* 是否已应用过 IO 配置 */
    bool serial_enabled;                     /* 串口输出是否启用 */
    bool display_enabled;                    /* LCD 显示是否启用 */
    bool serial_debug_enabled;               /* 串口调试模式（用户主动开启） */
    bool serial_fallback_debug_enabled;      /* 串口回退调试模式（USB 失败自动启用） */
    bool usb_log_enabled;                    /* USB 日志功能是否启用 */
    bool usb_log_started;                    /* USB 日志任务是否已启动 */
    bool renamed_with_time;                  /* 日志文件是否已按时间戳重命名 */
    bool rename_pending;                     /* 是否有待处理的重命名请求 */
    bool vprintf_installed;                  /* ESP vprintf 钩子是否已安装 */
    bool maintenance_mode;                   /* 是否处于 OTA 维护模式 */
    uint32_t next_seq;                       /* 下一个事件序列号（递增前） */
    uint32_t dropped;                        /* 总丢弃事件计数 */
    uint32_t ring_overwrites;                /* 环形缓冲区覆盖次数 */
    uint32_t sink_dropped;                   /* sink 端丢弃计数 */
    uint32_t published;                      /* 总发布事件计数 */
    uint32_t maintenance_skipped;            /* 维护模式跳过的日志行数 */
    uint32_t oom_dropped;                    /* 内存不足丢弃计数 */
    uint32_t usb_queue_dropped;              /* USB 队列满丢弃数 */
    uint32_t display_queue_dropped;          /* 显示队列满丢弃数 */
    uint32_t max_usb_queue_waiting;          /* USB 队列历史高水位 */
    uint32_t max_display_queue_waiting;      /* 显示队列历史高水位 */
    uint32_t usb_pending;                    /* 已入队或正在处理的 USB 消息 */
    uint32_t display_pending;                /* 已入队或正在处理的显示消息 */
    app_event_sink_mask_t route[APP_EVENT_KIND_COUNT]; /* 每种事件的路由掩码 */
    cc_log_level_t serial_log_level;         /* 串口最低日志级别 */
    cc_log_level_t usb_log_level;            /* USB 日志最低日志级别 */
    char usb_dir[128];                       /* USB 日志存储目录路径 */
    char maintenance_reason[64];             /* 维护模式进入原因 */
    char usb_current_path[192];              /* 当前日志文件完整路径 */
    char usb_base_path[176];                 /* 日志文件基础路径（不含分卷号） */
    FILE *usb_file;                          /* 当前打开的日志文件句柄 */
    size_t usb_max_bytes;                    /* 单个日志文件最大字节数 */
    int usb_max_files;                       /* 最大保留日志文件数量 */
    int usb_part;                            /* 当前日志文件分卷号 */
    size_t usb_current_bytes;                /* 当前日志文件已写入字节数 */
    size_t usb_bytes_written;                /* 所有日志文件累计写入字节数 */
    app_event_record_t *recent;              /* 环形缓冲区指针（PSRAM 或内部 RAM） */
    size_t recent_capacity;                  /* 环形缓冲区槽位容量 */
    size_t recent_count;                     /* 环形缓冲区当前有效记录数 */
    size_t recent_start;                     /* 环形缓冲区最旧记录索引 */
    QueueHandle_t usb_queue;                 /* USB 日志写入队列句柄 */
    TaskHandle_t usb_task;                   /* USB 日志写入任务句柄 */
    QueueHandle_t display_queue;             /* 显示端事件队列句柄 */
    EventGroupHandle_t idle_events;          /* sink drain 等待事件组 */
    SemaphoreHandle_t mutex;                 /* 全局状态互斥锁 */
    SemaphoreHandle_t file_mutex;            /* 日志文件操作互斥锁 */
} app_event_bus_state_t;

extern app_event_bus_state_t g_app_event_bus;

/** 获取全局互斥锁，阻塞直到成功。 */
void app_event_bus_lock(void);
/** 释放全局互斥锁。 */
void app_event_bus_unlock(void);

/** 分配内存并复制指定长度字符串，优先 PSRAM。 */
char *app_event_strdup_len(const char *text, size_t len);
/** 分配内存并复制完整字符串。 */
char *app_event_strdup(const char *text);
app_event_envelope_t *app_event_envelope_create(const char *text);
void app_event_envelope_retain(app_event_envelope_t *envelope);
void app_event_envelope_release(app_event_envelope_t *envelope);
/** 安全拷贝源字符串到目标缓冲区。 */
void app_event_copy_text(char *dst, size_t dst_size, const char *src);
/** 追加文本到已有字符串末尾，超出容量时拒绝。 */
bool app_event_append_text(char *out, size_t out_size, const char *text);
/** 拼接目录路径和文件名，自动补 '/'。 */
bool app_event_join_path(char *out, size_t out_size, const char *dir, const char *name);
/** 将 uint32 转十进制追加到缓冲区末尾。 */
bool app_event_append_uint32(char *out, size_t out_size, uint32_t value);

/** 字符串解析为日志级别枚举。 */
cc_log_level_t app_event_parse_log_level(const char *text);
/** 日志级别枚举转字符串。 */
const char *app_event_log_level_string(cc_log_level_t level);

/** 查询指定事件种类的路由掩码（需持锁）。 */
app_event_sink_mask_t app_event_route_for_kind_locked(app_event_kind_t kind);
/** 根据 IO 配置填充路由表（需持锁）。 */
void app_event_configure_routes_locked(const app_runtime_io_config_t *io);
/** 设置串口回退路由（需持锁）。 */
void app_event_configure_fallback_routes_locked(void);
/** 将 sink 掩码转为逗号分隔字符串。 */
void app_event_sink_mask_string(app_event_sink_mask_t mask, char *out, size_t out_size);
/** 判断事件是否应写入 USB 日志。 */
bool app_event_route_to_usb_log(app_event_kind_t kind, cc_log_level_t level, app_event_sink_mask_t route);
/** 判断事件是否应向串口输出。 */
bool app_event_route_to_serial(app_event_kind_t kind, cc_log_level_t level, app_event_sink_mask_t route);
/** 判断事件是否应输出到 LCD 显示。 */
bool app_event_route_to_display(app_event_kind_t kind, app_event_sink_mask_t route);

/** 确保环形缓冲区已分配（需持锁）。 */
bool app_event_ring_ensure_locked(void);
/** 向环形缓冲区追加事件记录（需持锁）。 */
bool app_event_ring_append_locked(const app_event_record_t *record);
/** 清空环形缓冲区（需持锁）。 */
void app_event_ring_clear_locked(void);
/** JSON 转义并追加到字符串构建器。 */
void app_event_json_escape_append(cc_string_builder_t *sb, const char *text);

/** 启动 USB 日志服务，返回 ESP 错误码。 */
esp_err_t app_event_usb_init(void);
esp_err_t app_event_usb_start(void);
/** 停止 USB 日志服务。 */
esp_err_t app_event_usb_stop(uint32_t timeout_ms);
/** 将已格式化的日志行放入 USB 写入队列。 */
void app_event_usb_enqueue(app_event_envelope_t *envelope,
                           uint32_t seq,
                           app_event_kind_t kind,
                           cc_log_level_t level);

/** 将事件放入显示端消费队列。 */
esp_err_t app_event_display_init(void);
void app_event_display_emit(app_event_kind_t kind, app_event_envelope_t *envelope);
/** 向显示队列发送清空消息。 */
void app_event_display_clear(void);
/** 丢弃并释放显示队列中尚未消费的 envelope。 */
void app_event_display_discard_all(void);
/** 标记存在未 drain 的异步 sink 工作。 */
void app_event_bus_mark_sink_busy(void);
/** 当 USB/display sink 均已 drain 时唤醒 wait_idle 调用方。 */
void app_event_bus_notify_idle_if_drained(void);

/** 安装 ESP-IDF 和 SDK 日志钩子。 */
void app_event_log_hooks_install(void);
/** 恢复默认日志输出并移除 SDK logger sink。 */
void app_event_log_hooks_uninstall(void);
/** 根据总线配置同步 ESP-IDF 日志级别。 */
void app_event_log_hooks_apply_esp_level(void);
