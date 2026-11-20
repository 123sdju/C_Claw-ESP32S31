#include "app_config.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cc/util/cc_json.h"
#include "cc/internal/cc_json_syntax_check.h"
#include "cc/app/cc_memory_extractor.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_secret_store.h"
#include "esp_log.h"

static const char *TAG = "app_config";

/**
 * @brief 经 .tmp 写入、同步和替换安装文本，分别报告可见安装与不确定状态。
 * @param[in] path 有效目标路径，本次调用借用。
 * @param[in] text 借用文本，NULL 按空串写入。
 * @param[out] installed 有效槽，先置 false，替换成功或错误后读回完全一致时置 true。
 * @param[out] uncertain 有效槽，先置 false，既有 .tmp 或开始写入后的失败置 true。
 * @return 全部必需同步完成返回 true；失败返回 false，installed 仍可能为 true。
 * @note 每次获取并释放默认文件系统；保留恢复文件，调用者负责独占目标与挂载寿命。
 */
static bool write_text_file_atomic(const char *path, const char *text, bool *installed, bool *uncertain)
{
    *installed = false;
    *uncertain = false;
    cc_filesystem_t fs;
    memset(&fs, 0, sizeof(fs));
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK || !fs.vtable || !fs.vtable->write_text ||
        !fs.vtable->atomic_replace || !fs.vtable->sync_file || !fs.vtable->stat || !fs.vtable->read_text) {
        cc_result_free(&rc);
        if (fs.vtable && fs.vtable->destroy) fs.vtable->destroy(fs.self);
        return false;
    }
    size_t path_len = strlen(path);
    if (path_len > SIZE_MAX - 5) {
        if (fs.vtable->destroy) fs.vtable->destroy(fs.self);
        return false;
    }
    char *tmp = malloc(path_len + 5);
    if (!tmp) {
        if (fs.vtable->destroy) fs.vtable->destroy(fs.self);
        return false;
    }
    memcpy(tmp, path, path_len);
    memcpy(tmp + path_len, ".tmp", 5);
    const char *stage = "inspect_temporary";
    bool temporary_started = false;
    cc_filesystem_stat_t temporary_info = {.size = sizeof(temporary_info)};
    rc = fs.vtable->stat(fs.self, tmp, &temporary_info);
    if (rc.code == CC_OK && temporary_info.type != CC_FS_ENTRY_MISSING) {
        *uncertain = true;
        rc = cc_result_error(CC_ERR_STORAGE, "Configuration temporary file requires inspection");
    }
    if (rc.code == CC_OK) {
        stage = "write_temporary";
        temporary_started = true;
        rc = fs.vtable->write_text(fs.self, tmp, text ? text : "");
    }
    if (rc.code == CC_OK) {
        stage = "sync_temporary";
        rc = fs.vtable->sync_file(fs.self, tmp);
    }
    if (rc.code == CC_OK) {
        stage = "replace_destination";
        rc = fs.vtable->atomic_replace(fs.self, tmp, path);
        *installed = rc.code == CC_OK;
        if (rc.code != CC_OK) {
            *uncertain = true;
            char *visible = NULL;
            cc_result_t verify = fs.vtable->read_text(fs.self, path, &visible);
            *installed = verify.code == CC_OK && visible && strcmp(visible, text ? text : "") == 0;
            if (visible) cc_secret_zero(visible, strlen(visible));
            free(visible);
            cc_result_free(&verify);
        }
    }
    if (rc.code == CC_OK && fs.vtable->sync_dir) {
        stage = "sync_directory";
        char *parent = strdup(path);
        if (!parent) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy config parent path");
        else {
            char *slash = strrchr(parent, '/');
            if (slash == parent) slash[1] = '\0';
            else if (slash) *slash = '\0';
            cc_result_t sync_rc = fs.vtable->sync_dir(fs.self, slash && parent[0] ? parent : ".");
            if (sync_rc.code != CC_OK && sync_rc.code != CC_ERR_UNSUPPORTED) rc = sync_rc;
            else cc_result_free(&sync_rc);
            free(parent);
        }
    }
    bool ok = rc.code == CC_OK;
    if (!ok && temporary_started) *uncertain = true;
    if (!ok) {
        ESP_LOGE(TAG, "config save failed stage=%s code=%d detail=%s",
                 stage, (int)rc.code, rc.message ? rc.message : "unavailable");
    }
    cc_result_free(&rc);
    free(tmp);
    if (fs.vtable->destroy) fs.vtable->destroy(fs.self);
    return ok;
}

/**
 * @brief 先复制新字符串，再释放字段的旧拥有值。
 * @param[in,out] field 拥有字符串的槽，可为 NULL。
 * @param[in] value 借用新值，可与旧值重叠，NULL 表示保留。
 * @return 成功或 NULL 参数跳过返回 true，分配失败返回 false 并保留旧值。
 */
static bool replace_string(char **field, const char *value)
{
    if (!field || !value) {
        return true;
    }
    char *copy = strdup(value);
    if (!copy) {
        return false;
    }
    free(*field);
    *field = copy;
    return true;
}

/**
 * @brief 释放列表拥有的各字符串及指针数组。
 * @param[in,out] list 已初始化列表，可为 NULL。
 * @post 非 NULL 列表归零，原有元素借用失效。
 */
static void free_config_string_list(cc_config_string_list_t *list)
{
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/**
 * @brief 深拷贝字符串并追加到拥有列表。
 * @param[in,out] list 初始化列表，NULL 时跳过。
 * @param[in] value 本次调用借用的新元素，NULL 时跳过。
 * @return 追加或跳过返回 true，分配失败返回 false 并保留原列表。
 */
static bool append_config_string(cc_config_string_list_t *list, const char *value)
{
    if (!list || !value) {
        return true;
    }
    char *copy = strdup(value);
    if (!copy) {
        return false;
    }
    char **next = realloc(list->items, (list->count + 1) * sizeof(char *));
    if (!next) {
        free(copy);
        return false;
    }
    list->items = next;
    list->items[list->count++] = copy;
    return true;
}

/**
 * @brief 清空旧列表，复制 JSON 数组中的字符串元素并跳过其他类型。
 * @param[in] array 借用数组，NULL 或非数组生成空列表。
 * @param[in,out] out 已初始化目标，不能为空。
 * @return 完成返回 true，目标无效或分配失败返回 false；失败保留已复制部分供统一清理。
 */
static bool parse_string_array(cc_json_value_t *array, cc_config_string_list_t *out)
{
    if (!out) {
        return false;
    }
    free_config_string_list(out);
    if (!array || !cc_json_is_array(array)) {
        return true;
    }
    int count = cc_json_array_size(array);
    for (int i = 0; i < count; i++) {
        const char *value = cc_json_string_value(cc_json_array_get(array, i));
        if (value && !append_config_string(out, value)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 仅在键存在时用其数组内容替换字符串列表。
 * @param[in] obj 借用父对象，NULL 时保留目标。
 * @param[in] key 有效键名，本次调用借用。
 * @param[in,out] out 初始化目标列表，键存在时必须有效。
 * @return 字段缺失或解析完成返回 true，目标无效或分配失败返回 false；部分结果由调用者清理。
 */
static bool parse_optional_string_array(cc_json_value_t *obj, const char *key, cc_config_string_list_t *out)
{
    if (!obj) {
        return true;
    }
    cc_json_value_t *array = cc_json_object_get(obj, key);
    return !array || parse_string_array(array, out);
}

/**
 * @brief 清空旧列表后逐项深拷贝静态默认值。
 * @param[in,out] out 有效初始化列表。
 * @param[in] items 至少含 count 项的借用数组，NULL 元素跳过。
 * @param[in] count 输入元素数量。
 * @return 完成返回 true，目标无效或分配失败返回 false，已复制部分仍归 out 拥有。
 */
static bool set_string_list(cc_config_string_list_t *out, const char *const *items, size_t count)
{
    if (!out) {
        return false;
    }
    free_config_string_list(out);
    for (size_t i = 0; i < count; i++) {
        if (!append_config_string(out, items[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 释放 IO 字符串与全部事件路由。
 * @param[in,out] io 已清零或初始化的配置，可为 NULL。
 * @post 非 NULL 配置归零，其字段借用失效。
 */
static void app_runtime_io_config_destroy(app_runtime_io_config_t *io)
{
    if (!io) {
        return;
    }
    free(io->session_id);
    free(io->serial.log_level);
    free(io->usb_log.dir);
    free(io->usb_log.level);
    free_config_string_list(&io->routes.log);
    free_config_string_list(&io->routes.chat);
    free_config_string_list(&io->routes.thinking);
    free_config_string_list(&io->routes.tool);
    free_config_string_list(&io->routes.status);
    free_config_string_list(&io->routes.error);
    free_config_string_list(&io->routes.profile);
    memset(io, 0, sizeof(*io));
}

/**
 * @brief 释放性能剖析级别字符串。
 * @param[in,out] profile 已清零或初始化的配置，可为 NULL。
 * @post 非 NULL 配置归零，原 level 借用失效。
 */
static void app_profile_config_destroy(app_profile_config_t *profile)
{
    if (!profile) {
        return;
    }
    free(profile->level);
    memset(profile, 0, sizeof(*profile));
}

/**
 * @brief 建立设备队列、通道、日志及事件路由默认配置。
 * @param[out] io 有效空目标，直接归零初始化，不接管其旧资源。
 * @return 成功返回 CC_OK，NULL 返回参数错误，分配失败清理并归零后返回 OOM。
 */
static cc_result_t app_runtime_io_config_load_default(app_runtime_io_config_t *io)
{
    if (!io) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null app io config");
    }
    memset(io, 0, sizeof(*io));
    io->queue_depth = 8;
    io->max_input_bytes = 1024;
    io->max_pending_operations = 64;
    io->serial.enabled = 1;
    io->web.enabled = 1;
    io->web.port = 80;
    io->web.event_limit = 120;
    io->display.enabled = 1;
    io->usb_log.enabled = 1;
    io->usb_log.max_bytes = 512 * 1024;
    io->usb_log.max_files = 20;

    const char *log_route[] = {"serial", "web", "usb_log"};
    const char *chat_route[] = {"serial", "web", "display"};
    const char *thinking_route[] = {"serial", "web", "display", "usb_log"};
    const char *tool_route[] = {"serial", "web", "display"};
    const char *status_route[] = {"serial", "web", "display"};
    const char *error_route[] = {"serial", "web", "display", "usb_log"};
    const char *profile_route[] = {"web", "usb_log"};

    if (!replace_string(&io->session_id, "esp32s31-main") ||
        !replace_string(&io->serial.log_level, "debug") ||
        !replace_string(&io->usb_log.dir, "/usb0/cclaw/logs") ||
        !replace_string(&io->usb_log.level, "debug") ||
        !set_string_list(&io->routes.log, log_route, 3) ||
        !set_string_list(&io->routes.chat, chat_route, 3) ||
        !set_string_list(&io->routes.thinking, thinking_route, 4) ||
        !set_string_list(&io->routes.tool, tool_route, 3) ||
        !set_string_list(&io->routes.status, status_route, 3) ||
        !set_string_list(&io->routes.error, error_route, 4) ||
        !set_string_list(&io->routes.profile, profile_route, 2)) {
        app_runtime_io_config_destroy(io);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate app io defaults");
    }
    return cc_result_ok();
}

/**
 * @brief 检查配置允许的六种小写日志级别。
 * @param[in] level 借用级别字符串，NULL 按 info 检查。
 * @return trace/debug/info/warn/error/fatal 返回 true，其他返回 false。
 */
static bool app_event_log_level_known(const char *level)
{
    const char *l = level ? level : "info";
    return strcmp(l, "trace") == 0 || strcmp(l, "debug") == 0 ||
           strcmp(l, "info") == 0 || strcmp(l, "warn") == 0 ||
           strcmp(l, "error") == 0 || strcmp(l, "fatal") == 0;
}

/**
 * @brief 将已知日志级别转换为从 trace 到 fatal 的递增等级。
 * @param[in] level 借用级别字符串，可为 NULL，warning 与 warn 同级。
 * @param[in] fallback NULL 或未知名称的返回值。
 * @return 已知级别返回 0 至 5，否则返回 fallback。
 */
static int app_event_log_level_value(const char *level, int fallback)
{
    const char *l = level ? level : "";
    if (strcmp(l, "trace") == 0) return 0;
    if (strcmp(l, "debug") == 0) return 1;
    if (strcmp(l, "info") == 0) return 2;
    if (strcmp(l, "warn") == 0 || strcmp(l, "warning") == 0) return 3;
    if (strcmp(l, "error") == 0) return 4;
    if (strcmp(l, "fatal") == 0) return 5;
    return fallback;
}

/**
 * @brief 检查设备支持的事件接收端名称。
 * @param[in] sink 借用名称，可为 NULL。
 * @return serial/web/display/usb_log 返回 true，NULL 或其他名称返回 false。
 */
static bool app_event_sink_known(const char *sink)
{
    return sink &&
           (strcmp(sink, "serial") == 0 ||
            strcmp(sink, "web") == 0 ||
            strcmp(sink, "display") == 0 ||
            strcmp(sink, "usb_log") == 0);
}

/**
 * @brief 检查一个事件路由非空且所有接收端均受支持。
 * @param[in] name 有效借用路由名，用于构造错误信息。
 * @param[in] route 借用列表，可为 NULL；非空列表须满足内部数组一致性。
 * @return 合法返回 CC_OK，否则返回拥有诊断信息的参数错误，由调用者释放结果。
 */
static cc_result_t validate_event_route(const char *name, const cc_config_string_list_t *route)
{
    if (!route || route->count == 0) {
        return cc_result_errf(CC_ERR_INVALID_ARGUMENT, "events.routes.%s must contain at least one sink", name);
    }
    for (size_t i = 0; i < route->count; i++) {
        if (!app_event_sink_known(route->items[i])) {
            return cc_result_errf(CC_ERR_INVALID_ARGUMENT,
                                  "events.routes.%s has unknown sink '%s'",
                                  name,
                                  route->items[i] ? route->items[i] : "");
        }
    }
    return cc_result_ok();
}

/**
 * @brief 校验会话、正值预算、端口范围、日志级别与全部事件路由。
 * @param[in] io 借用 IO 配置，NULL 返回参数错误。
 * @return 合法返回 CC_OK，否则返回首个参数错误，结果由调用者释放。
 */
static cc_result_t app_runtime_io_config_validate(const app_runtime_io_config_t *io)
{
    if (!io) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null app io config");
    }
    if (!io->session_id || !io->session_id[0]) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "runtime.session_id must be non-empty");
    }
    if (io->queue_depth <= 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "runtime.queue_depth must be positive");
    }
    if (io->max_input_bytes <= 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "runtime.max_input_bytes must be positive");
    }
    if (io->max_pending_operations <= 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "runtime.max_pending_operations must be positive");
    }
    if (!app_event_log_level_known(io->serial.log_level)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "events.serial.level is invalid");
    }
    if (io->web.port < 0 || io->web.port > 65535) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "runtime.web_port must be 0..65535");
    }
    if (io->web.event_limit <= 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "runtime.event_limit must be positive");
    }
    if (!io->usb_log.dir || !io->usb_log.dir[0]) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "events.usb_log.dir must be non-empty");
    }
    if (io->usb_log.max_bytes == 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "events.usb_log.max_bytes must be positive");
    }
    if (io->usb_log.max_files < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "events.usb_log.max_files must be non-negative");
    }
    if (!app_event_log_level_known(io->usb_log.level)) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "events.usb_log.level is invalid");
    }
    cc_result_t rc = validate_event_route("log", &io->routes.log);
    if (rc.code != CC_OK) return rc;
    rc = validate_event_route("chat", &io->routes.chat);
    if (rc.code != CC_OK) return rc;
    rc = validate_event_route("thinking", &io->routes.thinking);
    if (rc.code != CC_OK) return rc;
    rc = validate_event_route("tool", &io->routes.tool);
    if (rc.code != CC_OK) return rc;
    rc = validate_event_route("status", &io->routes.status);
    if (rc.code != CC_OK) return rc;
    rc = validate_event_route("error", &io->routes.error);
    if (rc.code != CC_OK) return rc;
    return validate_event_route("profile", &io->routes.profile);
}

