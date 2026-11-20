#include "lua_tool.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_profile.h"
#include "cc/core/cc_tool_call.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_tool.h"
#include "cc/util/cc_json.h"
#include "esp_heap_caps.h"
#include "lua_driver.h"
#include "lua_runtime.h"
#include "sdkconfig.h"

#define LUA_TOOL_QUERY_OUTPUT_MAX 4096
#define LUA_SCRIPT_SEARCH_OUTPUT_MAX 4096
#define LUA_SCRIPT_SEARCH_LIMIT_DEFAULT 20
#define LUA_SCRIPT_SEARCH_LIMIT_MAX 50
#define LUA_SCRIPT_PATH_MAX 192
#define LUA_SCRIPT_META_NAME_MAX 64
#define LUA_SCRIPT_META_TEXT_MAX 192

typedef struct {
    int mode;
    cc_filesystem_t filesystem;
} lua_tool_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t max_bytes;
} lua_tool_buffer_t;

enum {
    LUA_TOOL_MODE_CODE = 1,
    LUA_TOOL_MODE_FILE = 2,
    LUA_TOOL_MODE_DRIVER_QUERY = 3,
    LUA_TOOL_MODE_SCRIPT_QUERY = 4,
};

typedef struct {
    char path[LUA_SCRIPT_PATH_MAX];
    char name[LUA_SCRIPT_META_NAME_MAX];
    char summary[LUA_SCRIPT_META_TEXT_MAX];
    char args[LUA_SCRIPT_META_TEXT_MAX];
    char tags[LUA_SCRIPT_META_TEXT_MAX];
    char comments[LUA_SCRIPT_META_TEXT_MAX];
    size_t size;
    long mtime;
} lua_script_info_t;

/**
 * @brief 申请工具辅助块，优先 PSRAM 并回退内部 RAM。
 * @param[in] size 字节数，零规范为一字节。
 * @return 调用者拥有块，失败 NULL，由 lua_tool_release 或兼容 free 回收，可并发。
 */
static void *lua_tool_alloc(size_t size)
{
    if (size == 0) {
        size = 1;
    }
#if CONFIG_SPIRAM
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr) return ptr;
#else
    (void)size;
#endif
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/**
 * @brief 调整工具辅助拥有块，优先 PSRAM 并回退内部 RAM。
 * @param[in,out] ptr 独占拥有块，可 NULL。
 * @param[in] size 新字节数，零规范为一字节。
 * @return 成功新拥有块且旧指针失效，失败 NULL 且原块保持。
 */
static void *lua_tool_resize(void *ptr, size_t size)
{
    if (size == 0) {
        size = 1;
    }
#if CONFIG_SPIRAM
    void *next = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (next) return next;
#else
    (void)size;
#endif
    return heap_caps_realloc(ptr, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/**
 * @brief 归还工具辅助拥有块。
 * @param[in] ptr 转移拥有权的能力堆指针，可 NULL。
 * @post 非空块失效，调用者清理其引用，禁止并发访问同一块。
 */
static void lua_tool_release(void *ptr)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
}

/**
 * @brief 复制字符串到工具辅助存储。
 * @param[in] text 调用期 NUL 借用，NULL 按空串。
 * @return 调用者拥有副本，PSRAM/内部 RAM 均失败时 NULL，由兼容 free 回收。
 */
static char *lua_tool_dup(const char *text)
{
    const char *safe = text ? text : "";
    size_t len = strlen(safe);
    char *copy = (char *)lua_tool_alloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, safe, len + 1);
    return copy;
}

/**
 * @brief 将已分配诊断交给结果对象。
 * @param[in] code 按值状态码。
 * @param[in] message 转移拥有权的诊断，可 NULL。
 * @return 拥有字段由接收者 cc_result_free 回收，无额外分配。
 */
static cc_result_t lua_tool_result_owned(cc_error_code_t code, char *message)
{
    cc_result_t result;
    memset(&result, 0, sizeof(result));
    result.size = sizeof(result);
    result.code = code;
    result.message = message;
    return result;
}

/**
 * @brief 复制诊断并构造系统级结果。
 * @param[in] code 按值状态码，诊断分配失败仍保留。
 * @param[in] message 调用期 NUL 借用，NULL 按空串。
 * @return 拥有诊断由 cc_result_free，OOM 时 message 可 NULL。
 */
static cc_result_t lua_tool_system_error(cc_error_code_t code, const char *message)
{
    return lua_tool_result_owned(code, lua_tool_dup(message));
}

/**
 * @brief 在内容预算加一范围内扩容独占缓冲。
 * @param[in,out] buffer 调用期独占状态借用，NULL 返回 false。
 * @param[in] needed 包含 NUL 的所需容量。
 * @return 已满足或扩容成功 true，溢出/超预算/OOM false，失败保留原块及容量。
 */
static bool lua_tool_buffer_reserve(lua_tool_buffer_t *buffer, size_t needed)
{
    if (!buffer) return false;
    if (needed <= buffer->cap) {
        return true;
    }

    size_t hard_cap = buffer->max_bytes + 1;
    if (hard_cap == 0) {
        return false;
    }
    if (needed > hard_cap) return false;

    size_t next_cap = buffer->cap ? buffer->cap : 256;
    while (next_cap < needed) {
        if (next_cap > hard_cap / 2) {
            next_cap = hard_cap;
            break;
        }
        next_cap *= 2;
    }
    if (next_cap > hard_cap) {
        next_cap = hard_cap;
    }

    char *next = (char *)lua_tool_resize(buffer->data, next_cap);
    if (!next) {
        return false;
    }
    buffer->data = next;
    buffer->cap = next_cap;
    return true;
}

/**
 * @brief 按剩余字节预算追加数据并静默裁剪。
 * @param[in,out] buffer 独占缓冲借用，NULL 跳过。
 * @param[in] text 至少 len 字节借用，NULL 跳过，禁止与可搬迁输出块别名。
 * @param[in] len 输入字节数，零跳过。
 * @return 接纳/截断 true，扩容失败 false，原块保持，由 buffer 继续拥有。
 */
