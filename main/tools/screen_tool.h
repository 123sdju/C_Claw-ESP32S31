#pragma once

#include "cc/app/cc_runtime_features.h"

/*
 * 创建 screen_draw 工具实例，允许 Agent 通过 JSON 指令在 LCD 工具区域进行绘图操作
 * （clear 清空 / append 追加日志行 / text 绘制文字 / rect 绘制矩形 / flush 刷新显示）。
 * 参数: ctx - 工具工厂上下文（当前未使用）
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 成功，CC_ERR_INVALID_ARGUMENT 空输出指针，CC_ERR_OUT_OF_MEMORY 内存分配失败
 */
cc_result_t screen_tool_create(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool);