/**
 * @brief 从 tools.enabled 重建独立拥有的兼容工具数组。
 * @param[in,out] config 有效 SDK 配置，先释放旧缓存，再逐条记录新缓存拥有数量。
 * @return 完成返回 true，NULL 或分配失败返回 false；部分缓存仍可由 cc_config_destroy 完整释放。
 */
static bool sync_enabled_tools_cache(cc_config_t *config)
{
    if (!config) {
        return false;
    }
    for (size_t i = 0; i < config->enabled_tools_count; i++) {
        free(config->enabled_tools[i]);
    }
    free(config->enabled_tools);
    config->enabled_tools = NULL;
    config->enabled_tools_count = 0;

    if (config->tools.enabled.count == 0) {
        return true;
    }
    config->enabled_tools = calloc(config->tools.enabled.count, sizeof(char *));
    if (!config->enabled_tools) {
        return false;
    }
    for (size_t i = 0; i < config->tools.enabled.count; i++) {
        config->enabled_tools[i] = strdup(config->tools.enabled.items[i]);
        if (!config->enabled_tools[i]) {
            return false;
        }
        config->enabled_tools_count++;
    }
    return true;
}

/**
 * @brief 按 SDK 对象查找规则获取对象类型的子节点。
 * @param[in] root 借用父节点，可为 NULL。
 * @param[in] key 有效借用键名，大小写按 SDK 的非敏感查找处理。
 * @return 借用子对象，缺失或类型不符返回 NULL，借用止于所属树修改或销毁。
 */
static cc_json_value_t *object_at(cc_json_value_t *root, const char *key)
{
    cc_json_value_t *value = cc_json_object_get(root, key);
    return value && cc_json_is_object(value) ? value : NULL;
}

/**
 * @brief 获取字符串类型字段的借用内容。
 * @param[in] obj 借用父节点，可为 NULL。
 * @param[in] key 有效借用键名。
 * @return 字符串借用或 NULL，借用止于所属节点替换或树销毁。
 */
static const char *string_at(cc_json_value_t *obj, const char *key)
{
    return cc_json_string_value(cc_json_object_get(obj, key));
}

/**
 * @brief 读取兼容字符串、布尔和数字形式的开关。
 * @param[in] obj 借用对象，可为 NULL。
 * @param[in] key 有效借用键名。
 * @param[in] fallback 缺失或其他类型的默认值。
 * @return 字符串仅 true/1/on/yes 为真，其他字符串为假；数字按 SDK 整数非零判真。
 */
static bool bool_at(cc_json_value_t *obj, const char *key, bool fallback)
{
    cc_json_value_t *value = cc_json_object_get(obj, key);
    if (!value) {
        return fallback;
    }
    const char *text = cc_json_string_value(value);
    if (text) {
        return strcmp(text, "true") == 0 || strcmp(text, "1") == 0 ||
               strcmp(text, "on") == 0 || strcmp(text, "yes") == 0;
    }
    if (cc_json_is_bool(value)) {
        return cc_json_bool_value(value) != 0;
    }
    if (cc_json_is_number(value)) {
        return cc_json_int_value(value) != 0;
    }
    return fallback;
}

/**
 * @brief 按 SDK 整数转换规则读取字段。
 * @param[in] obj 借用对象，可为 NULL。
 * @param[in] key 有效借用键名。
 * @param[in] fallback 字段缺失时的默认值。
 * @return 存在时返回 SDK 转换值，类型不符通常为零；具体范围由各配置校验器检查。
 */
static int int_at(cc_json_value_t *obj, const char *key, int fallback)
{
    cc_json_value_t *value = cc_json_object_get(obj, key);
    return value ? cc_json_int_value(value) : fallback;
}

/**
 * @brief 读取 SDK 浮点字段值。
 * @param[in] obj 借用对象，可为 NULL。
 * @param[in] key 有效借用键名。
 * @param[in] fallback 字段缺失时的默认值。
 * @return 存在时返回 SDK 浮点访问值，类型不符为零，此处不检查范围。
 */
static double number_at(cc_json_value_t *obj, const char *key, double fallback)
{
    cc_json_value_t *value = cc_json_object_get(obj, key);
    return value ? cc_json_number_value(value) : fallback;
}

/**
 * @brief 优先查找 snake_case 字段，缺失时再查 camelCase 别名。
 * @param[in] obj 借用对象，NULL 返回 NULL。
 * @param[in] snake_key 有效首选键名。
 * @param[in] camel_key 可为 NULL 的后备键名。
 * @return 借用节点或 NULL，首选字段即使类型不符也优先，寿命受所属树约束。
 */
static cc_json_value_t *value_at_alias(cc_json_value_t *obj, const char *snake_key, const char *camel_key)
{
    if (!obj) {
        return NULL;
    }
    cc_json_value_t *value = cc_json_object_get(obj, snake_key);
    return value || !camel_key ? value : cc_json_object_get(obj, camel_key);
}

/**
 * @brief 从首选键或后备别名读取字符串。
 * @param[in] obj 借用对象，可为 NULL。
 * @param[in] snake_key 有效首选键名。
 * @param[in] camel_key 可为 NULL 的后备键名。
 * @return 借用字符串或 NULL，寿命受所属节点约束，首选非字符串不会转用别名。
 */
static const char *string_at_alias(cc_json_value_t *obj, const char *snake_key, const char *camel_key)
{
    cc_json_value_t *value = value_at_alias(obj, snake_key, camel_key);
    return cc_json_string_value(value);
}

/**
 * @brief 从兼容键读取可安全转换为 size_t 的非负有限数值。
 * @param[in] obj 借用对象，可为 NULL。
 * @param[in] snake_key 有效首选键名。
 * @param[in] camel_key 可为 NULL 的后备键名。
 * @param[in,out] out_value 有效现值槽，缺失或失败保留，成功按截去小数的值更新。
 * @return 缺失或有效返回 CC_OK，类型、有限性或范围不符返回参数错误。
 */
static cc_result_t size_at_alias(cc_json_value_t *obj,
                                 const char *snake_key,
                                 const char *camel_key,
                                 size_t *out_value)
{
    cc_json_value_t *value = value_at_alias(obj, snake_key, camel_key);
    if (!value) {
        return cc_result_ok();
    }
    double number = cc_json_number_value(value);
    if (!cc_json_is_number(value) || !isfinite(number) || number < 0 ||
        number >= ldexp(1.0, (int)(sizeof(size_t) * CHAR_BIT))) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Configuration size is outside size_t range");
    }
    *out_value = (size_t)number;
    return cc_result_ok();
}

/**
 * @brief 为缺失或空字符串选择后备指针。
 * @param[in] value 可为 NULL 的借用候选。
 * @param[in] fallback 可为 NULL 的借用后备值。
 * @return 非空 value 或 fallback，所有权与原输入一致。
 */
static const char *nonempty_or(const char *value, const char *fallback)
{
    return value && value[0] ? value : fallback;
}

/**
 * @brief 选择非空值或默认值后复制到拥有字段。
 * @param[in,out] field 拥有字符串的槽，可为 NULL。
 * @param[in] value 可为 NULL 的借用候选。
 * @param[in] fallback 可为 NULL 的借用默认值，最终为 NULL 时保留旧值。
 * @return 复制或跳过返回 true，分配失败返回 false 并保留旧值。
 */
static bool replace_string_default(char **field, const char *value, const char *fallback)
{
    return replace_string(field, nonempty_or(value, fallback));
}

/**
 * @brief 精确分配完整派生路径，必要时在两个部分间补斜线。
 * @param[in,out] field 有效拥有字符串槽，成功后释放旧值。
 * @param[in] base 借用根路径，NULL 或空串使用空根并补前导斜线。
 * @param[in] leaf 借用末级路径，NULL 按空串处理。
 * @return 成功返回 true，长度溢出或分配失败返回 false 并保留旧值。
 */
static bool replace_joined_path(char **field, const char *base, const char *leaf)
{
    const char *safe_base = nonempty_or(base, "");
    const char *safe_leaf = leaf ? leaf : "";
    size_t base_length = strlen(safe_base);
    size_t leaf_length = strlen(safe_leaf);
    size_t separator_length = base_length && safe_base[base_length - 1] == '/' ? 0 : 1;
    if (base_length > SIZE_MAX - separator_length - 1 ||
        leaf_length > SIZE_MAX - base_length - separator_length - 1) return false;
    char *path = malloc(base_length + separator_length + leaf_length + 1);
    if (!path) return false;
    memcpy(path, safe_base, base_length);
    if (separator_length) path[base_length] = '/';
    memcpy(path + base_length + separator_length, safe_leaf, leaf_length + 1);
    free(*field);
    *field = path;
    return true;
}

/**
 * @brief 检查单字节 ASCII 字母。
 * @param[in] ch 待检查字节。
 * @return A 至 Z 或 a 至 z 返回 true，其他返回 false。
 */
static bool is_ascii_letter(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

/**
 * @brief 检测字母盘符前缀或反斜线形式的主机路径。
 * @param[in] path 借用路径，NULL 或空串允许。
 * @return 字母后接冒号或包含反斜线返回 true，其他返回 false。
 */
static bool is_host_path(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    return (is_ascii_letter(path[0]) && path[1] == ':') || strchr(path, '\\') != NULL;
}

/**
 * @brief 拒绝存储、工作区、提示文件与日志目录中的主机路径形式。
 * @param[in] runtime 借用 SDK 配置，可为 NULL 时跳过其字段。
 * @param[in] io 借用 IO 配置，可为 NULL 时跳过日志目录。
 * @return 未发现主机形式返回 CC_OK，否则返回参数错误，结果由调用者释放。
 * @note 此处只检查路径形式，目录可达性、挂载身份与访问范围由各存储所有者验证。
 */
static cc_result_t validate_device_paths(const cc_config_t *runtime, const app_runtime_io_config_t *io)
{
    const struct {
        const char *name;
        const char *path;
    } paths[] = {
        {"storage.root", runtime ? runtime->data_dir : NULL},
        {"storage.sessions", runtime ? runtime->storage_path : NULL},
        {"workspace.root", runtime ? runtime->workspace_path : NULL},
        {"storage.memory", runtime ? runtime->memory_path : NULL},
        {"events.usb_log.dir", io ? io->usb_log.dir : NULL},
        {"system.soul_file", runtime ? runtime->soul_file : NULL},
        {"system.user_file", runtime ? runtime->user_file : NULL},
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (is_host_path(paths[i].path)) {
            return cc_result_errf(CC_ERR_INVALID_ARGUMENT,
                                  "%s must be an ESP32 path, got host path: %s",
                                  paths[i].name,
                                  paths[i].path);
        }
    }
    return cc_result_ok();
}

/**
 * @brief 将文本按字节复制到定长字段。
 * @param[out] dst 可写目标，NULL 时跳过。
 * @param[in] dst_size 目标容量，含 NUL 字节，零时跳过。
 * @param[in] src 与目标不重叠的借用字符串，NULL 按空串处理。
 * @post 有效非零容量目标保证 NUL 结尾，过长内容按字节截断。
 */
static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

/** @brief 本次解析拥有的独立凭据缓冲区，转移或擦除后统一归零。 */
typedef struct app_resolved_secrets {
    char *model_api_key; /**< 待转移到 runtime 的 API 凭据。 */
    char *sta_password; /**< 待复制到定长 STA 字段的凭据。 */
    char *ap_password; /**< 待复制到定长 AP 字段的凭据。 */
} app_resolved_secrets_t;

/**
 * @brief 擦除并释放本次解析仍拥有的凭据。
 * @param[in,out] secrets 初始化目标，可为 NULL。
 * @post 非 NULL 目标归零，已通过置 NULL 转移的字段不再释放。
 */
static void resolved_secrets_destroy(app_resolved_secrets_t *secrets)
{
    if (!secrets) return;
    cc_secret_free(secrets->model_api_key);
    cc_secret_free(secrets->sta_password);
    cc_secret_free(secrets->ap_password);
    memset(secrets, 0, sizeof(*secrets));
}

/**
 * @brief 为单个凭据选择获准的 USB 明文副本或从引用存储读取拥有值。
 * @param[in] store 借用凭据端口，可为 NULL，使用引用时须支持 get。
 * @param[in] object 借用配置对象，NULL 时跳过。
 * @param[in] value_field 有效明文字段名。
 * @param[in] reference_field 有效引用字段名。
 * @param[in,out] out_secret 有效空槽，成功获得凭据时接收须用 cc_secret_free 释放的缓冲区，跳过时保留。
 * @param[in] allow_usb_plaintext 是否允许非空 USB 明文，明文 *** 按占位处理。
 * @return 成功或无凭据返回 CC_OK，权限、分配或端口失败返回错误，输出遵循端口 get 的失败契约。
 */
static cc_result_t normalize_secret_field(
    cc_secret_store_t *store,
    cc_json_value_t *object,
    const char *value_field,
    const char *reference_field,
    char **out_secret,
    bool allow_usb_plaintext
)
{
    if (!object) return cc_result_ok();
    const char *plain = string_at(object, value_field);
    const char *reference = string_at(object, reference_field);
    if (plain && plain[0] && strcmp(plain, "***") != 0) {
        if (!allow_usb_plaintext) {
            return cc_result_error(CC_ERR_PERMISSION_DENIED,
                                   "Plaintext secrets are accepted only from the boot USB config");
        }
        size_t secret_len = strlen(plain);
        char *secret = malloc(secret_len + 1);
        if (!secret) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to copy USB config secret");
        }
        memcpy(secret, plain, secret_len + 1);
        *out_secret = secret;
        /* USB 显式明文优先于引用；此处只复制到可擦除缓冲区，不迁移到 NVS。 */
        (void)store;
        return cc_result_ok();
    }
    if (!reference || !reference[0]) return cc_result_ok();
    if (!store || !store->vtable || !store->vtable->get) {
        return cc_result_error(CC_ERR_UNSUPPORTED,
                               "Secret reference requires an available SecretStore");
    }
    return store->vtable->get(store->self, reference, out_secret);
}

