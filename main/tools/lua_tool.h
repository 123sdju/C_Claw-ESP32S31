#pragma once

#include "cc/app/cc_runtime_features.h"
#include "cc/core/cc_result.h"

/**
 * @brief 创建受资源预算约束的 Lua 代码执行工具。
 * @param[in] ctx 工厂借用，要求 root-scoped 读、枚举、stat，文件端口存活覆盖工具期。
 * @param[out] out_tool 必填独占空槽，成功取得实例拥有权，用 vtable destroy 回收。
 * @return CC_OK、不支持或 OOM，诊断由 cc_result_free 回收。
 * @note 销毁须等待在途调用，预算为可被 pcall/xpcall 捕获的协作错误。
 */
cc_result_t lua_run_code_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                     cc_tool_t *out_tool);

/**
 * @brief 创建工作区 lua 目录的脚本执行工具。
 * @param[in] ctx 工厂借用，要求 root-scoped 读、枚举、stat，文件端口存活覆盖工具期。
 * @param[out] out_tool 必填独占空槽，成功取得实例拥有权，用 vtable destroy 回收。
 * @return CC_OK、不支持或 OOM，诊断由 cc_result_free 回收。
 * @note 销毁须等待在途调用，取消令牌按单次工具上下文借用。
 */
cc_result_t lua_run_file_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                     cc_tool_t *out_tool);

/**
 * @brief 创建常驻 Lua 驱动文档查询工具。
 * @param[in] ctx 工厂借用，要求 root-scoped 读、枚举、stat，文件端口存活覆盖工具期。
 * @param[out] out_tool 必填独占空槽，成功取得实例拥有权，用 vtable destroy 回收。
 * @return CC_OK、不支持或 OOM，诊断由 cc_result_free 回收。
 * @note 销毁须等待在途调用，常驻描述符无额外拥有资源。
 */
cc_result_t lua_driver_info_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                    cc_tool_t *out_tool);

/**
 * @brief 创建工作区 Lua 脚本关键词及路径查询工具。
 * @param[in] ctx 工厂借用，要求 root-scoped 读、枚举、stat，文件端口存活覆盖工具期。
 * @param[out] out_tool 必填独占空槽，成功取得实例拥有权，用 vtable destroy 回收。
 * @return CC_OK、不支持或 OOM，诊断由 cc_result_free 回收。
 * @note 文件端口负责并发读取，销毁须等待在途调用完成。
 */
cc_result_t lua_script_search_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,
                                    cc_tool_t *out_tool);
