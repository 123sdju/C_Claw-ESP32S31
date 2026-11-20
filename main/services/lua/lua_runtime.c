#include "lua_runtime.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
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
    TickType_t last_yield_ticks;
    TickType_t max_ticks;
    int timeout_ms;
    cc_cancel_token_t *cancel_token;
} lua_timeout_state_t;

static char s_lua_timeout_registry_key;

/**
 * @brief 返回每次独立执行的默认资源限制。
 * @return 按值结构，脚本 4096 字节、输出 2048 字节、Lua 堆 128 KiB、协作超时 1000 毫秒，取消令牌 NULL。
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

/**
 * @brief 为运行期辅助数据申请 PSRAM。
 * @param[in] size 字节数，零按一字节申请。
 * @return 调用者拥有的能力堆块，OOM 或未启用 PSRAM 返回 NULL，由 lua_runtime_release 回收。
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

/**
 * @brief 调整拥有块并保留失败时原块。
 * @param[in,out] ptr 原能力堆拥有块，可 NULL。
 * @param[in] size 目标字节数，零规范为一字节。
 * @return 成功转交新块并使旧指针失效，失败返回 NULL 且原块保持。
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

/**
 * @brief 归还运行期辅助存储。
 * @param[in] ptr 能力堆拥有指针，NULL 跳过。
 * @post 非空块归还，调用者清理自己的引用。
 */
static void lua_runtime_release(void *ptr)
{
    if (ptr) {
        heap_caps_free(ptr);
    }
}

/**
 * @brief 复制诊断或输出字符串到 PSRAM。
 * @param[in] text 调用期 NUL 借用，NULL 按空串。
 * @return 调用者拥有的 NUL 副本，OOM 为 NULL，由能力堆或兼容 free 回收。
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

/**
 * @brief 将已分配诊断转交结果对象。
 * @param[in] code 按值保存的状态码。
 * @param[in] message 转移拥有权的能力堆诊断，可 NULL。
 * @return 当前布局结果，接收者 cc_result_free 回收诊断。
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

/**
 * @brief 复制可选诊断并保存状态码。
 * @param[in] code 按值状态码，复制失败也保留。
 * @param[in] message 调用期 NUL 借用，NULL 按空串。
 * @return 拥有诊断的结果，OOM 时 message 可 NULL，由调用者 cc_result_free。
 */
static cc_result_t lua_runtime_error(cc_error_code_t code, const char *message)
{
    return lua_runtime_result_owned(code, lua_runtime_dup(message));
}

/**
 * @brief 先测量再分配格式化诊断。
 * @param[in] code 按值状态码。
 * @param[in] fmt 调用期 printf 格式借用，NULL 按空串。
 * @param[in] ... 与格式匹配并覆盖两次格式化的参数借用。
 * @return 状态码保持，格式化或分配失败诊断可 NULL，拥有字段由 cc_result_free 回收。
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
                int written = vsnprintf(message, (size_t)needed + 1, fmt ? fmt : "", args);
                if (written < 0 || written > needed) {
                    lua_runtime_release(message);
                    message = NULL;
                }
            }
        }
    }
    va_end(args);

    return lua_runtime_result_owned(code, message);
}

/**
 * @brief 原地补齐零值资源字段。
 * @param[in,out] limits 独占结构借用，NULL 跳过。
 * @post 零字段替换为默认值，负 timeout_ms 保留为关闭时间预算，取消令牌保持。
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

/**
 * @brief 按 Lua 分配协议维护独占状态堆预算。
 * @param[in,out] ud 非 NULL 分配统计借用，覆盖状态创建到关闭。
 * @param[in,out] ptr 旧拥有块，NULL 表示新分配。
 * @param[in] osize ptr 非空时的原字节数，NULL 时 Lua 可传类型标签，本函数按零处理。
 * @param[in] nsize 新字节数，零释放。
 * @return 成功新拥有块，超限/OOM 为 NULL 且旧块与统计保持，释放路径返回 NULL。
 * @pre 同一 Lua 状态由单一调用任务访问，Lua 保证原块大小与统计一致。
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

/**
 * @brief 为独立输出缓冲扩容并保留失败时内容。
 * @param[in,out] output 独占输出状态，NULL 返回 false。
 * @param[in] needed 包含 NUL 的所需字节容量。
 * @return 已满足或扩容成功为 true，容量越界或 OOM 为 false，原块保持。
 */
