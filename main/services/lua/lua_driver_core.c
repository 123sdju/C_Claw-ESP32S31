#include "lua_driver.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "lauxlib.h"

/**
 * @brief 向栈顶堆信息 table 写入字节计数。
 * @param[in,out] L 独占状态借用，栈顶为 table，处于 Lua 保护边界。
 * @param[in] key 调用期非 NULL NUL 字段名，Lua 复制。
 * @param[in] value 堆字节数，当前目标 size_t 可由 lua_Integer 表示。
 * @post table 保留，临时整数消费，OOM 向保护调用传播。
 */
static void lua_core_set_integer(lua_State *L, const char *key, size_t value)
{
    lua_pushinteger(L, (lua_Integer)value);
    lua_setfield(L, -2, key);
}

/**
 * @brief 实现无参数 cclaw.heap 并采样各能力堆。
 * @param[in,out] L 独占有效状态借用，栈含本次参数。
 * @return 成功推入一个拥有的 table 并返回 1，参数非零或 OOM 抛出 Lua 错误。
 * @note 各字段单位为字节，各次采样反映对应调用时刻。
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

/**
 * @brief 在新状态登记 cclaw 全局表及 heap 回调。
 * @param[in,out] L 独占状态借用，由 runtime 在保护边界调用。
 * @post table 交给 Lua 状态，C 回调常驻，注册 OOM 向保护调用传播。
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
