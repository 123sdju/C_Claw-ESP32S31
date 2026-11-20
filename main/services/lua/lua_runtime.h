#pragma once

#include <stddef.h>

#include "cc/core/cc_result.h"
#include "cc/app/cc_cancel_token.h"
#include "cc/ports/cc_filesystem.h"

#define LUA_RUNTIME_DEFAULT_WORKSPACE_DIR "/usb0/workspace"
#define LUA_RUNTIME_SCRIPT_SUBDIR "lua"

typedef struct lua_runtime_limits {
    size_t max_script_bytes;
    size_t max_output_bytes;
    size_t max_heap_bytes;
    int timeout_ms;
    cc_cancel_token_t *cancel_token; /**< 调用期取消令牌借用，NULL 禁用协作取消。 */
} lua_runtime_limits_t;

/**
 * @brief 获取脚本 4096 字节、输出 2048 字节、Lua 堆 128 KiB、超时 1000 ms 的默认限制。
 * @return 按值结构，取消令牌 NULL，无分配，可并发调用。
 */
lua_runtime_limits_t lua_runtime_default_limits(void);

/**
 * @brief 在独立 PSRAM Lua 状态中同步执行文本，收集 print 和返回值。
 * @param[in] code 调用期 NUL 源码借用，必填。
 * @param[in] limits 调用期限制借用，NULL 用默认值，零资源字段规范为默认值。
 * @param[out] out_text 必填空拥有槽，入口清空，成功输出由 heap_caps_free 回收。
 * @return 状态及拥有诊断由 cc_result_free 回收，失败输出 NULL，无 PSRAM 返回不支持。
 * @pre 取消令牌与输入存活覆盖调用，各 Lua 状态独占，底层驱动负责共享硬件同步。
 * @note pcall/xpcall 保留，指令超时与取消为可捕获的协作错误，C 库及最终化禁钩子区间按自身实现运行。
 */
cc_result_t lua_runtime_run_code(const char *code,
                                 const lua_runtime_limits_t *limits,
                                 char **out_text);

/**
 * @brief 读取工作区 lua 子目录中的 .lua 文件并以独立状态执行。
 * @param[in] filesystem 调用期 root-scoped 文本读取端口借用，负责并发与目录边界。
 * @param[in] workspace_dir 调用期 NUL 根目录借用，NULL/空用默认目录。
 * @param[in] path 调用期相对 .lua 路径借用，可省略 lua/。
 * @param[in] limits 调用期限制借用，NULL 用默认值，取消令牌覆盖调用期。
 * @param[out] out_text 必填空拥有槽，入口清空，成功由 heap_caps_free 回收。
 * @return 状态及拥有诊断由 cc_result_free 回收，失败输出 NULL，文件内容由本函数释放。
 * @note 文件完整读取后校验脚本大小，执行预算语义与 lua_runtime_run_code 相同。
 */
cc_result_t lua_runtime_run_file(const cc_filesystem_t *filesystem,
                                 const char *workspace_dir,
                                 const char *path,
                                 const lua_runtime_limits_t *limits,
                                 char **out_text);
