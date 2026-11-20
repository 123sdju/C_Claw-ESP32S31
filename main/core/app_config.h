#pragma once

/* 设备级恢复数据使用固定根目录，独立于可配置的工作区。 */
#ifndef APP_RECOVERY_ROOT
#define APP_RECOVERY_ROOT "/usb0/cclaw/recovery"
#endif

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
    char *log_level;        /* 拥有的日志级别字符串，取 trace/debug/info/warn/error/fatal。 */
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
    char *dir;              /* 拥有的日志文件存储目录字符串。 */
    size_t max_bytes;       /* 单个日志文件最大字节数 */
    int max_files;          /* 最大保留日志文件数 */
    char *level;            /* 拥有的 USB 日志最低级别字符串。 */
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
    char *session_id;              /* 拥有的启动会话标识符。 */
    int queue_depth;               /* 交互队列深度 */
    int max_input_bytes;           /* 用户输入最大字节数 */
    int max_pending_operations;    /* 设备级待核对操作上限，重启生效 */
    app_config_serial_t serial;    /* 串口配置 */
    app_config_web_t web;          /* Web 控制台配置 */
    app_config_display_t display;  /* LCD 显示配置 */
    app_config_usb_log_t usb_log;  /* USB 日志配置 */
    app_config_event_routes_t routes; /* 事件路由配置 */
} app_runtime_io_config_t;

/* 性能剖析配置 */
typedef struct {
    int enabled;            /* 是否启用性能剖析 */
    char *level;            /* 拥有的剖析级别字符串，取 basic/trace。 */
    int interval_ms;        /* 采集间隔（毫秒），0 表示单次 */
} app_profile_config_t;

/**
 * @brief 拥有运行时、IO、网络及规范化 JSON 的应用配置快照。
 * @note 首次使用须清零。内部指针由本快照拥有，禁止浅拷贝形成两个所有者。
 * 所有访问均由外层串行化，替换或销毁前须先停止借用这些字段的组件。
 * 产品解析拒绝凭据路径上的大小写重复字段，包括 root 的 model/network、network 的 sta/ap，
 * 以及模型 api_key/api_key_ref 和 STA/AP 的 password/password_ref。单个大写键继续支持。
 * model.extra_body 等独立业务 JSON 不应用这组局部重复规则。
 */
typedef struct {
    cc_config_t runtime;              /* SDK 运行时配置（provider、model、storage 等） */
    app_runtime_io_config_t io;       /* IO 层配置（串口、Web、显示、日志路由） */
    size_t managed_memory_capacity;   /* 活动长期记忆条数上限，重启生效 */
    size_t managed_memory_pending_items; /* 记忆待处理队列的条数上限，重启生效 */
    size_t managed_memory_pending_bytes; /* 待处理队列的字节预算，重启生效 */
    size_t summary_cache_per_session; /* 每个会话持久化摘要数上限，重启生效 */
    size_t summary_cache_max_entries;  /* 所有会话的摘要数上限，重启生效 */
    size_t summary_cache_max_bytes;    /* 摘要内容的 UTF-8 字节上限，重启生效 */
    app_profile_config_t profile;     /* 性能剖析配置 */
    network_config_t network;         /* WiFi/AP/mDNS 网络配置 */
    char *raw_json;                   /* 拥有的重新序列化 JSON，可能含明文凭据，销毁时擦除。 */
    char display_font_path[160];      /* LCD 字体路径，按字节截断并保证 NUL 结尾。 */
    char camera_default_save_path[160]; /* 抓拍默认路径，按字节截断并保证 NUL 结尾。 */
    int camera_default_quality;       /* JPEG 抓拍默认质量 (1-100) */
    bool loaded;                      /* 配置是否已成功加载 */
    bool updates_blocked;             /* 保存结果待核对，重启加载前禁止继续保存 */
} app_config_t;

/**
 * @brief 一次配置事务的阶段报告，由应用生命周期协调者填写。
 * @note applied 字段记录本次曾成功完成的阶段，回滚后仍保留；restart_required 是差异分类，
 * 本身不能证明提交成功。error 和 failed_subsystem 描述本次返回的失败阶段。
 */
