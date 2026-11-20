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
#ifndef APP_EVENT_RECENT_MAX_BYTES
#define APP_EVENT_RECENT_MAX_BYTES (512U * 1024U)
#endif
#define APP_EVENT_TEXT_MAX 2048U
#define APP_EVENT_USB_QUEUE_DEPTH 32U
#define APP_EVENT_USB_TASK_STACK 6144U
#define APP_EVENT_USB_TASK_PRIORITY 4
#define APP_EVENT_DISPLAY_QUEUE_DEPTH 24U
#define APP_EVENT_DISPLAY_PUMP_MAX 8U
#define APP_EVENT_BUS_IDLE_BIT (1U << 0)
#define APP_EVENT_USB_TASK_EXITED_BIT (1U << 1)

/**
 * @brief 将累计统计加一并在 uint32_t 上界饱和。
 * @param[in,out] counter 必需统计槽，调用者持总线状态锁或独占未发布状态。
 * @post 统计单调增加至 UINT32_MAX，保持饱和值，序号和清屏代数使用各自换代规则。
 */
static inline void app_event_counter_increment(uint32_t *counter)
{
    if (*counter < UINT32_MAX) (*counter)++;
}

/**
 * @brief 消费构造器步骤返回的拥有诊断，实际失败仍保留在构造器 sticky 状态。
 * @param[in] result 按值交接唯一诊断拥有权的结果。
 * @post 结果动态字段释放，调用者最后通过 take 检查构造器是否完成。
 */
static inline void app_event_discard_result(cc_result_t result)
{
    cc_result_free(&result);
}

/** @brief 事件路由的可组合输出位集合。 */
typedef enum {
    APP_EVENT_SINK_SERIAL = 1U << 0,
    APP_EVENT_SINK_WEB = 1U << 1,
    APP_EVENT_SINK_DISPLAY = 1U << 2,
    APP_EVENT_SINK_USB_LOG = 1U << 3,
} app_event_sink_mask_t;

/** @brief 独立分配且正文发布后只读的共享对象，每个 ring/快照/队列各持引用。 */
typedef struct app_event_envelope {
    unsigned refs; /* 仅原子访问的活跃引用数。 */
    size_t len;    /* 正文字节数，排除末尾 NUL。 */
    char text[];   /* 与对象同块拥有的 NUL 正文。 */
} app_event_envelope_t;

typedef struct {
    uint32_t seq;                /* 当前显示代内从 1 递增，耗尽时换代清近期视图。 */
    int64_t timestamp_ms;        /* 事件时间戳（毫秒），基于 FreeRTOS tick */
    app_event_kind_t kind;       /* 事件种类 */
    app_event_source_t source;   /* 事件来源 */
    cc_log_level_t level;        /* 日志级别 */
    app_event_envelope_t *envelope; /* 本记录所在拥有容器持一个引用，按值复制须另 retain。 */
} app_event_record_t;

typedef struct {
    app_event_envelope_t *envelope; /* 普通队列消息拥有引用，停止哨兵为 NULL。 */
    uint32_t seq;                  /* 已发布序号。 */
    app_event_kind_t kind;         /* 已发布种类。 */
    cc_log_level_t level;          /* 已发布级别。 */
    bool shutdown;                /* 停止哨兵，不计入普通 pending。 */
} app_event_usb_log_msg_t;

typedef struct {
    app_event_kind_t kind;          /* 页内事件种类。 */
    app_event_envelope_t *envelope; /* 所在页容器拥有的正文引用。 */
} app_event_view_record_t;

typedef struct app_event_display_view {
    size_t count;                    /* 同块数组有效项数，至多 16。 */
    app_event_view_record_t items[];  /* 消息拥有整个数组及每项正文引用。 */
} app_event_display_view_t;

