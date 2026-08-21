#include "app_config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cc/util/cc_json.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_secret_store.h"
#include "esp_log.h"

static const char *TAG = "app_config";

/*
 * 以二进制方式读取文件全部内容到字符串。
 *
 * 参数:
 *   path - 文件路径
 * 返回: 成功返回 malloc 分配的字符串（调用方负责释放），失败返回 NULL。
 */
static char *read_text_file(const char *path)
{
    cc_filesystem_t fs;
    memset(&fs, 0, sizeof(fs));
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK || !fs.vtable || !fs.vtable->read_text) {
        cc_result_free(&rc);
        return NULL;
    }
    char *text = NULL;
    rc = fs.vtable->read_text(fs.self, path, &text);
    if (rc.code != CC_OK && fs.vtable->atomic_replace) {
        const char *suffixes[] = {".tmp", ".bak"};
        for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]) && !text; i++) {
            size_t path_len = strlen(path);
            size_t suffix_len = strlen(suffixes[i]);
            if (path_len > SIZE_MAX - suffix_len - 1) break;
            char *candidate = malloc(path_len + suffix_len + 1);
            if (!candidate) break;
            memcpy(candidate, path, path_len);
            memcpy(candidate + path_len, suffixes[i], suffix_len + 1);
            char *candidate_text = NULL;
            cc_result_t candidate_rc = fs.vtable->read_text(fs.self, candidate, &candidate_text);
            cc_json_value_t *root = NULL;
            cc_result_t parse_rc = candidate_rc.code == CC_OK
                ? cc_json_parse(candidate_text ? candidate_text : "", &root) : candidate_rc;
            if (parse_rc.code == CC_OK && root) {
                cc_result_t replace_rc = fs.vtable->atomic_replace(fs.self, candidate, path);
                if (replace_rc.code == CC_OK) text = candidate_text;
                else free(candidate_text);
                cc_result_free(&replace_rc);
            } else {
                free(candidate_text);
            }
            cc_json_destroy(root);
            cc_result_free(&parse_rc);
            free(candidate);
        }
    }
    cc_result_free(&rc);
    if (fs.vtable->destroy) fs.vtable->destroy(fs.self);
    return text;
}

/*
 * 将文本内容写入文件。
 *
 * 参数:
 *   path - 目标文件路径
 *   text - 待写入的文本内容（NULL 视为空字符串）
 * 返回: 写入成功返回 true，失败返回 false。
 */
