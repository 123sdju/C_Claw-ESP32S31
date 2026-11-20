#include "lua_driver.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board_ui.h"
#include "lauxlib.h"

/**
 * @brief 解析工具区域的角色名称。
 * @param[in] role 调用期 NUL 借用，NULL 默认 tool，空串无效。
 * @param[out] out_role 独占输出借用，必填，仅成功时写入。
 * @return 已识别为 true，输出 NULL 或未知名称为 false，无分配。
 */
static bool lua_screen_parse_role(const char *role, board_conversation_role_t *out_role)
{
    if (!out_role) {
        return false;
    }
    if (!role || strcmp(role, "tool") == 0) {
        *out_role = BOARD_CONV_TOOL;
        return true;
    }
    if (strcmp(role, "status") == 0) {
        *out_role = BOARD_CONV_STATUS;
        return true;
    }
    if (strcmp(role, "thinking") == 0) {
        *out_role = BOARD_CONV_THINKING;
        return true;
    }
    if (strcmp(role, "error") == 0) {
        *out_role = BOARD_CONV_ERROR;
        return true;
    }
    return false;
}

/**
 * @brief 解析恰好四位 RGB565 十六进制颜色及可选井号。
 * @param[in] text 调用期 NUL 借用，NULL/空默认为白色。
 * @param[out] out_color 独占输出借用，必填，仅成功时更新。
 * @return 格式有效为 true，输出 NULL 或格式错误 false，无分配。
 */
static bool lua_screen_parse_rgb565(const char *text, uint16_t *out_color)
{
    if (!out_color) {
        return false;
    }
    if (!text || !text[0]) {
        *out_color = 0xffff;
        return true;
    }
    if (text[0] == '#') {
        text++;
    }

    unsigned value = 0;
    for (int i = 0; i < 4; i++) {
        char c = text[i];
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= (unsigned)(c - 'A' + 10);
        } else {
            return false;
        }
    }
    if (text[4] != '\0') {
        return false;
    }

    *out_color = (uint16_t)value;
    return true;
}

/**
 * @brief 读取 Lua 可选角色参数，缺省采用 tool。
 * @param[in,out] L 独占状态借用，由 Lua 保护调用覆盖，字符串转型可分配。
 * @param[in] index 参数栈索引。
 * @param[out] out_role 必填独占枚举输出借用，无拥有权转移。
 * @return 成功 0，参数或角色无效抛出 Lua 错误，OOM 向保护调用传播。
 */
static int lua_screen_role_arg(lua_State *L, int index, board_conversation_role_t *out_role)
{
    const char *role = luaL_optstring(L, index, "tool");
    if (!lua_screen_parse_role(role, out_role)) {
        return luaL_error(L, "screen role must be one of: tool, status, thinking, error");
    }
    return 0;
}

/**
 * @brief 实现 screen.clear 并请求清空工具区域。
 * @param[in,out] L 独占状态借用，要求零个 Lua 参数。
 * @return 成功零个返回值，参数数量错误抛 Lua 错误，UI 同步由 board_ui 负责。
 */
static int lua_screen_clear(lua_State *L)
{
    if (lua_gettop(L) != 0) {
        return luaL_error(L, "screen.clear() expects no arguments");
    }
    board_ui_tool_area_clear();
    return 0;
}

/**
 * @brief 实现 screen.append 并追加工具区域日志。
 * @param[in,out] L 独占状态借用，Lua 参数为 text 与可选 role，调用期字符串借用。
 * @return 成功零值，参数错误/OOM 向保护调用传播，文本由 board_ui 在调用内复制。
 * @note UI 持锁及异步刷新由 board_ui 负责，Lua 栈值不转交拥有权。
 */
static int lua_screen_append(lua_State *L)
{
    int argc = lua_gettop(L);
    if (argc < 1 || argc > 2) {
        return luaL_error(L, "screen.append(text [, role]) expects 1 or 2 arguments");
    }

    const char *text = luaL_checkstring(L, 1);
    board_conversation_role_t role = BOARD_CONV_TOOL;
    lua_screen_role_arg(L, 2, &role);
    board_ui_tool_area_append(role, text);
    return 0;
}

