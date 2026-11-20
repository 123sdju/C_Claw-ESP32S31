#include "lua_driver.h"

#include <string.h>

extern const lua_driver_descriptor_t g_lua_driver_core;
extern const lua_driver_descriptor_t g_board_lua_screen_driver;

static const lua_driver_descriptor_t *const s_lua_drivers[] = {
    &g_lua_driver_core,
    &g_board_lua_screen_driver,
};

/**
 * @brief 在栈顶 table 登记常驻 C 回调。
 * @param[in,out] L 独占有效状态借用，栈顶为 table，调用者提供保护边界。
 * @param[in] name 调用期有效 NUL 字段名，由 Lua 复制。
 * @param[in] fn 非 NULL 常驻函数地址。
 * @post 消费临时函数栈值并保留原 table，OOM 以 Lua 错误传播。
 */
void lua_driver_set_function(lua_State *L, const char *name, lua_CFunction fn)
{
    lua_pushcfunction(L, fn);
    lua_setfield(L, -2, name);
}

/**
 * @brief 顺序调用常驻描述符的注册回调。
 * @param[in,out] L 独占状态借用，NULL 跳过，非空时位于 Lua 保护边界内。
 * @post 全局表归状态拥有，本模块不保留 L，注册失败由保护调用清理状态。
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

/**
 * @brief 查询编译期驱动表长度。
 * @return 驱动元素数量，无动态分配。
 */
size_t lua_driver_count(void)
{
    return sizeof(s_lua_drivers) / sizeof(s_lua_drivers[0]);
}

/**
 * @brief 读取指定常驻描述符。
 * @param[in] index 从零开始的索引。
 * @return 进程期只读借用，越界为 NULL。
 */
const lua_driver_descriptor_t *lua_driver_at(size_t index)
{
    if (index >= lua_driver_count()) {
        return NULL;
    }
    return s_lua_drivers[index];
}

/**
 * @brief 匹配描述符的名称或全局表名。
 * @param[in] name 调用期 NUL 文本借用，NULL/空串返回 NULL。
 * @return 进程期只读借用，无匹配为 NULL，比较区分大小写。
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
