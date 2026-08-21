#include "lua_runtime.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"
#include "lua_driver.h"
#include "lualib.h"
#include "sdkconfig.h"

#define LUA_RUNTIME_DEFAULT_MAX_SCRIPT_BYTES 4096
#define LUA_RUNTIME_DEFAULT_MAX_OUTPUT_BYTES 2048
#define LUA_RUNTIME_DEFAULT_MAX_HEAP_BYTES (128 * 1024)
#define LUA_RUNTIME_DEFAULT_TIMEOUT_MS 1000
#define LUA_RUNTIME_PATH_MAX 192

typedef struct {
    size_t used;
    size_t max_bytes;
} lua_alloc_state_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    size_t max_bytes;
    bool truncated;
} lua_output_t;

typedef struct {
    TickType_t start_ticks;
    TickType_t max_ticks;
    int timeout_ms;
} lua_timeout_state_t;

static char s_lua_timeout_registry_key;

/*
 * 返回 Lua 运行时的默认限制参数（脚本4KB、输出2KB、堆128KB、超时1s）。
 * 返回: 填充默认值的 lua_runtime_limits_t 结构体
 */
lua_runtime_limits_t lua_runtime_default_limits(void)
{
    lua_runtime_limits_t limits = {
        .max_script_bytes = LUA_RUNTIME_DEFAULT_MAX_SCRIPT_BYTES,
        .max_output_bytes = LUA_RUNTIME_DEFAULT_MAX_OUTPUT_BYTES,
        .max_heap_bytes = LUA_RUNTIME_DEFAULT_MAX_HEAP_BYTES,
        .timeout_ms = LUA_RUNTIME_DEFAULT_TIMEOUT_MS,
    };
    return limits;
}

/*
 * PSRAM 内存分配（CONFIG_SPIRAM 启用时），size=0 时自动调整为1字节。
 * 参数: size - 分配大小
 * 返回: 分配的内存指针，无 PSRAM 时返回 NULL
 */
static void *lua_runtime_alloc(size_t size)
{
    if (size == 0) {
        size = 1;
    }
#if CONFIG_SPIRAM
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    (void)size;
    return NULL;
#endif
}

/*
 * PSRAM 内存重新分配（CONFIG_SPIRAM 启用时），size=0 时调整为1字节。
 * 参数: ptr - 原指针
 *       size - 新大小
 * 返回: 新指针，无 PSRAM 时返回 NULL
 */
static void *lua_runtime_resize(void *ptr, size_t size)
{
    if (size == 0) {
        size = 1;
    }
#if CONFIG_SPIRAM
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    (void)ptr;
    (void)size;
    return NULL;
#endif
}

/*
 * 释放 PSRAM 内存。
 * 参数: ptr - 要释放的指针，为 NULL 时无操作
 */
static void lua_runtime_release(void *ptr)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
}

/*
 * 字符串 PSRAM 深拷贝。
 * 参数: text - 源字符串，为 NULL 时按空字符串处理
 * 返回: 新分配的 PSRAM 字符串，失败返回 NULL
 */
static char *lua_runtime_dup(const char *text)
{
    const char *safe = text ? text : "";
    size_t len = strlen(safe);
    char *copy = (char *)lua_runtime_alloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, safe, len + 1);
    return copy;
}

/*
 * 构造携带消息所有权的 cc_result_t（调用者负责释放 message）。
 * 参数: code - 错误码
 *       message - PSRAM 分配的消息，所有权转移
 * 返回: cc_result_t
 */
static cc_result_t lua_runtime_result_owned(cc_error_code_t code, char *message)
{
    cc_result_t result;
    memset(&result, 0, sizeof(result));
    result.size = sizeof(result);
    result.code = code;
    result.message = message;
    return result;
}

/*
 * 创建携带静态错误消息的 cc_result_t（自动 PSRAM 拷贝）。
 * 参数: code - 错误码
 *       message - 错误消息字符串，为 NULL 时生成空消息
 * 返回: cc_result_t
 */
static cc_result_t lua_runtime_error(cc_error_code_t code, const char *message)
{
    return lua_runtime_result_owned(code, lua_runtime_dup(message));
}

