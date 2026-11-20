#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "cc/ports/cc_logger.h"
#include "esp_err.h"

/** @brief 近期事件种类，种类与 source 共同描述展示语义。 */
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

/** @brief 事件生成入口，串口 USER 用于避免重复回显。 */
typedef enum {
    APP_EVENT_SOURCE_SYSTEM = 0,
    APP_EVENT_SOURCE_SERIAL,
    APP_EVENT_SOURCE_WEB,
    APP_EVENT_SOURCE_RUNTIME,
    APP_EVENT_SOURCE_TOOL,
} app_event_source_t;

/** @brief 总线生命周期，可重试失败保留未释放原语。 */
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

/* 历史页同时受条数和正文预算限制，超限拒绝整页，调用者可改取更小页面。 */
#define APP_EVENT_VIEW_MAX_ITEMS 16U
#define APP_EVENT_VIEW_MAX_BYTES (32U * 1024U)
/** @brief 调用方借用的单条可见历史页输入。 */
typedef struct {
    app_event_kind_t kind; /* 有效事件种类。 */
    const char *text;      /* 替换调用期间借用的 NUL 文本，NULL 按空串。 */
} app_event_view_item_t;

/** @brief 无外部借用的总线状态快照，各字段在同一状态锁期复制。 */
typedef struct {
    app_event_bus_lifecycle_t lifecycle;     /* 事件总线当前生命周期状态 */
    bool serial_debug_enabled;               /* 串口调试模式是否已启用 */
    bool maintenance_mode;                   /* 是否处于 OTA 维护模式 */
    bool usb_log_enabled;                    /* USB 日志功能是否已配置启用 */
    bool usb_log_started;                    /* USB writer 是否已启动且允许接纳新消息。 */
    bool usb_log_fallback_serial;            /* 是否因 USB 失败而回退到串口 */
    uint32_t dropped;                        /* 各路径累计丢弃或淘汰次数，同事件可在不同 sink 分别计数。 */
    uint32_t ring_overwrites;                /* ring 淘汰旧项或拒绝超单条预算的次数。 */
    uint32_t sink_dropped;                   /* sink 丢弃或文件 I/O 失败次数，含维护和正文 OOM。 */
    uint32_t published;                      /* 总发布事件计数 */
    uint32_t maintenance_skipped;            /* 维护模式下跳过的日志行数 */
    uint32_t oom_dropped;                    /* 因内存不足丢弃的事件计数 */
    uint32_t usb_queue_dropped;              /* USB 入队失败和文件 I/O 错误次数，周期 flush 失败也计一次。 */
    uint32_t display_queue_dropped;          /* 显示队列满丢弃计数 */
    uint32_t max_usb_queue_waiting;          /* USB 每次发送前采样到的最大排队数。 */
    uint32_t max_display_queue_waiting;      /* 显示每次普通消息发送前采样到的最大排队数。 */
    uint32_t oldest_seq;                     /* 缓冲区内最旧事件的序列号 */
    uint32_t latest_seq;                     /* 最近发布的事件序列号 */
    size_t capacity;                         /* 环形缓冲区容量 */
    size_t retained_count;                   /* 近期 ring 当前记录数，不含额外固定历史页引用。 */
    size_t retained_bytes;                   /* ring 正文头、文本和 NUL 的申请字节，包含两类堆，排除分配器开销。 */
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

/**
 * @brief 初始化总线自有原语、近期 ring 和 sink，随后应用配置或进入串口回退。
 * @param[in] io 调用期稳定 IO 配置借用，NULL 启用串口 DEBUG 回退。
 * @return ESP_OK 成功，分配、配置或 USB 启动失败返回对应错误，已创建资源归总线拥有，可重试或 destroy。
 * @pre 生命周期 owner 串行调用 init/apply/stop/destroy/串口回退，配置输入在调用期间保持有效；发布、状态查询和 LVGL pump 可与 stop/destroy 并发。
 * @note 普通事件正文最多 2048 字节，近期 ring 同时受 1024 条及 512 KiB 正文申请字节预算约束，固定历史页单独保留引用。
 */
esp_err_t app_event_bus_init(const app_runtime_io_config_t *io);
/**
 * @brief 复制 IO 路由和标量并重新配置 USB 输出。
 * @param[in] io 必需配置借用，各非零 count 列表须有 items，USB 目录须适合 128 字节含 NUL 槽。
 * @return 参数/目录大小错误在状态修改前返回，入口关闭返回 INVALID_STATE，其他失败透传；USB 启动失败时新配置已复制并尝试串口回退。
 * @pre 生命周期 owner 与其他启停操作串行，整个调用期间输入稳定。
 */
esp_err_t app_event_bus_apply_config(const app_runtime_io_config_t *io);
/**
 * @brief 关闭异步 sink 接纳，排空已接纳消息并等待 USB writer 最后一次总线访问结束。
 * @param[in] timeout_ms 单一毫秒总预算，零使用 2000，正值至少一 tick 且保持有限。
 * @return ESP_OK 完整停止，超时或文件关闭错误保留剩余资源供 owner 重试，缺少原语为 INVALID_STATE。
 * @pre 生命周期 owner 串行调用，LCD pump 继续运行以消费已接纳显示消息，禁止从 writer 自身停止自身。
 * @note writer 完成位在原生 SetBits 返回后发布，kernel 自删与 TCB 回收随后完成。FILE 后端调用和短状态锁受原生调度影响。
 */
esp_err_t app_event_bus_stop(uint32_t timeout_ms);
/**
 * @brief 停止 sink 后关闭资源租约门，等待已接纳调用退出并卸载 hook、释放对象。
 * @param[in] timeout_ms 毫秒总预算，零使用 2000，停止和租约退出共享此预算。
 * @return 未初始化或全部释放为 ESP_OK，失败保留剩余对象供重试；接纳门已经关闭时后续调用使用各自关闭态行为。
 * @pre 生命周期 owner 串行调用，调用者自身未持有总线同步回调中的租约。
 */
esp_err_t app_event_bus_destroy(uint32_t timeout_ms);
/** @brief 停止 USB 后启用串口 DEBUG 回退并补充必要路由。 @return 入口关闭为 INVALID_STATE，USB 5000 毫秒内停止失败透传，提交回退成功为 ESP_OK。 @pre 生命周期 owner 与配置和启停串行。 */
esp_err_t app_event_bus_start_serial_fallback_debug(void);
/** @brief 登记时间命名请求。 @post writer 在后续消息处理中检查真实时间并尝试重命名，入口关闭时忽略。 */
void app_event_bus_note_time_synced(void);
/** @brief 设置串口阈值并同步 ESP 日志级别。 @param[in] enabled true 设 DEBUG，false 设 INFO。 @post 清自动回退标志，保留串口开关和路由，入口关闭时无操作。 */
void app_event_bus_set_serial_debug(bool enabled);
/** @brief 切换维护事件过滤。 @param[in] enabled 目标维护状态。 @param[in] reason 调用期原因借用，可 NULL，复制至 64 字节槽并允许截断。 @post 启用后只接收既有维护白名单并投递串口，关闭时清原因，已接纳异步消息继续处理；入口关闭时无操作。 */
void app_event_bus_set_maintenance_mode(bool enabled, const char *reason);
/** @brief 读取串口调试状态快照。 @return 当前调试位，自动回退也可置位，入口关闭为 false。 */
bool app_event_bus_serial_debug_enabled(void);
/** @brief 读取维护过滤状态快照。 @return 当前维护位，入口关闭为 false。 */
bool app_event_bus_maintenance_mode_enabled(void);
/** @brief 等待两类异步 sink 的已接纳计数归零。 @param[in] timeout_ms 毫秒预算，零使用 5000。 @return 观察到归零为 true，入口关闭、原语不足或到期为 false。 @note 运行中后续发布可再增加消息，USB 周期 flush 与 stop 最终 flush 分别完成各自职责。 */
bool app_event_bus_wait_idle(uint32_t timeout_ms);
/** @brief 复制单次状态锁内的一致快照。 @param[out] out_status 可选输出，非 NULL 先整体清零。 @post 入口关闭为全零，正常输出无借用字段，累计计数在 uint32_t 上界饱和，USB 累计字节在 SIZE_MAX 饱和。 */
void app_event_bus_get_status(app_event_bus_status_t *out_status);
/** @brief 在状态锁内复制当前 USB 日志路径。 @param[out] out_path 可选输出缓冲。 @param[in] out_size 字节容量，含 NUL，零无操作。 @post 有效输出先置空，入口关闭为空串，容量不足按 UTF-8 边界截断。 */
void app_event_bus_copy_current_log_path(char *out_path, size_t out_size);

/**
 * @brief 脱敏并复制事件，按发布顺序路由到串口、近期 ring 和异步 sink。
 * @param[in] kind 按值种类。
 * @param[in] source 按值来源，串口来源 USER 避免重复回显。
 * @param[in] level 按值级别，供路由阈值使用。
 * @param[in] text 调用期 NUL 文本借用，NULL 按空串，函数不保留输入。
 * @post 入队成功的 sink 独立持引用，OOM/维护过滤/队列满按统计丢弃，重入发布直接丢弃，入口关闭时仅脱敏串口输出。
 * @note 序号耗尽时清空近期 ring、历史页与显示，从 1 重新编号并改变 clear_generation。Web 轮询通过代数变化或 cursor_reset 刷新，代数允许 MAX 回零，持久会话记录维持原范围。
 */
void app_event_publish(app_event_kind_t kind,
                       app_event_source_t source,
                       cc_log_level_t level,
                       const char *text);
/** @brief 发布启动状态，LCD 使用启动日志层并与清屏排序。 @param[in] kind 按值种类。 @param[in] source 按值来源。 @param[in] level 按值级别。 @param[in] text 调用期文本借用，可 NULL。 @post 普通显示路由未启用时仍可投递启动层，维护与退场遵循发布过滤规则。 */
void app_event_publish_boot_status(app_event_kind_t kind,
                                   app_event_source_t source,
                                   cc_log_level_t level,
                                   const char *text);
/** @brief 格式化有界正文后普通发布。 @param[in] kind 按值种类。 @param[in] source 按值来源。 @param[in] level 按值级别。 @param[in] fmt printf 格式借用，NULL 无操作。 @param[in] ... 匹配格式的实参。 @post 格式化失败忽略，超长输出按完整 UTF-8 前缀截断再交普通发布。 */
void app_event_publishf(app_event_kind_t kind,
                        app_event_source_t source,
                        cc_log_level_t level,
                        const char *fmt,
                        ...);
/** @brief 按发布顺序清空近期 ring、固定历史页、LCD 以及已启用串口显示。 @post clear_generation 改变供 Web 轮询刷新，允许 MAX 回零；持久会话记录保持原范围，入口关闭或递归发布时忽略。 */
void app_event_bus_clear_conversation(void);
/** @brief 事务式替换脱敏历史页并同步 Web、串口和已启用 LCD。 @param[in] items count 项调用期借用，零条时可 NULL，每项 text 可 NULL。 @param[in] count 最多 16 条，原文与脱敏输出分别受含 NUL 的 32 KiB 总预算约束，零条清页。 @return 成功 ESP_OK，参数、大小、OOM、接纳状态或显示排队失败返回对应错误并保留旧页。 @post 成功时换代且独立拥有正文，序号余额不足先从 1 起算，局部资源全部释放。 */
esp_err_t app_event_bus_replace_conversation(const app_event_view_item_t *items, size_t count);
/** @brief 在锁内保留正文引用，再解锁构造事件 JSON 快照。 @param[in] since_seq 增量游标，零取当前视图，超出 latest 触发 cursor_reset。 @param[in] limit 输出事件条数，非正或超过 ring 槽位容量时使用槽位容量，匹配项超额时保留最新一段。 @param[in] kind_mask 种类位集合，零不筛选，非零按有效种类位过滤。 @return 调用者 free 的完整 JSON 字符串，接纳关闭或任意分配/构造失败为 NULL。 @note 单次快照含 clear_generation、gap 和固定历史页恢复信息，轮询者接受代数零值并比较是否改变。 */
char *app_event_bus_json(uint32_t since_seq, int limit, uint32_t kind_mask);
/** @brief 取得种类名称。 @param[in] kind 按值种类。 @return 静态字符串借用，未知为 unknown。 */
const char *app_event_kind_string(app_event_kind_t kind);
/** @brief 取得来源名称。 @param[in] source 按值来源。 @return 静态字符串借用，未知为 unknown。 */
const char *app_event_source_string(app_event_source_t source);
/** @brief 取得生命周期名称。 @param[in] lifecycle 按值状态。 @return 静态字符串借用，未知为 unknown。 */
const char *app_event_bus_lifecycle_string(app_event_bus_lifecycle_t lifecycle);
/** @brief 在 UI owner 上消费有界显示批次并更新 LCD。 @post 消费或丢弃的消息引用释放并更新 pending，接纳门关闭时无操作。 @pre 由具有 LVGL/板级 UI 访问权限的单一调用者驱动，stop 等待显示消息期间仍须 pump。 */
void app_event_display_pump(void);