/**
 * @brief 实现 screen.text 并提交工具区域坐标文本。
 * @param[in,out] L 独占状态借用，Lua 参数 x/y 为像素整数，随后为 text 和可选 role。
 * @return 成功零值，参数错误/OOM 抛 Lua 错误，文本借用只覆盖底层同步调用。
 * @note 坐标裁剪及 UI 并发同步由 board_ui 负责。
 */
static int lua_screen_text(lua_State *L)
{
    int argc = lua_gettop(L);
    if (argc < 3 || argc > 4) {
        return luaL_error(L, "screen.text(x, y, text [, role]) expects 3 or 4 arguments");
    }

    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    const char *text = luaL_checkstring(L, 3);
    board_conversation_role_t role = BOARD_CONV_TOOL;
    lua_screen_role_arg(L, 4, &role);
    board_ui_tool_area_draw_text(x, y, role, text);
    return 0;
}

/**
 * @brief 实现 screen.rect 并提交填充矩形。
 * @param[in,out] L 独占状态借用，Lua 参数 x/y/width/height 为像素整数，color 可选。
 * @return 成功零值，尺寸非正、颜色或参数无效抛 Lua 错误，无 C 拥有资源。
 * @note 默认白色，绘制裁剪及 UI 并发由 board_ui 负责。
 */
static int lua_screen_rect(lua_State *L)
{
    int argc = lua_gettop(L);
    if (argc < 4 || argc > 5) {
        return luaL_error(L, "screen.rect(x, y, width, height [, color]) expects 4 or 5 arguments");
    }

    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int width = (int)luaL_checkinteger(L, 3);
    int height = (int)luaL_checkinteger(L, 4);
    if (width <= 0 || height <= 0) {
        return luaL_error(L, "screen.rect width and height must be positive");
    }

    uint16_t color = 0xffff;
    const char *color_text = luaL_optstring(L, 5, "FFFF");
    if (!lua_screen_parse_rgb565(color_text, &color)) {
        return luaL_error(L, "screen.rect color must be RGB565 hex like F800 or #F800");
    }

    board_ui_tool_area_draw_rect(x, y, width, height, color);
    return 0;
}

/**
 * @brief 实现 screen.flush 并请求工具区域刷新。
 * @param[in,out] L 独占状态借用，要求零个 Lua 参数。
 * @return 成功零值，参数错误抛 Lua 错误，实际刷新由 UI 任务调度。
 */
static int lua_screen_flush(lua_State *L)
{
    if (lua_gettop(L) != 0) {
        return luaL_error(L, "screen.flush() expects no arguments");
    }
    board_ui_tool_area_request_flush();
    return 0;
}

/**
 * @brief 在当前独立状态登记 screen 表及五个绘制回调。
 * @param[in,out] L 独占状态借用，调用者提供 Lua 保护边界，OOM 向外传播。
 * @post 表归 Lua 状态拥有，C 回调进程期常驻，正常返回栈平衡。
 */
static void board_lua_screen_register(lua_State *L)
{
    lua_newtable(L);
    lua_driver_set_function(L, "clear", lua_screen_clear);
    lua_driver_set_function(L, "append", lua_screen_append);
    lua_driver_set_function(L, "text", lua_screen_text);
    lua_driver_set_function(L, "rect", lua_screen_rect);
    lua_driver_set_function(L, "flush", lua_screen_flush);
    lua_setglobal(L, "screen");
}

const lua_driver_descriptor_t g_board_lua_screen_driver = {
    .name = "screen",
    .global_name = "screen",
    .summary = "Draw and write inside the LCD Tool Area from Lua.",
    .docs =
        "Global: screen\n"
        "Functions:\n"
        "- screen.clear()\n"
        "- screen.append(text [, role]) where role is tool, status, thinking, or error.\n"
        "- screen.text(x, y, text [, role]) draws text inside the Tool Area.\n"
        "- screen.rect(x, y, width, height [, color]) draws a rectangle; color is RGB565 hex like F800 or #F800.\n"
        "- screen.flush()\n"
        "Example:\n"
        "screen.clear()\n"
        "screen.append('Lua is running', 'status')\n"
        "screen.rect(0, 0, 40, 20, '07E0')\n"
        "screen.flush()\n",
    .register_fn = board_lua_screen_register,
};
