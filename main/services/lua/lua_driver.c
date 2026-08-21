#include "lua_driver.h"

#include <string.h>

extern const lua_driver_descriptor_t g_lua_driver_core;
extern const lua_driver_descriptor_t g_board_lua_screen_driver;

static const lua_driver_descriptor_t *const s_lua_drivers[] = {
    &g_lua_driver_core,
    &g_board_lua_screen_driver,
};

/*
 * 在栈顶 table 上注册一个 Lua C 函数为指定名称的字段。
 * 参数: L    - Lua 状态指针（栈顶应为目标 table）
 * 参数: name - Lua 函数名
 * 参数: fn   - C 函数指针
 */
void lua_driver_set_function(lua_State *L, const char *name, lua_CFunction fn)
{
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

/*
 * 遍历 s_lua_drivers[] 注册表，对每个有效的驱动描述符调用其 register_fn(L) 注册到 Lua 全局环境。
 * 参数: L - Lua 状态指针
 */
void lua_driver_register_all(lua_State *L)
{
    if (!L) {
        return;
    }

    for (size_t i = 0; i < lua_driver_count(); i++) {
        const lua_driver_descriptor_t *driver = s_lua_drivers[i];
        if (driver && driver->register_fn) {
            driver->register_fn(L);
        }
    }
}

/*
 * 返回注册表中驱动的总数。
 * 返回: 驱动数量
 */
size_t lua_driver_count(void)
{
    return sizeof(s_lua_drivers) / sizeof(s_lua_drivers[0]);
}

/*
 * 按索引获取驱动描述符。
 * 参数: index - 驱动索引（0-based）
 * 返回: 描述符指针，越界返回 NULL
 */
const lua_driver_descriptor_t *lua_driver_at(size_t index)
{
    if (index >= lua_driver_count()) {
        return NULL;
    }
    return s_lua_drivers[index];
}

/*
 * 按名称查找驱动描述符，匹配 name 或 global_name 字段。
 * 参数: name - 驱动名
 * 返回: 描述符指针，未找到返回 NULL
 */
const lua_driver_descriptor_t *lua_driver_find(const char *name)
{
    if (!name || !name[0]) {
        return NULL;
    }

    for (size_t i = 0; i < lua_driver_count(); i++) {
        const lua_driver_descriptor_t *driver = s_lua_drivers[i];
        if (!driver) {
            continue;
        }
        if ((driver->name && strcmp(driver->name, name) == 0) ||
            (driver->global_name && strcmp(driver->global_name, name) == 0)) {
            return driver;
        }
    }
    return NULL;
}
