#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cc/util/cc_config.h"
#include "esp_err.h"
#include "network_config.h"

/* USB 大容量存储上 config.json 的标准路径 */
#define APP_CONFIG_FILE_PATH "/usb0/config.json"

/* 串口控制台日志级别配置 */
typedef struct {
    int enabled;            /* 是否启用串口输出 */
    char *log_level;        /* 日志级别字符串: trace/debug/info/warn/error/fatal */
} app_config_serial_t;

/* Web 控制台配置 */
typedef struct {
    int enabled;            /* 是否启用 HTTP 控制台 */
    int port;               /* HTTP 监听端口，默认 80 */
    int event_limit;        /* 单次 /api/events 返回的最大事件数 */
} app_config_web_t;

/* 板载 LCD 显示配置 */
typedef struct {
    int enabled;            /* 是否启用 LCD 显示 */
} app_config_display_t;

/* USB 日志滚动写入配置 */
typedef struct {
    int enabled;            /* 是否启用 USB 日志写入 */
    char *dir;              /* 日志文件存储目录 */
    size_t max_bytes;       /* 单个日志文件最大字节数 */
    int max_files;          /* 最大保留日志文件数 */
    char *level;            /* 写入 USB 日志的最低级别 */
} app_config_usb_log_t;

/* 事件路由配置：指定各类事件发送到哪些 sink（serial/web/display/usb_log） */
typedef struct {
    cc_config_string_list_t log;       /* SDK 日志事件路由 */
    cc_config_string_list_t chat;      /* 对话消息事件路由 */
    cc_config_string_list_t thinking;  /* 思考过程事件路由 */
    cc_config_string_list_t tool;      /* 工具执行事件路由 */
    cc_config_string_list_t status;    /* 状态变更事件路由 */
    cc_config_string_list_t error;     /* 错误事件路由 */
    cc_config_string_list_t profile;   /* 性能剖析事件路由 */
} app_config_event_routes_t;

/* 应用 IO 配置，汇总所有传输层和事件路由设置 */
typedef struct {
    char *session_id;              /* 会话标识符，用于识别设备 */
    int queue_depth;               /* 交互队列深度 */
    int max_input_bytes;           /* 用户输入最大字节数 */
    app_config_serial_t serial;    /* 串口配置 */
    app_config_web_t web;          /* Web 控制台配置 */
    app_config_display_t display;  /* LCD 显示配置 */
    app_config_usb_log_t usb_log;  /* USB 日志配置 */
    app_config_event_routes_t routes; /* 事件路由配置 */
} app_runtime_io_config_t;

/* 性能剖析配置 */
typedef struct {
    int enabled;            /* 是否启用性能剖析 */
    char *level;            /* 剖析级别: basic/trace */
    int interval_ms;        /* 采集间隔（毫秒），0 表示单次 */
} app_profile_config_t;

/* 应用配置根结构体，聚合运行时配置、IO 配置、网络配置等 */
typedef struct {
    cc_config_t runtime;              /* SDK 运行时配置（provider、model、storage 等） */
    app_runtime_io_config_t io;       /* IO 层配置（串口、Web、显示、日志路由） */
    app_profile_config_t profile;     /* 性能剖析配置 */
    network_config_t network;         /* WiFi/AP/mDNS 网络配置 */
    char *raw_json;                   /* config.json 原始 JSON 文本 */
    char display_font_path[160];      /* LCD 字体文件路径 */
    char camera_default_save_path[160]; /* 摄像头抓拍默认保存路径 */
    int camera_default_quality;       /* JPEG 抓拍默认质量 (1-100) */
    bool loaded;                      /* 配置是否已成功加载 */
} app_config_t;

/*
 * 一次配置事务的可观测结果。每个字段只在对应阶段实际完成后置 true；error 和
 * failed_subsystem 指向首个失败阶段。restart_required 表示配置已提交，但其中至少
 * 一项只能在下次启动时生效。
 */
typedef struct {
    bool parsed;
    bool event_bus_applied;
    bool interaction_applied;
    bool network_applied;
    bool profile_applied;
    bool font_applied;
    bool runtime_controls_applied;
    bool durable_committed;
    bool rollback_attempted;
    bool rollback_succeeded;
    bool restart_required;
    esp_err_t error;
    char failed_subsystem[24];
} app_config_apply_result_t;

/*
 * 从指定路径加载 config.json 并解析到 config 结构体。
 *
 * 参数:
 *   config - 指向目标 app_config_t（旧内容会被释放）
 *   path   - config.json 文件路径
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_ARG 参数无效，ESP_FAIL 文件读取/解析失败。
 */
esp_err_t app_config_load(app_config_t *config, const char *path);
/*
 * 保存 JSON 配置并原子替换当前 config。
 *
 * 参数:
 *   config    - 当前配置（会被替换为解析后的新配置）
 *   path      - 目标文件路径
 *   json_text - 待保存的 JSON 文本
 * 返回: ESP_OK 成功。会先验证 JSON 合法性，然后通过临时文件 + rename 实现原子写入。
 */
esp_err_t app_config_save(app_config_t *config, const char *path, const char *json_text);
esp_err_t app_config_prepare(app_config_t *prepared, const char *json_text);
/*
 * Prepare a Web/UI update from a redacted full-config snapshot. Secret placeholders are replaced
 * from current only inside this trusted merge; new plaintext submitted by the caller is rejected.
 */
esp_err_t app_config_prepare_update(
    app_config_t *prepared,
    const app_config_t *current,
    const char *json_text
);
/* 判断 prepared 中是否包含不能在当前 MCU 进程安全热替换的配置变更。 */
bool app_config_requires_restart(const app_config_t *current, const app_config_t *prepared);
esp_err_t app_config_commit_prepared(
    app_config_t *current,
    app_config_t *prepared,
    const char *path
);
/*
 * 释放配置中的动态内存并清零。
 *
 * 参数:
 *   config - 指向待销毁的 app_config_t。
 */
void app_config_destroy(app_config_t *config);
/*
 * 获取 SDK 运行时配置的只读引用。
 *
 * 返回: 指向 cc_config_t 的指针，未加载时返回 NULL。
 */
const cc_config_t *app_config_runtime(const app_config_t *config);
/*
 * 获取 IO 层配置的只读引用。
 *
 * 返回: 指向 app_runtime_io_config_t 的指针，未加载时返回 NULL。
 */
const app_runtime_io_config_t *app_config_io(const app_config_t *config);
/*
 * 获取网络配置的只读引用。
 *
 * 返回: 指向 network_config_t 的指针，未加载时返回 NULL。
 */
const network_config_t *app_config_network(const app_config_t *config);
/*
 * 获取原始 JSON 配置文本。
 *
 * 返回: 原始 JSON 字符串，未加载时返回 "{}"。
 */
const char *app_config_json(const app_config_t *config);
/*
 * 查询是否启用了思考模式（thinking mode）。
 *
 * 返回: true 如果配置已加载且 thinking_mode 非零。
 */
bool app_config_runtime_thinking(const app_config_t *config);