static bool lua_tool_buffer_append_len(lua_tool_buffer_t *buffer, const char *text, size_t len)
{
    if (!buffer || !text || len == 0) {
        return true;
    }
    size_t room = buffer->max_bytes > buffer->len ? buffer->max_bytes - buffer->len : 0;
    if (len > room) len = room;
    if (len == 0) {
        return true;
    }
    if (!lua_tool_buffer_reserve(buffer, buffer->len + len + 1)) {
        return false;
    }
    memcpy(buffer->data + buffer->len, text, len);
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return true;
}

/**
 * @brief 追加可选 NUL 字符串。
 * @param[in,out] buffer 独占缓冲借用，可 NULL 时跳过。
 * @param[in] text 调用期非别名借用，NULL 按空串。
 * @return 接纳/截断 true，扩容失败 false，无拥有权转交。
 */
static bool lua_tool_buffer_append(lua_tool_buffer_t *buffer, const char *text)
{
    return lua_tool_buffer_append_len(buffer, text, text ? strlen(text) : 0);
}

/**
 * @brief 格式化后在字节预算内追加，短文本使用栈缓冲。
 * @param[in,out] buffer 独占缓冲借用。
 * @param[in] fmt 调用期 printf 格式借用，NULL 按空串。
 * @param[in] ... 与格式匹配并覆盖两次格式化的参数借用。
 * @return 接纳/截断 true，格式化或分配失败 false，临时拥有块始终释放。
 */
static bool lua_tool_buffer_appendf(lua_tool_buffer_t *buffer, const char *fmt, ...)
{
    char stack[256];
    va_list args;

    va_start(args, fmt);
    va_list measure_args;
    va_copy(measure_args, args);
    int needed = vsnprintf(stack, sizeof(stack), fmt ? fmt : "", measure_args);
    va_end(measure_args);
    if (needed < 0) {
        va_end(args);
        return false;
    }

    bool ok = false;
    if ((size_t)needed < sizeof(stack)) {
        ok = lua_tool_buffer_append_len(buffer, stack, (size_t)needed);
    } else {
        char *tmp = (char *)lua_tool_alloc((size_t)needed + 1);
        if (tmp) {
            int written = vsnprintf(tmp, (size_t)needed + 1, fmt ? fmt : "", args);
            if (written >= 0 && written <= needed)
                ok = lua_tool_buffer_append_len(buffer, tmp, (size_t)written);
            lua_tool_release(tmp);
        }
    }
    va_end(args);
    return ok;
}

/**
 * @brief 向空结果交付工具失败诊断。
 * @param[out] out_result 必填独占空结果，拥有诊断由 cc_tool_result_cleanup。
 * @param[in] message 调用期 NUL 借用，NULL 用默认错误。
 * @return 成功分配 CC_OK，OOM 返回系统错误，其诊断由 cc_result_free。
 */
static cc_result_t lua_tool_set_error(cc_tool_result_t *out_result, const char *message)
{
    out_result->ok = 0;
    out_result->error = lua_tool_dup(message ? message : "Lua tool failed");
    if (!out_result->error) {
        return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua tool error in PSRAM");
    }
    return cc_result_ok();
}

/**
 * @brief 查询实例模式对应的工具名称。
 * @param[in] self 调用期实例借用，NULL/未知模式采用代码工具。
 * @return 常驻只读名称借用，无分配，实例存活期间可并发读取。
 */
static const char *lua_tool_name(void *self)
{
    lua_tool_t *tool = (lua_tool_t *)self;
    if (tool && tool->mode == LUA_TOOL_MODE_FILE) {
        return "lua_run_file";
    }
    if (tool && tool->mode == LUA_TOOL_MODE_DRIVER_QUERY) {
        return "lua_driver_info";
    }
    if (tool && tool->mode == LUA_TOOL_MODE_SCRIPT_QUERY) {
        return "lua_script_search";
    }
    return "lua_run_code";
}

/**
 * @brief 查询实例模式对应的功能描述。
 * @param[in] self 调用期实例借用，NULL/未知模式采用代码工具。
 * @return 常驻只读描述借用，无分配，实例存活期间可并发读取。
 */
static const char *lua_tool_description(void *self)
{
    lua_tool_t *tool = (lua_tool_t *)self;
    if (tool && tool->mode == LUA_TOOL_MODE_FILE) {
        return "Run a bounded Lua script from the workspace lua directory after file_write writes lua/name.lua";
    }
    if (tool && tool->mode == LUA_TOOL_MODE_DRIVER_QUERY) {
        return "List available Lua drivers or explain one driver before writing Lua code";
    }
    if (tool && tool->mode == LUA_TOOL_MODE_SCRIPT_QUERY) {
        return "Find reusable Lua scripts in the workspace lua directory before creating or running Lua files";
    }
    return "Run bounded Lua source code directly with safe standard libraries and C-Claw Lua drivers";
}

/**
 * @brief 查询代码、文件、驱动查询或脚本搜索模式的 Schema。
 * @param[in] self 调用期实例借用，NULL/未知模式采用代码工具。
 * @return 常驻只读 JSON 借用，无分配，可并发读取。
 */
static const char *lua_tool_schema_json(void *self)
{
    lua_tool_t *tool = (lua_tool_t *)self;
    if (tool && tool->mode == LUA_TOOL_MODE_FILE) {
        return "{"
            "\"type\":\"object\","
            "\"properties\":{"
                "\"path\":{\"type\":\"string\",\"description\":\"Workspace-relative Lua script path such as lua/name.lua\"},"
                "\"max_script_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32768},"
                "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000},"
                "\"max_output_bytes\":{\"type\":\"integer\",\"minimum\":128,\"maximum\":8192},"
                "\"max_heap_bytes\":{\"type\":\"integer\",\"minimum\":16384,\"maximum\":262144}"
            "},"
            "\"required\":[\"path\"]"
        "}";
    }
    if (tool && tool->mode == LUA_TOOL_MODE_DRIVER_QUERY) {
        return "{"
            "\"type\":\"object\","
            "\"properties\":{"
                "\"driver\":{\"type\":\"string\",\"description\":\"Optional Lua driver name such as screen or cclaw. Omit to list all drivers\"}"
            "}"
        "}";
    }
    if (tool && tool->mode == LUA_TOOL_MODE_SCRIPT_QUERY) {
        return "{"
            "\"type\":\"object\","
            "\"properties\":{"
                "\"query\":{\"type\":\"string\",\"description\":\"Optional keyword to match script name, summary, args, tags, or path\"},"
                "\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":50},"
                "\"path\":{\"type\":\"string\",\"description\":\"Optional lua/name.lua or script name under the workspace lua directory\"}"
            "}"
        "}";
    }
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"code\":{\"type\":\"string\",\"description\":\"Lua source code to execute directly\"},"
            "\"max_script_bytes\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":32768},"
            "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":5000},"
            "\"max_output_bytes\":{\"type\":\"integer\",\"minimum\":128,\"maximum\":8192},"
            "\"max_heap_bytes\":{\"type\":\"integer\",\"minimum\":16384,\"maximum\":262144}"
        "},"
        "\"required\":[\"code\"]"
    "}";
}