typedef struct {
    bool parsed; /**< 完整提交快照已解析和校验。 */
    bool event_bus_applied; /**< 事件路由应用成功。 */
    bool interaction_applied; /**< 交互服务配置应用成功。 */
    bool network_applied; /**< Web 端口及 WiFi 配置应用成功。 */
    bool profile_applied; /**< 性能剖析配置应用成功。 */
    bool font_applied; /**< 字体路径已提交给显示服务。 */
    bool runtime_controls_applied; /**< 运行时思考模式开关已应用。 */
    bool durable_committed; /**< 配置提交返回 ESP_OK，所需同步已完成。 */
    bool rollback_attempted; /**< 已尝试恢复旧配置对应的热更新组件。 */
    bool rollback_succeeded; /**< 全部有返回状态的回滚调用均成功。 */
    bool restart_required; /**< 本次差异包含启动时重装配的字段。 */
    esp_err_t error; /**< 本次事务错误码，初始为 ESP_OK。 */
    char failed_subsystem[24]; /**< NUL 结尾失败阶段名，过长名称截断。 */
} app_config_apply_result_t;

/**
 * @brief 读取配置候选、确认必要修复后替换内存快照，并尽力创建工作区目录。
 * @param[in,out] config 有效且已清零或已初始化的目标；成功释放旧内容，失败保留旧内容。
 * @param[in] path 非空配置文件路径，本次调用借用。
 * @return 完整加载返回 ESP_OK；参数或配置无效、内存不足、文件变化及修复失败返回对应错误。
 * @note 使用与 stage 相同的候选选择和校验，启动 USB 配置可含明文凭据。
 * 调用者须独占当前介质，修复失败可能已改变磁盘，目录创建失败只记录日志。
 */
esp_err_t app_config_load(app_config_t *config, const char *path);

typedef struct app_config_stage app_config_stage_t;
/**
 * @brief 只读选择正式文件、.tmp、.bak 中首个可解析的 JSON 对象，并校验配置语义。
 * @param[in] path 非空正式文件路径，本次调用借用。
 * @param[out] out_stage 有效输出槽，先置 NULL，成功接收须用 stage_destroy 释放的暂存对象。
 * @return 成功返回 ESP_OK；读失败、文件超过 1 MiB、分配失败及配置无效返回错误。
 * @note 只有缺失或明确无效的候选才继续选择，无法判定的解析失败终止选择。
 * 已选对象的语义错误（包括重复凭据字段或容器）终止加载，须修正当前配置后重试。
 * 暂存对象拥有选中及更高优先级文件的逐字节快照和凭据。
 * 所有 stage 调用均须外层排除并发写入及卸载，并在调用前后独立核对挂载代次。
 */
esp_err_t app_config_stage_open(const char *path, app_config_stage_t **out_stage);
/**
 * @brief 借用尚未转移的暂存配置。
 * @param[in] stage 可为 NULL 的暂存对象，借用期间不得 repair、take 或 destroy。
 * @return 有效且未消费时返回配置，其他状态返回 NULL。
 */
const app_config_t *app_config_stage_config(const app_config_stage_t *stage);
/**
 * @brief 查询尚未消费的配置来源。
 * @param[in] stage 可为 NULL 的暂存对象。
 * @return 正式文件为 0，.tmp 为 1，.bak 为 2，NULL、失效或已消费返回 -1。
 */
int app_config_stage_source(const app_config_stage_t *stage);
/**
 * @brief 比较选中及更高优先级候选的存在状态和完整字节快照。
 * @param[in] stage 有效暂存对象，take 后仍可校验至销毁，NULL 返回状态错误。
 * @return 全部一致返回 ESP_OK，失效或文件变化返回 ESP_ERR_INVALID_STATE，读取及分配失败返回错误。
 * @note 此检查不锁定介质，调用者仍须满足 stage_open 的挂载与写入互斥约束。
 */
