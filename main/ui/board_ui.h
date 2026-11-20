#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_conversation.h"
#include "esp_err.h"

/* 摄像头性能统计 */
typedef struct {
    uint32_t preview_frames;    /* 预览帧累计计数 */
    uint32_t preview_drops;     /* 丢失帧累计计数 */
    uint32_t preview_fps;       /* 当前预览帧率 */
    size_t   latest_jpeg_bytes; /* 最近一帧 JPEG 大小 */
    int      width;             /* 预览宽度 */
    int      height;            /* 预览高度 */
} board_ui_camera_stats_t;

/** @brief CPU 到 LVGL 源图交接统计，累计项在 UINT32_MAX 饱和，帧标识沿输入计数。 */
typedef struct {
    uint32_t input_frames; /**< 参数有效后进入生产者的帧数。 */
    uint32_t published_frames; /**< 已复制并发布为 pending 的帧数。 */
    uint32_t source_updates; /**< LVGL 定时器已安装源描述符的次数。 */
    uint32_t rate_limited_frames; /**< 预览限频跳过帧数。 */
    uint32_t replaced_pending_frames; /**< 新帧取代尚未消费 pending 的次数。 */
    uint32_t no_buffer_frames; /**< 无可写源缓冲的次数。 */
    uint32_t cache_sync_errors; /**< 目标 C2M 同步的实际错误数，NOT_SUPPORTED 不计。 */
    uint32_t input_row_bytes; /**< 输入有效行字节数，当前为 480。 */
    uint32_t stride_bytes; /**< LVGL 源缓冲的对齐行跨度，单位为字节。 */
    uint32_t buffer_bytes; /**< 单个预览源缓冲字节数。 */
    uint32_t buffer_count; /**< 编译配置的源缓冲槽数。 */
    uint32_t integrity_checks; /**< 消费端执行采样比较的次数。 */
    uint32_t integrity_mismatches; /**< 输入采样散列与消费前采样不一致次数。 */
    uint32_t last_mismatch_frame; /**< 最近不一致的输入帧计数标识。 */
    uint32_t pink_jump_candidates; /**< 平均颜色达到偏粉跳变阈值的次数。 */
    uint32_t last_pink_frame; /**< 最近偏粉跳变候选的输入帧计数标识。 */
    uint32_t last_pink_before_rgb; /**< 候选之前的平均 0x00RRGGBB。 */
    uint32_t last_pink_after_rgb; /**< 候选帧的平均 0x00RRGGBB。 */
} board_ui_preview_stats_t;

/** @brief UI 借用的配置回调，调用期间由 LVGL 锁保护成对上下文。 */
typedef struct {
    /**
     * @brief 读取独立的配置 JSON 副本。
     * @param[in,out] user_data owner 提供的借用上下文，覆盖调用期。
     * @return malloc 兼容的拥有 NUL 字符串，UI 调用 free，失败可返回 NULL 并按空对象打开表单。
     * @pre 同步执行于 LVGL 锁内，回调须遵守应用配置锁序。
     */
    char *(*load_config_json)(void *user_data);
    /**
     * @brief 同步保存并应用候选配置。
     * @param[in] json 完整候选 NUL JSON 借用，仅本次回调有效。
     * @param[in,out] user_data 与读取回调相同的借用上下文。
     * @return 保存成功为 true，失败为 false，回调保留 JSON 时须另行复制。
     * @pre 同步执行于 LVGL 锁内，owner 关闭外部入口并成功 detach 后才能释放上下文。
     */
    bool (*save_config_json)(const char *json, void *user_data);
    void *user_data; /**< 借用上下文，回调安装不接管其所有权。 */
} board_ui_callbacks_t;

/**
 * @brief 启动时按值复制回调及借用上下文。
 * @param[in] callbacks 调用期回调结构借用，NULL 清空全部回调。
 * @post 函数指针和上下文安装完成，不接管 user_data。
 * @pre 初始化 owner 在 board_ui_init 之前串行调用，借用上下文覆盖后续回调直至成功 detach。
 */