/**
 * @brief 读取整数字段并按上下界钳位。
 * @param[in] root 调用期 JSON 对象借用。
 * @param[in] key 非 NULL 调用期 NUL 键名借用。
 * @param[in] default_value 字段缺失时返回值。
 * @param[in] min_value 允许下界，不大于 max_value。
 * @param[in] max_value 允许上界。
 * @return 字段经 JSON 整数转换及钳位的值，缺失用默认值，无分配。
 */
static int lua_json_int_or_default(cc_json_value_t *root,
                                   const char *key,
                                   int default_value,
                                   int min_value,
                                   int max_value)
{
    cc_json_value_t *value = cc_json_object_get(root, key);
    if (!value) {
        return default_value;
    }

    int parsed = cc_json_int_value(value);
    if (parsed < min_value) {
        return min_value;
    }
    if (parsed > max_value) {
        return max_value;
    }
    return parsed;
}

/**
 * @brief 解析运行预算并收紧到工具上下文的正超时。
 * @param[in] root 调用期 JSON 对象借用。
 * @param[in] ctx 调用期上下文借用，可 NULL，取消令牌借用传给同步执行。
 * @return 按值限制，字节及时间字段按 Schema 范围钳位，无拥有资源。
 * @pre 上下文取消令牌存活覆盖随后执行，SDK 锁内查询允许其他任务并发取消。
 */
static lua_runtime_limits_t lua_tool_parse_limits(cc_json_value_t *root,
                                                  const cc_tool_context_t *ctx)
{
    lua_runtime_limits_t limits = lua_runtime_default_limits();
    limits.cancel_token = ctx ? ctx->cancel_token : NULL;
    limits.max_script_bytes = (size_t)lua_json_int_or_default(root,
                                                             "max_script_bytes",
                                                             (int)limits.max_script_bytes,
                                                             1,
                                                             32768);
    limits.timeout_ms = lua_json_int_or_default(root, "timeout_ms", limits.timeout_ms, 1, 5000);
    limits.max_output_bytes = (size_t)lua_json_int_or_default(root,
                                                             "max_output_bytes",
                                                             (int)limits.max_output_bytes,
                                                             128,
                                                             8192);
    limits.max_heap_bytes = (size_t)lua_json_int_or_default(root,
                                                           "max_heap_bytes",
                                                           (int)limits.max_heap_bytes,
                                                           16 * 1024,
                                                           256 * 1024);
    if (ctx && ctx->timeout_ms > 0 && limits.timeout_ms > ctx->timeout_ms) {
        limits.timeout_ms = ctx->timeout_ms;
    }
    return limits;
}

/**
 * @brief 格式化全部常驻驱动信息。
 * @param[out] out_result 必填独占空结果，成功取得 text 拥有权，由 cc_tool_result_cleanup。
 * @return CC_OK 或 OOM，诊断由 cc_result_free，失败释放缓冲，输出最多 4096 字节。
 */
static cc_result_t lua_driver_info_all(cc_tool_result_t *out_result)
{
    lua_tool_buffer_t buffer = {
        .max_bytes = LUA_TOOL_QUERY_OUTPUT_MAX,
    };

    if (!lua_tool_buffer_append(&buffer, "Available Lua drivers:\n")) {
        lua_tool_release(buffer.data);
        return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua driver list in PSRAM");
    }
    for (size_t i = 0; i < lua_driver_count(); i++) {
        const lua_driver_descriptor_t *driver = lua_driver_at(i);
        if (!driver) {
            continue;
        }
        if (!lua_tool_buffer_appendf(&buffer,
                                     "- %s (global %s): %s\n",
                                     driver->name ? driver->name : "",
                                     driver->global_name ? driver->global_name : "",
                                     driver->summary ? driver->summary : "")) {
            lua_tool_release(buffer.data);
            return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua driver list in PSRAM");
        }
    }
    if (!lua_tool_buffer_append(&buffer, "Query one driver with {\"driver\":\"screen\"} before writing driver-specific Lua.\n")) {
        lua_tool_release(buffer.data);
        return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua driver list in PSRAM");
    }

    out_result->ok = 1;
    out_result->text = buffer.data;
    return cc_result_ok();
}

/**
 * @brief 格式化指定常驻驱动的详细文档。
 * @param[in] name 调用期 NUL 名称借用，NULL 无匹配。
 * @param[out] out_result 必填独占空结果，拥有文本/诊断由 cc_tool_result_cleanup。
 * @return CC_OK，未知驱动令 ok 为零，OOM 为系统错误并清理缓冲，诊断由 cc_result_free。
 */
static cc_result_t lua_driver_info_one(const char *name, cc_tool_result_t *out_result)
{
    const lua_driver_descriptor_t *driver = lua_driver_find(name);
    if (!driver) {
        char message[160];
        snprintf(message,
                 sizeof(message),
                 "Unknown Lua driver: %s. Call lua_driver_info without driver to list available drivers.",
                 name ? name : "");
        return lua_tool_set_error(out_result, message);
    }

    lua_tool_buffer_t buffer = {
        .max_bytes = LUA_TOOL_QUERY_OUTPUT_MAX,
    };
    if (!lua_tool_buffer_appendf(&buffer,
                                 "Lua driver: %s\nGlobal: %s\nSummary: %s\n\n%s",
                                 driver->name ? driver->name : "",
                                 driver->global_name ? driver->global_name : "",
                                 driver->summary ? driver->summary : "",
                                 driver->docs ? driver->docs : "")) {
        lua_tool_release(buffer.data);
        return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua driver docs in PSRAM");
    }

    out_result->ok = 1;
    out_result->text = buffer.data;
    return cc_result_ok();
}

