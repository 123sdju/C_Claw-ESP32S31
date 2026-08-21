#pragma once

#include <stddef.h>

#include "cc/app/cc_runtime_features.h"

/*
 * 返回应用层工具描述符数组，包含 6 个工具（camera_capture / lua_run_code / lua_run_file /
 * lua_driver_info / lua_script_search / screen_draw），供运行时工具注册使用。
 * 参数: out_count - [出参] 接收工具描述符数量，可为 NULL
 * 返回: 静态工具描述符数组首地址
 */
const cc_tool_descriptor_t *app_tool_registry_descriptors(size_t *out_count);
