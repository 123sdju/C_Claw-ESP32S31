#pragma once

#include "cc/app/cc_runtime_features.h"

/**
 * @brief 创建工具区域绘制工具，支持清空、追加、文字、矩形及刷新。
 * @param[in] ctx 未使用工厂借用，可 NULL。
 * @param[out] out_tool 必填独占空槽，入口清空，成功用 vtable destroy 回收实例。
 * @return CC_OK，输出 NULL 为参数错误，分配失败 OOM，诊断由 cc_result_free 回收。
 * @note UI 并发由 board_ui 负责，销毁须等待在途调用完成。
 */
cc_result_t screen_tool_create(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool);