/**
 * @brief 按可选 driver 字段分发驱动查询。
 * @param[in] root 调用期 JSON 借用，字段字符串不逸出。
 * @param[out] out_result 必填独占空结果，拥有字段由 cc_tool_result_cleanup。
 * @return 下层状态，系统诊断由 cc_result_free，空名称列出全部。
 */
static cc_result_t lua_driver_info_call(cc_json_value_t *root,
                                         cc_tool_result_t *out_result)
{
    const char *driver = cc_json_string_value(cc_json_object_get(root, "driver"));
    if (!driver || !driver[0]) {
        return lua_driver_info_all(out_result);
    }
    return lua_driver_info_one(driver, out_result);
}

/**
 * @brief 检查区分大小写的前缀。
 * @param[in] text 调用期 NUL 文本借用，可 NULL。
 * @param[in] prefix 调用期 NUL 前缀借用，可 NULL。
 * @return 匹配 true，参数 NULL 或不匹配 false，无分配。
 */
static bool lua_script_starts_with(const char *text, const char *prefix)
{
    return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

/**
 * @brief 检查区分大小写的后缀。
 * @param[in] text 调用期 NUL 文本借用，可 NULL。
 * @param[in] suffix 调用期 NUL 后缀借用，可 NULL。
 * @return 匹配 true，参数 NULL 或不匹配 false，无分配。
 */
static bool lua_script_ends_with(const char *text, const char *suffix)
{
    if (!text || !suffix) return false;
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len && strcmp(text + text_len - suffix_len, suffix) == 0;
}

/**
 * @brief 原地裁剪空格、制表、回车及换行。
 * @param[in,out] text 独占可写 NUL 文本借用，可 NULL。
 * @return 原块内裁剪后起点借用，NULL 输入返回 NULL，尾部写 NUL，无分配。
 */
static char *lua_script_trim(char *text)
{
    if (!text) return NULL;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    char *end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return text;
}

/**
 * @brief 将文本按容量截断复制到字段。
 * @param[out] dst 独占输出数组借用，NULL 跳过。
 * @param[in] dst_size 含 NUL 容量，零跳过。
 * @param[in] src 调用期非重叠 NUL 借用，NULL 按空串。
 * @post 有效输出始终 NUL 终止，无分配，无拥有权转移。
 */
static void lua_script_copy_field(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

/**
 * @brief 拼接目录与名称并按需补斜杠。
 * @param[out] out 独占输出数组借用，与输入不重叠。
 * @param[in] out_size 含 NUL 容量。
 * @param[in] dir 调用期非 NULL NUL 目录借用，内部长度受路径数组上限约束。
 * @param[in] name 调用期非 NULL NUL 名称借用。
 * @return 成功 true，参数无效或容量不足 false，失败不写输出，无分配。
 */
static bool lua_script_join_path(char *out, size_t out_size, const char *dir, const char *name)
{
    if (!out || out_size == 0 || !dir || !name) return false;
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);
    bool needs_slash = dir_len > 0 && dir[dir_len - 1] != '/';
    size_t total = dir_len + (needs_slash ? 1U : 0U) + name_len;
    if (total >= out_size) return false;
    memcpy(out, dir, dir_len);
    size_t pos = dir_len;
    if (needs_slash) out[pos++] = '/';
    memcpy(out + pos, name, name_len);
    out[total] = '\0';
    return true;
}

/**
 * @brief 构造以斜杠结尾的工作区 lua 根路径。
 * @param[in] ctx 调用期上下文借用，NULL/空工作区用默认目录。
 * @param[out] out_root 独占路径数组借用，与输入不重叠。
 * @param[in] out_root_size 含 NUL 容量。
 * @return 成功 true，参数无效或容量不足 false，失败可能保留部分路径，无分配。
 */
static bool lua_script_build_root(const cc_tool_context_t *ctx,
                                  char *out_root,
                                  size_t out_root_size)
{
    const char *workspace = (ctx && ctx->workspace_dir && ctx->workspace_dir[0]) ?
        ctx->workspace_dir : LUA_RUNTIME_DEFAULT_WORKSPACE_DIR;
    if (!lua_script_join_path(out_root, out_root_size, workspace, LUA_RUNTIME_SCRIPT_SUBDIR)) {
        return false;
    }
    size_t len = strlen(out_root);
    if (len + 1 >= out_root_size) return false;
    if (len == 0 || out_root[len - 1] != '/') {
        out_root[len++] = '/';
        out_root[len] = '\0';
    }
    return true;
}

/**
 * @brief 检查空路径及双点、反斜杠、冒号。
 * @param[in] path 调用期 NUL 借用，可 NULL。
 * @return 含禁用文本或缺失为 true，否则 false，无分配。
 */
static bool lua_script_path_is_unsafe(const char *path)
{
    return !path || !path[0] ||
           strstr(path, "..") != NULL ||
           strchr(path, '\\') != NULL ||
           strchr(path, ':') != NULL;
}

/**
 * @brief 构造工作区绝对路径与 lua/ 相对运行路径。
 * @param[in] ctx 调用期上下文借用，可 NULL 用默认工作区。
 * @param[in] path 调用期相对 .lua 路径借用，禁止双点、反斜杠、冒号及起始斜杠。
 * @param[out] out_abs 独占绝对路径数组借用，与输入不重叠。
 * @param[in] out_abs_size 绝对路径含 NUL 容量。
 * @param[out] out_run_path 独占运行路径数组借用，与其他数组不重叠。
 * @param[in] out_run_path_size 运行路径含 NUL 容量，内部调用统一为 192。
 * @return 成功 true，校验/容量失败 false，失败输出可部分写入，无分配。
 */
static bool lua_script_resolve_path(const cc_tool_context_t *ctx,
                                    const char *path,
                                    char *out_abs,
                                    size_t out_abs_size,
                                    char *out_run_path,
                                    size_t out_run_path_size)
{
    if (lua_script_path_is_unsafe(path) || path[0] == '/' ||
        !lua_script_ends_with(path, ".lua")) {
        return false;
    }
    char root[LUA_SCRIPT_PATH_MAX];
    if (!lua_script_build_root(ctx, root, sizeof(root))) return false;

    if (lua_script_starts_with(path, LUA_RUNTIME_SCRIPT_SUBDIR "/")) {
        const char *workspace = (ctx && ctx->workspace_dir && ctx->workspace_dir[0]) ?
            ctx->workspace_dir : LUA_RUNTIME_DEFAULT_WORKSPACE_DIR;
        if (!lua_script_join_path(out_abs, out_abs_size, workspace, path)) return false;
        lua_script_copy_field(out_run_path, out_run_path_size, path);
        return true;
    }
    if (!lua_script_join_path(out_abs, out_abs_size, root, path)) return false;
    if (!lua_script_join_path(out_run_path, out_run_path_size, LUA_RUNTIME_SCRIPT_SUBDIR, path)) {
        return false;
    }
    return true;
}

/**
 * @brief 从注释体起点匹配元数据键并跳过空白或冒号。
 * @param[in] body 调用期 NUL 注释借用，可 NULL。
 * @param[in] key 必填调用期 NUL 键名借用。
 * @return body 内值部分借用，未匹配返回 NULL，无分配，返回值存活依赖 body。
 */
static const char *lua_script_meta_value(const char *body, const char *key)
{
    size_t key_len = strlen(key);
    if (!body || strncmp(body, key, key_len) != 0) return NULL;
    char c = body[key_len];
    if (c != ' ' && c != '\t' && c != ':') return NULL;
    body += key_len;
    while (*body == ' ' || *body == '\t' || *body == ':') body++;
    return body;
}

/**
 * @brief 在固定 comments 字段按空格追加并截断文本。
 * @param[in,out] info 独占有效信息借用，NULL 跳过。
 * @param[in] text 调用期非别名 NUL 借用，NULL/空跳过。
 * @post 字段 NUL 终止，容量不足静默保留或截断，无分配。
 */
static void lua_script_append_comment(lua_script_info_t *info, const char *text)
{
    if (!info || !text || !text[0]) return;
    size_t used = strlen(info->comments);
    if (used + 2 >= sizeof(info->comments)) return;
    if (used > 0) {
        info->comments[used++] = ' ';
        info->comments[used] = '\0';
    }
    snprintf(info->comments + used, sizeof(info->comments) - used, "%s", text);
}

/**
 * @brief 从路径末段提取默认名称并去掉可见 .lua 后缀。
 * @param[out] info 必填独占信息借用，只写 name 字段。
 * @param[in] run_path 必填调用期 NUL 路径借用，与输出不重叠。
 * @post 名称按字段容量截断，无分配，无拥有权转交。
 */
static void lua_script_default_name(lua_script_info_t *info, const char *run_path)
{
    const char *name = strrchr(run_path, '/');
    name = name ? name + 1 : run_path;
    lua_script_copy_field(info->name, sizeof(info->name), name);
    char *dot = strrchr(info->name, '.');
    if (dot && strcmp(dot, ".lua") == 0) {
        *dot = '\0';
    }
}

/**
 * @brief 读取脚本并解析前 24 行头注释元数据及可选文件统计。
 * @param[in] tool 调用期实例借用，要求文本读取端口，端口由工厂拥有。
 * @param[in] ctx 调用期上下文借用，可 NULL 用默认工作区。
 * @param[in] abs_path 调用期 NUL 绝对路径借用，供可选 stat 使用。
 * @param[in] run_path 调用期 NUL 相对路径借用，供 root-scoped 读取使用。
 * @param[out] info 必填独占结构，成功包含副本，失败可能部分写入。
 * @return 读取解析成功 true，参数或读取失败 false，临时文件文本始终 free。
 * @note 完整文件先读入再分析，stat 失败时统计为零，端口负责并发。
 */
static bool lua_script_parse_info(const lua_tool_t *tool,
                                  const cc_tool_context_t *ctx,
                                  const char *abs_path,
                                  const char *run_path,
                                  lua_script_info_t *info)
{
    if (!tool || !tool->filesystem.vtable ||
        !tool->filesystem.vtable->read_relative_text ||
        !abs_path || !run_path || !info) return false;
    memset(info, 0, sizeof(*info));
    lua_script_copy_field(info->path, sizeof(info->path), run_path);
    lua_script_default_name(info, run_path);

    if (tool->filesystem.vtable->stat) {
        cc_filesystem_stat_t st = {0};
        cc_result_t stat_rc = tool->filesystem.vtable->stat(
            tool->filesystem.self, abs_path, &st);
        if (stat_rc.code == CC_OK) {
            info->size = st.byte_size > SIZE_MAX ? SIZE_MAX : (size_t)st.byte_size;
            info->mtime = (long)(st.modified_ms / 1000U);
        }
        cc_result_free(&stat_rc);
    }

    const char *workspace = ctx && ctx->workspace_dir && ctx->workspace_dir[0] ?
        ctx->workspace_dir : LUA_RUNTIME_DEFAULT_WORKSPACE_DIR;
    char *text = NULL;
    cc_result_t read_rc = tool->filesystem.vtable->read_relative_text(
        tool->filesystem.self, workspace, run_path, &text);
    if (read_rc.code != CC_OK) {
        cc_result_free(&read_rc);
        return false;
    }
    cc_result_free(&read_rc);
    char *line = text;
    int lines = 0;
    while (line && line[0] && lines++ < 24) {
        char *current = line;
        char *next = strchr(current, '\n');
        if (next) *next++ = '\0';
        line = next;
        char *p = lua_script_trim(current);
        if (!p || !p[0]) continue;
        if (strncmp(p, "--", 2) != 0) break;
        char *body = lua_script_trim(p + 2);
        const char *value = NULL;
        if ((value = lua_script_meta_value(body, "@name")) != NULL) {
            lua_script_copy_field(info->name, sizeof(info->name), value);
        } else if ((value = lua_script_meta_value(body, "@summary")) != NULL) {
            lua_script_copy_field(info->summary, sizeof(info->summary), value);
        } else if ((value = lua_script_meta_value(body, "@args")) != NULL) {
            lua_script_copy_field(info->args, sizeof(info->args), value);
        } else if ((value = lua_script_meta_value(body, "@tags")) != NULL) {
            lua_script_copy_field(info->tags, sizeof(info->tags), value);
        } else if (body[0] != '@') {
            lua_script_append_comment(info, body);
        }
    }
    free(text);
    if (!info->summary[0] && info->comments[0]) {
        lua_script_copy_field(info->summary, sizeof(info->summary), info->comments);
    }
    return true;
}

/**
 * @brief 按 C locale 逐字节执行忽略大小写的子串搜索。
 * @param[in] text 调用期 NUL 借用，可 NULL。
 * @param[in] needle 调用期 NUL 借用，NULL/空匹配全部。
 * @return 匹配 true，否则 false，无分配，非空关键词下 NULL 文本无匹配。
 */
static bool lua_script_contains_ci(const char *text, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!text) return false;
    size_t needle_len = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < needle_len &&
               p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) return true;
    }
    return false;
}