/*
 * 创建携带格式化错误消息的 cc_result_t（自动 PSRAM 分配和格式化）。
 * 参数: code - 错误码
 *       fmt - printf 格式字符串
 *       ... - 格式参数
 * 返回: cc_result_t
 */
static cc_result_t lua_runtime_errorf(cc_error_code_t code, const char *fmt, ...)
{
    char stack[256];
    char *message = NULL;
    va_list args;

    va_start(args, fmt);
    va_list measure_args;
    va_copy(measure_args, args);
    int needed = vsnprintf(stack, sizeof(stack), fmt ? fmt : "", measure_args);
    va_end(measure_args);

    if (needed >= 0) {
        message = (char *)lua_runtime_alloc((size_t)needed + 1);
        if (message) {
            if ((size_t)needed < sizeof(stack)) {
                memcpy(message, stack, (size_t)needed + 1);
            } else {
                vsnprintf(message, (size_t)needed + 1, fmt ? fmt : "", args);
            }
        }
    }
    va_end(args);

    return lua_runtime_result_owned(code, message);
}

/*
 * 将限制中为零的字段填充为默认值。
 * 参数: limits - 要规范化的限制结构体指针，为 NULL 时无操作
 */
static void lua_runtime_normalize_limits(lua_runtime_limits_t *limits)
{
    if (!limits) {
        return;
    }
    if (limits->max_script_bytes == 0) {
        limits->max_script_bytes = LUA_RUNTIME_DEFAULT_MAX_SCRIPT_BYTES;
    }
    if (limits->max_output_bytes == 0) {
        limits->max_output_bytes = LUA_RUNTIME_DEFAULT_MAX_OUTPUT_BYTES;
    }
    if (limits->max_heap_bytes == 0) {
        limits->max_heap_bytes = LUA_RUNTIME_DEFAULT_MAX_HEAP_BYTES;
    }
    if (limits->timeout_ms == 0) {
        limits->timeout_ms = LUA_RUNTIME_DEFAULT_TIMEOUT_MS;
    }
}

/*
 * Lua 自定义内存分配器：基于 lua_alloc_state_t 限制最大堆使用量。
 * nsize=0 时释放内存；nsize>0 时检查增长是否超过限制。
 * 参数: ud - lua_alloc_state_t 用户数据
 *       ptr - 原指针
 *       osize - 原大小
 *       nsize - 新大小
 * 返回: 新指针或 NULL（超限）
 */
static void *lua_limited_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    lua_alloc_state_t *state = (lua_alloc_state_t *)ud;
    size_t old_size = ptr ? osize : 0;

    if (nsize == 0) {
        if (ptr) {
            lua_runtime_release(ptr);
            state->used = old_size > state->used ? 0 : state->used - old_size;
        }
        return NULL;
    }

    if (nsize > old_size) {
        size_t grow = nsize - old_size;
        if (grow > state->max_bytes || state->used > state->max_bytes - grow) {
            return NULL;
        }
    }

    void *next = lua_runtime_resize(ptr, nsize);
    if (!next) {
        return NULL;
    }

    state->used = state->used - old_size + nsize;
    return next;
}

/*
 * 确保输出缓冲区容量足够（按需倍增扩容，上限为 max_bytes+1）。
 * 参数: output - 输出缓冲区
 *       needed - 所需容量
 * 返回: true 成功（容量已足够或扩容成功）
 */
static bool lua_output_reserve(lua_output_t *output, size_t needed)
{
    if (!output || needed <= output->cap) {
        return true;
    }

    size_t hard_cap = output->max_bytes + 1;
    if (hard_cap == 0) {
        return false;
    }

    size_t next_cap = output->cap ? output->cap : 128;
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

    char *next = (char *)lua_runtime_resize(output->data, next_cap);
    if (!next) {
        return false;
    }
    output->data = next;
    output->cap = next_cap;
    return true;
}

/*
 * 将指定长度的文本追加到输出缓冲区，超过 max_bytes 时截断并标记 truncated。
 * 参数: output - 输出缓冲区
 *       text - 要追加的文本
 *       len - 文本长度
 * 返回: true 成功
 */