esp_err_t app_config_stage_validate(const app_config_stage_t *stage);
/**
 * @brief 校验候选，必要时将所选恢复副本安装为正式配置。
 * @param[in,out] stage 有效暂存对象；开始变更后的失败将其标记失效，必须重新打开。
 * @return 正式文件已匹配或修复并确认成功返回 ESP_OK；不支持修复、文件变化和 IO 失败返回错误。
 * 安装后目录同步未确认可返回 ESP_ERR_NOT_FINISHED，磁盘此时可能已安装新文件。
 * @pre 当前介质已绑定，外层已排除并发写入与卸载。
 * @note 成功更新来源为正式文件，保留已消费状态。失败保留恢复现场，禁止在同一失效对象重试。
 */
esp_err_t app_config_stage_repair(app_config_stage_t *stage);
/**
 * @brief 重新校验后把配置所有权一次转移给目标。
 * @param[in,out] stage 未失效且未消费的暂存对象，成功后标记已消费。
 * @param[in,out] out_config 与暂存内部配置不同的有效目标，须清零或初始化；成功释放并替换旧内容。
 * @return 成功返回 ESP_OK；非法参数、重复消费、文件变化及读取失败返回错误，失败保留目标。
 * @pre 当前介质已绑定，并满足 stage_open 的互斥与挂载核对约束。
 * @note 只转移内存，不写文件；逐字节快照继续由 stage 持有，供后续 validate 使用。
 */
esp_err_t app_config_stage_take(app_config_stage_t *stage, app_config_t *out_config);
/**
 * @brief 擦除文件快照及未转移凭据，释放暂存对象和文件系统端口。
 * @param[in,out] stage 可为 NULL，非 NULL 时消费其所有权。
 * @post 所有内部借用失效；已 take 的目标配置继续由接收者拥有，磁盘文件保留。
 */
void app_config_stage_destroy(app_config_stage_t *stage);
/**
 * @brief 校验完整 JSON 后执行 prepare 和 commit_prepared 的保存流程。
 * @param[in,out] config 已清零或初始化的当前配置，安装确认后替换，结果不确定时可能锁定后续保存。
 * @param[in] path 有效目标路径，本次调用借用。
 * @param[in] json_text 借用的完整 NUL 结尾配置，拒绝解码 NUL 与新明文凭据。
 * @return 校验或提交阶段的错误码，提交结果及部分成功语义见 commit_prepared。
 * @note 外层须串行化配置操作并独占目标介质；临时解析对象始终由本调用清理。
 */
esp_err_t app_config_save(app_config_t *config, const char *path, const char *json_text);
/**
 * @brief 在独立临时快照完成解析、凭据解析和校验后替换 prepared。
 * @param[in,out] prepared 有效且已清零或初始化的目标，失败保留旧内容。
 * @param[in] json_text 借用的完整 NUL 结尾 JSON，可借用 prepared 的 raw_json；拒绝解码 NUL 和新明文凭据。
 * @return 成功返回 ESP_OK，解析或语义无效返回 ESP_ERR_INVALID_ARG，凭据解析失败返回状态错误，分配失败可返回 ESP_ERR_NO_MEM。
 * @note 无文件写入和目录创建，引用凭据通过默认 secret_store 解析，凭据字段歧义须先修正。
 */
esp_err_t app_config_prepare(app_config_t *prepared, const char *json_text);
/**
 * @brief 从 Web/UI 完整快照保留脱敏凭据并准备更新。
 * @param[in,out] prepared 有效初始化目标，必须与 current 不同，失败保留旧内容。
 * @param[in] current 有效当前快照，提供受信任的既有凭据，调用期间保持稳定。
 * @param[in] json_text 借用的完整 NUL 结尾 JSON，拒绝解码 NUL 和非空新明文凭据。
 * @return 成功返回 ESP_OK，提交格式或明文策略错误返回 ESP_ERR_INVALID_ARG，当前快照无效返回状态错误，分配失败返回 ESP_ERR_NO_MEM 或解析阶段错误。
 * @note 占位符或省略凭据对保留旧值，显式空明文清空凭据，非空引用切换到引用。
 * 当前和提交快照均须通过局部凭据唯一性校验，拒绝同名及大小写重复的值、引用和承载容器。
 * 只在受信任的合并结果中允许当前已有明文，调用不修改 current 或磁盘。
 */
