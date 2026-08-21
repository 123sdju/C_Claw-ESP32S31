#pragma once

#include "cc/app/cc_runtime_features.h"
#include "cc/core/cc_result.h"

/*
 * 创建 lua_run_code 工具实例，允许 Agent 直接执行带安全限制的 Lua 源代码。
 * 参数: ctx - 工具工厂上下文
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 或错误
 */
cc_result_t lua_run_code_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                     cc_tool_t *out_tool);

/*
 * 创建 lua_run_file 工具实例，允许 Agent 运行 lua/ 目录下经安全检查的脚本文件。
 * 参数: ctx - 工具工厂上下文
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 或错误
 */
cc_result_t lua_run_file_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                     cc_tool_t *out_tool);

/*
 * 创建 lua_driver_info 工具实例，用于查询可用 Lua 驱动列表或指定驱动的详细文档。
 * 参数: ctx - 工具工厂上下文
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 或错误
 */
cc_result_t lua_driver_info_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                    cc_tool_t *out_tool);

/*
 * 创建 lua_script_search 工具实例，用于按关键词/路径搜索 workspace lua 目录下的可复用脚本及其元数据。
 * 参数: ctx - 工具工厂上下文
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 或错误
 */
cc_result_t lua_script_search_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                    cc_tool_t *out_tool);