typedef struct {
    app_event_kind_t kind;       /* 事件种类 */
    app_event_envelope_t *envelope; /* 普通消息拥有引用，clear/view 消息可为 NULL。 */
    bool clear;                  /* 清空消息标志，消费时触发 board_conversation_clear */
    bool boot;                   /* 输出到启动日志层，不依赖普通显示路由 */
    app_event_display_view_t *view; /* 完整历史页面，消费或丢弃时释放 */
} app_event_display_msg_t;

/**
 * @brief 总线唯一自有状态，普通配置、视图、统计和路径快照统一由状态锁保护。
 * @note FILE、usb_base_path 和 usb_part 的访问由 file_mutex 串行，文件操作需要更新可观测字段时按 file_mutex 再状态锁的顺序。队列/锁/任务句柄由生命周期 owner 串行创建和释放，外部调用先持进程期租约，writer 用最终完成发布覆盖自身资源借用。
 */
typedef struct {
    app_event_bus_lifecycle_t lifecycle;     /* 事件总线当前生命周期状态 */
    bool config_applied;                     /* 是否已应用过 IO 配置 */
    bool serial_enabled;                     /* 串口输出是否启用 */
    bool display_enabled;                    /* LCD 显示是否启用 */
    bool serial_debug_enabled;               /* 串口调试模式（用户主动开启） */
    bool serial_fallback_debug_enabled;      /* 串口回退调试模式（USB 失败自动启用） */
    bool usb_log_enabled;                    /* USB 日志功能是否启用 */
    bool usb_log_started;                    /* writer 已启动且允许新入队接纳。 */
    bool usb_stop_requested;                 /* 已向日志任务发送停止消息 */
    bool renamed_with_time;                  /* 日志文件是否已按时间戳重命名 */
    bool rename_pending;                     /* 是否有待处理的重命名请求 */
    bool vprintf_installed;                  /* ESP vprintf 钩子是否已安装 */
    bool maintenance_mode;                   /* 是否处于 OTA 维护模式 */
    uint32_t next_seq;                       /* 当前代最新已分配序号，下一次先递增；MAX 时清近期显示并归零。 */
    uint32_t clear_generation;               /* 每次清屏/换页/序号换代递增，允许 MAX 回零，客户端比较变化。 */
    uint32_t dropped;                        /* 各路径丢弃/淘汰累计次数，同事件可跨 sink 分别计数。 */
    uint32_t ring_overwrites;                /* ring 淘汰旧项或拒绝超单条预算次数。 */
    uint32_t sink_dropped;                   /* sink 丢弃/失败次数，含维护与正文 OOM。 */
    uint32_t published;                      /* 总发布事件计数 */
    uint32_t maintenance_skipped;            /* 维护模式跳过的日志行数 */
    uint32_t oom_dropped;                    /* 内存不足丢弃计数 */
    uint32_t usb_queue_dropped;              /* USB 入队失败与文件 I/O 错误次数，包含周期 flush。 */
    uint32_t display_queue_dropped;          /* 显示队列满丢弃数 */
    uint32_t max_usb_queue_waiting;          /* 每次发送前观察到的 USB 最大排队数。 */
    uint32_t max_display_queue_waiting;      /* 每次普通消息发送前观察到的显示最大排队数。 */
    uint32_t usb_pending;                    /* 已接纳的 USB 消息，覆盖预留、入队与处理阶段。 */
    uint32_t display_pending;                /* 已接纳的显示消息，覆盖预留、入队与渲染阶段。 */
    app_event_sink_mask_t route[APP_EVENT_KIND_COUNT]; /* 每种事件的路由掩码 */
    cc_log_level_t serial_log_level;         /* 串口最低日志级别 */
    cc_log_level_t usb_log_level;            /* USB 日志最低日志级别 */
    char usb_dir[128];                       /* USB 日志存储目录路径 */
    char maintenance_reason[64];             /* 维护模式进入原因 */
    char usb_current_path[192];              /* 当前日志文件完整路径 */
    char usb_base_path[176];                 /* file_mutex 保护的基础路径，不含分卷号。 */
    FILE *usb_file;                          /* file_mutex 保护的唯一 FILE，writer/owner 关闭消费。 */
    size_t usb_max_bytes;                    /* 单个日志文件最大字节数 */
    int usb_max_files;                       /* 最大保留日志文件数量 */
    int usb_part;                            /* file_mutex 保护的当前卷号，INT_MAX 后换卷失败。 */
    size_t usb_current_bytes;                /* 当前日志文件已写入字节数 */
    size_t usb_bytes_written;                /* 所有日志文件累计写入字节数 */
    app_event_record_t *recent;              /* 环形缓冲区指针（PSRAM 或内部 RAM） */
    size_t recent_capacity;                  /* 环形缓冲区槽位容量 */
    size_t recent_count;                     /* 环形缓冲区当前有效记录数 */
    size_t recent_bytes;                     /* ring 正文头、文本及 NUL 的申请字节，排除分配器开销。 */
    size_t recent_start;                     /* 环形缓冲区最旧记录索引 */
    /* 固定历史页独立保留引用，普通 ring 覆盖后迟到 Web 仍可恢复选中页面。 */
    app_event_record_t view[APP_EVENT_VIEW_MAX_ITEMS];
    size_t view_count;                       /* 固定页有效项数。 */
    uint32_t view_latest_seq;                /* 固定页最后序号，空页使用提交时最新序号。 */
    bool view_active;                        /* 页已显式安装，零项页也可为 true。 */
    QueueHandle_t usb_queue;                 /* USB 日志写入队列句柄 */
    bool usb_queue_with_caps;               /* 队列须由能力版本删除函数释放。 */
    TaskHandle_t usb_task;                   /* USB 日志写入任务句柄 */
    bool usb_task_completed;                 /* 仅原子访问，writer 在最后一次总线访问时 release 发布。 */
    QueueHandle_t display_queue;             /* 显示端事件队列句柄 */
    bool display_queue_with_caps;           /* 队列须由能力版本删除函数释放。 */
    EventGroupHandle_t idle_events;          /* sink drain 等待事件组 */
    SemaphoreHandle_t mutex;                 /* 全局状态互斥锁 */
    SemaphoreHandle_t file_mutex;            /* 日志文件操作互斥锁 */
} app_event_bus_state_t;

