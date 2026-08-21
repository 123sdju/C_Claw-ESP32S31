#pragma once

#include <stddef.h>

#include "lua.h"

typedef void (*lua_driver_register_fn_t)(lua_State *L);

typedef struct lua_driver_descriptor {
    const char *name;
    const char *global_name;
    const char *summary;
    const char *docs;
    lua_driver_register_fn_t register_fn;
} lua_driver_descriptor_t;

/*
 * 遍历 lua_driver 注册表，逐个调用每个驱动的 register_fn(L) 注册到 Lua 环境。
 * 参数: L - Lua 状态指针
 */
void lua_driver_register_all(lua_State *L);
/*
 * 获取 lua_driver 注册表中驱动的数量。
 * 返回: 驱动数量
 */
size_t lua_driver_count(void);
/*
 * 按索引获取驱动描述符。
 * 参数: index - 驱动索引（从 0 开始）
 * 返回: 驱动描述符指针（越界返回 NULL）
 */
const lua_driver_descriptor_t *lua_driver_at(size_t index);
/*
 * 按名称查找驱动描述符（匹配 name 或 global_name）。
 * 参数: name - 驱动名称
 * 返回: 驱动描述符指针（未找到返回 NULL）
 */
const lua_driver_descriptor_t *lua_driver_find(const char *name);
/*
 * 在栈顶 table 上注册一个 Lua C 函数。
 * 参数: L    - Lua 状态指针（栈顶应为目标 table）
 * 参数: name - 函数名
 * 参数: fn   - C 函数指针
 */
void lua_driver_set_function(lua_State *L, const char *name, lua_CFunction fn);