void board_ui_set_callbacks(const board_ui_callbacks_t *callbacks);
/**
 * @brief 等待已进入 LVGL 锁的配置回调退出后清空回调组。
 * @param[in] timeout_ms 最长锁等待毫秒数，超过 INT32_MAX 时按该上限等待，零为立即尝试。
 * @return ESP_OK 表示回调已清空，锁失败透传且保持原回调组。
 * @pre owner 先关闭新配置操作入口，初始化和 detach 串行，不能在仍需等待的回调内释放其自身上下文。
 * @post 成功后可归还旧 user_data，UI 对象本身继续存在。
 */
esp_err_t board_ui_detach_callbacks(uint32_t timeout_ms);

/**
 * @brief 初始化板级显示并构造 800×480 UI，完整候选就绪后发布 ready。
 * @return ESP_OK 表示可用，锁、队列、可检查控件及定时器 OOM 为 NO_MEM，板级错误透传。
 * @pre 单一 owner 串行初始化，BSP、adapter 和 UI 均为进程期资源。
 * @note UI 候选失败会清理树、缓冲和定时器，已建立的板级 display 保留供重试，预览缓冲不足允许降级。
 */
esp_err_t board_ui_init(void);
/**
 * @brief 原子读取 UI 完整初始化的发布状态。
 * @return true 表示对象树和队列已经可用，false 表示尚未成功初始化。
 */
bool board_ui_ready(void);

/**
 * @brief 锁内结束流并清空对话、工具和覆盖层显示及对应快照。
 * @post 已创建文本资源归还，UI 未就绪时跳过。
 */
void board_ui_conversation_clear(void);
/**
 * @brief 结束旧流后追加独立消息标签。
 * @param[in] role 消息角色，决定前缀和颜色。
 * @param[in] text 调用期 NUL 正文借用，NULL 作为空串。
 * @post 角色前缀及正文最多复制 511 字节，分配失败跳过候选标签，UI 锁内维护条数及字节预算。
 */
void board_ui_conversation_append(board_conversation_role_t role, const char *text);
/**
 * @brief 同角色片段复制进增长缓冲，角色变化时结束旧流。
 * @param[in] role 消息角色。
 * @param[in] text 调用期 NUL 文本借用，NULL 或空串忽略。
 * @post 单流前缀和 NUL 纳入默认 512KiB 上限，超额截取字符前缀，分配失败保留此前接纳正文。
 * @note 拼接缓冲、标签文本和快照各持有独立副本，512KiB 是保留标签正文预算，更新时存在候选副本峰值。
 */
void board_ui_conversation_append_fragment(board_conversation_role_t role, const char *text);
/**
 * @brief 尝试最后一次标签刷新后结束当前流。
 * @post 拼接缓冲归还，安装失败时标签保留此前正文，后续片段创建新标签。
 */
void board_ui_conversation_finish_fragment(void);
/**
 * @brief 在对话覆盖层创建填充矩形，原生父对象负责可见区域裁剪。
 * @param[in] x 相对覆盖层水平像素坐标。
 * @param[in] y 相对覆盖层垂直像素坐标。
 * @param[in] width 正宽度，单位为像素。
 * @param[in] height 正高度，单位为像素。
 * @param[in] color RGB565 颜色值。
 * @post 内部持锁创建对象，未就绪、非正尺寸或原生创建失败跳过，诊断快照为最佳努力。
 */
void board_ui_conversation_draw_rect(int x, int y, int width, int height, uint16_t color);
/**
 * @brief 在对话覆盖层创建带角色样式的文本对象。
 * @param[in] x 相对水平像素坐标，负值钳为零。
 * @param[in] y 相对垂直像素坐标，负值钳为零。
 * @param[in] role 决定颜色的角色值。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串，由 LVGL 复制。
 * @post 内部持锁创建，未就绪或创建失败跳过，对象归覆盖层所有。
 */
void board_ui_conversation_draw_text(int x, int y, board_conversation_role_t role, const char *text);
/**
 * @brief 标记主树、对话、覆盖层及工具区需要重绘。
 * @post 内部持锁设置失效区域，不等待物理屏完成，未就绪时跳过。
 */
void board_ui_conversation_request_flush(void);

/**
 * @brief 非阻塞提交工具区清空命令。
 * @post LVGL 任务随后清空对象及工具快照，未就绪或队列满时丢弃本命令。
 */