static bool lua_output_append_len(lua_output_t *output, const char *text, size_t len)
{
    if (!output || !text || len == 0) {
        return true;
    }

    if (output->len >= output->max_bytes) {
        output->truncated = true;
        return true;
    }

    size_t room = output->max_bytes - output->len;
    if (len > room) {
        len = room;
        output->truncated = true;
    }

    if (!lua_output_reserve(output, output->len + len + 1)) {
        return false;
    }
    memcpy(output->data + output->len, text, len);
    output->len += len;
    output->data[output->len] = '\0';
    return true;
}

/*
 * 将 C 字符串追加到输出缓冲区。
 * 参数: output - 输出缓冲区
 *       text - C 字符串
 * 返回: true 成功
 */
static bool lua_output_append(lua_output_t *output, const char *text)
{
    return lua_output_append_len(output, text, text ? strlen(text) : 0);
}

/*
 * Lua 全局 print 函数：将所有参数转为字符串并追加到输出缓冲区（制表符分隔，换行结尾）。
 * 参数: L - Lua 状态机
 * 返回: 0（无返回值推入 Lua 栈）
 */
static int lua_cclaw_print(lua_State *L)
{
    lua_output_t *output = (lua_output_t *)lua_touserdata(L, lua_upvalueindex(1));
    int n = lua_gettop(L);

    for (int i = 1; i <= n; i++) {
        if (i > 1 && !lua_output_append(output, "\t")) {
            return luaL_error(L, "Lua output PSRAM allocation failed");
        }

        size_t len = 0;
        const char *text = luaL_tolstring(L, i, &len);
        if (!lua_output_append_len(output, text ? text : "", len)) {
            lua_pop(L, 1);
            return luaL_error(L, "Lua output PSRAM allocation failed");
        }
        lua_pop(L, 1);
    }

    if (!lua_output_append(output, "\n")) {
        return luaL_error(L, "Lua output PSRAM allocation failed");
    }
    return 0;
}

/*
 * 注册 Lua 安全基础库（base/table/string/math），禁用 dofile/loadfile/require，
 * 替换 print 为自定义实现，注册所有 Lua 驱动。
 * 参数: L - Lua 状态机
 *       output - 输出缓冲区（作为 print 的 upvalue）
 */
static void lua_open_safe_libs(lua_State *L, lua_output_t *output)
{
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);

    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_pushnil(L);
    lua_setglobal(L, "require");

    lua_pushlightuserdata(L, output);
    lua_pushcclosure(L, lua_cclaw_print, 1);
    lua_setglobal(L, "print");

    lua_driver_register_all(L);
}

/*
 * lua_open_safe_libs 的保护调用包装（用于 pcall）。
 * 从 upvalue 获取 lua_output_t 指针后调用 lua_open_safe_libs。
 */
static int lua_open_safe_libs_protected(lua_State *L)
{
    lua_output_t *output = (lua_output_t *)lua_touserdata(L, lua_upvalueindex(1));
    lua_open_safe_libs(L, output);
    return 0;
}

/*
 * Lua 超时钩子：每1000条指令检查一次已用时间，超时则抛出 Lua 错误。
 * 从 registry 中读取 lua_timeout_state_t 获取超时参数。
 */