/**
 * @brief 顺序解析模型、STA 与 AP 凭据，并在必要时获取默认凭据端口。
 * @param[in] root 有效借用配置树，保持内容不变。
 * @param[out] out 有效空凭据结构，初始化为零，成功拥有解析结果。
 * @param[in] allow_usb_plaintext 是否允许当前受信任 USB 快照中的明文。
 * @return 成功返回 CC_OK；失败擦除已获得凭据并返回参数、策略、分配或端口错误。
 * @note 默认端口仅在此调用内拥有，始终释放；配置树不被改写。
 */
static cc_result_t normalize_and_resolve_secrets(
    cc_json_value_t *root,
    app_resolved_secrets_t *out,
    bool allow_usb_plaintext
)
{
    if (!root || !out) return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid secret normalization");
    memset(out, 0, sizeof(*out));
    cc_json_value_t *model = object_at(root, "model");
    cc_json_value_t *network = object_at(root, "network");
    cc_json_value_t *sta = network ? object_at(network, "sta") : NULL;
    cc_json_value_t *ap = network ? object_at(network, "ap") : NULL;
    const char *model_plain = model ? string_at(model, "api_key") : NULL;
    const char *sta_plain = sta ? string_at(sta, "password") : NULL;
    const char *ap_plain = ap ? string_at(ap, "password") : NULL;
    bool reference_needed =
        ((!model_plain || !model_plain[0] || strcmp(model_plain, "***") == 0) && model &&
         string_at(model, "api_key_ref") && string_at(model, "api_key_ref")[0]) ||
        ((!sta_plain || !sta_plain[0] || strcmp(sta_plain, "***") == 0) && sta &&
         string_at(sta, "password_ref") && string_at(sta, "password_ref")[0]) ||
        ((!ap_plain || !ap_plain[0] || strcmp(ap_plain, "***") == 0) && ap &&
         string_at(ap, "password_ref") && string_at(ap, "password_ref")[0]);

    cc_secret_store_t store;
    memset(&store, 0, sizeof(store));
    bool store_available = false;
    if (reference_needed) {
        cc_result_t store_rc = cc_secret_store_get_default(&store);
        store_available = store_rc.code == CC_OK &&
                          store.vtable &&
                          store.vtable->get;
        if (store_rc.code != CC_OK) {
            if (store.vtable && store.vtable->destroy) store.vtable->destroy(store.self);
            return store_rc;
        }
        cc_result_free(&store_rc);
    }
    cc_result_t rc = normalize_secret_field(store_available ? &store : NULL,
                                            model, "api_key", "api_key_ref",
                                            &out->model_api_key, allow_usb_plaintext);
    if (rc.code == CC_OK) {
        rc = normalize_secret_field(store_available ? &store : NULL,
                                    sta, "password", "password_ref",
                                    &out->sta_password, allow_usb_plaintext);
    }
    if (rc.code == CC_OK) {
        rc = normalize_secret_field(store_available ? &store : NULL,
                                    ap, "password", "password_ref",
                                    &out->ap_password, allow_usb_plaintext);
    }
    if (store.vtable && store.vtable->destroy) store.vtable->destroy(store.self);
    if (rc.code != CC_OK) resolved_secrets_destroy(out);
    return rc;
}

/**
 * @brief 从默认值构建 STA、AP 和 mDNS 定长网络配置。
 * @param[in] root 借用配置树，缺失节使用默认值。
 * @param[out] out 有效目标，完整覆盖，字符串按字段容量截断。
 * @return 始终为 true；超时、重试、信道和连接数无效时回退默认值。
 */
static bool parse_network(cc_json_value_t *root, network_config_t *out)
{
    network_config_t next = {
        .enabled = false,
        .connect_timeout_ms = 20000,
        .max_retry = 5,
        .softap_enabled = false,
        .softap_channel = 6,
        .softap_max_connections = 4,
        .mdns_enabled = true,
    };
    copy_text(next.mdns_hostname, sizeof(next.mdns_hostname), "cclaw");

    cc_json_value_t *network = object_at(root, "network");
    cc_json_value_t *sta = network ? object_at(network, "sta") : NULL;
    cc_json_value_t *ap = network ? object_at(network, "ap") : NULL;
    cc_json_value_t *mdns = network ? object_at(network, "mdns") : NULL;

    if (sta) {
        next.enabled = bool_at(sta, "enabled", next.enabled);
        copy_text(next.ssid, sizeof(next.ssid), string_at(sta, "ssid"));
        copy_text(next.password, sizeof(next.password), string_at(sta, "password"));
        copy_text(next.password_ref, sizeof(next.password_ref), string_at(sta, "password_ref"));
        next.connect_timeout_ms = int_at(sta, "connect_timeout_ms", next.connect_timeout_ms);
        next.max_retry = int_at(sta, "max_retry", next.max_retry);
    }
    if (ap) {
        next.softap_enabled = bool_at(ap, "enabled", next.softap_enabled);
        copy_text(next.softap_ssid, sizeof(next.softap_ssid), string_at(ap, "ssid"));
        copy_text(next.softap_password, sizeof(next.softap_password), string_at(ap, "password"));
        copy_text(next.softap_password_ref, sizeof(next.softap_password_ref),
                  string_at(ap, "password_ref"));
        next.softap_channel = int_at(ap, "channel", next.softap_channel);
        next.softap_max_connections = int_at(ap, "max_connections", next.softap_max_connections);
    }
    if (mdns) {
        next.mdns_enabled = bool_at(mdns, "enabled", next.mdns_enabled);
        copy_text(next.mdns_hostname, sizeof(next.mdns_hostname), string_at(mdns, "hostname"));
        if (!next.mdns_hostname[0]) {
            copy_text(next.mdns_hostname, sizeof(next.mdns_hostname), "cclaw");
        }
    }

    if (next.connect_timeout_ms <= 0) next.connect_timeout_ms = 20000;
    if (next.max_retry <= 0) next.max_retry = 5;
    if (next.softap_channel <= 0 || next.softap_channel > 13) next.softap_channel = 6;
    if (next.softap_max_connections <= 0 || next.softap_max_connections > 10) {
        next.softap_max_connections = 4;
    }
    *out = next;
    return true;
}

/**
 * @brief 用 network.http 覆盖 HTTP 超时、重试、日志等级与持久化跟踪开关。
 * @param[in] root 借用配置树，缺失或非对象节跳过。
 * @param[in,out] out SDK 配置，可为 NULL。
 * @post 已提供的标量覆盖原值，未知日志级别保留；超时单位为毫秒，范围由 SDK 校验。
 */
static void parse_network_http_config(cc_json_value_t *root, cc_config_t *out)
{
    cc_json_value_t *network = object_at(root, "network");
    cc_json_value_t *http = network ? object_at(network, "http") : NULL;
    if (!http || !out) {
        return;
    }

    out->http_connect_timeout_ms =
        int_at(http, "connect_timeout_ms", out->http_connect_timeout_ms);
    out->http_first_byte_timeout_ms =
        int_at(http, "first_byte_timeout_ms", out->http_first_byte_timeout_ms);
    out->http_stream_idle_timeout_ms =
        int_at(http, "stream_idle_timeout_ms", out->http_stream_idle_timeout_ms);
    out->http_retry_count = int_at(http, "retry_count", out->http_retry_count);

    const char *level = string_at(http, "log_level");
    if (level && app_event_log_level_known(level)) {
        out->http_log_level = app_event_log_level_value(level, out->http_log_level);
    }

    cc_json_value_t *trace = object_at(http, "trace");
    if (trace) {
        out->http_trace_persist =
            bool_at(trace, "persist", out->http_trace_persist != 0) ? 1 : 0;
    }
}

/**
 * @brief 覆盖 image、audio、video、file 能力开关。
 * @param[in] obj 借用能力对象，NULL 或非对象时跳过。
 * @param[in,out] flags 能力结构，可为 NULL，缺失字段保留当前值。
 * @post 有效字段按兼容布尔规则转换成零或一。
 */
static void parse_modality_flags(cc_json_value_t *obj, cc_config_modality_flags_t *flags)
{
    if (!obj || !cc_json_is_object(obj) || !flags) {
        return;
    }
    flags->image = bool_at(obj, "image", flags->image != 0) ? 1 : 0;
    flags->audio = bool_at(obj, "audio", flags->audio != 0) ? 1 : 0;
    flags->video = bool_at(obj, "video", flags->video != 0) ? 1 : 0;
    flags->file = bool_at(obj, "file", flags->file != 0) ? 1 : 0;
}

/**
 * @brief 先合并 model.capabilities，再用 multimodal 覆盖能力与限制。
 * @param[in] root 借用配置树。
 * @param[in,out] out 有效 SDK 配置，已有默认值，MIME 列表由其拥有。
 * @return 完成返回 CC_OK，数值范围或类型错误返回参数错误，复制 MIME 失败返回 OOM。
 * @note 数量按项、大小按字节，失败可能已更新部分字段，由临时配置所有者统一清理。
 */
static cc_result_t parse_multimodal_config(cc_json_value_t *root, cc_config_t *out)
{
    cc_json_value_t *model = object_at(root, "model");
    cc_json_value_t *capabilities = model ? object_at(model, "capabilities") : NULL;
    if (capabilities) {
        parse_modality_flags(object_at(capabilities, "input"), &out->multimodal.input);
        parse_modality_flags(object_at(capabilities, "output"), &out->multimodal.output);
    }

    cc_json_value_t *multimodal = object_at(root, "multimodal");
    if (!multimodal) {
        return cc_result_ok();
    }

    parse_modality_flags(object_at(multimodal, "input"), &out->multimodal.input);
    parse_modality_flags(object_at(multimodal, "output"), &out->multimodal.output);

    cc_json_value_t *limits = object_at(multimodal, "limits");
    if (!limits) {
        return cc_result_ok();
    }
    cc_result_t rc = size_at_alias(limits, "max_artifacts", "maxArtifacts",
                                    &out->multimodal.limits.max_artifacts);
    if (rc.code != CC_OK) return rc;
    rc = size_at_alias(limits, "max_artifact_bytes", "maxArtifactBytes",
                       &out->multimodal.limits.max_artifact_bytes);
    if (rc.code != CC_OK) return rc;
    rc = size_at_alias(limits, "max_base64_bytes", "maxBase64Bytes",
                       &out->multimodal.limits.max_base64_bytes);
    if (rc.code != CC_OK) return rc;

    cc_json_value_t *allow_inline = value_at_alias(limits, "allow_inline_base64", "allowInlineBase64");
    if (allow_inline) {
        out->multimodal.limits.allow_inline_base64 =
            bool_at(limits, "allow_inline_base64", out->multimodal.limits.allow_inline_base64 != 0) ? 1 : 0;
        if (!cc_json_object_get(limits, "allow_inline_base64")) {
            out->multimodal.limits.allow_inline_base64 =
                bool_at(limits, "allowInlineBase64", out->multimodal.limits.allow_inline_base64 != 0) ? 1 : 0;
        }
    }

    cc_json_value_t *mimes = value_at_alias(limits, "allowed_mime_prefixes", "allowedMimePrefixes");
    if (mimes && !parse_string_array(mimes, &out->multimodal.limits.allowed_mime_prefixes)) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate multimodal MIME prefixes");
    }
    return cc_result_ok();
}

/**
 * @brief 解析存储后端、完整派生路径和会话分片字节预算。
 * @param[in] root 借用配置树，缺失路径以 /usb0/cclaw 派生默认值。
 * @param[in,out] out 有效 SDK 配置，接收拥有路径与后端字符串。
 * @param[in,out] io 有效 IO 配置，接收拥有日志目录。
 * @return 完成返回 CC_OK，路径分配或长度溢出返回 OOM，分片预算无效返回参数错误。
 * @note 零分片值保留默认值；失败可能已替换部分字段，调用者须清理整个临时配置。
 */
