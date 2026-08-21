#include "app_tool_registry.h"

#include "camera_tool.h"
#include "lua_tool.h"
#include "screen_tool.h"

static const cc_tool_descriptor_t s_app_tools[] = {
    {"camera_capture", "camera", 1, camera_tool_create},
    {"lua_run_code", "lua_code", 1, lua_run_code_tool_create},
    {"lua_run_file", "lua_file", 1, lua_run_file_tool_create},
    {"lua_driver_info", "lua_drivers", 1, lua_driver_info_tool_create},
    {"lua_script_search", "lua_scripts", 1, lua_script_search_tool_create},
    {"screen_draw", "display", 1, screen_tool_create},
};

/*
 * 返回应用层工具描述符数组，包含 6 个工具（camera_capture / lua_run_code / lua_run_file /
 * lua_driver_info / lua_script_search / screen_draw），供运行时工具注册使用。
 * 参数: out_count - [出参] 接收工具描述符数量，可为 NULL
 * 返回: 静态工具描述符数组首地址
 */
const cc_tool_descriptor_t *app_tool_registry_descriptors(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_app_tools) / sizeof(s_app_tools[0]);
    }
    return s_app_tools;
}
