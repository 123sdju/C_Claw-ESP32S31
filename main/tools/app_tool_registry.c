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

/**
 * @brief 查询摄像头、Lua 和屏幕的六个应用工具描述符。
 * @param[out] out_count 可选输出，接收静态数组的元素数，允许 NULL。
 * @return 进程期有效的只读借用数组，工厂在 generation 装配时才创建工具实例。
 * @note 查询使用不可变静态数组，可并发调用。调用者按 count 遍历，数组按固定长度保存。
 */
const cc_tool_descriptor_t *app_tool_registry_descriptors(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(s_app_tools) / sizeof(s_app_tools[0]);
    }
    return s_app_tools;
}