extern app_event_bus_state_t g_app_event_bus;

/** @brief 在进程期接纳门登记资源租约。 @return 门开放为 true 并登记一次借用，关闭为 false。 @post 成功必须配对 release，destroy 等待全部登记归还后释放动态原语。 */
bool app_event_bus_acquire(void);
/** @brief 归还已登记资源租约。 @pre 恰有一次成功 acquire 未归还。 @post 活跃数减一，此后该借用结束。 */
void app_event_bus_release(void);

/** @brief 将有限毫秒向上转换为有限 tick。 @param[in] timeout_ms 毫秒数，零返回零。 @return 正值至少一 tick，超限截到 portMAX_DELAY-1。 */
TickType_t app_event_timeout_ticks(uint32_t timeout_ms);
/** @brief 计算同一 tick 预算剩余量。 @param[in] start 初始 tick。 @param[in] budget 有限 tick 预算。 @return 到期为零，否则为剩余 tick，使用无符号差值处理一次回绕。 */
TickType_t app_event_remaining_ticks(TickType_t start, TickType_t budget);

/** @brief 取得状态锁，同任务支持配对递归。 @pre 租约、writer 存活权或生命周期独占保证原语有效。 @post 有 mutex 时取得一层，未初始化时无操作。 */
void app_event_bus_lock(void);
/** @brief 归还当前任务的一层状态锁。 @post 最外层释放原生 mutex，非 owner 无操作。 @pre 与成功 lock 配对且原语仍有效。 */
void app_event_bus_unlock(void);