void board_ui_tool_area_clear(void);
/**
 * @brief 非阻塞提交工具区追加文本命令。
 * @param[in] role 按值角色，决定颜色及前缀。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串，命令最多复制 383 字节。
 * @post 队列拥有命令副本，满队列或未就绪时跳过，返回后无输入借用。
 */
void board_ui_tool_area_append(board_conversation_role_t role, const char *text);
/**
 * @brief 非阻塞提交工具区矩形命令。
 * @param[in] x 相对工具画布的水平像素坐标，消费时钳入画布。
 * @param[in] y 相对工具画布的垂直像素坐标，消费时钳入画布。
 * @param[in] width 正像素宽度，消费时裁至剩余可见区域。
 * @param[in] height 正像素高度，消费时裁至剩余可见区域。
 * @param[in] color RGB565 颜色值。
 * @post 参数非正、未就绪或队列满时跳过，接纳后由 LVGL 任务创建对象。
 */
void board_ui_tool_area_draw_rect(int x, int y, int width, int height, uint16_t color);
/**
 * @brief 非阻塞提交工具画布指定位置的文本命令。
 * @param[in] x 水平像素坐标，消费时钳入画布。
 * @param[in] y 垂直像素坐标，消费时钳入画布。
 * @param[in] role 决定颜色的角色。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串，最多复制 383 字节。
 * @post 接纳的命令独立持有文本副本，队列满或未就绪时跳过。
 */
void board_ui_tool_area_draw_text(int x, int y, board_conversation_role_t role, const char *text);
/**
 * @brief 非阻塞提交工具区失效刷新命令。
 * @post 接纳后由 LVGL 任务设置失效区域，不等待物理屏完成，队列满或未就绪时跳过。
 */
void board_ui_tool_area_request_flush(void);

/**
 * @brief 向启动日志覆盖层追加带角色前缀的一行，覆盖层结束后改投对话。
 * @param[in] role 显示角色。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串，完整显示行最多保留 511 字节。
 * @post 内部持锁，启动日志最多保留 80 个标签，创建失败尽力跳过。
 */
void board_ui_boot_log_append(board_conversation_role_t role, const char *text);
/**
 * @brief 清空启动日志对象及对应显示快照。
 * @post 内部持锁归还启动行资源，UI 尚未就绪时跳过。
 */
void board_ui_boot_log_clear(void);
/**
 * @brief 删除启动覆盖层并显示主 UI。
 * @post 内部持锁清空启动对象借用，启动显示快照按既有行为保留至显式清空。
 */
void board_ui_boot_log_finish(void);

/**
 * @brief 复制一帧 CPU 可读 RGB565 到后台源缓冲并发布 pending。
 * @param[in] rgb565 调用期稳定的紧密排列 115200 字节借用，NULL 忽略。
 * @param[in] width 图像像素宽度，必须为 240。
 * @param[in] height 图像像素高度，必须为 240。
 * @post 函数返回前完成源复制，不保留输入借用，限频、无缓冲或缓存同步失败时跳过，可能主动 yield 一 tick。
 * @pre 每个 UI 仅单一摄像头生产者调用，输入 DMA 的 CPU 可见性由调用者保证，调用者不持 LVGL 锁。
 * @note 目标 C2M 同步后才发布，输入保持 CPU 读语义，消费者在 LVGL 定时器中接管 pending。
 */
void board_ui_camera_preview_update(const void *rgb565, int width, int height);
/**
 * @brief 在短临界区复制预览交接统计及布局信息。
 * @param[out] out_stats 可写输出槽，NULL 跳过，UI 未就绪时清零。
 * @post 交付按值独立快照，不包含原始像素或内部缓冲借用。
 */
void board_ui_camera_preview_get_stats(board_ui_preview_stats_t *out_stats);
/**
 * @brief 在短临界区发布摄像头性能统计副本。
 * @param[in] stats 调用期结构借用，NULL 忽略，宽高单位为像素，JPEG 大小单位为字节。
 * @post 状态栏随后读取整组副本，调用结束不保留输入指针。
 */