static void lua_timeout_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;

    lua_pushlightuserdata(L, (void *)&s_lua_timeout_registry_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_timeout_state_t *timeout = (lua_timeout_state_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (!timeout || timeout->max_ticks == 0) {
        return;
    }

    TickType_t elapsed = xTaskGetTickCount() - timeout->start_ticks;
    if (elapsed >= timeout->max_ticks) {
        luaL_error(L, "Lua execution timed out after %d ms", timeout->timeout_ms);
    }
}

/*
 * 安装 Lua 超时钩子：分配 lua_timeout_state_t，存入 registry，调用 lua_sethook。
 * 参数: L - Lua 状态机
 *       timeout_ms - 超时毫秒数，<=0 时不安装
 * 返回: true 成功
 */
static bool lua_install_timeout_hook(lua_State *L, int timeout_ms)
{
    if (timeout_ms <= 0) {
        return true;
    }

    lua_timeout_state_t *timeout = (lua_timeout_state_t *)lua_runtime_alloc(sizeof(*timeout));
    if (!timeout) {
        return false;
    }
    memset(timeout, 0, sizeof(*timeout));
    timeout->start_ticks = xTaskGetTickCount();
    timeout->max_ticks = pdMS_TO_TICKS(timeout_ms);
    timeout->timeout_ms = timeout_ms;
    if (timeout->max_ticks == 0) {
        timeout->max_ticks = 1;
    }

    lua_pushlightuserdata(L, (void *)&s_lua_timeout_registry_key);
    lua_pushlightuserdata(L, timeout);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_sethook(L, lua_timeout_hook, LUA_MASKCOUNT, 1000);
    return true;
}

/*
 * 移除超时钩子：调用 lua_sethook(NULL)，从 registry 清除并释放 lua_timeout_state_t。
 * 参数: L - Lua 状态机
 */
static void lua_remove_timeout_hook(lua_State *L)
{
    lua_sethook(L, NULL, 0, 0);
    lua_pushlightuserdata(L, (void *)&s_lua_timeout_registry_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_timeout_state_t *timeout = (lua_timeout_state_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    lua_pushlightuserdata(L, (void *)&s_lua_timeout_registry_key);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_runtime_release(timeout);
}

/*
 * 将 Lua 栈上的所有返回值转为字符串追加到输出缓冲区（"return: " 前缀，制表符分隔）。
 * 参数: L - Lua 状态机
 *       output - 输出缓冲区
 * 返回: true 成功
 */
static bool lua_append_return_values(lua_State *L, lua_output_t *output)
{
    int n = lua_gettop(L);
    if (n <= 0) {
        return true;
    }

    if (output->len > 0 && !lua_output_append(output, "\n")) {
        return false;
    }
    if (!lua_output_append(output, "return: ")) {
        return false;
    }

    for (int i = 1; i <= n; i++) {
        if (i > 1 && !lua_output_append(output, "\t")) {
            return false;
        }
        size_t len = 0;
        const char *text = luaL_tolstring(L, i, &len);
        if (!lua_output_append_len(output, text ? text : "", len)) {
            lua_pop(L, 1);
            return false;
        }
        lua_pop(L, 1);
    }
    return lua_output_append(output, "\n");
}

/*
 * 判断错误消息是否为内存不足相关错误。
 * 参数: message - 错误消息
 * 返回: true 是内存错误
 */
static bool lua_message_is_memory_error(const char *message)
{
    return message &&
        (strstr(message, "not enough memory") ||
         strstr(message, "memory allocation") ||
         strstr(message, "PSRAM allocation failed"));
}

/*
 * 根据 Lua 执行状态码和错误消息，映射为 cc_error_code_t。
 * LUA_ERRSYNTAX→CC_ERR_INVALID_ARGUMENT, LUA_ERRMEM→CC_ERR_OUT_OF_MEMORY,
 * 超时→CC_ERR_TIMEOUT, 其他→CC_ERR_TOOL。
 * 参数: status - Lua 执行状态码
 *       message - 错误消息
 * 返回: 对应的 cc_error_code_t
 */
static cc_error_code_t lua_status_error_code(int status, const char *message)
{
    if (status == LUA_ERRSYNTAX) {
        return CC_ERR_INVALID_ARGUMENT;
    }
    if (status == LUA_ERRMEM || lua_message_is_memory_error(message)) {
        return CC_ERR_OUT_OF_MEMORY;
    }
    if (message && strstr(message, "timed out")) {
        return CC_ERR_TIMEOUT;
    }
    return CC_ERR_TOOL;
}

/*
 * 核心 Lua 代码执行函数：创建 lua_State（PSRAM），注册安全库，安装超时钩子，
 * 加载并执行代码，收集输出和返回值。
 * 参数: code - Lua 源代码
 *       source_name - 用于错误消息的源名称
 *       limits - 限制参数（为 NULL 时使用默认值）
 *       out_text - 输出文本（调用者需释放）
 * 返回: cc_result_t
 */
static cc_result_t lua_runtime_run_code_named(const char *code,
                                              const char *source_name,
                                              const lua_runtime_limits_t *limits,
                                              char **out_text)
{
    if (!code || !out_text) {
        return lua_runtime_error(CC_ERR_INVALID_ARGUMENT, "Lua code and output are required");
    }
    *out_text = NULL;

#if !CONFIG_SPIRAM
    return lua_runtime_error(CC_ERR_UNSUPPORTED, "Lua runtime requires PSRAM");
#endif

    lua_runtime_limits_t effective = limits ? *limits : lua_runtime_default_limits();
    lua_runtime_normalize_limits(&effective);

    size_t code_len = strlen(code);
    if (code_len > effective.max_script_bytes) {
        return lua_runtime_errorf(CC_ERR_LIMIT_EXCEEDED,
                                  "Lua script is too large: %u > %u bytes",
                                  (unsigned)code_len,
                                  (unsigned)effective.max_script_bytes);
    }

    lua_alloc_state_t alloc = {
        .used = 0,
        .max_bytes = effective.max_heap_bytes,
    };
    lua_output_t output = {
        .max_bytes = effective.max_output_bytes,
    };

    lua_State *L = lua_newstate(lua_limited_alloc, &alloc, luaL_makeseed(NULL));
    if (!L) {
        return lua_runtime_error(CC_ERR_OUT_OF_MEMORY, "Failed to create Lua state in PSRAM");
    }

    lua_pushlightuserdata(L, &output);
    lua_pushcclosure(L, lua_open_safe_libs_protected, 1);
    int init_status = lua_pcall(L, 0, 0, 0);
    if (init_status != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        cc_result_t rc = lua_runtime_errorf(CC_ERR_OUT_OF_MEMORY,
                                            "Failed to initialize Lua libs: %s",
                                            message ? message : "unknown error");
        lua_close(L);
        lua_runtime_release(output.data);
        return rc;
    }

    if (!lua_install_timeout_hook(L, effective.timeout_ms)) {
        lua_close(L);
        lua_runtime_release(output.data);
        return lua_runtime_error(CC_ERR_OUT_OF_MEMORY, "Failed to install Lua timeout hook in PSRAM");
    }

    int status = luaL_loadbuffer(L,
                                 code,
                                 code_len,
                                 source_name && source_name[0] ? source_name : "lua_run_code");
    if (status == LUA_OK) {
        status = lua_pcall(L, 0, LUA_MULTRET, 0);
    }
    lua_remove_timeout_hook(L);

    if (status != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        cc_error_code_t code_value = lua_status_error_code(status, message);
        const char *kind = status == LUA_ERRSYNTAX ? "syntax" :
            (code_value == CC_ERR_TIMEOUT ? "timeout" :
             (code_value == CC_ERR_OUT_OF_MEMORY ? "memory" : "runtime"));
        cc_result_t rc = lua_runtime_errorf(code_value,
                                            "Lua %s error in %s: %s",
                                            kind,
                                            source_name && source_name[0] ? source_name : "lua_run_code",
                                            message ? message : "unknown error");
        lua_close(L);
        lua_runtime_release(output.data);
        return rc;
    }

    if (!lua_append_return_values(L, &output)) {
        lua_close(L);
        lua_runtime_release(output.data);
        return lua_runtime_error(CC_ERR_OUT_OF_MEMORY, "Failed to format Lua output in PSRAM");
    }

    if (output.truncated && output.len < output.max_bytes) {
        lua_output_append(&output, "\n[truncated]\n");
    }
    if (!output.data) {
        output.data = lua_runtime_dup("(no output)\n");
    }
    if (!output.data) {
        lua_close(L);
        return lua_runtime_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate Lua output in PSRAM");
    }

    *out_text = output.data;
    lua_close(L);
    return cc_result_ok();
}

/*
 * 执行 Lua 代码（公共接口，source_name 固定为 "lua_run_code"）。
 * 参数: code - Lua 源代码
 *       limits - 限制参数
 *       out_text - 输出文本
 * 返回: cc_result_t
 */
cc_result_t lua_runtime_run_code(const char *code,
                                 const lua_runtime_limits_t *limits,
                                 char **out_text)
{
    return lua_runtime_run_code_named(code, "lua_run_code", limits, out_text);
}

/*
 * 执行 Lua 脚本文件：解析路径、检查文件大小、读取内容、调用 lua_runtime_run_code_named 执行。
 * 参数: workspace_dir - 工作目录
 *       path - 脚本文件路径
 *       limits - 限制参数
 *       out_text - 输出文本（调用者需释放）
 * 返回: cc_result_t
 */
static int lua_runtime_has_lua_suffix(const char *path)
{
    size_t len = path ? strlen(path) : 0;
    return len > 4 && strcmp(path + len - 4, ".lua") == 0;
}

cc_result_t lua_runtime_run_file(const cc_filesystem_t *filesystem,
                                 const char *workspace_dir,
                                 const char *path,
                                 const lua_runtime_limits_t *limits,
                                 char **out_text)
{
    if (!filesystem || !filesystem->vtable ||
        !(filesystem->capabilities & CC_FS_CAP_ROOT_SCOPED) ||
        !filesystem->vtable->read_relative_text ||
        !path || !path[0] || !out_text) {
        return lua_runtime_error(CC_ERR_INVALID_ARGUMENT, "Lua file path and output are required");
    }
    *out_text = NULL;

#if !CONFIG_SPIRAM
    return lua_runtime_error(CC_ERR_UNSUPPORTED, "Lua runtime requires PSRAM");
#endif

    if (path[0] == '/' || path[0] == '\\' || strchr(path, '\\') || strchr(path, ':') ||
        strstr(path, "../") || strstr(path, "/..") || strcmp(path, "..") == 0) {
        return lua_runtime_error(CC_ERR_PERMISSION_DENIED,
                                 "Lua file path must be relative to the workspace");
    }
    char relative_path[LUA_RUNTIME_PATH_MAX];
    int written = strncmp(path, LUA_RUNTIME_SCRIPT_SUBDIR "/",
                          strlen(LUA_RUNTIME_SCRIPT_SUBDIR) + 1) == 0 ?
        snprintf(relative_path, sizeof(relative_path), "%s", path) :
        snprintf(relative_path, sizeof(relative_path), "%s/%s",
                 LUA_RUNTIME_SCRIPT_SUBDIR, path);
    if (written <= 0 || (size_t)written >= sizeof(relative_path) ||
        !lua_runtime_has_lua_suffix(relative_path)) {
        return lua_runtime_error(CC_ERR_INVALID_ARGUMENT,
                                 "Lua file must be a relative .lua path");
    }

    lua_runtime_limits_t effective = limits ? *limits : lua_runtime_default_limits();
    lua_runtime_normalize_limits(&effective);

    const char *root = workspace_dir && workspace_dir[0] ?
        workspace_dir : LUA_RUNTIME_DEFAULT_WORKSPACE_DIR;
    char *code = NULL;
    cc_result_t read_rc = filesystem->vtable->read_relative_text(
        filesystem->self, root, relative_path, &code);
    if (read_rc.code != CC_OK) return read_rc;
    cc_result_free(&read_rc);
    size_t code_bytes = strlen(code);
    if (code_bytes > effective.max_script_bytes) {
        free(code);
        return lua_runtime_errorf(CC_ERR_LIMIT_EXCEEDED,
                                  "Lua file is too large: %u > %u bytes",
                                  (unsigned)code_bytes,
                                  (unsigned)effective.max_script_bytes);
    }

    char source_name[LUA_RUNTIME_PATH_MAX + 2];
    written = snprintf(source_name, sizeof(source_name), "@%s", relative_path);
    const char *source = (written > 0 && (size_t)written < sizeof(source_name)) ?
        source_name : "lua_run_file";

    cc_result_t rc = lua_runtime_run_code_named(code, source, &effective, out_text);
    free(code);
    return rc;
}