/** @brief 复制指定字节文本，优先 PSRAM 后内部 RAM。 @param[in] text 至少 len 字节借用，NULL 按空串。 @param[in] len 输入字节数，超过 2048 按 UTF-8 完整前缀截断。 @return heap_caps_free 释放的 NUL 副本，OOM 为 NULL。 */
char *app_event_strdup_len(const char *text, size_t len);
/** @brief 测量并复制有界 NUL 文本。 @param[in] text 调用期借用，可 NULL。 @return 至多 2048 字节正文的拥有副本，heap_caps_free 释放，OOM 为 NULL。 */
char *app_event_strdup(const char *text);
/** @brief 复制普通事件到共享正文。 @param[in] text 调用期文本借用，可 NULL，长文本按 UTF-8 边界截至 2048。 @return 初始一个引用，最后由 release 释放，OOM 为 NULL。 */
app_event_envelope_t *app_event_envelope_create(const char *text);
/** @brief 新增正文拥有引用。 @param[in,out] envelope 活跃对象，可 NULL。 @post 原子增加引用，持有者负责配对 release。 */
void app_event_envelope_retain(app_event_envelope_t *envelope);
/** @brief 消费一个正文引用。 @param[in,out] envelope 交接引用，可 NULL。 @post 最后引用归还时 heap_caps_free，调用者停止使用该引用。 */
void app_event_envelope_release(app_event_envelope_t *envelope);
/** @brief 允许重叠地有界复制文本。 @param[out] dst 可选缓冲。 @param[in] dst_size 含 NUL 的字节容量，零无操作。 @param[in] src 调用期文本借用，NULL 按空串。 @post 有效输出 NUL 结尾，截断时去掉半个 UTF-8 字符。 */
void app_event_copy_text(char *dst, size_t dst_size, const char *src);
/** @brief 一次追加完整文本。 @param[in,out] out 已有 NUL 文本缓冲。 @param[in] out_size 总字节容量。 @param[in] text 必需 NUL 借用，与写入区不重叠。 @return 成功 true，参数/容量不足 false 并保留输出。 */
bool app_event_append_text(char *out, size_t out_size, const char *text);
/** @brief 拼接目录和名称，按需补斜杠。 @param[out] out 不与输入重叠的缓冲。 @param[in] out_size 字节容量含 NUL。 @param[in] dir 必需目录借用，可空串。 @param[in] name 必需名称借用，可空串。 @return 完整输出 true，参数/容量不足 false 并保留输出。 */
bool app_event_join_path(char *out, size_t out_size, const char *dir, const char *name);
/** @brief 追加十进制无符号整数。 @param[in,out] out 已有 NUL 文本缓冲。 @param[in] out_size 字节容量含 NUL。 @param[in] value 按值整数。 @return 成功 true，参数/容量不足 false 并保留输出。 */
bool app_event_append_uint32(char *out, size_t out_size, uint32_t value);

/** @brief 按精确小写名解析级别。 @param[in] text 文本借用，可 NULL。 @return trace/debug/warn/error/fatal 对应值，其他均 INFO。 */
cc_log_level_t app_event_parse_log_level(const char *text);
/** @brief 取得稳定级别名称。 @param[in] level 按值级别。 @return 静态文本借用，未知为 info。 */
const char *app_event_log_level_string(cc_log_level_t level);