void board_ui_camera_stats_update(const board_ui_camera_stats_t *stats);
/**
 * @brief 发布配置显示状态，已加载且 UI 就绪时执行完整字体重绑事务。
 * @param[in] loaded 配置加载标志。
 * @param[in] provider 调用期 NUL 提供商名借用，NULL 为空，最多复制 31 字节。
 * @param[in] model 调用期 NUL 模型名借用，NULL 为空，最多复制 63 字节。
 * @post 状态短临界区发布，加载与六棵实际对象树重绑及旧字体释放均在同一 LVGL 锁内完成。
 * @pre 字体路径设置、文件挂载和加载由启动 owner 串行，外部字体文件覆盖整个加载期。
 */
void board_ui_set_config_loaded(bool loaded, const char *provider, const char *model);
/**
 * @brief 发布状态栏使用的 USB 挂载标志。
 * @param[in] mounted 按值挂载状态。
 * @post 短临界区更新标志，不等待 LVGL 锁，状态栏后续读取副本。
 */
void board_ui_set_usb_mounted(bool mounted);
/**
 * @brief 复制 Wi-Fi 状态及状态栏详情后在短临界区发布。
 * @param[in] connected 按值连接状态。
 * @param[in] detail 调用期 NUL 字符串借用，NULL 为空，最多复制 63 字节。
 * @post 两个字段成组发布，不保留输入借用，不等待 LVGL 锁。
 */
void board_ui_set_wifi_state(bool connected, const char *detail);
/**
 * @brief 发布状态栏的重启提示标志。
 * @param[in] required 按值提示状态。
 * @post 短临界区更新，不等待 LVGL 锁，可在配置事务中调用。
 */
void board_ui_set_runtime_restart_required(bool required);
/**
 * @brief 标记 OTA 活跃并尽力显示准备覆盖层。
 * @param[in] name 调用期 NUL 固件名借用，NULL 或空串使用默认名称。
 * @param[in] expected_size 预期固件字节数，零表示未知。
 * @param[in] message 调用期 NUL 状态借用，NULL 或空串使用默认等待说明。
 * @post 内部持锁复制文本并设置零进度，创建失败跳过显示，状态栏在 active 期间暂停。
 * @pre OTA owner 串行调用各阶段接口。
 */
void board_ui_ota_prepare(const char *name, size_t expected_size, const char *message);
/**
 * @brief 标记 OTA 活跃并尽力显示写入开始状态。
 * @param[in] name 调用期 NUL 固件名借用，NULL 或空串使用默认名称。
 * @param[in] expected_size 预期固件字节数，零表示未知。
 * @post 内部持锁复制文本并初始化进度，创建失败跳过显示。
 * @pre OTA owner 串行调用各阶段接口。
 */
void board_ui_ota_begin(const char *name, size_t expected_size);
/**
 * @brief 节流更新 OTA 进度，达到预期大小时立即更新。
 * @param[in] bytes_written 已写入字节数。
 * @param[in] expected_size 预期总字节数，零显示零百分比。
 * @post 未完成进度的更新窗口为 250ms，内部持锁尽力显示 0 到 100 的百分比。
 * @pre OTA owner 串行调用，写入计数的真实性由 OTA 服务保证。
 */
void board_ui_ota_progress(size_t bytes_written, size_t expected_size);
/**
 * @brief 尽力显示 OTA 成功或失败结果并结束状态栏暂停。
 * @param[in] ok 按值结果状态，控制绿色满进度或红色零进度。
 * @param[in] message 调用期 NUL 说明借用，NULL 或空串使用默认说明。
 * @post 内部持锁复制文本，随后原子清 active，覆盖层保留结果。
 * @pre OTA owner 串行调用各阶段接口。
 */
void board_ui_ota_end(bool ok, const char *message);
/**
 * @brief 尽力显示黑色顶层覆盖并强制一次 LVGL 刷新，随后延迟 250ms。
 * @post 延迟前归还 UI 锁，未就绪直接跳过，物理面板完成责任仍由 BSP 和 adapter 管理。
 */
void board_ui_prepare_for_reboot(void);
/**
 * @brief 锁内加载配置副本并构造完整表单及顶层键盘。
 * @post 模态框已存在时复用，解析或可检查创建失败回收整个候选，不保留 JSON 副本借用。
 * @note 配置回调与 detach 使用同一 LVGL 锁域，回调 user_data 必须覆盖同步调用期。
 */
void board_ui_show_config_editor(void);
