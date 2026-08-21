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

/*
 * Lua 工具专用内存分配，优先使用 PSRAM（若启用），失败则回退到内部 RAM。
 * 参数: size - 分配字节数，0 时按 1 字节处理
 * 返回: 分配的内存指针，失败返回 NULL
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

/*
 * Lua 工具专用内存重分配，优先 PSRAM，失败回退内部 RAM。
 * 参数: ptr - 原内存指针
 *       size - 新大小，0 时按 1 字节处理
 * 返回: 重分配后的指针，失败返回 NULL
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

/*
 * 释放 lua_tool_alloc 分配的内存。
 * 参数: ptr - 待释放的内存指针，可为 NULL
 */
static void lua_tool_release(void *ptr)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
}

/*
 * 使用 PSRAM 分配复制字符串（类似 strdup），NULL 输入视为空串。
 * 参数: text - 源字符串，可为 NULL
 * 返回: 新分配的副本，失败返回 NULL
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

/*
 * 构造一个拥有 message 所有权的 cc_result_t，调用方负责传入已分配的消息字符串。
 * 参数: code - 错误码
 *       message - 已分配的错误消息（将被 result 接管所有权）
 * 返回: 构造的 cc_result_t
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

/*
 * 复制消息字符串并构造系统级错误 cc_result_t。
 * 参数: code - 错误码
 *       message - 错误消息字符串
 * 返回: 构造的 cc_result_t（消息由 lua_tool_dup 分配）
 */
static cc_result_t lua_tool_system_error(cc_error_code_t code, const char *message)
{
    return lua_tool_result_owned(code, lua_tool_dup(message));
}

/*
 * 确保 lua_tool_buffer_t 容量至少为 needed 字节，采用倍增策略扩容，
 * 受 max_bytes 硬上限约束。
 * 参数: buffer - 动态缓冲区
 *       needed - 所需最小容量
 * 返回: true 成功，false 内存不足
 */
