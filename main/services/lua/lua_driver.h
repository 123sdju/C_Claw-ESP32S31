#pragma once

#include <stddef.h>

#include "lua.h"

/**
 * @brief 在独占 Lua 状态中登记驱动全局表。
 * @param[in,out] L 有效状态借用，调用处位于 Lua 保护边界内，注册可因 OOM 抛出 Lua 错误。
 * @post 注册后的 C 函数和描述符为进程期常驻，状态由 runtime 关闭。
 */
typedef void (*lua_driver_register_fn_t)(lua_State *L);

typedef struct lua_driver_descriptor {
    const char *name; /**< 进程期名称借用，可 NULL。 */
    const char *global_name; /**< 进程期全局表名借用，可 NULL。 */
    const char *summary; /**< 进程期摘要借用，可 NULL。 */
    const char *docs; /**< 进程期文档借用，可 NULL。 */
    lua_driver_register_fn_t register_fn; /**< 常驻注册回调，可 NULL 时跳过。 */
} lua_driver_descriptor_t;

/**
 * @brief 遍历常驻注册表并在当前状态登记驱动。
 * @param[in,out] L 独占状态借用，NULL 跳过，非空时调用者提供 Lua 保护边界。
 * @post 不保留状态地址，成功注册的表归 Lua 状态拥有。
 */
void lua_driver_register_all(lua_State *L);
/**
 * @brief 查询常驻驱动数量。
 * @return 编译期注册表元素数，无动态资源。
 */
size_t lua_driver_count(void);
/**
 * @brief 按索引查询常驻驱动。
 * @param[in] index 从零开始的元素索引。
 * @return 进程期只读描述符借用，越界返回 NULL。
 */
const lua_driver_descriptor_t *lua_driver_at(size_t index);
/**
 * @brief 按精确名称或全局表名查询驱动。
 * @param[in] name 调用期 NUL 名称借用，NULL 或空串无匹配。
 * @return 进程期只读描述符借用，找不到返回 NULL。
 */
const lua_driver_descriptor_t *lua_driver_find(const char *name);
/**
 * @brief 将 C 函数写入栈顶 table 字段。
 * @param[in,out] L 独占有效状态借用，栈顶必须是目标 table。
 * @param[in] name 调用期有效 NUL 字段名，Lua 复制持有。
 * @param[in] fn 进程期常驻非 NULL Lua C 函数。
 * @post 栈顶 table 保持，字段更新，分配失败由调用者 Lua 保护边界接收。
 */
void lua_driver_set_function(lua_State *L, const char *name, lua_CFunction fn);