/**
 * @brief 在六个元数据字段中匹配查询关键词。
 * @param[in] info 非 NULL 调用期有效信息借用。
 * @param[in] query 调用期 NUL 借用，NULL/空匹配全部。
 * @return 任一字段忽略大小写匹配为 true，否则 false，无分配。
 */
static bool lua_script_matches(const lua_script_info_t *info, const char *query)
{
    return !query || !query[0] ||
           lua_script_contains_ci(info->name, query) ||
           lua_script_contains_ci(info->summary, query) ||
           lua_script_contains_ci(info->args, query) ||
           lua_script_contains_ci(info->tags, query) ||
           lua_script_contains_ci(info->path, query) ||
           lua_script_contains_ci(info->comments, query);
}

/**
 * @brief 格式化脚本信息并按预算追加到缓冲。
 * @param[in,out] buffer 独占输出状态借用，持有扩容块。
 * @param[in] info 非 NULL 调用期有效信息借用。
 * @param[in] detail 是否包含 comments 字段。
 * @return 接纳/截断 true，格式化或分配失败 false，失败可能已有部分追加。
 */
static bool lua_script_append_info(lua_tool_buffer_t *buffer,
                                   const lua_script_info_t *info,
                                   bool detail)
{
    if (!lua_tool_buffer_appendf(buffer,
                                 "- %s (%s, %u bytes, mtime=%ld)\n",
                                 info->name,
                                 info->path,
                                 (unsigned)info->size,
                                 info->mtime)) {
        return false;
    }
    if (info->summary[0] &&
        !lua_tool_buffer_appendf(buffer, "  summary: %s\n", info->summary)) {
        return false;
    }
    if (info->args[0] &&
        !lua_tool_buffer_appendf(buffer, "  args: %s\n", info->args)) {
        return false;
    }
    if (info->tags[0] &&
        !lua_tool_buffer_appendf(buffer, "  tags: %s\n", info->tags)) {
        return false;
    }
    if (detail && info->comments[0] &&
        !lua_tool_buffer_appendf(buffer, "  comments: %s\n", info->comments)) {
        return false;
    }
    return true;
}