static bool lua_tool_buffer_reserve(lua_tool_buffer_t *buffer, size_t needed)
{
    if (!buffer || needed <= buffer->cap) {
        return true;
    }

    size_t hard_cap = buffer->max_bytes + 1;
    if (hard_cap == 0) {
        return false;
    }

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

/*
 * 向动态缓冲区追加指定长度的数据，自动扩容并截断到 max_bytes 限制。
 * 参数: buffer - 动态缓冲区
 *       text - 待追加数据
 *       len - 数据长度（字节）
 * 返回: true 成功，false 内存不足
 */
static bool lua_tool_buffer_append_len(lua_tool_buffer_t *buffer, const char *text, size_t len)
{
    if (!buffer || !text || len == 0) {
        return true;
    }
    if (buffer->len + len >= buffer->max_bytes) {
        len = buffer->max_bytes > buffer->len ? buffer->max_bytes - buffer->len : 0;
    }
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

/*
 * 向动态缓冲区追加 C 字符串（以 '\0' 结尾）。
 * 参数: buffer - 动态缓冲区
 *       text - 待追加的字符串，可为 NULL
 * 返回: true 成功，false 内存不足
 */
static bool lua_tool_buffer_append(lua_tool_buffer_t *buffer, const char *text)
{
    return lua_tool_buffer_append_len(buffer, text, text ? strlen(text) : 0);
}

/*
 * 格式化追加到动态缓冲区（printf 风格），小格式串走栈缓冲区优化。
 * 参数: buffer - 动态缓冲区
 *       fmt - printf 格式串，可为 NULL
 *       ... - 可变参数
 * 返回: true 成功，false 内存不足
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
            vsnprintf(tmp, (size_t)needed + 1, fmt ? fmt : "", args);
            ok = lua_tool_buffer_append_len(buffer, tmp, (size_t)needed);
            lua_tool_release(tmp);
        }
    }
    va_end(args);
    return ok;
}

/*
 * 将错误消息写入工具调用结果，使用 PSRAM 分配错误文本。
 * 参数: out_result - 工具调用结果
 *       message - 错误消息，NULL 时使用默认 "Lua tool failed"
 * 返回: CC_OK 或内存分配失败的系统错误
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

/*
 * 根据工具 mode 返回对应的名称标识符：
 *   CODE → "lua_run_code", FILE → "lua_run_file",
 *   DRIVER_QUERY → "lua_driver_info", SCRIPT_QUERY → "lua_script_search"
 * 参数: self - lua_tool_t 实例
 * 返回: 工具名称字符串
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

/*
 * 根据工具 mode 返回对应功能描述字符串，提供给 Agent 理解工具用途。
 * 参数: self - lua_tool_t 实例
 * 返回: 英文描述字符串
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

/*
 * 根据工具 mode 返回对应的 JSON Schema：
 *   CODE - 要求 "code" 字段 + 运行限制参数
 *   FILE - 要求 "path" 字段 + 运行限制参数
 *   DRIVER_QUERY - 可选 "driver" 字段查询单个驱动
 *   SCRIPT_QUERY - 可选 "query"/"limit"/"path" 搜索脚本
 * 参数: self - lua_tool_t 实例
 * 返回: JSON Schema 字符串
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

/*
 * 从 JSON 对象中读取整数字段，不存在时用默认值，越界时钳位到 [min, max]。
 * 参数: root - JSON 对象
 *       key - 字段名
 *       default_value - 默认值
 *       min_value - 下限
 *       max_value - 上限
 * 返回: 解析并钳位后的整数值
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

/*
 * 从 JSON 参数和工具上下文中解析 Lua 运行限制（脚本大小/超时/输出/堆内存），
 * 各参数均有安全上下限钳位。若 ctx 设定了更小的 timeout_ms 则以 ctx 为准。
 * 参数: root - JSON 对象
 *       ctx - 工具上下文（用于获取外层超时限制）
 * 返回: 解析后的 lua_runtime_limits_t
 */
static lua_runtime_limits_t lua_tool_parse_limits(cc_json_value_t *root,
                                                  const cc_tool_context_t *ctx)
{
    lua_runtime_limits_t limits = lua_runtime_default_limits();
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

/*
 * 列出所有已注册 Lua 驱动的名称、全局变量名和摘要，结果写入 out_result->text。
 * 参数: out_result - [出参] 工具调用结果
 * 返回: CC_OK 或内存不足错误
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

/*
 * 查询单个 Lua 驱动的详细信息（名称、全局变量、摘要、文档），结果写入 out_result->text。
 * 参数: name - 驱动名称
 *       out_result - [出参] 工具调用结果
 * 返回: CC_OK（成功/驱动不存在时通过 out_result 区分）
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

/*
 * 驱动查询分发：有 "driver" 参数则查单个驱动，否则列出全部驱动。
 * 参数: root - JSON 对象
 *       out_result - [出参] 工具调用结果
 * 返回: CC_OK 或错误
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

/*
 * 判断字符串 text 是否以 prefix 开头。
 * 参数: text - 待检查字符串
 *       prefix - 前缀
 * 返回: true 匹配，false 任一为 NULL 或不匹配
 */
static bool lua_script_starts_with(const char *text, const char *prefix)
{
    return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

/*
 * 判断字符串 text 是否以 suffix 结尾（主要用于检测 ".lua" 后缀）。
 * 参数: text - 待检查字符串
 *       suffix - 后缀
 * 返回: true 匹配，false 任一为 NULL 或不匹配
 */
static bool lua_script_ends_with(const char *text, const char *suffix)
{
    if (!text || !suffix) return false;
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len && strcmp(text + text_len - suffix_len, suffix) == 0;
}

/*
 * 原地去除字符串首尾空白字符（空格/制表/回车/换行）。
 * 参数: text - 待处理字符串，可为 NULL
 * 返回: 去除首部空白后的指针（直接修改原字符串尾部）
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

/*
 * 安全复制字符串到固定大小字段，NULL 输入视为空串。
 * 参数: dst - 目标缓冲区
 *       dst_size - 缓冲区大小
 *       src - 源字符串，可为 NULL
 */
static void lua_script_copy_field(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    snprintf(dst, dst_size, "%s", src ? src : "");
}

/*
 * 拼接目录和文件名为完整路径，自动补全分隔符 '/'，缓冲区不足时返回失败。
 * 参数: out - 输出缓冲区
 *       out_size - 缓冲区大小
 *       dir - 目录路径
 *       name - 文件名
 * 返回: true 成功，false 缓冲区不足或参数无效
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

/*
 * 构建 Lua 脚本根目录路径：workspace_dir/lua/（末尾确保有 '/'）。
 * 参数: ctx - 工具上下文（获取 workspace_dir）
 *       out_root - 输出缓冲区
 *       out_root_size - 缓冲区大小
 * 返回: true 成功，false 路径过长
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

/*
 * 路径安全检查：拒绝空路径、含 ".." 的目录穿越、含 '\' 或 ':' 的非法字符。
 * 参数: path - 待检查路径
 * 返回: true 不安全，false 通过安全检查
 */
static bool lua_script_path_is_unsafe(const char *path)
{
    return !path || !path[0] ||
           strstr(path, "..") != NULL ||
           strchr(path, '\\') != NULL ||
           strchr(path, ':') != NULL;
}

/*
 * 将 Lua 脚本路径解析为绝对路径和运行时路径，只接受 lua/name.lua 或裸文件名。
 * 参数: ctx - 工具上下文
 *       path - 输入路径
 *       out_abs - [出参] 绝对文件系统路径
 *       out_abs_size - 绝对路径缓冲区大小
 *       out_run_path - [出参] 运行时路径（lua/name.lua 格式）
 *       out_run_path_size - 运行时路径缓冲区大小
 * 返回: true 解析成功，false 路径不安全或超出范围
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

/*
 * 从 Lua 注释行中提取元数据值（如 "-- @name hello" → "hello"），
 * key 必须以注释体开头且后跟空格/制表/冒号。
 * 参数: body - 注释体（"-- " 之后的内容）
 *       key - 元数据键（如 "@name"）
 * 返回: 匹配成功时返回值部分的指针，否则 NULL
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

/*
 * 向脚本信息的 comments 字段追加文本，用空格分隔多条注释。
 * 参数: info - 脚本信息结构体
 *       text - 待追加的注释文本
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

/*
 * 从运行时路径中提取默认脚本名称（取最后一段文件名，去除 .lua 后缀）。
 * 参数: info - [出参] 脚本信息（写入 name 字段）
 *       run_path - 运行时路径（如 lua/hello.lua）
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

/*
 * 解析 Lua 脚本文件头部注释元数据（@name/@summary/@args/@tags），获取文件大小和
 * 修改时间。解析前 24 行 "-- " 开头的注释，非元数据行拼入 comments 字段。
 * 参数: abs_path - 绝对文件路径
 *       run_path - 运行时路径
 *       info - [出参] 解析结果
 * 返回: true 成功，false 文件无法打开
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

/*
 * 大小写不敏感的朴素子串搜索。
 * 参数: text - 被搜索文本，可为 NULL
 *       needle - 搜索关键词，NULL 或空串视为匹配
 * 返回: true 匹配，false 不匹配
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

/*
 * 判断脚本是否匹配查询关键词，在 name/summary/args/tags/path/comments
 * 六个字段中进行大小写不敏感搜索。空查询匹配所有。
 * 参数: info - 脚本信息
 *       query - 查询关键词，NULL 或空串匹配所有
 * 返回: true 匹配，false 不匹配
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

/*
 * 将脚本信息格式化为可读文本追加到缓冲区，detail 控制是否输出 comments 字段。
 * 参数: buffer - 动态缓冲区
 *       info - 脚本信息
 *       detail - true 输出 comments 字段
 * 返回: true 成功，false 内存不足
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

/*
 * 按精确路径搜索单个 Lua 脚本（通过 "path" 参数指定），解析并返回完整元数据。
 * 参数: ctx - 工具上下文
 *       path - 脚本路径
 *       out_result - [出参] 工具调用结果
 * 返回: CC_OK 或错误
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

/*
 * 遍历 lua/ 目录下所有 .lua 文件，按 query 关键词过滤（匹配 name/summary/args/
 * tags/path/comments），受 limit 上限约束。结果格式化输出到 out_result->text。
 * 参数: ctx - 工具上下文
 *       query - 搜索关键词，NULL 或空串返回所有
 *       limit - 最大返回条目数
 *       out_result - [出参] 工具调用结果
 * 返回: CC_OK 或错误
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

/*
 * 脚本搜索分发：有 "path" 参数则精确搜索单个脚本，否则按 query+limit 搜索全部。
 * 参数: root - JSON 对象
 *       ctx - 工具上下文
 *       out_result - [出参] 工具调用结果
 * 返回: CC_OK 或错误
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

/*
 * Lua 工具总入口：根据 mode 分发到 driver_info / script_search / run_code / run_file
 * 四种子流程。run_code/run_file 通过 lua_runtime 执行并受 limits 约束。
 * 参数: self - lua_tool_t 实例
 *       args_json - JSON 参数字符串
 *       ctx - 工具上下文
 *       out_result - [出参] 工具调用结果
 * 返回: CC_OK 或系统错误
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

/*
 * 释放 lua_tool_t 工具实例内存。
 * 参数: self - 工具实例指针
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

/*
 * 按指定 mode 创建 Lua 工具实例，分配 lua_tool_t 并挂载虚函数表。
 * 参数: ctx - 工具工厂上下文（当前未使用）
 *       out_tool - [出参] 接收创建的工具实例
 *       mode - 工具模式（CODE/FILE/DRIVER_QUERY/SCRIPT_QUERY）
 * 返回: CC_OK 成功，CC_ERR_INVALID_ARGUMENT 空输出，CC_ERR_OUT_OF_MEMORY 内存不足
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

/*
 * 创建 lua_run_code 工具实例（mode=CODE），允许 Agent 直接执行 Lua 源代码。
 * 参数: ctx - 工具工厂上下文
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 或错误
 */
cc_result_t lua_run_code_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                     cc_tool_t *out_tool)
{
    return lua_tool_create_mode(ctx, out_tool, LUA_TOOL_MODE_CODE);
}

/*
 * 创建 lua_run_file 工具实例（mode=FILE），允许 Agent 运行 lua/ 目录下的脚本文件。
 * 参数: ctx - 工具工厂上下文
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 或错误
 */
cc_result_t lua_run_file_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                     cc_tool_t *out_tool)
{
    return lua_tool_create_mode(ctx, out_tool, LUA_TOOL_MODE_FILE);
}

/*
 * 创建 lua_driver_info 工具实例（mode=DRIVER_QUERY），用于查询可用 Lua 驱动列表或单个驱动详情。
 * 参数: ctx - 工具工厂上下文
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 或错误
 */
cc_result_t lua_driver_info_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                    cc_tool_t *out_tool)
{
    return lua_tool_create_mode(ctx, out_tool, LUA_TOOL_MODE_DRIVER_QUERY);
}

/*
 * 创建 lua_script_search 工具实例（mode=SCRIPT_QUERY），用于搜索/列出 workspace lua 目录下的可复用脚本。
 * 参数: ctx - 工具工厂上下文
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 或错误
 */
cc_result_t lua_script_search_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                    cc_tool_t *out_tool)
{
    return lua_tool_create_mode(ctx, out_tool, LUA_TOOL_MODE_SCRIPT_QUERY);
}