static cc_result_t parse_storage_config(cc_json_value_t *root,
                                        cc_config_t *out,
                                        app_runtime_io_config_t *io)
{
    cc_json_value_t *storage = object_at(root, "storage");
    const char *storage_root = storage ? string_at(storage, "root") : NULL;
    storage_root = nonempty_or(storage_root, "/usb0/cclaw");

    if (!replace_string(&out->data_dir, storage_root) ||
        !replace_string_default(&out->storage_type,
                                storage ? string_at_alias(storage, "session_backend", "sessionBackend") : NULL,
                                "json_segmented") ||
        !replace_string_default(&out->memory_backend,
                                storage ? string_at_alias(storage, "memory_backend", "memoryBackend") : NULL,
                                "json_file")) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate storage config");
    }

    const char *sessions = storage ? string_at(storage, "sessions") : NULL;
    if (sessions && sessions[0]) {
        if (!replace_string(&out->storage_path, sessions)) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate session storage path");
        }
    } else if (!replace_joined_path(&out->storage_path,
                                    storage_root,
                                    out->storage_type && strcmp(out->storage_type, "json_segmented") == 0 ?
                                        "sessions" : "sessions.json")) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate session storage path");
    }

    if (storage) {
        size_t segment_bytes = out->session_segment_bytes;
        cc_result_t rc = size_at_alias(
            storage,
            "session_segment_bytes",
            "sessionSegmentBytes",
            &segment_bytes);
        if (rc.code != CC_OK) return rc;
        if (segment_bytes > 0) {
            out->session_segment_bytes = segment_bytes;
        }
        const char *media_dir = string_at_alias(storage, "session_media_dir", "sessionMediaDir");
        if (media_dir && !replace_string(&out->session_media_dir, media_dir)) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate session media dir");
        }
    }

    const char *memory = storage ? string_at(storage, "memory") : NULL;
    if (memory && memory[0]) {
        if (!replace_string(&out->memory_path, memory)) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory storage path");
        }
    } else if (!replace_joined_path(&out->memory_path, storage_root, "memory.json")) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate memory storage path");
    }

    const char *logs = storage ? string_at(storage, "logs") : NULL;
    if (logs && logs[0]) {
        if (!replace_string(&io->usb_log.dir, logs)) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate USB log path");
        }
    } else if (!replace_joined_path(&io->usb_log.dir, storage_root, "logs")) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate USB log path");
    }

    /* 会话历史和托管长期记忆分别拥有持久数据，设备配置关闭逐轮重复摘要写入。 */
    out->active_memory_enabled=0;
    out->active_memory_write_summary=0;
    return cc_result_ok();
}

/**
 * @brief 覆盖系统提示、soul/user 路径、调试与上下文摘要配置。
 * @param[in] root 借用配置树，system 缺失时保留现值。
 * @param[in,out] out 有效 SDK 配置，拥有复制的提示和路径。
 * @return 完成返回 CC_OK，分配失败返回 OOM；部分替换值由 out 所有者清理，范围由 SDK 校验。
 */
static cc_result_t parse_system_config(cc_json_value_t *root, cc_config_t *out)
{
    cc_json_value_t *system = object_at(root, "system");
    if (!system) {
        return cc_result_ok();
    }

    const char *prompt = string_at(system, "prompt");
    if (!prompt) {
        prompt = string_at(system, "system_prompt");
    }
    if (prompt && !replace_string(&out->system_prompt, prompt)) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate system prompt");
    }
    if (!replace_string_default(&out->soul_file, string_at(system, "soul_file"), "") ||
        !replace_string_default(&out->user_file, string_at(system, "user_file"), "")) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate system file config");
    }

    out->debug_mode = bool_at(system, "debug", out->debug_mode != 0) ? 1 : 0;
    out->context_window_tokens = int_at(system, "context_window_tokens", out->context_window_tokens);
    out->context_compress_threshold =
        number_at(system, "context_compress_threshold", out->context_compress_threshold);
    out->context_keep_recent = int_at(system, "context_keep_recent", out->context_keep_recent);
    out->summary_max_tokens = int_at(system, "summary_max_tokens", out->summary_max_tokens);
    out->summary_temperature = number_at(system, "summary_temperature", out->summary_temperature);
    return cc_result_ok();
}

/**
 * @brief 覆盖事件输出通道、USB 日志字节预算和路由列表。
 * @param[in] root 借用配置树，缺失节或字段保留已有值，已出现日志节的缺省等级使用 debug。
 * @param[in,out] io 有效初始化 IO 配置，拥有新字符串与路由。
 * @return 完成返回 CC_OK，分配失败返回 OOM，字节预算转换失败返回参数错误；部分结果保留供统一清理。
 */
static cc_result_t parse_events_config(cc_json_value_t *root, app_runtime_io_config_t *io)
{
    cc_json_value_t *events = object_at(root, "events");
    if (!events) {
        return cc_result_ok();
    }

    cc_json_value_t *serial = object_at(events, "serial");
    if (serial) {
        io->serial.enabled = bool_at(serial, "enabled", io->serial.enabled != 0) ? 1 : 0;
        if (!replace_string_default(&io->serial.log_level,
                                    string_at(serial, "level"),
                                    "debug")) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate serial event config");
        }
    }

    cc_json_value_t *display = object_at(events, "display");
    if (display) {
        io->display.enabled = bool_at(display, "enabled", io->display.enabled != 0) ? 1 : 0;
    }

    cc_json_value_t *usb_log = object_at(events, "usb_log");
    if (usb_log) {
        io->usb_log.enabled = bool_at(usb_log, "enabled", io->usb_log.enabled != 0) ? 1 : 0;
        cc_result_t rc = size_at_alias(usb_log, "max_bytes", "maxBytes", &io->usb_log.max_bytes);
        if (rc.code != CC_OK) return rc;
        io->usb_log.max_files = int_at(usb_log, "max_files", io->usb_log.max_files);
        if (!replace_string_default(&io->usb_log.level,
                                    string_at(usb_log, "level"),
                                    "debug")) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate USB log level");
        }
        const char *dir = string_at(usb_log, "dir");
        if (dir && !replace_string(&io->usb_log.dir, dir)) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate USB log dir");
        }
    }

    cc_json_value_t *routes = object_at(events, "routes");
    if (routes &&
        (!parse_optional_string_array(routes, "log", &io->routes.log) ||
         !parse_optional_string_array(routes, "chat", &io->routes.chat) ||
         !parse_optional_string_array(routes, "thinking", &io->routes.thinking) ||
         !parse_optional_string_array(routes, "tool", &io->routes.tool) ||
         !parse_optional_string_array(routes, "status", &io->routes.status) ||
         !parse_optional_string_array(routes, "error", &io->routes.error) ||
         !parse_optional_string_array(routes, "profile", &io->routes.profile))) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate event routes");
    }
    return cc_result_ok();
}

/**
 * @brief 初始化禁用、零毫秒间隔和 basic 等级的剖析默认值。
 * @param[out] profile 有效空目标，直接清零，不释放旧资源。
 * @return 成功返回 CC_OK，NULL 返回参数错误，字符串分配失败返回 OOM 且目标仍为零。
 */
static cc_result_t app_profile_config_load_default(app_profile_config_t *profile)
{
    if (!profile) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null profile config");
    }
    memset(profile, 0, sizeof(*profile));
    profile->enabled = 0;
    profile->interval_ms = 0;
    if (!replace_string(&profile->level, "basic")) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate profile defaults");
    }
    return cc_result_ok();
}

/**
 * @brief 初始化并解析剖析开关、毫秒间隔和 basic/trace 等级。
 * @param[in] root 借用配置树，缺失节使用默认值。
 * @param[out] profile 有效空目标，拥有新等级字符串。
 * @return 完成返回 CC_OK，分配或校验失败返回错误，已初始化字段仍须由调用者销毁。
 */
static cc_result_t parse_profile_config(cc_json_value_t *root, app_profile_config_t *profile)
{
    cc_result_t rc = app_profile_config_load_default(profile);
    if (rc.code != CC_OK) {
        return rc;
    }

    cc_json_value_t *node = object_at(root, "profile");
    if (!node) {
        return cc_result_ok();
    }
    profile->enabled = bool_at(node, "enabled", profile->enabled != 0) ? 1 : 0;
    profile->interval_ms = int_at(node, "interval_ms", profile->interval_ms);
    const char *level = string_at(node, "level");
    if (level && !replace_string_default(&profile->level, level, "basic")) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate profile level");
    }
    if (strcmp(profile->level ? profile->level : "basic", "basic") != 0 &&
        strcmp(profile->level ? profile->level : "basic", "trace") != 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "profile.level must be basic or trace");
    }
    if (profile->interval_ms < 0) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "profile.interval_ms must be non-negative");
    }
    return cc_result_ok();
}

/**
 * @brief 用策略对象重建拥有名称的工具并发数与毫秒超时数组。
 * @param[in] policies 借用策略对象，NULL 或非对象时保留原策略。
 * @param[in,out] out 有效 SDK 配置，重建前释放旧数组。
 * @return 完成返回 CC_OK，分配失败返回 OOM；policy_count 只覆盖已拥有名称的条目，供统一清理。
 */
static cc_result_t parse_tool_policies(cc_json_value_t *policies, cc_config_t *out)
{
    if (!policies || !cc_json_is_object(policies)) {
        return cc_result_ok();
    }
    for (size_t i = 0; i < out->tools.policy_count; i++) {
        free(out->tools.policies[i].name);
    }
    free(out->tools.policies);
    out->tools.policies = NULL;
    out->tools.policy_count = 0;

    int count = cc_json_object_size(policies);
    if (count <= 0) {
        return cc_result_ok();
    }
    out->tools.policies = calloc((size_t)count, sizeof(cc_config_tool_policy_t));
    if (!out->tools.policies) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool policies");
    }
    for (int i = 0; i < count; i++) {
        const char *name = cc_json_object_key_at(policies, i);
        cc_json_value_t *policy_json = cc_json_object_value_at(policies, i);
        cc_config_tool_policy_t *policy = &out->tools.policies[out->tools.policy_count];
        policy->name = name ? strdup(name) : NULL;
        if (name && !policy->name) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate tool policy name");
        }
        policy->concurrency = int_at(policy_json, "concurrency", policy->concurrency);
        policy->timeout_ms = int_at(policy_json, "timeout_ms", policy->timeout_ms);
        out->tools.policy_count++;
    }
    return cc_result_ok();
}

/**
 * @brief 解析工具启用列表、兼容缓存、默认毫秒超时及 policies 映射。
 * @param[in] root 借用配置树，缺失 tools 保留现值。
 * @param[in,out] out 有效 SDK 配置，拥有新列表、缓存和策略。
 * @return 完成返回 CC_OK，分配失败返回 OOM；部分结果继续由 out 拥有。
 */
static cc_result_t parse_tools_config(cc_json_value_t *root, cc_config_t *out)
{
    cc_json_value_t *tools = object_at(root, "tools");
    if (!tools) {
        return cc_result_ok();
    }

    cc_json_value_t *enabled = cc_json_object_get(tools, "enabled");
    if (enabled && (!parse_string_array(enabled, &out->tools.enabled) ||
                    !sync_enabled_tools_cache(out))) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate enabled tool list");
    }
    out->tools.default_timeout_ms = int_at(tools, "default_timeout_ms", out->tools.default_timeout_ms);

    cc_result_t rc = parse_tool_policies(object_at(tools, "policies"), out);
    if (rc.code != CC_OK) {
        return rc;
    }
    return cc_result_ok();
}

/**
 * @brief 从 SDK 默认值和设备预算建立运行时与 IO 配置，再顺序覆盖各节并校验。
 * @param[in] root 借用配置对象，整个解析期间保持有效。
 * @param[out] out 有效空 SDK 配置，接收拥有资源与各类预算。
 * @param[out] io 有效空 IO 配置，接收拥有字符串和路由。
 * @return 完成返回 CC_OK，否则返回首个构建或校验错误；两份部分结果均由调用者清理。
 * @note thinking 会强制 stream，超时均为毫秒，各输入和结果容量按字节，初始化不创建文件。
 */