/** @brief 读种类路由。 @param[in] kind 按值种类。 @return 已配置位集合，越界零。 @pre 持状态锁及存活权。 */
app_event_sink_mask_t app_event_route_for_kind_locked(app_event_kind_t kind);
/** @brief 复制配置路由位集合。 @param[in] io 必需稳定借用，非零列表 count 须有 items。 @pre 持状态锁。 @post 全路由更新，未保留输入字符串。 */
void app_event_configure_routes_locked(const app_runtime_io_config_t *io);
/** @brief 安装缺省串口回退路由。 @pre 持状态锁。 @post THINKING 零路由，其他种类仅 SERIAL。 */
void app_event_configure_fallback_routes_locked(void);
/** @brief 编码路由位名称。 @param[in] mask 路由集合。 @param[out] out 可选输出缓冲。 @param[in] out_size 字节容量，含 NUL。 @post 有效输出 NUL 结尾，仅写入能放下的完整片段并去尾逗号。 */
void app_event_sink_mask_string(app_event_sink_mask_t mask, char *out, size_t out_size);
/** @brief 判定 USB 路由意图。 @param[in] kind 当前未参与筛选的种类。 @param[in] level 按值级别。 @param[in] route 路由集合。 @return 配置启用、含 USB_LOG 且达到阈值为 true。 @pre 持状态锁，实际入队另查接纳。 */
bool app_event_route_to_usb_log(app_event_kind_t kind, cc_log_level_t level, app_event_sink_mask_t route);
/** @brief 判定串口路由。 @param[in] kind LOG 使用普通阈值。 @param[in] level 按值级别。 @param[in] route 路由集合。 @return 串口或回退启用、含 SERIAL 且满足阈值为 true，回退免 LOG 阈值。 @pre 持状态锁。 */
bool app_event_route_to_serial(app_event_kind_t kind, cc_log_level_t level, app_event_sink_mask_t route);
/** @brief 判定普通 LCD 路由。 @param[in] kind 排除 LOG/RUN_STATE/PROFILE。 @param[in] route 路由集合。 @return 配置启用且含 DISPLAY 并通过种类筛选为 true。 @pre 持状态锁。 */
bool app_event_route_to_display(app_event_kind_t kind, app_event_sink_mask_t route);

/** @brief 确保固定 ring 槽位已分配。 @return 已有或分配成功为 true，双堆 OOM 为 false。 @pre 持状态锁，失败不改变已有记录。 */
bool app_event_ring_ensure_locked(void);
/** @brief 在条数与字节预算内保留事件引用。 @param[in] record 必需记录借用，正文为有效对象或 NULL。 @return 追加成功 true，分配或单条超预算 false 并按实现统计丢弃。 @pre 持状态锁且输入记录有效，成功新增 ring 引用，调用者引用保持。 */
bool app_event_ring_append_locked(const app_event_record_t *record);
/** @brief 归还 ring 和固定页的全部正文引用。 @pre 持状态锁。 @post 槽位分配保留，记录、字节和页状态清零，序号与代数交上层处理。 */
void app_event_ring_clear_locked(void);
/** @brief 追加带双引号的 JSON 字符串。 @param[in,out] sb 必需独占构造器，错误保留 sticky。 @param[in] text NUL 文本借用，NULL 按空串。 @post 控制字节与引号转义，步骤结果诊断全部消费，UTF-8 由文本来源保证。 */
void app_event_json_escape_append(cc_string_builder_t *sb, const char *text);

/** @brief 补齐 USB 文件锁与队列。 @return ESP_OK 成功，OOM 为 NO_MEM，部分已建对象归总线拥有可重试/销毁。 @pre 生命周期 owner 独占。 */
esp_err_t app_event_usb_init(void);
/** @brief 开启启动卷并确保 writer 已启动。 @return 成功 ESP_OK，原语不足 INVALID_STATE，文件/任务失败 ESP_FAIL，旧任务退场失败透传。 @pre 生命周期 owner 串行，配置已复制且原语有效。 */
esp_err_t app_event_usb_start(void);
/** @brief 在独立有限预算内停止 USB writer。 @param[in] timeout_ms 毫秒数，零即时尝试。 @return 成功、超时、自停或文件关闭错误，失败保留剩余对象可重试。 @pre 生命周期 owner 串行。 */
esp_err_t app_event_usb_stop(uint32_t timeout_ms);
/** @brief 在外层同一预算内停止 USB writer。 @param[in] start 外层初始 tick。 @param[in] budget 有限 tick 预算。 @return 成功、超时或关闭错误，超时保留句柄供重试。 */
esp_err_t app_event_usb_stop_until(TickType_t start, TickType_t budget);
/** @brief 独立保留正文引用并零等待入队。 @param[in,out] envelope 有效正文借用，NULL 忽略，调用者原引用保留。 @param[in] seq 已发布序号。 @param[in] kind 事件种类。 @param[in] level 事件级别。 @post 成功消息拥有新增引用，满队列回滚 pending/引用并计数，接纳关闭忽略。 */
void app_event_usb_enqueue(app_event_envelope_t *envelope,
                           uint32_t seq,
                           app_event_kind_t kind,
                           cc_log_level_t level);

