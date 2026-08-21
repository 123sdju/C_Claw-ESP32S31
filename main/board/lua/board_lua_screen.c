#include "lua_driver.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "board_ui.h"
#include "lauxlib.h"

/*
 * 将字符串角色名称转换为 board_conversation_role_t 枚举。
 * 参数: role     - 角色名称字符串（"tool"/"status"/"thinking"/"error"），NULL 或空默认 tool
 *       out_role - 输出枚举值
 * 返回: true 解析成功，false 角色名无效
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

/*
 * 将 RGB565 十六进制字符串（如 "F800" 或 "#F800"）解析为 uint16_t 颜色值。
 * 参数: text      - 十六进制颜色字符串，NULL 默认返回白色 0xFFFF
 *       out_color - 输出 16 位颜色值
 * 返回: true 解析成功，false 格式无效
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

/*
 * 从 Lua 栈读取可选角色参数并解析为枚举值，默认角色为 tool。若无效则触发 Lua 错误。
 * 参数: L        - Lua 状态机
 *       index    - 角色参数在栈上的位置
 *       out_role - 输出枚举值
 * 返回: 0 成功（正常返回），或通过 luaL_error 不返回
 */
static int lua_screen_role_arg(lua_State *L, int index, board_conversation_role_t *out_role)
{
    const char *role = luaL_optstring(L, index, "tool");
    if (!lua_screen_parse_role(role, out_role)) {
        return luaL_error(L, "screen role must be one of: tool, status, thinking, error");
    }
    return 0;
}

/*
 * Lua screen.clear() — 清空 Tool Area 中所有绘制和文本内容。
 * 返回: 0（Lua 栈上无返回值）
 */
static int lua_screen_clear(lua_State *L)
{
    if (lua_gettop(L) != 0) {
        return luaL_error(L, "screen.clear() expects no arguments");
    }
    board_ui_tool_area_clear();
    return 0;
}

/*
 * Lua screen.append(text [, role]) — 向 Tool Area 追加一行文本。
 * 参数: text - 文本内容（必须）
 *       role - 可选角色（默认 "tool"）
 * 返回: 0（无返回值）
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

/*
 * Lua screen.text(x, y, text [, role]) — 在 Tool Area 指定坐标绘制文本。
 * 参数: x/y - 像素坐标
 *       text - 文本内容
 *       role - 可选角色（默认 "tool"）
 * 返回: 0（无返回值）
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

/*
 * Lua screen.rect(x, y, width, height [, color]) — 在 Tool Area 绘制填充矩形。
 * 参数: x/y/width/height - 矩形位置和尺寸（width/height 必须 > 0）
 *       color - 可选 RGB565 十六进制颜色（默认 "FFFF" 白色），如 "F800" 或 "#F800"
 * 返回: 0（无返回值）
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

/*
 * Lua screen.flush() — 触发 Tool Area 区域重绘。
 * 返回: 0（无返回值）
 */
static int lua_screen_flush(lua_State *L)
{
    if (lua_gettop(L) != 0) {
        return luaL_error(L, "screen.flush() expects no arguments");
    }
    board_ui_tool_area_request_flush();
    return 0;
}

/*
 * 向 Lua 全局注册 screen 表，内含 clear/append/text/rect/flush 五个绘制函数。
 * 参数: L - Lua 状态机
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