static cc_result_t parse_runtime(cc_json_value_t *root, cc_config_t *out, app_runtime_io_config_t *io)
{
    cc_result_t rc = cc_config_load_default(out);
    if (rc.code != CC_OK) {
        return rc;
    }
    /* 设备预算作为默认值，后续显式 runtime 字段可以覆盖。 */
    out->max_steps = 4;
    out->run_timeout_ms = 180000;
    out->provider_timeout_ms = 120000;
    out->tool_timeout_ms = 10000;
    out->max_input_bytes = 1024;
    out->max_output_bytes = 32 * 1024;
    out->max_stream_bytes = 32 * 1024;
    out->max_tool_result_bytes = 16 * 1024;
    out->max_concurrency = 1;
    out->tools.default_timeout_ms = 10000;
    rc = app_runtime_io_config_load_default(io);
    if (rc.code != CC_OK) {
        return rc;
    }

    cc_json_value_t *model = object_at(root, "model");
    if (model) {
        if (!replace_string(&out->provider, string_at(model, "provider")) ||
            !replace_string(&out->model, string_at(model, "name")) ||
            !replace_string(&out->base_url, string_at(model, "base_url")) ||
            !replace_string(&out->api_key, string_at(model, "api_key")) ||
            !replace_string(&out->api_key_ref, string_at(model, "api_key_ref"))) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate model config");
        }
        out->max_tokens = int_at(model, "max_tokens", out->max_tokens);
        out->temperature = number_at(model, "temperature", out->temperature);
        cc_json_value_t *extra_body = object_at(model, "extra_body");
        if (!extra_body) {
            extra_body = object_at(model, "extraBody");
        }
        if (extra_body) {
            char *extra_body_json = cc_json_stringify_unformatted(extra_body);
            if (!extra_body_json) {
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to serialize model.extra_body");
            }
            bool ok = replace_string(&out->model_extra_body_json, extra_body_json);
            free(extra_body_json);
            if (!ok) {
                return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate model.extra_body");
            }
        }
        cc_json_value_t *tls = object_at(model, "tls");
        if (tls) {
            out->tls_verify = bool_at(tls, "verify", out->tls_verify != 0) ? 1 : 0;
        }
    }

    cc_json_value_t *runtime = object_at(root, "runtime");
    if (runtime) {
        out->stream_mode = bool_at(runtime, "stream", out->stream_mode != 0) ? 1 : 0;
        out->thinking_mode = bool_at(runtime, "thinking", out->thinking_mode != 0) ? 1 : 0;
        out->max_steps = int_at(runtime, "max_steps", out->max_steps);
        if (!replace_string_default(&io->session_id,
                                    string_at(runtime, "session_id"),
                                    "esp32s31-main")) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate runtime session config");
        }
        io->queue_depth = int_at(runtime, "queue_depth", io->queue_depth);
        io->max_input_bytes = int_at(runtime, "max_input_bytes", io->max_input_bytes);
        io->max_pending_operations = int_at(runtime, "max_pending_operations", io->max_pending_operations);
        out->run_timeout_ms = int_at(runtime, "run_timeout_ms", out->run_timeout_ms);
        out->provider_timeout_ms = int_at(runtime, "provider_timeout_ms", out->provider_timeout_ms);
        out->tool_timeout_ms = int_at(runtime, "tool_timeout_ms", out->tool_timeout_ms);
        out->max_input_bytes = io->max_input_bytes;
        out->max_output_bytes = int_at(runtime, "max_output_bytes", out->max_output_bytes);
        out->max_stream_bytes = int_at(runtime, "max_stream_bytes", out->max_stream_bytes);
        out->max_tool_result_bytes = int_at(runtime, "max_tool_result_bytes", out->max_tool_result_bytes);
        out->max_concurrency = int_at(runtime, "max_concurrency", out->max_concurrency);
        io->web.enabled = bool_at(runtime, "web_enabled", io->web.enabled != 0) ? 1 : 0;
        io->web.port = int_at(runtime, "web_port", io->web.port);
        io->web.event_limit = int_at(runtime, "event_limit", io->web.event_limit);
        out->context_window_tokens = int_at(runtime, "context_window_tokens", out->context_window_tokens);
        out->context_compress_threshold =
            number_at(runtime, "context_compress_threshold", out->context_compress_threshold);
        out->context_keep_recent = int_at(runtime, "context_keep_recent", out->context_keep_recent);
        out->summary_max_tokens = int_at(runtime, "summary_max_tokens", out->summary_max_tokens);
        out->summary_temperature = number_at(runtime, "summary_temperature", out->summary_temperature);
    }

    parse_network_http_config(root, out);

    cc_json_value_t *workspace = object_at(root, "workspace");
    if (!replace_string_default(&out->workspace_path,
                                workspace ? string_at(workspace, "root") : NULL,
                                "/usb0/workspace")) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate workspace config");
    }

    rc = parse_storage_config(root, out, io);
    if (rc.code != CC_OK) {
        return rc;
    }

    rc = parse_system_config(root, out);
    if (rc.code != CC_OK) {
        return rc;
    }

    rc = parse_events_config(root, io);
    if (rc.code != CC_OK) {
        return rc;
    }

    rc = parse_tools_config(root, out);
    if (rc.code != CC_OK) {
        return rc;
    }

    rc = parse_multimodal_config(root, out);
    if (rc.code != CC_OK) {
        return rc;
    }

    if (out->thinking_mode) {
        out->stream_mode = 1;
    }
    rc = validate_device_paths(out, io);
    if (rc.code != CC_OK) {
        return rc;
    }
    rc = app_runtime_io_config_validate(io);
    if (rc.code != CC_OK) {
        return rc;
    }
    return cc_config_validate(out);
}

/**
 * @brief 尽力创建工作区及完整的 lua 子目录路径。
 * @param[in] config 借用 SDK 配置，可为 NULL；缺失路径使用 /usb0/workspace。
 * @post 分配与端口资源均释放，目录创建失败只记录日志，配置加载结果保持成功。
 */
static void ensure_workspace_dirs(const cc_config_t *config)
{
    const char *workspace = config && config->workspace_path ? config->workspace_path : "/usb0/workspace";
    char *lua_dir = NULL;
    if (!replace_joined_path(&lua_dir, workspace, "lua")) {
        ESP_LOGW(TAG, "workspace Lua directory path allocation failed");
        return;
    }
    cc_filesystem_t fs;
    memset(&fs, 0, sizeof(fs));
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code == CC_OK && fs.vtable && fs.vtable->make_dir) {
        cc_result_t mkdir_rc = fs.vtable->make_dir(fs.self, workspace);
        if (mkdir_rc.code == CC_OK) {
            cc_result_free(&mkdir_rc);
            mkdir_rc = fs.vtable->make_dir(fs.self, lua_dir);
        }
        if (mkdir_rc.code != CC_OK) {
            ESP_LOGW(TAG, "workspace directory creation failed: %s",
                     mkdir_rc.message ? mkdir_rc.message : cc_error_string(mkdir_rc.code));
        }
        cc_result_free(&mkdir_rc);
    }
    cc_result_free(&rc);
    if (fs.vtable && fs.vtable->destroy) fs.vtable->destroy(fs.self);
    free(lua_dir);
}

/**
 * @brief 设置托管长期记忆默认预算，并校验配置中的整数覆盖值。
 * @param[in] root 借用配置树。
 * @param[in,out] out 有效应用配置，三个预算先重置为默认值，随后按字段更新。
 * @return 完成返回 CC_OK，类型或范围错误返回参数错误，前序已赋值字段保留。
 * @note capacity/pending_items 按条，pending_bytes 按字节，配置仅在启动重装配时生效。
 */
static cc_result_t parse_managed_memory_settings(const cc_json_value_t *root, app_config_t *out)
{
    out->managed_memory_capacity=100U;
    out->managed_memory_pending_items=4U;
    out->managed_memory_pending_bytes=256U*1024U;
    cc_json_value_t *storage=cc_json_object_get(root,"storage");
    cc_json_value_t *managed=cc_json_is_object(storage) ?
        cc_json_object_get(storage,"managed_memory") : NULL;
    if (!managed) return cc_result_ok();
    if (!cc_json_is_object(managed))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"storage.managed_memory must be an object");
    const char *const keys[]={"capacity","pending_items","pending_bytes"};
    const size_t maxima[]={CC_MEMORY_EXTRACTION_MAX_EXISTING,64U,8U*1024U*1024U};
    size_t *const outputs[]={&out->managed_memory_capacity,&out->managed_memory_pending_items,
                             &out->managed_memory_pending_bytes};
    for (size_t i=0;i<3U;i++) {
        cc_json_value_t *value=cc_json_object_get(managed,keys[i]);
        if (!value) continue;
        if (!cc_json_is_number(value))
            return cc_result_errf(CC_ERR_INVALID_ARGUMENT,"storage.managed_memory.%s must be an integer",keys[i]);
        double number=cc_json_number_value(value);
        if (!(number>=1.0 && number<=(double)maxima[i]) || number!=(double)(size_t)number)
            return cc_result_errf(CC_ERR_INVALID_ARGUMENT,"storage.managed_memory.%s is out of range",keys[i]);
        *outputs[i]=(size_t)number;
    }
    return cc_result_ok();
}

/**
 * @brief 设置摘要缓存默认预算，并校验整数覆盖值。
 * @param[in] root 借用配置树。
 * @param[in,out] out 有效应用配置，先写默认值，再逐项更新每会话条数、总条数和总字节预算。
 * @return 完成返回 CC_OK，非对象、非整数或范围错误返回参数错误，已赋值字段保留。
 */
static cc_result_t parse_summary_cache_settings(const cc_json_value_t *root, app_config_t *out)
{
    out->summary_cache_per_session=8U;
    out->summary_cache_max_entries=128U;
    out->summary_cache_max_bytes=512U*1024U;
    cc_json_value_t *storage=cc_json_object_get(root,"storage");
    cc_json_value_t *cache=cc_json_is_object(storage) ?
        cc_json_object_get(storage,"summary_cache") : NULL;
    if (!cache) return cc_result_ok();
    if (!cc_json_is_object(cache))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"storage.summary_cache must be an object");
    const char *const keys[]={"per_session","max_entries","max_bytes"};
    const size_t maxima[]={4096U,4096U,16U*1024U*1024U};
    size_t *const outputs[]={&out->summary_cache_per_session,&out->summary_cache_max_entries,
                             &out->summary_cache_max_bytes};
    for (size_t i=0;i<3U;i++) {
        cc_json_value_t *value=cc_json_object_get(cache,keys[i]);
        if (!value) continue;
        if (!cc_json_is_number(value))
            return cc_result_errf(CC_ERR_INVALID_ARGUMENT,"storage.summary_cache.%s must be an integer",keys[i]);
        double number=cc_json_number_value(value);
        if (!(number>=1.0 && number<=(double)maxima[i]) || number!=(double)(size_t)number)
            return cc_result_errf(CC_ERR_INVALID_ARGUMENT,"storage.summary_cache.%s is out of range",keys[i]);
        *outputs[i]=(size_t)number;
    }
    return cc_result_ok();
}

/**
 * @brief 比较配置 schema 中的 ASCII 字段名，兼容已有大小写非敏感读取。
 * @param[in] actual 借用对象键，可为 NULL。
 * @param[in] expected 有效借用规范键名，仅含 ASCII 字符。
 * @return 完整忽略 ASCII 大小写相等返回 true，其他返回 false。
 */
