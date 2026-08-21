#include "lua_driver.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lauxlib.h"

/*
 * 在栈顶 table 上设置一个整数键值对。
 * 参数: L     - Lua 状态指针（栈顶应为目标 table）
 * 参数: key   - 字段名
 * 参数: value - 整数值
 */
static void lua_core_set_integer(lua_State *L, const char *key, size_t value)
{
    lua_pushinteger(L, (lua_Integer)value);
    lua_setfield(L, -2, key);
}

/*
 * cclaw.heap() —— 返回当前 ESP32 堆内存信息表。
 * 无参数。返回一个包含 free、min_free、internal_free、psram_free、psram_largest 字段的 table。
 */
static int lua_cclaw_heap(lua_State *L)
{
    if (lua_gettop(L) != 0) {
        return luaL_error(L, "cclaw.heap() expects no arguments");
    }

    lua_createtable(L, 0, 5);
    lua_core_set_integer(L, "free", esp_get_free_heap_size());
    lua_core_set_integer(L, "min_free", esp_get_minimum_free_heap_size());
    lua_core_set_integer(L, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    lua_core_set_integer(L, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    lua_core_set_integer(L, "psram_largest", heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return 1;
}

/*
 * 注册 cclaw 全局 table，包含 heap() 函数。
 * 由 lua_driver 系统在 Lua 沙箱初始化时自动调用。
 * 参数: L - Lua 状态指针
 */
static void lua_driver_core_register(lua_State *L)
{
    lua_newtable(L);
    lua_driver_set_function(L, "heap", lua_cclaw_heap);
    lua_setglobal(L, "cclaw");
}

const lua_driver_descriptor_t g_lua_driver_core = {
    .name = "cclaw",
    .global_name = "cclaw",
    .summary = "Core C-Claw runtime helpers exposed to Lua.",
    .docs =
        "Global: cclaw\n"
        "Functions:\n"
        "- cclaw.heap() -> table with free, min_free, internal_free, psram_free, psram_largest.\n"
        "Example:\n"
        "local h = cclaw.heap()\n"
        "print('psram_free', h.psram_free)\n",
    .register_fn = lua_driver_core_register,
};