/** @brief 补齐显示队列并记录实际释放方式。 @return 已有或创建成功 ESP_OK，失败 NO_MEM。 @pre 生命周期 owner 独占。 */
esp_err_t app_event_display_init(void);
/** @brief 检查板级 UI 是否可用。 @return board_ui_ready 当前结果。 */
bool app_event_display_available(void);
/** @brief 投递普通显示消息。 @param[in] kind 事件种类。 @param[in,out] envelope 有效正文借用，成功另 retain，原引用保留。 @post 不可用/退场忽略，满队列回滚并计数，流式发送按 5 毫秒向上取整为有限 tick，正值至少一 tick。 */
void app_event_display_emit(app_event_kind_t kind, app_event_envelope_t *envelope);
/** @brief 投递独立启动显示层消息。 @param[in] kind 事件种类。 @param[in,out] envelope 正文借用，成功新增队列引用。 @post 与普通 emit 共用接纳和失败清理，消费时写启动层。 */
void app_event_display_emit_boot(app_event_kind_t kind, app_event_envelope_t *envelope);
/** @brief 用单个队列项提交完整显示页。 @param[in] items count 项借用，零时可 NULL，各正文成功新增引用。 @param[in] count 至多 16。 @return 成功 ESP_OK，参数、UI/接纳、OOM 或排队失败返回对应错误并清理新增引用。 @pre 持发布顺序门排斥其他生产者，LCD 可并发消费。 @post 接纳后丢弃旧显示排队，成功项拥有全部页正文。 */
esp_err_t app_event_display_replace(const app_event_view_record_t *items, size_t count);
/** @brief 丢弃旧显示排队并投递清屏。 @pre 持发布顺序门，UI owner 继续 pump。 @post 不可用/退场忽略，消息拥有 pending，消费时清普通及启动显示层。 */
void app_event_display_clear(void);
/** @brief 零等待取走全部当前排队显示项。 @pre 资源存活，调用者排斥新生产者或处于退场。 @post 归还正文/页引用及 pending，已被 pump 取出的消息仍由 pump 完成。 */
void app_event_display_discard_all(void);
/** @brief 清 idle 提示。 @pre pending 已先登记且原语存活。 @post 有事件组时清 idle 位。 */
void app_event_bus_mark_sink_busy(void);
/** @brief 在两类 pending 均零时设置 idle 提示。 @pre 资源租约或 writer 存活权覆盖调用。 @post 等待者被唤醒后仍须复核计数。 */
void app_event_bus_notify_idle_if_drained(void);

/** @brief 安装 ESP vprintf 与 SDK logger sink。 @pre 生命周期 owner 维持总线原语存活并协调 hook 启停。 @post 已装标志与 SDK sink 按实现更新。 */
void app_event_log_hooks_install(void);
/** @brief 恢复默认 ESP 输出并移除 SDK sink。 @pre owner 已关闭入口且已接纳总线调用退出。 @post 后续 hook 不再投递总线，已取到的外部回调按关闭态入口处理。 */
void app_event_log_hooks_uninstall(void);
/** @brief 在状态锁内按当前配置同步整批 ESP 模块日志阈值。 @pre 总线原语存活，原生级别设置与 sink 注册不得同步回调总线。 @post 整批原生全局/指定模块阈值按状态锁顺序更新，避免旧快照后覆盖新配置。 */
void app_event_log_hooks_apply_esp_level(void);