/**
 * @brief 按路径读取并交付一个脚本的详细元数据。
 * @param[in] tool 调用期工具借用，持有工厂文件端口借用。
 * @param[in] ctx 调用期上下文借用，可 NULL 用默认工作区。
 * @param[in] path 调用期 NUL 相对脚本路径借用。
 * @param[out] out_result 必填独占空结果，拥有字段由 cc_tool_result_cleanup。
 * @return CC_OK 且 ok 表示查找结果，输出分配失败返回 OOM，系统诊断由 cc_result_free。
 * @note 失败释放中间缓冲，文件端口负责并发。
 */
static cc_result_t lua_script_search_path(const lua_tool_t *tool,
                                         const cc_tool_context_t *ctx,
                                         const char *path,
                                         cc_tool_result_t *out_result)
{
    char abs_path[LUA_SCRIPT_PATH_MAX];
    char run_path[LUA_SCRIPT_PATH_MAX];
    if (!lua_script_resolve_path(ctx, path, abs_path, sizeof(abs_path), run_path, sizeof(run_path))) {
        return lua_tool_set_error(out_result, "lua_script_search path must stay under workspace lua directory and end with .lua");
    }
    lua_script_info_t info;
    if (!lua_script_parse_info(tool, ctx, abs_path, run_path, &info)) {
        return lua_tool_set_error(out_result, "Lua script not found or unreadable");
    }
    lua_tool_buffer_t buffer = {.max_bytes = LUA_SCRIPT_SEARCH_OUTPUT_MAX};
    if (!lua_tool_buffer_append(&buffer, "Lua script:\n") ||
        !lua_script_append_info(&buffer, &info, true)) {
        lua_tool_release(buffer.data);
        return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua script result in PSRAM");
    }
    out_result->ok = 1;
    out_result->text = buffer.data;
    return cc_result_ok();
}

/**
 * @brief 枚举 lua 目录直接条目并按元数据关键词筛选。
 * @param[in] tool 非 NULL 调用期工具借用，端口必须含 list_dir 和文本读取。
 * @param[in] ctx 调用期上下文借用，可 NULL 用默认工作区。
 * @param[in] query 调用期 NUL 关键词借用，NULL/空匹配全部。
 * @param[in] limit 返回条目上限，非正用 20，大于 50 钳位到 50。
 * @param[out] out_result 必填独占空结果，拥有字段由 cc_tool_result_cleanup。
 * @return CC_OK 或 OOM，系统诊断由 cc_result_free，目录不可用作为成功说明文本交付。
 * @note 列表、文件和中间缓冲按路径释放，读取失败跳过条目，端口负责并发。
 */