static bool write_text_file_atomic(const char *path, const char *text)
{
    cc_filesystem_t fs;
    memset(&fs, 0, sizeof(fs));
    cc_result_t rc = cc_filesystem_get_default(&fs);
    if (rc.code != CC_OK || !fs.vtable || !fs.vtable->write_text ||
        !fs.vtable->atomic_replace || !fs.vtable->sync_file) {
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
    rc = fs.vtable->write_text(fs.self, tmp, text ? text : "");
    if (rc.code == CC_OK) rc = fs.vtable->sync_file(fs.self, tmp);
    if (rc.code == CC_OK) rc = fs.vtable->atomic_replace(fs.self, tmp, path);
    if (rc.code == CC_OK && fs.vtable->sync_dir) {
        char *parent = strdup(path);
        if (!parent) rc = cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot copy config parent path");
        else {
            char *slash = strrchr(parent, '/');
            if (slash) *slash = '\0';
            cc_result_t sync_rc = fs.vtable->sync_dir(fs.self, slash && parent[0] ? parent : ".");
            if (sync_rc.code != CC_OK && sync_rc.code != CC_ERR_UNSUPPORTED) rc = sync_rc;
            else cc_result_free(&sync_rc);
            free(parent);
        }
    }
    bool ok = rc.code == CC_OK;
    if (!ok && fs.vtable->remove) {
        cc_result_t cleanup = fs.vtable->remove(fs.self, tmp);
        cc_result_free(&cleanup);
    }
    cc_result_free(&rc);
    free(tmp);
    if (fs.vtable->destroy) fs.vtable->destroy(fs.self);
    return ok;
}

/*
 * 安全替换字符串字段：先分配新内存，成功后释放旧值。
 *
 * 参数:
 *   field - 指向字符串指针的指针（待替换的字段）
 *   value - 新值
 * 返回: 替换成功返回 true，分配失败返回 false。
 *
 * 若 field 或 value 为 NULL 则直接返回 true（不做任何操作）。
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

/*
 * 释放字符串列表中的所有动态内存并归零列表结构。
 *
 * 参数:
 *   list - 待释放的字符串列表（NULL 忽略）
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

/*
 * 向字符串列表追加一个字符串（深拷贝）。
 *
 * 参数:
 *   list  - 目标列表
 *   value - 待追加的字符串值（NULL 则忽略并返回 true）
 * 返回: 追加成功返回 true，分配失败返回 false。
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

/*
 * 从 JSON 数组解析字符串列表，先清空原有内容再填充。
 *
 * 参数:
 *   array - JSON 数组值（NULL 或非数组则清空列表后返回 true）
 *   out   - 输出字符串列表
 * 返回: 解析成功返回 true，内存分配失败返回 false。
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

/*
 * 从 JSON 对象中按 key 获取数组并解析为字符串列表（可选字段，不存在则忽略）。
 *
 * 参数:
 *   obj - JSON 对象（NULL 则直接返回 true）
 *   key - 键名
 *   out - 输出字符串列表
 * 返回: 解析成功返回 true。
 */
static bool parse_optional_string_array(cc_json_value_t *obj, const char *key, cc_config_string_list_t *out)
{
    if (!obj) {
        return true;
    }
    cc_json_value_t *array = cc_json_object_get(obj, key);
    return !array || parse_string_array(array, out);
}

/*
 * 从 C 字符串数组构建字符串列表，先清空原有内容。
 *
 * 参数:
 *   out   - 输出字符串列表
 *   items - C 风格字符串数组
 *   count - 数组元素个数
 * 返回: 成功返回 true，分配失败返回 false。
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

/*
 * 释放 IO 配置中的所有动态内存（session_id、日志级别、USB日志目录及所有事件路由列表）。
 *
 * 参数:
 *   io - 待销毁的 IO 配置（NULL 忽略）
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

/*
 * 释放性能剖析配置中的动态内存（level 字段）。
 *
 * 参数:
 *   profile - 待销毁的 profile 配置（NULL 忽略）
 */
static void app_profile_config_destroy(app_profile_config_t *profile)
{
    if (!profile) {
        return;
    }
    free(profile->level);
    memset(profile, 0, sizeof(*profile));
}

/*
 * 加载 IO 配置的默认值：设置队列深度、缓冲区大小、各通道开关及默认路由。
 *
 * 参数:
 *   io - IO 配置结构（非 NULL）
 * 返回: 成功返回 CC_OK，分配失败返回 OOM 错误。
 *
 * 若分配中途失败会调用 app_runtime_io_config_destroy 清理已分配资源。
 */
static cc_result_t app_runtime_io_config_load_default(app_runtime_io_config_t *io)
{
    if (!io) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null app io config");
    }
    memset(io, 0, sizeof(*io));
    io->queue_depth = 8;
    io->max_input_bytes = 1024;
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

/*
 * 判断日志级别字符串是否为已知合法值（trace/debug/info/warn/error/fatal）。
 *
 * 参数:
 *   level - 日志级别字符串（NULL 视为 "info"）
 * 返回: 合法的日志级别返回 true。
 */
static bool app_event_log_level_known(const char *level)
{
    const char *l = level ? level : "info";
    return strcmp(l, "trace") == 0 || strcmp(l, "debug") == 0 ||
           strcmp(l, "info") == 0 || strcmp(l, "warn") == 0 ||
           strcmp(l, "error") == 0 || strcmp(l, "fatal") == 0;
}

/*
 * 将日志级别字符串映射为数值（0=trace ~ 5=fatal），同时兼容 "warning" 别名。
 *
 * 参数:
 *   level    - 日志级别字符串
 *   fallback - 未知级别时的默认返回值
 * 返回: 对应的数值。
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

/*
 * 判断事件 sink 名称是否为已知合法值（serial/web/display/usb_log）。
 *
 * 参数:
 *   sink - sink 名称
 * 返回: 合法返回 true。
 */
static bool app_event_sink_known(const char *sink)
{
    return sink &&
           (strcmp(sink, "serial") == 0 ||
            strcmp(sink, "web") == 0 ||
            strcmp(sink, "display") == 0 ||
            strcmp(sink, "usb_log") == 0);
}

/*
 * 校验事件路由配置：至少包含一个 sink，且所有 sink 名称必须合法。
 *
 * 参数:
 *   name  - 路由名称（用于错误消息）
 *   route - 路由列表
 * 返回: 校验通过返回 CC_OK，否则返回错误。
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

/*
 * 对 IO 配置进行完整校验：session_id、队列深度、缓冲区、Web端口、日志级别和所有事件路由。
 *
 * 参数:
 *   io - IO 配置结构
 * 返回: 校验通过返回 CC_OK，否则返回具体错误。
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

/*
 * 同步 enabled_tools 缓存：将 tools.enabled 列表深拷贝到 config->enabled_tools 指针数组。
 * 先释放旧缓存，再重建。用于工具启用的快速查询。
 *
 * 参数:
 *   config - 配置结构
 * 返回: 成功返回 true，分配失败返回 false。
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
    }
    config->enabled_tools_count = config->tools.enabled.count;
    return true;
}

/*
 * 从 JSON 对象中获取子对象，NULL 安全。
 *
 * 参数:
 *   root - 父 JSON 对象
 *   key  - 键名
 * 返回: 找到且为对象类型返回该对象，否则返回 NULL。
 */
static cc_json_value_t *object_at(cc_json_value_t *root, const char *key)
{
    cc_json_value_t *value = cc_json_object_get(root, key);
    return value && cc_json_is_object(value) ? value : NULL;
}

/*
 * 从 JSON 对象获取字符串值。
 *
 * 参数:
 *   obj - JSON 对象
 *   key - 键名
 * 返回: 字符串值或 NULL。
 */
static const char *string_at(cc_json_value_t *obj, const char *key)
{
    return cc_json_string_value(cc_json_object_get(obj, key));
}

/*
 * 从 JSON 获取布尔值，支持字符串（"true"/"1"/"on"/"yes"）、原生 bool 和数字形式。
 *
 * 参数:
 *   obj      - JSON 对象
 *   key      - 键名
 *   fallback - 默认值
 * 返回: 布尔值。
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

/*
 * 从 JSON 获取整数值。
 *
 * 参数:
 *   obj      - JSON 对象
 *   key      - 键名
 *   fallback - 默认值
 * 返回: 整数值。
 */
static int int_at(cc_json_value_t *obj, const char *key, int fallback)
{
    cc_json_value_t *value = cc_json_object_get(obj, key);
    return value ? cc_json_int_value(value) : fallback;
}

/*
 * 从 JSON 获取浮点数值。
 *
 * 参数:
 *   obj      - JSON 对象
 *   key      - 键名
 *   fallback - 默认值
 * 返回: 浮点数值。
 */
static double number_at(cc_json_value_t *obj, const char *key, double fallback)
{
    cc_json_value_t *value = cc_json_object_get(obj, key);
    return value ? cc_json_number_value(value) : fallback;
}

/*
 * 支持 snake_case / camelCase 双键名访问：先查 snake_key，未找到再查 camel_key。
 *
 * 参数:
 *   obj       - JSON 对象（NULL 直接返回 NULL）
 *   snake_key - snake_case 键名
 *   camel_key - camelCase 键名（可为 NULL）
 * 返回: 找到的 JSON 值或 NULL。
 */
static cc_json_value_t *value_at_alias(cc_json_value_t *obj, const char *snake_key, const char *camel_key)
{
    if (!obj) {
        return NULL;
    }
    cc_json_value_t *value = cc_json_object_get(obj, snake_key);
    return value || !camel_key ? value : cc_json_object_get(obj, camel_key);
}

/*
 * 带别名的字符串访问：先查 snake_key，再查 camel_key。
 *
 * 参数:
 *   obj       - JSON 对象
 *   snake_key - snake_case 键名
 *   camel_key - camelCase 键名
 * 返回: 字符串值或 NULL。
 */
static const char *string_at_alias(cc_json_value_t *obj, const char *snake_key, const char *camel_key)
{
    cc_json_value_t *value = value_at_alias(obj, snake_key, camel_key);
    return cc_json_string_value(value);
}

/*
 * 带别名的 size_t 值访问：先查 snake_key，再查 camel_key。
 *
 * 参数:
 *   obj       - JSON 对象
 *   snake_key - snake_case 键名
 *   camel_key - camelCase 键名
 *   fallback  - 默认值
 * 返回: 正数时返回对应 size_t 值，否则返回 0。
 */
static size_t size_at_alias(cc_json_value_t *obj,
                            const char *snake_key,
                            const char *camel_key,
                            size_t fallback)
{
    cc_json_value_t *value = value_at_alias(obj, snake_key, camel_key);
    if (!value) {
        return fallback;
    }
    double number = cc_json_number_value(value);
    return number > 0 ? (size_t)number : 0;
}

/*
 * 返回非空值或 fallback。
 */
static const char *nonempty_or(const char *value, const char *fallback)
{
    return value && value[0] ? value : fallback;
}

/*
 * 带默认值的字符串替换：value 非空时使用 value，否则使用 fallback。
 */
static bool replace_string_default(char **field, const char *value, const char *fallback)
{
    return replace_string(field, nonempty_or(value, fallback));
}

/*
 * 将 base 和 leaf 拼接为完整路径后写入 field（释放旧值）。
 *
 * 参数:
 *   base  - 根目录路径
 *   leaf  - 子目录/文件名
 * 返回: true 成功（含分配成功），false 内存不足。
 *
 * 自动处理分隔斜线：如果 base 不以 '/' 结尾则自动补上。
 * 使用固定 192 字节栈缓冲构造路径。
 */
static bool replace_joined_path(char **field, const char *base, const char *leaf)
{
    char path[192];
    const char *safe_base = nonempty_or(base, "");
    snprintf(path, sizeof(path), "%s%s%s",
             safe_base,
             safe_base[0] && safe_base[strlen(safe_base) - 1] == '/' ? "" : "/",
             leaf ? leaf : "");
    return replace_string(field, path);
}

/*
 * 判断字符是否为 ASCII 字母（A-Z 或 a-z）。
 *
 * 参数:
 *   ch - 待判断字符
 * 返回: 是字母返回 true。
 */
static bool is_ascii_letter(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

/*
 * 判断路径是否为 Windows 主机风格（盘符如 C:\ 或含反斜杠）。
 *
 * 参数:
 *   path - 路径字符串
 * 返回: 是主机风格路径返回 true。
 */
static bool is_host_path(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    return (is_ascii_letter(path[0]) && path[1] == ':') || strchr(path, '\\') != NULL;
}

/*
 * 校验配置中的设备路径是否为 ESP32 路径（不允许 Windows 风格如 C:\ 或反斜杠）。
 *
 * 参数:
 *   runtime - SDK 运行时配置
 *   io      - IO 配置
 * 返回: 所有路径合法返回 CC_OK，否则返回错误。
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

/*
 * 安全拷贝文本到定长缓冲区。
 *
 * 参数:
 *   dst      - 目标缓冲区
 *   dst_size - 缓冲区大小
 *   src      - 源字符串（NULL 视为空串）
 */
static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

typedef struct app_resolved_secrets {
    char *model_api_key;
    char *sta_password;
    char *ap_password;
} app_resolved_secrets_t;

static void resolved_secrets_destroy(app_resolved_secrets_t *secrets)
{
    if (!secrets) return;
    cc_secret_free(secrets->model_api_key);
    cc_secret_free(secrets->sta_password);
    cc_secret_free(secrets->ap_password);
    memset(secrets, 0, sizeof(*secrets));
}

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
        /*
         * Firmware policy: an explicit value in /usb0/config.json wins over a reference.
         * It remains only in the USB file and the runtime-owned wiped buffer; it is not copied
         * into NVS, environment variables, logs, status, or Web/UI config responses.
         */
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
        ((!model_plain || !model_plain[0]) && model &&
         string_at(model, "api_key_ref") && string_at(model, "api_key_ref")[0]) ||
        ((!sta_plain || !sta_plain[0]) && sta &&
         string_at(sta, "password_ref") && string_at(sta, "password_ref")[0]) ||
        ((!ap_plain || !ap_plain[0]) && ap &&
         string_at(ap, "password_ref") && string_at(ap, "password_ref")[0]);

    cc_secret_store_t store;
    memset(&store, 0, sizeof(store));
    bool store_available = false;
    if (reference_needed) {
        cc_result_t store_rc = cc_secret_store_get_default(&store);
        store_available = store_rc.code == CC_OK &&
                          store.vtable &&
                          store.vtable->get;
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

/*
 * 解析 network 配置节（STA/AP/mDNS），用默认值填充网络配置结构体。
 *
 * 参数:
 *   root - JSON 根对象
 *   out  - 输出网络配置结构体
 * 返回: 总是返回 true。
 *
 * 对超时、重试次数、信道和连接数做安全范围检查并回退到默认值。
 */
static bool parse_network(cc_json_value_t *root, network_config_t *out)
{
    network_config_t next = {
        .enabled = false,
        .connect_timeout_ms = 20000,
        .max_retry = 5,
        .softap_enabled = true,
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

/*
 * 解析 HTTP 客户端配置（超时、重试、日志级别、trace 开关）。
 *
 * 参数:
 *   root - JSON 根对象
 *   out  - 输出配置结构体
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

/*
 * 解析多模态能力标志（image/audio/video/file）。
 *
 * 参数:
 *   obj   - JSON 能力对象
 *   flags - 输出标志位
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

/*
 * 解析多模态配置：从 model.capabilities 和 multimodal 节合并 input/output 能力，
 * 并解析 limits（最大 artifact/Base64 大小、允许的 MIME 前缀等）。
 *
 * 参数:
 *   root - JSON 根对象
 *   out  - 输出配置结构体
 * 返回: 成功返回 CC_OK，分配失败返回 OOM 错误。
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
    out->multimodal.limits.max_artifacts =
        size_at_alias(limits, "max_artifacts", "maxArtifacts", out->multimodal.limits.max_artifacts);
    out->multimodal.limits.max_artifact_bytes =
        size_at_alias(limits, "max_artifact_bytes", "maxArtifactBytes",
                      out->multimodal.limits.max_artifact_bytes);
    out->multimodal.limits.max_base64_bytes =
        size_at_alias(limits, "max_base64_bytes", "maxBase64Bytes",
                      out->multimodal.limits.max_base64_bytes);

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

/*
 * 解析存储路径配置：data_dir、session 存储类型/路径/分片大小、memory 路径、
 * USB 日志目录以及 active_memory 配置。
 *
 * 参数:
 *   root - JSON 根对象
 *   out  - 输出 SDK 运行时配置
 *   io   - IO 配置（用于填入 USB 日志路径）
 * 返回: 成功返回 CC_OK，分配失败返回 OOM 错误。
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
        size_t segment_bytes = size_at_alias(
            storage,
            "session_segment_bytes",
            "sessionSegmentBytes",
            out->session_segment_bytes);
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

    cc_json_value_t *active = storage ? object_at(storage, "active_memory") : NULL;
    if (active) {
        out->active_memory_enabled = bool_at(active, "enabled", out->active_memory_enabled != 0) ? 1 : 0;
        out->active_memory_write_summary =
            bool_at(active, "write_summary", out->active_memory_write_summary != 0) ? 1 : 0;
        out->active_memory_max_value_chars =
            int_at(active, "max_value_chars", out->active_memory_max_value_chars);
        if (!replace_string_default(&out->active_memory_category,
                                    string_at(active, "category"),
                                    "active_summary")) {
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate active memory config");
        }
    }
    return cc_result_ok();
}

/*
 * 解析 system 配置节：system prompt、soul/user 文件、debug 模式以及
 * context window / 压缩 / summary 相关参数。
 *
 * 参数:
 *   root - JSON 根对象
 *   out  - 输出配置结构体
 * 返回: 成功返回 CC_OK，分配失败返回 OOM 错误。
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

/*
 * 解析 events 配置节：serial/display/usb_log 的开关与参数，以及所有事件路由。
 *
 * 参数:
 *   root - JSON 根对象
 *   io   - IO 配置结构体
 * 返回: 成功返回 CC_OK，分配失败返回 OOM 错误。
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
        io->usb_log.max_bytes =
            size_at_alias(usb_log, "max_bytes", "maxBytes", io->usb_log.max_bytes);
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

/*
 * 加载性能剖析配置默认值：禁用剖析、间隔为0、级别为 "basic"。
 *
 * 参数:
 *   profile - profile 配置结构（非 NULL）
 * 返回: 成功返回 CC_OK，分配失败返回 OOM 错误。
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

/*
 * 解析 profile 配置节：先加载默认值，再从 JSON 覆盖 enabled、interval_ms、level。
 * level 只允许 "basic" 或 "trace"。
 *
 * 参数:
 *   root    - JSON 根对象
 *   profile - 输出 profile 配置
 * 返回: 成功返回 CC_OK，校验失败返回错误。
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

/*
 * 解析工具策略 map：每个条目包含 name、concurrency、timeout_ms。
 * 先释放旧策略数组，再从 JSON 重建。
 *
 * 参数:
 *   policies - JSON 策略对象
 *   out      - 输出配置结构体
 * 返回: 成功返回 CC_OK，分配失败返回 OOM 错误。
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

/*
 * 解析 tools 配置节：enabled 列表、default_timeout_ms、以及工具策略（policies）。
 *
 * 参数:
 *   root - JSON 根对象
 *   out  - 输出配置结构体
 * 返回: 成功返回 CC_OK，分配失败返回 OOM 错误。
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

/*
 * 解析完整 runtime 配置的总入口：依次解析 model、runtime、workspace、storage、
 * system、events、tools、multimodal 各节，完成后执行校验。
 * 若 thinking 模式开启则强制流式模式。
 *
 * 参数:
 *   root - JSON 根对象
 *   out  - 输出 SDK 运行时配置
 *   io   - 输出 IO 配置
 * 返回: 成功返回 CC_OK，任何环节失败返回对应错误。
 */
static cc_result_t parse_runtime(cc_json_value_t *root, cc_config_t *out, app_runtime_io_config_t *io)
{
    cc_result_t rc = cc_config_load_default(out);
    if (rc.code != CC_OK) {
        return rc;
    }
    /* MCU defaults; explicit runtime fields parsed below take precedence. */
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

/*
 * 确保目录存在，不存在则创建（权限 0755）。
 *
 * 参数:
 *   path - 目录路径
 */
/*
 * 确保 workspace 和 workspace/lua 目录存在。
 *
 * 参数:
 *   config - 配置结构体（取 workspace_path 字段）
 */
static void ensure_workspace_dirs(const cc_config_t *config)
{
    const char *workspace = config && config->workspace_path ? config->workspace_path : "/usb0/workspace";
    char lua_dir[192];
    snprintf(lua_dir, sizeof(lua_dir), "%s%s%s", workspace,
             workspace[0] && workspace[strlen(workspace) - 1] == '/' ? "" : "/",
             "lua");
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
}

/*
 * 从 JSON 文本解析完整应用配置：解析 runtime、network、profile、display、camera 等。
 * 解析成功后销毁旧配置并替换为新配置，同时确保工作目录存在。
 *
 * 参数:
 *   config    - 目标配置结构体
 *   json_text - JSON 文本
 * 返回: 成功返回 ESP_OK，解析或校验失败返回 ESP_ERR_INVALID_ARG，内存不足返回 ESP_ERR_NO_MEM。
 */
static esp_err_t load_from_text(app_config_t *config,
                                const char *json_text,
                                bool allow_usb_plaintext)
{
    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(json_text, &root);
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
    free(next.runtime.api_key);
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

/*
 * 从文件读取 JSON 配置并加载。
 *
 * 参数:
 *   config - 目标配置结构体
 *   path   - JSON 配置文件路径
 * 返回: 成功返回 ESP_OK，读取或解析失败返回错误码。
 */
esp_err_t app_config_load(app_config_t *config, const char *path)
{
    if (!config || !path) {
        return ESP_ERR_INVALID_ARG;
    }
    char *json = read_text_file(path);
    if (!json) {
        ESP_LOGE(TAG, "read config failed: %s errno=%d", path, errno);
        return ESP_FAIL;
    }
    esp_err_t err = load_from_text(config, json, true);
    if (err == ESP_OK && strcmp(json, app_config_json(config)) != 0 &&
        !write_text_file_atomic(path, app_config_json(config))) {
        ESP_LOGE(TAG, "failed to durably rewrite migrated config");
        app_config_destroy(config);
        err = ESP_FAIL;
    }
    if (err == ESP_OK) ensure_workspace_dirs(&config->runtime);
    cc_secret_zero(json, strlen(json));
    free(json);
    return err;
}

/*
 * 保存配置到文件（原子写入）：先写 .tmp，备旧文件为 .bak，再 rename 到目标路径。
 * 保存前会先解析 JSON 并校验，失败则拒绝写入。
 *
 * 参数:
 *   config    - 当前配置（会被替换为新解析的配置）
 *   path      - 目标文件路径
 *   json_text - 待写入的 JSON 文本
 * 返回: 成功返回 ESP_OK，解析失败或写入失败返回错误码。
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

esp_err_t app_config_prepare(app_config_t *prepared, const char *json_text)
{
    if (!prepared || !json_text) return ESP_ERR_INVALID_ARG;
    app_config_destroy(prepared);
    return load_from_text(prepared, json_text, false);
}

static bool app_secret_placeholder(const char *value)
{
    return value &&
           (strcmp(value, "[REDACTED]") == 0 || strcmp(value, "***") == 0);
}

/*
 * Merge exactly one redacted secret field from the current USB snapshot.
 *
 * A real non-empty plaintext value from Web/UI is rejected. Empty means explicitly clear. A
 * reference remains allowed for compatibility, while a redaction placeholder or omitted pair
 * preserves the current USB value/reference.
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
        cc_json_object_set(submitted, value_field, empty);
        return ESP_OK;
    }
    if (!value_missing && !value_placeholder && incoming_value[0] == '\0') {
        cc_json_value_t *empty_ref = cc_json_create_string("");
        if (!empty_ref) return ESP_ERR_NO_MEM;
        cc_json_object_set(submitted, reference_field, empty_ref);
        return ESP_OK;
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
    cc_json_object_set(submitted, value_field, value);
    cc_json_object_set(submitted, reference_field, reference);
    return ESP_OK;
}

esp_err_t app_config_prepare_update(
    app_config_t *prepared,
    const app_config_t *current,
    const char *json_text)
{
    if (!prepared || !current || !json_text) return ESP_ERR_INVALID_ARG;
    cc_json_value_t *submitted = NULL;
    cc_json_value_t *existing = NULL;
    cc_result_t rc = cc_json_parse(json_text, &submitted);
    if (rc.code != CC_OK || !submitted || !cc_json_is_object(submitted)) {
        cc_result_free(&rc);
        if (submitted) cc_json_destroy(submitted);
        return ESP_ERR_INVALID_ARG;
    }
    cc_result_free(&rc);
    rc = cc_json_parse(app_config_json(current), &existing);
    if (rc.code != CC_OK || !existing || !cc_json_is_object(existing)) {
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
        app_config_destroy(prepared);
        /* Only current values survived the merge, so the boot-USB plaintext path is safe here. */
        err = load_from_text(prepared, merged, true);
    }
    if (merged) {
        cc_secret_zero(merged, strlen(merged));
        free(merged);
    }
    return err;
}

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

static bool runtime_key_is_hot(const char *key)
{
    return key && (strcmp(key, "thinking") == 0 ||
                   strcmp(key, "event_limit") == 0);
}

static bool network_key_is_hot(const char *key)
{
    return key && (strcmp(key, "sta") == 0 ||
                   strcmp(key, "ap") == 0 ||
                   strcmp(key, "mdns") == 0);
}

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

static bool root_key_is_fully_hot(const char *key)
{
    return key && (strcmp(key, "events") == 0 ||
                   strcmp(key, "profile") == 0 ||
                   strcmp(key, "display") == 0 ||
                   strcmp(key, "camera") == 0);
}

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

bool app_config_requires_restart(const app_config_t *current, const app_config_t *prepared)
{
    if (!current || !prepared || !current->loaded || !prepared->loaded) return true;
    cc_json_value_t *old_root = NULL;
    cc_json_value_t *new_root = NULL;
    cc_result_t old_rc = cc_json_parse(app_config_json(current), &old_root);
    cc_result_t new_rc = cc_json_parse(app_config_json(prepared), &new_root);
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

esp_err_t app_config_commit_prepared(
    app_config_t *current,
    app_config_t *prepared,
    const char *path
)
{
    if (!current || !prepared || !prepared->loaded || !path) return ESP_ERR_INVALID_ARG;
    if (!write_text_file_atomic(path, app_config_json(prepared))) return ESP_FAIL;
    app_config_destroy(current);
    *current = *prepared;
    memset(prepared, 0, sizeof(*prepared));
    return ESP_OK;
}

/*
 * 销毁应用配置，释放 runtime/io/profile/raw_json 等所有动态内存。
 *
 * 参数:
 *   config - 待销毁的配置（NULL 忽略）
 */
void app_config_destroy(app_config_t *config)
{
    if (!config) {
        return;
    }
    cc_config_destroy(&config->runtime);
    app_runtime_io_config_destroy(&config->io);
    app_profile_config_destroy(&config->profile);
    free(config->raw_json);
    memset(config, 0, sizeof(*config));
}

/*
 * 获取 SDK 运行时配置的只读指针。配置未加载时返回 NULL。
 */
const cc_config_t *app_config_runtime(const app_config_t *config)
{
    return config && config->loaded ? &config->runtime : NULL;
}

/*
 * 获取 IO 配置的只读指针。配置未加载时返回 NULL。
 */
const app_runtime_io_config_t *app_config_io(const app_config_t *config)
{
    return config && config->loaded ? &config->io : NULL;
}

/*
 * 获取网络配置的只读指针。配置未加载时返回 NULL。
 */
const network_config_t *app_config_network(const app_config_t *config)
{
    return config && config->loaded ? &config->network : NULL;
}

/*
 * 获取原始 JSON 字符串。未加载时返回 "{}"。
 */
const char *app_config_json(const app_config_t *config)
{
    return config && config->raw_json ? config->raw_json : "{}";
}

/*
 * 查询 thinking 模式是否开启。
 */
bool app_config_runtime_thinking(const app_config_t *config)
{
    return config && config->loaded && config->runtime.thinking_mode != 0;
}