esp_err_t app_config_prepare_update(
    app_config_t *prepared,
    const app_config_t *current,
    const char *json_text
);
/**
 * @brief 比较两份 JSON，判断是否含热更新白名单以外的变更。
 * @param[in] current 借用的已加载快照，可为 NULL。
 * @param[in] prepared 借用的已加载快照，可为 NULL。
 * @return 存在需重启字段、未加载或解析失败返回 true，只有白名单内变更返回 false。
 * @note 此函数只分类，不执行重启或热更新；并发修改须由外层排除。
 */
bool app_config_requires_restart(const app_config_t *current, const app_config_t *prepared);
/**
 * @brief 安装已准备的配置，按目标文件可见结果转移内存所有权。
 * @param[in,out] current 有效当前配置，必须与 prepared 不同；已锁定保存时拒绝提交。
 * @param[in,out] prepared 有效且 loaded 的独立快照，确认安装后转移给 current 并清零。
 * @param[in] path 有效目标文件路径，本次调用借用。
 * @return ESP_OK 表示同步完成。ESP_ERR_NOT_FINISHED 表示目标已验证、current 已接纳且 prepared 已消费，但耐久性未确认。
 * ESP_FAIL 表示未确认安装，保留两份快照，磁盘可能已变更。参数或保存锁定返回对应错误。
 * 将写入的规范化 JSON 超过启动读取的 1 MiB 上限时返回 ESP_ERR_INVALID_ARG，保留两份快照且不访问文件系统。
 * @note 既有 .tmp 或写入后的不确定结果设置 current.updates_blocked，须检查介质并重启重新加载后再保存。
 * 目标已接纳时不得回滚内存。外层须先完成热更新协调并独占介质，本函数无内部锁。
 */
esp_err_t app_config_commit_prepared(
    app_config_t *current,
    app_config_t *prepared,
    const char *path
);
/**
 * @brief 擦除凭据和 JSON，释放全部配置拥有的资源。
 * @param[in,out] config 已清零或初始化的配置，可为 NULL。
 * @post 非 NULL 目标清零，所有字段借用失效，重复销毁清零目标安全。
 * @pre 外层已停止并排空使用该快照的组件与访问。
 */
void app_config_destroy(app_config_t *config);
/**
 * @brief 借用当前 SDK 运行时配置。
 * @param[in] config 可为 NULL，借用期间保持稳定。
 * @return 已加载时返回字段地址，否则返回 NULL，借用止于配置替换或销毁。
 */
const cc_config_t *app_config_runtime(const app_config_t *config);
/**
 * @brief 借用当前 IO 配置。
 * @param[in] config 可为 NULL，借用期间保持稳定。
 * @return 已加载时返回字段地址，否则返回 NULL，借用止于配置替换或销毁。
 */
const app_runtime_io_config_t *app_config_io(const app_config_t *config);
/**
 * @brief 借用当前网络配置。
 * @param[in] config 可为 NULL，借用期间保持稳定。
 * @return 已加载时返回字段地址，否则返回 NULL，借用止于配置替换或销毁。
 */
const network_config_t *app_config_network(const app_config_t *config);
/**
 * @brief 借用保存的规范化 JSON 文本。
 * @param[in] config 可为 NULL，借用期间保持稳定。
 * @return raw_json 非空时返回该字段，否则返回静态 "{}"；字段借用止于替换或销毁。
 * @note 文本可能包含明文凭据，向外提供时须由调用方脱敏。
 */
const char *app_config_json(const app_config_t *config);
/**
 * @brief 查询当前思考模式开关。
 * @param[in] config 可为 NULL 的稳定快照。
 * @return 配置已加载且 thinking_mode 非零返回 true，其他情况返回 false。
 */
bool app_config_runtime_thinking(const app_config_t *config);