static bool config_key_matches(const char *actual, const char *expected)
{
    if (!actual) return false;
    while (*actual && *expected) {
        unsigned char a = (unsigned char)*actual++;
        unsigned char b = (unsigned char)*expected++;
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return *actual == '\0' && *expected == '\0';
}

/**
 * @brief 在一个配置对象内拒绝指定 schema 字段的同名或大小写重复。
 * @param[in] object 借用节点，NULL 或非对象时跳过，其类型继续由原配置解析规则处理。
 * @param[in] keys 至少含 key_count 项有效 ASCII 规范键名的借用数组。
 * @param[in] key_count 待检查的 schema 字段数。
 * @param[in] section 借用固定 schema 路径，只用于诊断，不包含凭据值。
 * @return 无歧义返回 CC_OK，重复返回含修正提示的参数错误，调用者须释放结果。
 * @note 不修改树，也不检查数组之外的业务字段。
 */
static cc_result_t validate_unique_config_fields(const cc_json_value_t *object,
                                                  const char *const *keys, size_t key_count,
                                                  const char *section)
{
    if (!cc_json_is_object(object)) return cc_result_ok();
    int count = cc_json_object_size(object);
    for (size_t key_index = 0; key_index < key_count; key_index++) {
        bool seen = false;
        for (int i = 0; i < count; i++) {
            if (!config_key_matches(cc_json_object_key_at(object, i), keys[key_index])) continue;
            if (seen)
                return cc_result_errf(CC_ERR_INVALID_ARGUMENT,
                    "凭据配置 %s.%s 存在重复字段（不区分大小写），请保留一个字段并修正配置。",
                    section, keys[key_index]);
            seen = true;
        }
    }
    return cc_result_ok();
}

/**
 * @brief 检查凭据 schema 路径上的容器及值、引用字段均只有一个匹配项。
 * @param[in] root 借用配置树，可为 NULL；仅检查对象中的 root.model/network、network.sta/ap 及各自凭据对。
 * @return 无歧义返回 CC_OK，否则返回首个重复路径的参数错误；不修改树或读取凭据值。
 * @note 单个大写键继续支持，model.extra_body 和其他业务嵌套字段保持原有独立语义。
 */
static cc_result_t validate_config_credential_fields(const cc_json_value_t *root)
{
    const char *const root_keys[] = {"model", "network"};
    const char *const network_keys[] = {"sta", "ap"};
    const char *const model_keys[] = {"api_key", "api_key_ref"};
    const char *const password_keys[] = {"password", "password_ref"};
    cc_result_t rc = validate_unique_config_fields(root, root_keys, 2, "root");
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    cc_json_value_t *model = cc_json_object_get(root, "model");
    rc = validate_unique_config_fields(model, model_keys, 2, "model");
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    cc_json_value_t *network = cc_json_object_get(root, "network");
    rc = validate_unique_config_fields(network, network_keys, 2, "network");
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    cc_json_value_t *sta = cc_json_object_get(network, "sta");
    rc = validate_unique_config_fields(sta, password_keys, 2, "network.sta");
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    cc_json_value_t *ap = cc_json_object_get(network, "ap");
    return validate_unique_config_fields(ap, password_keys, 2, "network.ap");
}

/**
 * @brief 解析完整配置 JSON，拒绝解码 NUL 和凭据 schema 路径上的重复字段。
 * @param[in] text 借用的 NUL 结尾文本，不能为空。
 * @param[out] out_root 有效输出槽，先置 NULL，成功接收调用方须销毁的 JSON 树。
 * @return 成功返回 CC_OK，NUL 转义、解析失败或凭据歧义返回错误；失败销毁并清空输出树。
 */
static cc_result_t parse_config_json(const char *text, cc_json_value_t **out_root)
{
    *out_root = NULL;
    if (cc_storage_json_has_nul_escape(text))
        return cc_result_error(CC_ERR_JSON, "Configuration contains a decoded NUL");
    cc_result_t rc = cc_json_parse_strict(text, out_root);
    if (rc.code != CC_OK) return rc;
    cc_result_free(&rc);
    rc = validate_config_credential_fields(*out_root);
    if (rc.code != CC_OK) {
        cc_json_destroy(*out_root);
        *out_root = NULL;
    }
    return rc;
}

/**
 * @brief 在独立临时快照内完成全部解析、凭据解析和校验，成功后替换目标。
 * @param[in,out] config 有效初始化目标，失败保持原资源和值。
 * @param[in] json_text 借用的完整 JSON，可借用目标的 raw_json，使用结束后才替换目标。
 * @param[in] allow_usb_plaintext 是否允许受信任启动快照中的明文凭据。
 * @return 成功返回 ESP_OK；JSON、运行时或 profile 失败返回参数错误，凭据失败返回状态错误，序列化分配失败返回 NO_MEM。
 * @note 其他构建分配失败随其所属阶段映射错误，独立解析凭据缓冲区均擦除释放；不创建目录或写文件。
 */
static esp_err_t load_from_text(app_config_t *config,
                                const char *json_text,
                                bool allow_usb_plaintext)
{
    cc_json_value_t *root = NULL;
    cc_result_t rc = parse_config_json(json_text, &root);
    if (rc.code != CC_OK) {
        ESP_LOGE(TAG, "config parse failed: %s", rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        return ESP_ERR_INVALID_ARG;
    }
    cc_result_free(&rc);
    if (!root || !cc_json_is_object(root)) {
        cc_json_destroy(root);
        return ESP_ERR_INVALID_ARG;
    }

    app_resolved_secrets_t secrets;
    memset(&secrets, 0, sizeof(secrets));
    rc = normalize_and_resolve_secrets(root, &secrets, allow_usb_plaintext);
    if (rc.code != CC_OK) {
        ESP_LOGE(TAG, "secret configuration invalid: %s",
                 rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        cc_json_destroy(root);
        return ESP_ERR_INVALID_STATE;
    }
    cc_result_free(&rc);

    app_config_t next = {0};
    rc = parse_runtime(root, &next.runtime, &next.io);
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = parse_managed_memory_settings(root, &next);
    }
    if (rc.code == CC_OK) {
        cc_result_free(&rc);
        rc = parse_summary_cache_settings(root, &next);
    }
    if (rc.code != CC_OK) {
        ESP_LOGE(TAG, "runtime config invalid: %s", rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        cc_json_destroy(root);
        resolved_secrets_destroy(&secrets);
        app_config_destroy(&next);
        return ESP_ERR_INVALID_ARG;
    }
    cc_result_free(&rc);
    parse_network(root, &next.network);
    cc_secret_free(next.runtime.api_key);
    next.runtime.api_key = secrets.model_api_key;
    secrets.model_api_key = NULL;
    if (secrets.sta_password) {
        copy_text(next.network.password, sizeof(next.network.password), secrets.sta_password);
    }
    if (secrets.ap_password) {
        copy_text(next.network.softap_password, sizeof(next.network.softap_password),
                  secrets.ap_password);
    }
    rc = parse_profile_config(root, &next.profile);
    if (rc.code != CC_OK) {
        ESP_LOGE(TAG, "profile config invalid: %s", rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        cc_json_destroy(root);
        resolved_secrets_destroy(&secrets);
        app_config_destroy(&next);
        return ESP_ERR_INVALID_ARG;
    }
    cc_result_free(&rc);

    cc_json_value_t *display = object_at(root, "display");
    copy_text(next.display_font_path,
              sizeof(next.display_font_path),
              display ? string_at(display, "font_path") : "/usb0/cclaw/fonts/default.bin");
    if (!next.display_font_path[0]) {
        copy_text(next.display_font_path, sizeof(next.display_font_path), "/usb0/cclaw/fonts/default.bin");
    }

    cc_json_value_t *camera = object_at(root, "camera");
    next.camera_default_quality = camera ? int_at(camera, "default_quality", 70) : 70;
    if (next.camera_default_quality < 1 || next.camera_default_quality > 100) {
        next.camera_default_quality = 70;
    }
    copy_text(next.camera_default_save_path,
              sizeof(next.camera_default_save_path),
              camera ? string_at(camera, "default_save_path") : "camera.jpg");
    if (!next.camera_default_save_path[0]) {
        copy_text(next.camera_default_save_path, sizeof(next.camera_default_save_path), "camera.jpg");
    }

    next.raw_json = cc_json_stringify(root);
    if (!next.raw_json) {
        cc_json_destroy(root);
        resolved_secrets_destroy(&secrets);
        app_config_destroy(&next);
        return ESP_ERR_NO_MEM;
    }
    next.loaded = true;

    app_config_destroy(config);
    *config = next;
    resolved_secrets_destroy(&secrets);
    cc_json_destroy(root);
    return ESP_OK;
}

/* 介质绑定前只读检查；保存选中及更高优先级候选的存在状态与完整字节，供绑定后复核。 */
#define APP_CONFIG_STAGE_CANDIDATES 3U
#define APP_CONFIG_STAGE_MAX_FILE_BYTES (1024U * 1024U)

/** @brief 一个候选文件的拥有快照，缺失文件也记录其存在状态。 */
typedef struct app_config_stage_file {
    char *path; /**< 拥有的候选路径。 */
    char *text; /**< 拥有的文件字节及额外 NUL，可包含文件内嵌 NUL，可能含凭据。 */
    size_t length; /**< 文件原始字节数，不含附加 NUL。 */
    bool exists; /**< 检查时是否存在普通文件。 */
} app_config_stage_file_t;

/** @brief 由单一外层所有者串行使用的配置检查事务。 */
struct app_config_stage {
    cc_filesystem_t fs; /**< 拥有的默认文件系统端口，存活到暂存对象销毁。 */
    app_config_stage_file_t files[APP_CONFIG_STAGE_CANDIDATES]; /**< 已检查候选的拥有快照。 */
    app_config_t config; /**< take 前拥有的已解析配置。 */
    int source; /**< 0 为正式文件，1 为 .tmp，2 为 .bak，选择前为 -1。 */
    bool consumed; /**< 已转移配置，文件快照仍可验证。 */
    bool invalidated; /**< 修复开始后尚未成功确认，禁止复用。 */
};

/**
 * @brief 按 stat 长度读取普通文件，拥有完整字节快照并补 NUL。
 * @param[in] fs 有效借用端口，须支持 stat 和 read_bytes。
 * @param[in] path 有效借用路径。
 * @param[out] out_exists 有效槽，先置 false，成功读取后置 true。
 * @param[out] out_text 有效槽，先置 NULL，成功接收须擦除并 free 的快照。
 * @param[out] out_length 有效槽，先置零，成功返回原始字节数，不含附加 NUL。
 * @return 缺失或读取成功返回 ESP_OK；非普通文件、超 1 MiB 或长度变化返回状态错误，端口失败为 ESP_FAIL，分配失败为 NO_MEM。
 * @note 失败清理所有临时字节，输出保持初始化状态；介质互斥由外层保证。
 */
static esp_err_t app_config_stage_read(const cc_filesystem_t *fs, const char *path,
                                       bool *out_exists, char **out_text, size_t *out_length)
{
    *out_exists = false;
    *out_text = NULL;
    *out_length = 0;
    cc_filesystem_stat_t st = {0};
    cc_result_t rc = fs->vtable->stat(fs->self, path, &st);
    if (rc.code != CC_OK) { cc_result_free(&rc); return ESP_FAIL; }
    cc_result_free(&rc);
    if (st.type == CC_FS_ENTRY_MISSING) return ESP_OK;
    if (st.type != CC_FS_ENTRY_FILE || st.byte_size > APP_CONFIG_STAGE_MAX_FILE_BYTES)
        return ESP_ERR_INVALID_STATE;
    void *data = NULL;
    size_t length = 0;
    rc = fs->vtable->read_bytes(fs->self, path, &data, &length);
    if (rc.code != CC_OK || (length && !data)) {
        cc_result_free(&rc);
        if (data) { cc_secret_zero(data, length); free(data); }
        return ESP_FAIL;
    }
    cc_result_free(&rc);
    if (length != st.byte_size || length > APP_CONFIG_STAGE_MAX_FILE_BYTES) {
        if (data) { cc_secret_zero(data, length); free(data); }
        return ESP_ERR_INVALID_STATE;
    }
    char *text = malloc(length + 1U);
    if (!text) {
        if (data) { cc_secret_zero(data, length); free(data); }
        return ESP_ERR_NO_MEM;
    }
    if (length) memcpy(text, data, length);
    text[length] = '\0';
    if (data) { cc_secret_zero(data, length); free(data); }
    *out_exists = true;
    *out_text = text;
    *out_length = length;
    return ESP_OK;
}

/**
 * @brief 擦除拥有快照与未转移配置，释放端口和暂存对象。
 * @param[in,out] stage 可为 NULL，非 NULL 时消费所有权。
 * @post 内存借用失效，已转移配置与磁盘恢复文件继续保留。
 */
void app_config_stage_destroy(app_config_stage_t *stage)
{
    if (!stage) return;
    for (size_t i = 0; i < APP_CONFIG_STAGE_CANDIDATES; i++) {
        free(stage->files[i].path);
        if (stage->files[i].text) {
            cc_secret_zero(stage->files[i].text, stage->files[i].length);
            free(stage->files[i].text);
        }
    }
    app_config_destroy(&stage->config);
    if (stage->fs.vtable && stage->fs.vtable->destroy) stage->fs.vtable->destroy(stage->fs.self);
    free(stage);
}

/**
 * @brief 顺序读取候选，只对缺失或明确无效内容尝试下一个来源。
 * @param[in] path 非空正式路径，本次调用借用。
 * @param[out] out_stage 有效槽，先置 NULL，成功接收拥有暂存对象。
 * @return 成功返回 ESP_OK；读取、未定解析失败、分配或业务校验失败返回错误并清理全部资源。
 * @note 先用完整 JSON 对象选择来源，再校验产品配置；选中后的语义错误终止加载。
 * 此调用只读，外层须排除并发挂载切换及写入并核对挂载代次。
 */
esp_err_t app_config_stage_open(const char *path, app_config_stage_t **out_stage)
{
    if (!out_stage) return ESP_ERR_INVALID_ARG;
    *out_stage = NULL;
    if (!path || !path[0]) return ESP_ERR_INVALID_ARG;
    const size_t path_len = strlen(path);
    if (path_len > SIZE_MAX - 5U) return ESP_ERR_INVALID_ARG;
    app_config_stage_t *stage = calloc(1, sizeof(*stage));
    if (!stage) return ESP_ERR_NO_MEM;
    stage->source = -1;
    cc_result_t rc = cc_filesystem_get_default(&stage->fs);
    if (rc.code != CC_OK || !stage->fs.vtable ||
        stage->fs.vtable->size < offsetof(cc_filesystem_vtable_t, stat) + sizeof(stage->fs.vtable->stat) ||
        !stage->fs.vtable->stat || !stage->fs.vtable->read_bytes) {
        cc_result_free(&rc);
        app_config_stage_destroy(stage);
        return ESP_FAIL;
    }
    cc_result_free(&rc);
    const char *suffixes[] = {"", ".tmp", ".bak"};
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < APP_CONFIG_STAGE_CANDIDATES; i++) {
        size_t suffix_len = strlen(suffixes[i]);
        stage->files[i].path = malloc(path_len + suffix_len + 1U);
        if (!stage->files[i].path) { err = ESP_ERR_NO_MEM; break; }
        memcpy(stage->files[i].path, path, path_len);
        memcpy(stage->files[i].path + path_len, suffixes[i], suffix_len + 1U);
        err = app_config_stage_read(&stage->fs, stage->files[i].path,
                                    &stage->files[i].exists, &stage->files[i].text, &stage->files[i].length);
        if (err != ESP_OK) break;
        if (stage->source < 0 && stage->files[i].exists &&
            !memchr(stage->files[i].text, '\0', stage->files[i].length)) {
            cc_json_value_t *root = NULL;
            rc = cc_json_parse_strict(stage->files[i].text, &root);
            if (rc.code == CC_OK && root && cc_json_is_object(root)) stage->source = (int)i;
            if (rc.code != CC_OK &&
                cc_json_syntax_check(stage->files[i].text) != CC_JSON_SYNTAX_INVALID)
                err = ESP_FAIL;
            cc_result_free(&rc);
            cc_json_destroy(root);
            if (err != ESP_OK) break;
        }
        if (stage->source >= 0) break;
    }
    if (err == ESP_OK && stage->source < 0) err = ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) err = load_from_text(&stage->config, stage->files[stage->source].text, true);
    if (err != ESP_OK) { app_config_stage_destroy(stage); return err; }
    *out_stage = stage;
    return ESP_OK;
}

/**
 * @brief 获取仍由暂存对象拥有的解析快照。
 * @param[in] stage 可为 NULL 的稳定借用对象。
 * @return 未消费且未失效时借用配置，其他状态返回 NULL，寿命止于 take、repair 或 destroy。
 */
const app_config_t *app_config_stage_config(const app_config_stage_t *stage)
{ return stage && !stage->invalidated && !stage->consumed && stage->config.loaded ? &stage->config : NULL; }

/**
 * @brief 获取可用暂存对象的候选来源编号。
 * @param[in] stage 可为 NULL 的稳定借用对象。
 * @return 正式文件、.tmp、.bak 分别为 0、1、2，失效、已消费或 NULL 返回 -1。
 */
int app_config_stage_source(const app_config_stage_t *stage)
{ return stage && !stage->invalidated && !stage->consumed ? stage->source : -1; }

/**
 * @brief 重新读取已选及更高优先级候选并逐字节核对快照。
 * @param[in] stage 有效且未失效的借用对象，take 后仍允许验证。
 * @return 存在状态、长度与字节一致返回 ESP_OK；变化或无效对象返回状态错误，读取失败原样返回。
 * @note 本函数不建立锁，外层仍须串行化介质写入和卸载。
 */
esp_err_t app_config_stage_validate(const app_config_stage_t *stage)
{
    if (!stage || stage->invalidated) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i <= (size_t)stage->source; i++) {
        bool exists = false;
        char *text = NULL;
        size_t length = 0;
        esp_err_t err = app_config_stage_read(&stage->fs, stage->files[i].path, &exists, &text, &length);
        if (err != ESP_OK) return err;
        bool same = exists == stage->files[i].exists &&
                    (!exists || (length == stage->files[i].length &&
                                 memcmp(text, stage->files[i].text, length) == 0));
        if (text) { cc_secret_zero(text, length); free(text); }
        if (!same) return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/**
 * @brief 将单个候选的当前字节与给定期望值比较，供修复前后确认使用。
 * @param[in] stage 有效借用暂存对象。
 * @param[in] index 小于 APP_CONFIG_STAGE_CANDIDATES 的有效下标。
 * @param[in] expected_exists 期望文件是否存在。
 * @param[in] expected 期望字节，存在且长度非零时须可读，本次调用借用。
 * @param[in] expected_length 期望文件字节数。
 * @return 完全匹配返回 ESP_OK，变化返回状态错误，读取或分配错误原样返回；临时快照始终擦除释放。
 */
static esp_err_t app_config_stage_match_file(const app_config_stage_t *stage,size_t index,
                                             bool expected_exists,const char *expected,size_t expected_length)
{
    bool exists=false; char *text=NULL; size_t length=0;
    esp_err_t err=app_config_stage_read(&stage->fs,stage->files[index].path,&exists,&text,&length);
    if (err==ESP_OK && (exists!=expected_exists ||
        (exists && (length!=expected_length || memcmp(text,expected,length)!=0))))
        err=ESP_ERR_INVALID_STATE;
    if (text) { cc_secret_zero(text,length); free(text); }
    return err;
}

/**
 * @brief 验证快照并同步安装恢复副本，安装后读回确认，再更新正式文件快照。
 * @param[in,out] stage 有效暂存对象，修复成功改来源为 0，失败一旦开始操作则保持失效。
 * @return 无需修复或成功返回 ESP_OK，缺少能力、分配、变化及 IO 失败返回错误，安装后目录同步未确认返回 NOT_FINISHED。
 * @pre 外层已绑定介质并排除并发写入和卸载。
 * @note 预分配失败可复用对象；开始操作后失败必须重新打开，take 的消费标记不清除。
 */
esp_err_t app_config_stage_repair(app_config_stage_t *stage)
{
    if (!stage || stage->source<0 || stage->source>=(int)APP_CONFIG_STAGE_CANDIDATES)
        return ESP_ERR_INVALID_STATE;
    esp_err_t err=app_config_stage_validate(stage);
    if (err!=ESP_OK || stage->source==0) return err;
    const cc_filesystem_vtable_t *vt=stage->fs.vtable;
    if (!vt || vt->size<offsetof(cc_filesystem_vtable_t,sync_file)+sizeof(vt->sync_file) ||
        !vt->atomic_replace || !vt->sync_file || (stage->source==2 && !vt->write_bytes))
        return ESP_ERR_NOT_SUPPORTED;
    const app_config_stage_file_t *selected=&stage->files[stage->source];
    char *promoted=malloc(selected->length+1U);
    if (!promoted) return ESP_ERR_NO_MEM;
    memcpy(promoted,selected->text,selected->length+1U);
    const size_t promoted_length=selected->length;
    /* 从首次可能写入开始，失败可留下恢复文件或已安装目标，因此须重新打开暂存事务。 */
    stage->invalidated=true;
    cc_result_t rc=cc_result_ok();
    if (stage->source==2)
        rc=vt->write_bytes(stage->fs.self,stage->files[1].path,promoted,promoted_length);
    if (rc.code==CC_OK) {
        cc_result_free(&rc);
        rc=vt->sync_file(stage->fs.self,stage->files[1].path);
    }
    if (rc.code!=CC_OK) err=ESP_FAIL;
    cc_result_free(&rc);
    if (err==ESP_OK)
        err=app_config_stage_match_file(stage,0,stage->files[0].exists,
                                        stage->files[0].text,stage->files[0].length);
    if (err==ESP_OK && stage->source==2)
        err=app_config_stage_match_file(stage,2,true,promoted,promoted_length);
    if (err==ESP_OK)
        err=app_config_stage_match_file(stage,1,true,promoted,promoted_length);
    if (err==ESP_OK) {
        rc=vt->atomic_replace(stage->fs.self,stage->files[1].path,stage->files[0].path);
        if (rc.code!=CC_OK) err=ESP_FAIL;
        cc_result_free(&rc);
    }
    if (err==ESP_OK)
        err=app_config_stage_match_file(stage,0,true,promoted,promoted_length);
    if (err==ESP_OK && vt->size>=offsetof(cc_filesystem_vtable_t,sync_dir)+sizeof(vt->sync_dir) && vt->sync_dir) {
        char *parent=strdup(stage->files[0].path);
        if (!parent) err=ESP_ERR_NOT_FINISHED;
        else {
            char *slash=strrchr(parent,'/');
            if (slash==parent) slash[1]='\0';
            else if (slash) *slash='\0';
            rc=vt->sync_dir(stage->fs.self,slash ? parent : ".");
            if (rc.code!=CC_OK && rc.code!=CC_ERR_UNSUPPORTED) err=ESP_ERR_NOT_FINISHED;
            cc_result_free(&rc); free(parent);
        }
    }
    if (err==ESP_OK) {
        if (stage->files[0].text) {
            cc_secret_zero(stage->files[0].text,stage->files[0].length);
            free(stage->files[0].text);
        }
        stage->files[0].text=promoted; stage->files[0].length=promoted_length;
        stage->files[0].exists=true; stage->source=0; stage->invalidated=false;
        promoted=NULL;
    }
    if (promoted) { cc_secret_zero(promoted,promoted_length); free(promoted); }
    return err;
}

/**
 * @brief 验证后一次移动配置资源，保留文件快照供后续核对。
 * @param[in,out] stage 有效未消费对象，成功清空内部配置并标记 consumed。
 * @param[in,out] out_config 有效初始化目标，不能别名内部配置，成功释放旧值并接收所有权。
 * @return 成功返回 ESP_OK，参数、状态或验证失败返回错误且保留目标。
 * @note 此调用不写文件，外层须满足介质绑定和并发排除约束。
 */
esp_err_t app_config_stage_take(app_config_stage_t *stage, app_config_t *out_config)
{
    if (!stage || !out_config || out_config == &stage->config) return ESP_ERR_INVALID_ARG;
    if (stage->consumed || !stage->config.loaded) return ESP_ERR_INVALID_STATE;
    esp_err_t err = app_config_stage_validate(stage);
    if (err != ESP_OK) return err;
    app_config_destroy(out_config);
    *out_config = stage->config;
    memset(&stage->config, 0, sizeof(stage->config));
    stage->consumed = true;
    return ESP_OK;
}

/**
 * @brief 经只读选择、修复确认和所有权转移加载配置，再尽力创建工作区目录。
 * @param[in,out] config 有效初始化目标，失败保留原配置。
 * @param[in] path 非空正式文件路径，本次调用借用。
 * @return 暂存阶段全部成功返回 ESP_OK，否则返回相应错误，修复失败可能已影响磁盘。
 * @note 启动明文凭据可用，调用者须独占介质；暂存对象始终在返回前释放。
 */
esp_err_t app_config_load(app_config_t *config, const char *path)
{
    if (!config || !path) return ESP_ERR_INVALID_ARG;
    app_config_stage_t *stage = NULL;
    esp_err_t err = app_config_stage_open(path, &stage);
    if (err == ESP_OK) err = app_config_stage_repair(stage);
    if (err == ESP_OK) err = app_config_stage_take(stage, config);
    app_config_stage_destroy(stage);
    if (err == ESP_OK) ensure_workspace_dirs(&config->runtime);
    return err;
}

/**
 * @brief 准备独立快照并提交，始终清理未转移的临时配置。
 * @param[in,out] config 有效初始化当前配置，部分成功和保存锁定遵循 commit_prepared。
 * @param[in] path 有效借用目标路径，调用期间由外层独占。
 * @param[in] json_text 借用完整 JSON，校验策略遵循 prepare。
 * @return 准备或提交阶段的结果，NOT_FINISHED 时新配置已接纳，ESP_FAIL 时可能有磁盘副作用。
 */
esp_err_t app_config_save(app_config_t *config, const char *path, const char *json_text)
{
    if (!config || !path || !json_text) {
        return ESP_ERR_INVALID_ARG;
    }
    app_config_t parsed = {0};
    esp_err_t err = app_config_prepare(&parsed, json_text);
    if (err != ESP_OK) {
        return err;
    }

    err = app_config_commit_prepared(config, &parsed, path);
    app_config_destroy(&parsed);
    return err;
}

/**
 * @brief 在临时快照中准备完整配置，拒绝新明文凭据。
 * @param[in,out] prepared 有效初始化目标，只有全流程成功才替换旧值。
 * @param[in] json_text 借用完整 JSON，可借用目标 raw_json。
 * @return 成功返回 ESP_OK，否则返回解析、凭据或构建阶段错误，目标保持原值。
 */
esp_err_t app_config_prepare(app_config_t *prepared, const char *json_text)
{
    if (!prepared || !json_text) return ESP_ERR_INVALID_ARG;
    return load_from_text(prepared, json_text, false);
}

/**
 * @brief 检查 Web/UI 使用的两种凭据占位符。
 * @param[in] value 借用字符串，可为 NULL。
 * @return 精确等于 [REDACTED] 或 *** 返回 true，其他返回 false。
 */
static bool app_secret_placeholder(const char *value)
{
    return value &&
           (strcmp(value, "[REDACTED]") == 0 || strcmp(value, "***") == 0);
}

/**
 * @brief 按配置实际匹配字段的大小写安装独占凭据节点，并确认所有权交接。
 * @param[in,out] object 有效借用对象，匹配字段按现有大小写非敏感规则选择。
 * @param[in] field 有效借用规范字段名，字段缺失时以此名新增。
 * @param[in,out] value 独占新节点，函数在所有路径消费，NULL 按分配失败处理。
 * @return 安装并确认成功返回 ESP_OK，复制实际键名或节点安装失败返回 NO_MEM。
 * @note 先复制旧节点拥有的键名，使消费型安装函数替换节点后仍能用有效键名验证结果。
 */
static esp_err_t app_set_secret_value(cc_json_value_t *object, const char *field,
                                      cc_json_value_t *value)
{
    if (!value) return ESP_ERR_NO_MEM;
    cc_json_value_t *matched = cc_json_object_get(object, field);
    const char *actual_key = field;
    if (matched) {
        int count = cc_json_object_size(object);
        for (int i = 0; i < count; i++) {
            if (cc_json_object_value_at(object, i) == matched) {
                actual_key = cc_json_object_key_at(object, i);
                break;
            }
        }
    }
    char *key = strdup(actual_key);
    if (!key) {
        cc_json_destroy(value);
        return ESP_ERR_NO_MEM;
    }
    bool installed = cc_json_object_set_owned(object, key, value) != 0;
    free(key);
    return installed ? ESP_OK : ESP_ERR_NO_MEM;
}

/**
 * @brief 处理一组提交凭据，保留脱敏旧值或执行显式清空与引用切换。
 * @param[in,out] submitted 借用提交对象，NULL 且当前有凭据时拒绝，成功修改仅限此树。
 * @param[in] current 可为 NULL 的借用当前对象，提供受信任旧凭据。
 * @param[in] value_field 有效明文字段名。
 * @param[in] reference_field 有效引用字段名。
 * @return 完成返回 ESP_OK，新明文或不完整提交返回参数错误，节点创建或安装失败返回 NO_MEM。
 * @note 失败可能已改动提交树，由上层丢弃整棵树；所有新节点交给树或消费型插入函数清理。
 */
static esp_err_t app_preserve_redacted_secret(
    cc_json_value_t *submitted,
    cc_json_value_t *current,
    const char *value_field,
    const char *reference_field)
{
    if (!submitted) {
        const char *current_value = current ? string_at(current, value_field) : NULL;
        const char *current_reference = current ? string_at(current, reference_field) : NULL;
        return ((current_value && current_value[0]) ||
                (current_reference && current_reference[0]))
            ? ESP_ERR_INVALID_ARG : ESP_OK;
    }
    const char *incoming_value = string_at(submitted, value_field);
    const char *incoming_reference = string_at(submitted, reference_field);
    bool value_missing = incoming_value == NULL;
    bool reference_missing = incoming_reference == NULL;
    bool value_placeholder = app_secret_placeholder(incoming_value);
    bool reference_placeholder = app_secret_placeholder(incoming_reference);

    if (incoming_value && incoming_value[0] && !value_placeholder) {
        return ESP_ERR_INVALID_ARG;
    }
    if (incoming_reference && incoming_reference[0] && !reference_placeholder) {
        cc_json_value_t *empty = cc_json_create_string("");
        if (!empty) return ESP_ERR_NO_MEM;
        return app_set_secret_value(submitted, value_field, empty);
    }
    if (!value_missing && !value_placeholder && incoming_value[0] == '\0') {
        cc_json_value_t *empty_ref = cc_json_create_string("");
        if (!empty_ref) return ESP_ERR_NO_MEM;
        return app_set_secret_value(submitted, reference_field, empty_ref);
    }
    if (!(value_placeholder || reference_placeholder ||
          (value_missing && reference_missing))) {
        return ESP_OK;
    }

    const char *current_value = current ? string_at(current, value_field) : NULL;
    const char *current_reference = current ? string_at(current, reference_field) : NULL;
    cc_json_value_t *value = cc_json_create_string(
        current_value && current_value[0] ? current_value : "");
    cc_json_value_t *reference = cc_json_create_string(
        (!current_value || !current_value[0]) &&
        current_reference && current_reference[0] ? current_reference : "");
    if (!value || !reference) {
        if (value) cc_json_destroy(value);
        if (reference) cc_json_destroy(reference);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = app_set_secret_value(submitted, value_field, value);
    if (err != ESP_OK) {
        cc_json_destroy(reference);
        return ESP_ERR_NO_MEM;
    }
    return app_set_secret_value(submitted, reference_field, reference);
}

/**
 * @brief 解析 Web/UI 完整快照，合并当前凭据，再事务性构建新配置。
 * @param[in,out] prepared 有效初始化目标，必须与 current 不同，失败保留旧值。
 * @param[in] current 有效稳定当前配置，借用其规范化 JSON 和凭据。
 * @param[in] json_text 借用完整提交 JSON，允许脱敏占位符，拒绝新明文。
 * @return 成功返回 ESP_OK；提交无效、当前 JSON 无效、分配或合并失败返回对应错误。
 * @note 两棵临时树与合并文本均在返回前清理，合并文本擦除，不写磁盘。
 */
esp_err_t app_config_prepare_update(
    app_config_t *prepared,
    const app_config_t *current,
    const char *json_text)
{
    if (!prepared || !current || prepared == current || !json_text) return ESP_ERR_INVALID_ARG;
    cc_json_value_t *submitted = NULL;
    cc_json_value_t *existing = NULL;
    cc_result_t rc = parse_config_json(json_text, &submitted);
    if (rc.code != CC_OK || !submitted || !cc_json_is_object(submitted)) {
        ESP_LOGE(TAG, "submitted configuration invalid: %s",
                 rc.message ? rc.message : "configuration root must be an object");
        cc_result_free(&rc);
        if (submitted) cc_json_destroy(submitted);
        return ESP_ERR_INVALID_ARG;
    }
    cc_result_free(&rc);
    rc = parse_config_json(app_config_json(current), &existing);
    if (rc.code != CC_OK || !existing || !cc_json_is_object(existing)) {
        ESP_LOGE(TAG, "current configuration invalid: %s",
                 rc.message ? rc.message : "configuration root must be an object");
        cc_result_free(&rc);
        cc_json_destroy(submitted);
        if (existing) cc_json_destroy(existing);
        return ESP_ERR_INVALID_STATE;
    }
    cc_result_free(&rc);

    cc_json_value_t *submitted_model = object_at(submitted, "model");
    cc_json_value_t *existing_model = object_at(existing, "model");
    cc_json_value_t *submitted_network = object_at(submitted, "network");
    cc_json_value_t *existing_network = object_at(existing, "network");
    esp_err_t err = app_preserve_redacted_secret(
        submitted_model, existing_model, "api_key", "api_key_ref");
    if (err == ESP_OK) {
        err = app_preserve_redacted_secret(
            submitted_network ? object_at(submitted_network, "sta") : NULL,
            existing_network ? object_at(existing_network, "sta") : NULL,
            "password", "password_ref");
    }
    if (err == ESP_OK) {
        err = app_preserve_redacted_secret(
            submitted_network ? object_at(submitted_network, "ap") : NULL,
            existing_network ? object_at(existing_network, "ap") : NULL,
            "password", "password_ref");
    }
    char *merged = err == ESP_OK ? cc_json_stringify(submitted) : NULL;
    cc_json_destroy(existing);
    cc_json_destroy(submitted);
    if (err == ESP_OK && !merged) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        /* 明文凭据只能来自当前快照的合并结果，因此允许沿启动配置路径重新解析。 */
        err = load_from_text(prepared, merged, true);
    }
    if (merged) {
        cc_secret_zero(merged, strlen(merged));
        free(merged);
    }
    return err;
}

/**
 * @brief 递归比较 JSON 类型和值，数组顺序敏感，对象按 SDK 键查找匹配。
 * @param[in] left 借用左节点，可为 NULL。
 * @param[in] right 借用右节点，可为 NULL。
 * @return 同指针或值相等返回 true，否则返回 false，数字按 double 精确相等比较。
 * @note 不分配或修改树，对象键匹配沿 SDK 的大小写非敏感规则。
 */
static bool json_values_equal(const cc_json_value_t *left, const cc_json_value_t *right)
{
    if (left == right) return true;
    if (!left || !right) return false;
    if (cc_json_is_null(left) || cc_json_is_null(right)) {
        return cc_json_is_null(left) && cc_json_is_null(right);
    }
    if (cc_json_is_string(left) || cc_json_is_string(right)) {
        if (!cc_json_is_string(left) || !cc_json_is_string(right)) return false;
        const char *a = cc_json_string_value(left);
        const char *b = cc_json_string_value(right);
        return strcmp(a ? a : "", b ? b : "") == 0;
    }
    if (cc_json_is_bool(left) || cc_json_is_bool(right)) {
        return cc_json_is_bool(left) && cc_json_is_bool(right) &&
               cc_json_bool_value(left) == cc_json_bool_value(right);
    }
    if (cc_json_is_number(left) || cc_json_is_number(right)) {
        return cc_json_is_number(left) && cc_json_is_number(right) &&
               cc_json_number_value(left) == cc_json_number_value(right);
    }
    if (cc_json_is_array(left) || cc_json_is_array(right)) {
        if (!cc_json_is_array(left) || !cc_json_is_array(right)) return false;
        int count = cc_json_array_size(left);
        if (count != cc_json_array_size(right)) return false;
        for (int i = 0; i < count; i++) {
            if (!json_values_equal(cc_json_array_get(left, i), cc_json_array_get(right, i))) {
                return false;
            }
        }
        return true;
    }
    if (cc_json_is_object(left) || cc_json_is_object(right)) {
        if (!cc_json_is_object(left) || !cc_json_is_object(right) ||
            cc_json_object_size(left) != cc_json_object_size(right)) {
            return false;
        }
        int count = cc_json_object_size(left);
        for (int i = 0; i < count; i++) {
            const char *key = cc_json_object_key_at(left, i);
            if (!key || !json_values_equal(cc_json_object_value_at(left, i),
                                           cc_json_object_get(right, key))) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/**
 * @brief 判断 runtime 字段是否位于热更新白名单。
 * @param[in] key 借用键名，可为 NULL。
 * @return 精确为 thinking 或 event_limit 返回 true，其他返回 false。
 */
static bool runtime_key_is_hot(const char *key)
{
    return key && (strcmp(key, "thinking") == 0 ||
                   strcmp(key, "event_limit") == 0);
}

/**
 * @brief 判断 network 子节是否允许整体热更新。
 * @param[in] key 借用键名，可为 NULL。
 * @return 精确为 sta、ap 或 mdns 返回 true，其他返回 false。
 */
static bool network_key_is_hot(const char *key)
{
    return key && (strcmp(key, "sta") == 0 ||
                   strcmp(key, "ap") == 0 ||
                   strcmp(key, "mdns") == 0);
}

/**
 * @brief 双向比较对象中白名单之外的字段，检测增加、删除与值变化。
 * @param[in] old_object 借用旧节点，可为 NULL。
 * @param[in] new_object 借用新节点，可为 NULL。
 * @param[in] is_hot 同步调用的借用键分类回调，可为 NULL 表示全部参与比较。
 * @return 存在非热更新变化返回 true，其他返回 false，非对象节点直接递归比较。
 */
static bool object_changed_except_hot(const cc_json_value_t *old_object,
                                      const cc_json_value_t *new_object,
                                      bool (*is_hot)(const char *key))
{
    if (!old_object && !new_object) return false;
    if (!cc_json_is_object(old_object) || !cc_json_is_object(new_object)) {
        return !json_values_equal(old_object, new_object);
    }
    int old_count = cc_json_object_size(old_object);
    for (int i = 0; i < old_count; i++) {
        const char *key = cc_json_object_key_at(old_object, i);
        if (!key || (is_hot && is_hot(key))) continue;
        if (!json_values_equal(cc_json_object_value_at(old_object, i),
                               cc_json_object_get(new_object, key))) {
            return true;
        }
    }
    int new_count = cc_json_object_size(new_object);
    for (int i = 0; i < new_count; i++) {
        const char *key = cc_json_object_key_at(new_object, i);
        if (!key || (is_hot && is_hot(key))) continue;
        if (!json_values_equal(cc_json_object_get(old_object, key),
                               cc_json_object_value_at(new_object, i))) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 判断顶层节是否允许整体热更新。
 * @param[in] key 借用键名，可为 NULL。
 * @return 精确为 events、profile、display 或 camera 返回 true，其他返回 false。
 */
static bool root_key_is_fully_hot(const char *key)
{
    return key && (strcmp(key, "events") == 0 ||
                   strcmp(key, "profile") == 0 ||
                   strcmp(key, "display") == 0 ||
                   strcmp(key, "camera") == 0);
}

/**
 * @brief 按顶层节及 runtime/network 子字段白名单比较重启需求。
 * @param[in] key 借用顶层键名，NULL 时跳过。
 * @param[in] old_root 借用旧配置对象。
 * @param[in] new_root 借用新配置对象。
 * @return 白名单之外有变化返回 true，全部允许热更新或相等返回 false。
 */
static bool root_section_requires_restart(const char *key,
                                          const cc_json_value_t *old_root,
                                          const cc_json_value_t *new_root)
{
    if (!key || root_key_is_fully_hot(key)) return false;
    const cc_json_value_t *old_value = cc_json_object_get(old_root, key);
    const cc_json_value_t *new_value = cc_json_object_get(new_root, key);
    if (strcmp(key, "runtime") == 0) {
        return object_changed_except_hot(old_value, new_value, runtime_key_is_hot);
    }
    if (strcmp(key, "network") == 0) {
        return object_changed_except_hot(old_value, new_value, network_key_is_hot);
    }
    return !json_values_equal(old_value, new_value);
}

/**
 * @brief 解析两份规范化 JSON 并比较热更新白名单之外的变化。
 * @param[in] current 借用当前配置，可为 NULL，调用期间须保持稳定。
 * @param[in] prepared 借用待提交配置，可为 NULL，调用期间须保持稳定。
 * @return 未加载、解析失败或发现需重启变化返回 true，只有允许热更新的变化返回 false。
 * @note 临时树始终清理，不修改配置，也不执行热更新或重启。
 */
bool app_config_requires_restart(const app_config_t *current, const app_config_t *prepared)
{
    if (!current || !prepared || !current->loaded || !prepared->loaded) return true;
    cc_json_value_t *old_root = NULL;
    cc_json_value_t *new_root = NULL;
    cc_result_t old_rc = parse_config_json(app_config_json(current), &old_root);
    cc_result_t new_rc = parse_config_json(app_config_json(prepared), &new_root);
    bool restart = old_rc.code != CC_OK || new_rc.code != CC_OK ||
                   !cc_json_is_object(old_root) || !cc_json_is_object(new_root);
    cc_result_free(&old_rc);
    cc_result_free(&new_rc);

    if (!restart) {
        int old_count = cc_json_object_size(old_root);
        for (int i = 0; i < old_count && !restart; i++) {
            const char *key = cc_json_object_key_at(old_root, i);
            restart = root_section_requires_restart(key, old_root, new_root);
        }
        int new_count = cc_json_object_size(new_root);
        for (int i = 0; i < new_count && !restart; i++) {
            const char *key = cc_json_object_key_at(new_root, i);
            restart = root_section_requires_restart(key, old_root, new_root);
        }
    }
    cc_json_destroy(old_root);
    cc_json_destroy(new_root);
    return restart;
}

/**
 * @brief 安装规范化 JSON，确认目标可见后移动 prepared 资源到 current。
 * @param[in,out] current 有效初始化当前配置，不能与 prepared 相同，已锁定保存时拒绝操作。
 * @param[in,out] prepared 独立且已加载的拥有快照，确认安装后清零，未确认时保留。
 * @param[in] path 有效借用目标路径，外层须独占介质与配置操作。
 * @return 同步完成为 ESP_OK；已接纳但耐久未确认为 NOT_FINISHED；未确认安装为 ESP_FAIL，参数或保存锁定另返回错误。
 * 超过启动读取上限的规范化文本返回 INVALID_ARG，检查发生在获取端口及任何磁盘操作之前。
 * @note 不确定写入锁定后续保存，重启重新加载后才能解除。返回失败时也可能已安装磁盘目标。
 */
esp_err_t app_config_commit_prepared(
    app_config_t *current,
    app_config_t *prepared,
    const char *path
)
{
    if (!current || !prepared || current == prepared || !prepared->loaded || !path)
        return ESP_ERR_INVALID_ARG;
    if (current->updates_blocked) return ESP_ERR_INVALID_STATE;
    const char *json = app_config_json(prepared);
    size_t json_length = strlen(json);
    if (json_length > APP_CONFIG_STAGE_MAX_FILE_BYTES) {
        ESP_LOGE(TAG, "配置保存文本为 %zu 字节，超过启动读取上限 %u 字节，请缩减配置后重试。",
                 json_length, (unsigned)APP_CONFIG_STAGE_MAX_FILE_BYTES);
        return ESP_ERR_INVALID_ARG;
    }
    bool installed = false;
    bool uncertain = false;
    bool synced = write_text_file_atomic(path, json, &installed, &uncertain);
    if (!installed) {
        current->updates_blocked = uncertain;
        return ESP_FAIL;
    }
    /* 目标已可见时内存接纳同一配置，后续同步失败也不能回滚热更新组件。 */
    app_config_destroy(current);
    *current = *prepared;
    current->updates_blocked = uncertain;
    memset(prepared, 0, sizeof(*prepared));
    return synced ? ESP_OK : ESP_ERR_NOT_FINISHED;
}

/**
 * @brief 擦除凭据及 JSON，释放 SDK、IO 和剖析拥有资源。
 * @param[in,out] config 已清零或初始化的配置，可为 NULL。
 * @post 非 NULL 目标完全归零，全部字段借用失效。
 * @pre 外层已停止并排空所有借用该快照的组件和调用。
 */
void app_config_destroy(app_config_t *config)
{
    if (!config) {
        return;
    }
    if (config->runtime.api_key) cc_secret_zero(config->runtime.api_key, strlen(config->runtime.api_key));
    cc_secret_zero(config->network.password, sizeof(config->network.password));
    cc_secret_zero(config->network.softap_password, sizeof(config->network.softap_password));
    if (config->raw_json) cc_secret_zero(config->raw_json, strlen(config->raw_json));
    cc_config_destroy(&config->runtime);
    app_runtime_io_config_destroy(&config->io);
    app_profile_config_destroy(&config->profile);
    free(config->raw_json);
    memset(config, 0, sizeof(*config));
}

/**
 * @brief 借用已加载的 SDK 配置字段。
 * @param[in] config 可为 NULL 的稳定快照。
 * @return 已加载返回字段地址，否则 NULL，借用止于替换或销毁。
 */
const cc_config_t *app_config_runtime(const app_config_t *config)
{
    return config && config->loaded ? &config->runtime : NULL;
}

/**
 * @brief 借用已加载的 IO 配置字段。
 * @param[in] config 可为 NULL 的稳定快照。
 * @return 已加载返回字段地址，否则 NULL，借用止于替换或销毁。
 */
const app_runtime_io_config_t *app_config_io(const app_config_t *config)
{
    return config && config->loaded ? &config->io : NULL;
}

/**
 * @brief 借用已加载的网络配置字段。
 * @param[in] config 可为 NULL 的稳定快照。
 * @return 已加载返回字段地址，否则 NULL，借用止于替换或销毁。
 */
const network_config_t *app_config_network(const app_config_t *config)
{
    return config && config->loaded ? &config->network : NULL;
}

/**
 * @brief 借用保存的规范化 JSON 或空对象常量。
 * @param[in] config 可为 NULL 的稳定快照。
 * @return raw_json 非空时返回其借用，否则返回静态 "{}"，字段借用止于替换或销毁。
 * @note 文本可能含明文凭据，向外暴露前由调用方脱敏。
 */
const char *app_config_json(const app_config_t *config)
{
    return config && config->raw_json ? config->raw_json : "{}";
}

/**
 * @brief 查询已加载配置中的思考模式。
 * @param[in] config 可为 NULL 的稳定快照。
 * @return 已加载且 thinking_mode 非零为 true，其他情况为 false。
 */
bool app_config_runtime_thinking(const app_config_t *config)
{
    return config && config->loaded && config->runtime.thinking_mode != 0;
}