static cc_result_t lua_script_search_all(const lua_tool_t *tool,
                                        const cc_tool_context_t *ctx,
                                        const char *query,
                                        int limit,
                                        cc_tool_result_t *out_result)
{
    char root[LUA_SCRIPT_PATH_MAX];
    if (!lua_script_build_root(ctx, root, sizeof(root))) {
        return lua_tool_set_error(out_result, "Lua script root path is too long");
    }
    if (limit <= 0) limit = LUA_SCRIPT_SEARCH_LIMIT_DEFAULT;
    if (limit > LUA_SCRIPT_SEARCH_LIMIT_MAX) limit = LUA_SCRIPT_SEARCH_LIMIT_MAX;

    lua_tool_buffer_t buffer = {.max_bytes = LUA_SCRIPT_SEARCH_OUTPUT_MAX};
    if (!lua_tool_buffer_appendf(&buffer, "Lua scripts under %s:\n", root)) {
        lua_tool_release(buffer.data);
        return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua script query in PSRAM");
    }

    char **entries = NULL;
    size_t entry_count = 0;
    cc_result_t list_rc = tool->filesystem.vtable->list_dir(
        tool->filesystem.self, root, &entries, &entry_count);
    if (list_rc.code != CC_OK) {
        cc_result_free(&list_rc);
        if (!lua_tool_buffer_append(&buffer, "Directory unavailable or empty. Create scripts with file_write under lua/name.lua.\n")) {
            lua_tool_release(buffer.data);
            return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua script query in PSRAM");
        }
        out_result->ok = 1;
        out_result->text = buffer.data;
        return cc_result_ok();
    }

    int matched = 0;
    cc_result_free(&list_rc);
    for (size_t entry_i = 0; entry_i < entry_count && matched < limit; entry_i++) {
        const char *entry = entries[entry_i];
        if (!entry || entry[0] == '\0' || entry[0] == '.' ||
            !lua_script_ends_with(entry, ".lua")) {
            continue;
        }
        char abs_path[LUA_SCRIPT_PATH_MAX];
        char run_path[LUA_SCRIPT_PATH_MAX];
        if (!lua_script_join_path(abs_path, sizeof(abs_path), root, entry)) continue;
        if (!lua_script_join_path(run_path, sizeof(run_path), LUA_RUNTIME_SCRIPT_SUBDIR, entry)) continue;
        lua_script_info_t info;
        if (!lua_script_parse_info(tool, ctx, abs_path, run_path, &info)) continue;
        if (!lua_script_matches(&info, query)) continue;
        if (!lua_script_append_info(&buffer, &info, false)) {
            for (size_t i = 0; i < entry_count; i++) free(entries[i]);
            free(entries);
            lua_tool_release(buffer.data);
            return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua script query in PSRAM");
        }
        matched++;
    }
    for (size_t i = 0; i < entry_count; i++) free(entries[i]);
    free(entries);

    if (matched == 0 &&
        !lua_tool_buffer_append(&buffer, "No matching Lua scripts. Create one with file_write under lua/name.lua.\n")) {
        lua_tool_release(buffer.data);
        return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua script query in PSRAM");
    }
    if (!lua_tool_buffer_append(&buffer, "Run a script with lua_run_file using the shown lua/name.lua path.\n")) {
        lua_tool_release(buffer.data);
        return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua script query in PSRAM");
    }
    out_result->ok = 1;
    out_result->text = buffer.data;
    return cc_result_ok();
}

/**
 * @brief 根据 path 或 query/limit 分发脚本搜索。
 * @param[in] tool 非 NULL 调用期工具借用，持有有效文件端口。
 * @param[in] root 调用期 JSON 对象借用，字段借用不逸出。
 * @param[in] ctx 调用期上下文借用，可 NULL。
 * @param[out] out_result 必填独占空结果，拥有字段由 cc_tool_result_cleanup。
 * @return 下层搜索状态，系统诊断由 cc_result_free，非空 path 优先。
 */
static cc_result_t lua_script_search_call(const lua_tool_t *tool,
                                         cc_json_value_t *root,
                                         const cc_tool_context_t *ctx,
                                         cc_tool_result_t *out_result)
{
    const char *path = cc_json_string_value(cc_json_object_get(root, "path"));
    if (path && path[0]) {
        return lua_script_search_path(tool, ctx, path, out_result);
    }
    const char *query = cc_json_string_value(cc_json_object_get(root, "query"));
    int limit = lua_json_int_or_default(root,
                                        "limit",
                                        LUA_SCRIPT_SEARCH_LIMIT_DEFAULT,
                                        1,
                                        LUA_SCRIPT_SEARCH_LIMIT_MAX);
    return lua_script_search_all(tool, ctx, query, limit, out_result);
}

/**
 * @brief 同步分发驱动查询、脚本搜索、代码执行及文件执行。
 * @param[in] self 调用期实例借用，NULL 按代码模式，实例不得与销毁并发。
 * @param[in] args_json 调用期 NUL JSON 借用，NULL/空按空对象。
 * @param[in] ctx 调用期上下文借用，可 NULL，取消令牌覆盖整个执行期。
 * @param[out] out_result 必填独占空结果，入口初始化，拥有字段由 cc_tool_result_cleanup。
 * @return CC_OK 时见 ok/error，参数输出 NULL 或诊断分配失败返回系统错误，由 cc_result_free。
 * @note JSON 始终释放，执行状态独立，文件端口及 UI 同步由底层负责。
 */
