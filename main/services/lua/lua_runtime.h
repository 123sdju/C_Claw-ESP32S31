#pragma once

#include <stddef.h>

#include "cc/core/cc_result.h"
#include "cc/ports/cc_filesystem.h"

#define LUA_RUNTIME_DEFAULT_WORKSPACE_DIR "/usb0/workspace"
#define LUA_RUNTIME_SCRIPT_SUBDIR "lua"

typedef struct lua_runtime_limits {
    size_t max_script_bytes;
    size_t max_output_bytes;
    size_t max_heap_bytes;
    int timeout_ms;
} lua_runtime_limits_t;

/*
 * 返回 Lua 运行时的默认资源限制（max_script_bytes=4096, max_output_bytes=2048,
 * max_heap_bytes=128KB, timeout_ms=1000）。
 * 返回: 填充默认值的 lua_runtime_limits_t 结构体
 */
lua_runtime_limits_t lua_runtime_default_limits(void);

/*
 * 在受控的 Lua 沙箱中执行一段 Lua 代码字符串。
 * 沙箱限制：PSRAM 分配器、内存上限、输出缓冲上限、超时钩子。
 * 只开放 base/table/string/math 库，屏蔽 dofile/loadfile/require。
 * 提供 cclaw.* 驱动注册表和自定义 print()（输出到缓冲区）。
 * 参数: code     - Lua 源码字符串
 * 参数: limits   - 资源限制（NULL 则使用默认值）
 * 参数: out_text - 输出文本（调用方需 heap_caps_free 释放）
 * 返回: cc_result_t，CC_OK 表示成功
 */
cc_result_t lua_runtime_run_code(const char *code,
                                 const lua_runtime_limits_t *limits,
                                 char **out_text);

/*
 * 读取并执行 Lua 脚本文件。路径安全校验后加载文件内容，调用 lua_runtime_run_code_named。
 * 参数: workspace_dir - 工作区根目录（NULL 则使用 /usb0/workspace）
 * 参数: path          - 脚本文件路径
 * 参数: limits        - 资源限制（NULL 则使用默认值）
 * 参数: out_text      - 输出文本（调用方需 heap_caps_free 释放）
 * 返回: cc_result_t，CC_OK 表示成功
 */
cc_result_t lua_runtime_run_file(const cc_filesystem_t *filesystem,
                                 const char *workspace_dir,
                                 const char *path,
                                 const lua_runtime_limits_t *limits,
                                 char **out_text);