static bool lua_output_reserve(lua_output_t *output, size_t needed)
{
    if (!output) return false;
    if (needed <= output->cap) {
        return true;
    }

    size_t hard_cap = output->max_bytes + 1;
    if (hard_cap == 0) {
        return false;
    }
    if (needed > hard_cap) return false;

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

/**
 * @brief 在输出字节预算内追加调用期文本。
 * @param[in,out] output 独占状态借用，NULL 跳过。
 * @param[in] text 至少 len 字节的借用，NULL 跳过，禁止与可搬迁输出块别名。
 * @param[in] len 输入字节数，零跳过。
 * @return 接纳或预算截断为 true，扩容失败为 false 并保留旧内容。
 * @pre max_bytes 留有 NUL 容量，内部内容按字节裁剪，可包含 Lua 字符串中的 NUL。
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

/**
 * @brief 追加可选 NUL 字符串。
 * @param[in,out] output 独占状态借用，NULL 跳过。
 * @param[in] text 调用期非别名 NUL 借用，NULL 按空串。
 * @return 接纳或截断 true，扩容失败 false，拥有权仍归 output。
 */
static bool lua_output_append(lua_output_t *output, const char *text)
{
    return lua_output_append_len(output, text, text ? strlen(text) : 0);
}

/**
 * @brief 将 Lua 参数按制表符和换行写入当前输出。
 * @param[in,out] L 独占状态借用，upvalue 一借用外层调用期输出状态。
 * @return 成功零个返回值，字符串转换异常或辅助存储 OOM 向外层保护调用传播。
 * @note luaL_tolstring 可执行用户 __tostring，指令预算属于可捕获的协作错误。
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

/**
 * @brief 实现仅接受文本字符串的受限 load。
 * @param[in,out] L 独占状态借用，栈中为 source、可选名称和 mode。
 * @return 成功一项函数，加载失败两项 nil/诊断，参数类型错误沿 Lua 保护边界传播。
 * @post 编译产物归 Lua 状态，源借用只覆盖本次调用。
 */
static int lua_safe_load_text(lua_State *L)
{
    size_t source_len = 0;
    const char *source = lua_tolstring(L, 1, &source_len);
    const char *chunk_name = luaL_optstring(L, 2, "=(load)");
    const char *mode = luaL_optstring(L, 3, "t");
    if (!source) {
        lua_pushnil(L);
        lua_pushliteral(L, "load accepts text strings only");
        return 2;
    }
    if (!mode || !strchr(mode, 't') || strchr(mode, 'b')) {
        lua_pushnil(L);
        lua_pushliteral(L, "load mode must allow text only");
        return 2;
    }
    int status = luaL_loadbufferx(L, source, source_len, chunk_name, "t");
    if (status == LUA_OK) return 1;
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
}

/**
 * @brief 注册选定基础库、受限 load、输出及常驻驱动。
 * @param[in,out] L 独占有效状态借用，处于外层保护调用。
 * @param[in,out] output 调用期状态借用，print upvalue 保留至 Lua 状态关闭。
 * @post base/table/string/math 已登记，文件/模块加载及模式搜索关闭，pcall/xpcall 保留。
 * @note 库分配错误向外层传播，C 库和驱动等待按各自能力约束。
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
    lua_pushcfunction(L, lua_safe_load_text);
    lua_setglobal(L, "load");

    lua_getglobal(L, LUA_STRLIBNAME);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, "dump");
        /* 指令钩子无法中断长时间 C 库调用，模式回溯缺少独立预算，当前关闭这些入口。 */
        lua_pushnil(L);
        lua_setfield(L, -2, "find");
        lua_pushnil(L);
        lua_setfield(L, -2, "match");
        lua_pushnil(L);
        lua_setfield(L, -2, "gmatch");
        lua_pushnil(L);
        lua_setfield(L, -2, "gsub");
    }
    lua_pop(L, 1);

    lua_pushlightuserdata(L, output);
    lua_pushcclosure(L, lua_cclaw_print, 1);
    lua_setglobal(L, "print");

    lua_driver_register_all(L);
}

/**
 * @brief 每千条 Lua 指令检查协作取消与墙钟预算并让出调度。
 * @param[in,out] L 独占状态借用，registry 中借用外层调用栈的预算状态。
 * @param[in] ar Lua 提供的调试记录，本函数忽略。
 * @post 预算耗尽以可被 pcall/xpcall 捕获的 Lua 错误传播，正常路径栈平衡。
 * @note C 回调和 Lua 内部禁钩子区间按自身实现返回，取消令牌由 SDK 锁内查询并允许其他任务取消。
 */