static cc_result_t lua_tool_call(void *self,
                                 const char *args_json,
                                 const cc_tool_context_t *ctx,
                                 cc_tool_result_t *out_result)
{
    if (!out_result) {
        return lua_tool_system_error(CC_ERR_INVALID_ARGUMENT, "Null Lua tool result");
    }

    lua_tool_t *tool = (lua_tool_t *)self;
    int mode = tool ? tool->mode : LUA_TOOL_MODE_CODE;

    memset(out_result, 0, sizeof(*out_result));
    cc_media_artifact_list_init(&out_result->artifacts);

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(args_json && args_json[0] ? args_json : "{}", &root);
    if (rc.code != CC_OK) {
        cc_result_t err = lua_tool_set_error(out_result, rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        return err;
    }
    cc_result_free(&rc);

    if (!cc_json_is_object(root)) {
        cc_json_destroy(root);
        return lua_tool_set_error(out_result, "Lua tool args must be an object");
    }

    if (mode == LUA_TOOL_MODE_DRIVER_QUERY) {
        rc = lua_driver_info_call(root, out_result);
        cc_json_destroy(root);
        return rc;
    }
    if (mode == LUA_TOOL_MODE_SCRIPT_QUERY) {
        rc = lua_script_search_call(tool, root, ctx, out_result);
        cc_json_destroy(root);
        return rc;
    }

    const char *code = cc_json_string_value(cc_json_object_get(root, "code"));
    const char *path = cc_json_string_value(cc_json_object_get(root, "path"));
    if (mode == LUA_TOOL_MODE_CODE && (!code || !code[0])) {
        cc_json_destroy(root);
        return lua_tool_set_error(out_result, "lua_run_code missing code");
    }
    if (mode == LUA_TOOL_MODE_FILE && (!path || !path[0])) {
        cc_json_destroy(root);
        return lua_tool_set_error(out_result, "lua_run_file missing path");
    }

    lua_runtime_limits_t limits = lua_tool_parse_limits(root, ctx);
    char *text = NULL;
    app_profile_span_t run_span = app_profile_span_begin("tool.lua",
                                                         mode == LUA_TOOL_MODE_FILE ? "run_file" : "run_code",
                                                         NULL,
                                                         NULL);
    if (mode == LUA_TOOL_MODE_FILE) {
        rc = lua_runtime_run_file(&tool->filesystem,
                                  ctx ? ctx->workspace_dir : NULL,
                                  path,
                                  &limits,
                                  &text);
    } else {
        rc = lua_runtime_run_code(code, &limits, &text);
    }
    app_profile_span_end(&run_span, rc.code == CC_OK ? "ok" : "error");

    cc_json_destroy(root);
    if (rc.code != CC_OK) {
        cc_result_t err = lua_tool_set_error(out_result, rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        return err;
    }
    cc_result_free(&rc);

    out_result->ok = 1;
    out_result->text = text;
    return cc_result_ok();
}

/**
 * @brief 释放工具实例并结束文件端口借用。
 * @param[in] self 转移拥有权的实例，可 NULL，调用者先等待在途调用完成。
 * @post 实例失效，文件端口仍由工厂拥有。
 */
static void lua_tool_destroy(void *self)
{
    lua_tool_release(self);
}

static const cc_tool_vtable_t s_lua_tool_vtable = {
    lua_tool_name,
    lua_tool_description,
    lua_tool_schema_json,
    lua_tool_call,
    lua_tool_destroy,
};

/**
 * @brief 按模式创建持有文件端口借用的 Lua 工具。
 * @param[in] ctx 工厂上下文借用，要求 root-scoped 文本读取、目录枚举及 stat。
 * @param[out] out_tool 必填独占空槽，校验通过后清空，成功用 vtable destroy 回收实例。
 * @param[in] mode 内部四种模式之一。
 * @return CC_OK，参数/能力不足返回不支持，分配失败 OOM，诊断由 cc_result_free。
 * @pre 工厂文件端口存活覆盖工具及其调用，不转移端口拥有权。
 */
static cc_result_t lua_tool_create_mode(const cc_runtime_tool_factory_ctx_t *ctx,
                                        cc_tool_t *out_tool,
                                        int mode)
{
    if (!ctx || !out_tool || !ctx->filesystem.vtable ||
        !(ctx->filesystem.capabilities & CC_FS_CAP_ROOT_SCOPED) ||
        !ctx->filesystem.vtable->read_relative_text ||
        !ctx->filesystem.vtable->list_dir ||
        !ctx->filesystem.vtable->stat) {
        return lua_tool_system_error(CC_ERR_UNSUPPORTED,
                                     "Lua tools require root-scoped filesystem read/list/stat");
    }

    memset(out_tool, 0, sizeof(*out_tool));
    lua_tool_t *tool = (lua_tool_t *)lua_tool_alloc(sizeof(*tool));
    if (!tool) {
        return lua_tool_system_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua tool in PSRAM");
    }
    memset(tool, 0, sizeof(*tool));

    tool->mode = mode;
    tool->filesystem = ctx->filesystem;
    out_tool->self = tool;
    out_tool->vtable = &s_lua_tool_vtable;
    return cc_result_ok();
}

/**
 * @brief 创建代码执行工具。
 * @param[in] ctx 调用期工厂借用，端口支持 root-scoped 读、枚举、stat，存活覆盖工具期。
 * @param[out] out_tool 必填独占空槽，成功取得实例拥有权，用 vtable destroy 回收。
 * @return 创建状态，能力/参数不足为不支持，OOM 为内存错误，诊断由 cc_result_free。
 */
cc_result_t lua_run_code_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                     cc_tool_t *out_tool)
{
    return lua_tool_create_mode(ctx, out_tool, LUA_TOOL_MODE_CODE);
}

/**
 * @brief 创建工作区脚本执行工具。
 * @param[in] ctx 调用期工厂借用，端口支持 root-scoped 读、枚举、stat，存活覆盖工具期。
 * @param[out] out_tool 必填独占空槽，成功取得实例拥有权，用 vtable destroy 回收。
 * @return 创建状态，能力/参数不足为不支持，OOM 为内存错误，诊断由 cc_result_free。
 */
cc_result_t lua_run_file_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                     cc_tool_t *out_tool)
{
    return lua_tool_create_mode(ctx, out_tool, LUA_TOOL_MODE_FILE);
}

/**
 * @brief 创建驱动文档查询工具。
 * @param[in] ctx 调用期工厂借用，端口支持 root-scoped 读、枚举、stat，存活覆盖工具期。
 * @param[out] out_tool 必填独占空槽，成功取得实例拥有权，用 vtable destroy 回收。
 * @return 创建状态，能力/参数不足为不支持，OOM 为内存错误，诊断由 cc_result_free。
 */
cc_result_t lua_driver_info_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                    cc_tool_t *out_tool)
{
    return lua_tool_create_mode(ctx, out_tool, LUA_TOOL_MODE_DRIVER_QUERY);
}

/**
 * @brief 创建脚本元数据搜索工具。
 * @param[in] ctx 调用期工厂借用，端口支持 root-scoped 读、枚举、stat，存活覆盖工具期。
 * @param[out] out_tool 必填独占空槽，成功取得实例拥有权，用 vtable destroy 回收。
 * @return 创建状态，能力/参数不足为不支持，OOM 为内存错误，诊断由 cc_result_free。
 */
cc_result_t lua_script_search_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                    cc_tool_t *out_tool)
{
    return lua_tool_create_mode(ctx, out_tool, LUA_TOOL_MODE_SCRIPT_QUERY);
}