static void lua_timeout_hook(lua_State *L, lua_Debug *ar)
{
    (void)ar;

    lua_pushlightuserdata(L, (void *)&s_lua_timeout_registry_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_timeout_state_t *timeout = (lua_timeout_state_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (!timeout) {
        return;
    }
    if (cc_cancel_token_is_cancelled(timeout->cancel_token)) {
        luaL_error(L, "Lua execution cancelled");
    }

    TickType_t elapsed = xTaskGetTickCount() - timeout->start_ticks;
    if (timeout->max_ticks && elapsed >= timeout->max_ticks) {
        luaL_error(L, "Lua execution timed out after %d ms", timeout->timeout_ms);
    }
    /* 主动阻塞给低优先级任务运行机会，调度耗时继续计入原始墙钟预算。 */
    TickType_t slice = pdMS_TO_TICKS(10);
    if (slice == 0) slice = 1;
    if ((TickType_t)(xTaskGetTickCount() - timeout->last_yield_ticks) >= slice) {
        vTaskDelay(1);
        timeout->last_yield_ticks = xTaskGetTickCount();
        if (timeout->max_ticks &&
            (TickType_t)(timeout->last_yield_ticks - timeout->start_ticks) >= timeout->max_ticks) {
            luaL_error(L, "Lua execution timed out after %d ms", timeout->timeout_ms);
        }
    }
}

/**
 * @brief 在保护调用内安装预算状态借用及指令钩子。
 * @param[in,out] L 独占状态借用，registry 扩容 OOM 向外层保护调用传播。
 * @param[in,out] timeout 非 NULL 调用期独占状态，初始化计时，存活到状态关闭。
 * @post 正超时向上取整为 tick，负超时关闭时间判定，取消和调度检查仍启用，无拥有权转移。
 */
static void lua_install_timeout_hook(lua_State *L, lua_timeout_state_t *timeout)
{
    timeout->start_ticks = xTaskGetTickCount();
    timeout->last_yield_ticks = timeout->start_ticks;
    if (timeout->timeout_ms > 0) {
        uint64_t ticks = ((uint64_t)timeout->timeout_ms * configTICK_RATE_HZ + 999U) / 1000U;
        uint64_t maximum = (uint64_t)portMAX_DELAY - 1U;
        timeout->max_ticks = (TickType_t)(ticks > maximum ? maximum : ticks);
        if (!timeout->max_ticks) timeout->max_ticks = 1;
    }

    lua_pushlightuserdata(L, (void *)&s_lua_timeout_registry_key);
    lua_pushlightuserdata(L, timeout);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_sethook(L, lua_timeout_hook, LUA_MASKCOUNT, 1000);
}

/**
 * @brief 在执行保护边界退出后关闭指令钩子。
 * @param[in,out] L 独占有效状态借用，随后由调用者关闭状态。
 * @post registry 的调用期预算借用随状态关闭销毁，本函数无分配且不释放预算对象。
 */
static void lua_remove_timeout_hook(lua_State *L)
{
    lua_sethook(L, NULL, 0, 0);
}

/**
 * @brief 将脚本返回值按制表符追加到带前缀的输出。
 * @param[in,out] L 独占状态借用，栈中为返回值，处于保护调用及指令钩子范围。
 * @param[in,out] output 非 NULL 独占输出借用，持有其扩容块。
 * @return 接纳或截断为 true，输出 OOM 为 false，转换错误向保护调用传播。
 * @post 正常返回栈平衡，用户 __tostring 可执行 Lua 并消耗协作预算。
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

/**
 * @brief 按已知诊断片段识别内存失败。
 * @param[in] message 调用期 NUL 借用，可 NULL。
 * @return 含已知内存文本为 true，否则 false，无分配。
 */
static bool lua_message_is_memory_error(const char *message)
{
    return message &&
        (strstr(message, "not enough memory") ||
         strstr(message, "memory allocation") ||
         strstr(message, "PSRAM allocation failed"));
}

/**
 * @brief 将 Lua 状态及诊断映射到工具错误码。
 * @param[in] status 保护调用或装载状态码。
 * @param[in] message 调用期可选 NUL 借用，脚本可自行构造相同文本。
 * @return 语法、内存、取消、超时或通用工具错误，先判定语法和内存，无分配。
 */
static cc_error_code_t lua_status_error_code(int status, const char *message)
{
    if (status == LUA_ERRSYNTAX) {
        return CC_ERR_INVALID_ARGUMENT;
    }
    if (status == LUA_ERRMEM || lua_message_is_memory_error(message)) {
        return CC_ERR_OUT_OF_MEMORY;
    }
    if (message && strstr(message, "Lua execution cancelled")) {
        return CC_ERR_CANCELLED;
    }
    if (message && strstr(message, "timed out")) {
        return CC_ERR_TIMEOUT;
    }
    return CC_ERR_TOOL;
}

typedef struct {
    const char *code;
    size_t code_len;
    const char *source_name;
    lua_output_t output;
    lua_timeout_state_t timeout;
    bool initialized;
    int load_status;
} lua_run_state_t;

/**
 * @brief 在一次 Lua 保护调用内完成库注册、钩子安装、脚本和返回值转换。
 * @param[in,out] L 独占状态借用，参数一为调用栈中 lua_run_state_t 的借用。
 * @return 成功返回零个 Lua 值，错误向外层 pcall 传播，输出缓冲由外层统一清理或交付。
 * @note 返回值 __tostring 仍受指令钩子约束，C 回调等待遵循各自实现。
 */
static int lua_runtime_run_protected(lua_State *L)
{
    lua_run_state_t *run = lua_touserdata(L, 1);
    lua_settop(L, 0);
    lua_open_safe_libs(L, &run->output);
    lua_install_timeout_hook(L, &run->timeout);
    run->initialized = true;
    if (cc_cancel_token_is_cancelled(run->timeout.cancel_token))
        return luaL_error(L, "Lua execution cancelled");
    int status = luaL_loadbufferx(L, run->code, run->code_len, run->source_name, "t");
    run->load_status = status;
    if (status != LUA_OK) return lua_error(L);
    lua_call(L, 0, LUA_MULTRET);
    if (!lua_append_return_values(L, &run->output))
        return luaL_error(L, "Lua output PSRAM allocation failed");
    if (run->output.truncated && run->output.len < run->output.max_bytes &&
        !lua_output_append(&run->output, "\n[truncated]\n"))
        return luaL_error(L, "Lua output PSRAM allocation failed");
    if (!run->output.data && !lua_output_append(&run->output, "(no output)\n"))
        return luaL_error(L, "Lua output PSRAM allocation failed");
    if (!run->output.data) return luaL_error(L, "Lua output PSRAM allocation failed");
    return 0;
}

/**
 * @brief 以独立受限 Lua 状态同步执行代码并交付输出。
 * @param[in] code 调用期 NUL 源码借用，必填，仅接受文本 chunk。
 * @param[in] source_name 调用期诊断名称借用，NULL 或空串采用默认名。
 * @param[in] limits 调用期完整限制结构借用，NULL 用默认值，零字段规范化。
 * @param[out] out_text 必填拥有输出槽，有效槽入口清空，成功后由调用者 heap_caps_free。
 * @return 结果诊断由调用者 cc_result_free，参数/大小/PSRAM/OOM/脚本/超时/取消失败不交付输出。
 * @pre 调用者保持取消令牌及源文本覆盖执行和状态关闭，每次调用独占自己的状态。
 */
static cc_result_t lua_runtime_run_code_named(const char *code,
                                              const char *source_name,
                                              const lua_runtime_limits_t *limits,
                                              char **out_text)
{
    if (out_text) *out_text = NULL;
    if (!code || !out_text)
        return lua_runtime_error(CC_ERR_INVALID_ARGUMENT, "Lua code and output are required");
#if !CONFIG_SPIRAM
    return lua_runtime_error(CC_ERR_UNSUPPORTED, "Lua runtime requires PSRAM");
#endif
    lua_runtime_limits_t effective = limits ? *limits : lua_runtime_default_limits();
    lua_runtime_normalize_limits(&effective);
    if (effective.max_output_bytes == SIZE_MAX)
        return lua_runtime_error(CC_ERR_INVALID_ARGUMENT, "Lua output limit must leave room for NUL");
    if (cc_cancel_token_is_cancelled(effective.cancel_token))
        return lua_runtime_error(CC_ERR_CANCELLED, "Lua execution cancelled");
    size_t code_len = strlen(code);
    if (code_len > effective.max_script_bytes)
        return lua_runtime_errorf(CC_ERR_LIMIT_EXCEEDED,
                                  "Lua script is too large: %zu > %zu bytes",
                                  code_len, effective.max_script_bytes);

    lua_alloc_state_t alloc = {.max_bytes = effective.max_heap_bytes};
    lua_run_state_t run = {
        .code = code,
        .code_len = code_len,
        .source_name = source_name && source_name[0] ? source_name : "lua_run_code",
        .output = {.max_bytes = effective.max_output_bytes},
        .timeout = {.timeout_ms = effective.timeout_ms, .cancel_token = effective.cancel_token},
    };
    lua_State *L = lua_newstate(lua_limited_alloc, &alloc, luaL_makeseed(NULL));
    if (!L) return lua_runtime_error(CC_ERR_OUT_OF_MEMORY, "Failed to create Lua state in PSRAM");

    /* 无 upvalue 的 C 函数和 lightuserdata 入栈均不分配，后续分配全部进入保护边界。 */
    lua_pushcfunction(L, lua_runtime_run_protected);
    lua_pushlightuserdata(L, &run);
    int status = lua_pcall(L, 1, 0, 0);
    cc_result_t result = cc_result_ok();
    if (status != LUA_OK) {
        const char *message = lua_type(L, -1) == LUA_TSTRING ?
            lua_tostring(L, -1) : "non-string Lua error";
        cc_error_code_t error_code = run.initialized ?
            lua_status_error_code(run.load_status == LUA_ERRSYNTAX ? LUA_ERRSYNTAX : status, message) :
            CC_ERR_OUT_OF_MEMORY;
        result = lua_runtime_errorf(error_code, "Lua error in %s: %s", run.source_name, message);
    }
    lua_remove_timeout_hook(L);
    lua_close(L);
    if (status != LUA_OK) {
        lua_runtime_release(run.output.data);
        return result;
    }
    *out_text = run.output.data;
    return result;
}

/**
 * @brief 同步执行独立文本脚本并关闭其 Lua 状态。
 * @param[in] code 调用期 NUL 源码借用，NULL 返回参数错误。
 * @param[in] limits 调用期资源限制借用，可 NULL 使用默认值。
 * @param[out] out_text 必填空拥有槽，入口清空，成功得到 heap_caps_free 回收的输出。
 * @return 状态及拥有诊断，由 cc_result_free 回收，失败输出为 NULL。
 * @pre 源码及取消令牌存活覆盖调用，各调用状态独立，驱动共享 UI 由 board_ui 同步。
 */
cc_result_t lua_runtime_run_code(const char *code,
                                 const lua_runtime_limits_t *limits,
                                 char **out_text)
{
    return lua_runtime_run_code_named(code, "lua_run_code", limits, out_text);
}

/**
 * @brief 检查区分大小写的 .lua 后缀及非空文件名。
 * @param[in] path 调用期 NUL 路径借用，NULL 无匹配。
 * @return 长度大于四且以 .lua 结束为 1，否则 0，无资源变化。
 */
static int lua_runtime_has_lua_suffix(const char *path)
{
    size_t len = path ? strlen(path) : 0;
    return len > 4 && strcmp(path + len - 4, ".lua") == 0;
}

/**
 * @brief 经 root-scoped 文件接口读取 lua 子目录脚本并同步执行。
 * @param[in] filesystem 调用期文件端口借用，必须支持 root-scoped 文本读取。
 * @param[in] workspace_dir 调用期根目录借用，NULL/空使用默认目录。
 * @param[in] path 调用期相对 .lua 路径借用，可省略 lua/ 前缀。
 * @param[in] limits 调用期限制借用，NULL 用默认值，取消令牌存活覆盖调用。
 * @param[out] out_text 必填空拥有槽，入口清空，成功由调用者 heap_caps_free。
 * @return 拥有诊断由 cc_result_free，失败输出 NULL，读取块始终在执行后释放。
 * @note 文件端口负责并发及目录边界，整份文件读取先于脚本长度校验。
 */
cc_result_t lua_runtime_run_file(const cc_filesystem_t *filesystem,
                                 const char *workspace_dir,
                                 const char *path,
                                 const lua_runtime_limits_t *limits,
                                 char **out_text)
{
    if (out_text) *out_text = NULL;
    if (!filesystem || !filesystem->vtable ||
        !(filesystem->capabilities & CC_FS_CAP_ROOT_SCOPED) ||
        !filesystem->vtable->read_relative_text ||
        !path || !path[0] || !out_text) {
        return lua_runtime_error(CC_ERR_INVALID_ARGUMENT, "Lua file path and output are required");
    }

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
    if (cc_cancel_token_is_cancelled(effective.cancel_token))
        return lua_runtime_error(CC_ERR_CANCELLED, "Lua execution cancelled");

    const char *root = workspace_dir && workspace_dir[0] ?
        workspace_dir : LUA_RUNTIME_DEFAULT_WORKSPACE_DIR;
    char *code = NULL;
    cc_result_t read_rc = filesystem->vtable->read_relative_text(
        filesystem->self, root, relative_path, &code);
    if (read_rc.code != CC_OK) return read_rc;
    cc_result_free(&read_rc);
    if (!code) return lua_runtime_error(CC_ERR_IO, "Filesystem returned no Lua text");
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
