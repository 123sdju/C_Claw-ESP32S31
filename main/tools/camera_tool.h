#pragma once

#include "cc/app/cc_runtime_features.h"

/*
 * 创建 camera_capture 工具实例，用于通过摄像头采集一帧、JPEG 编码并可选保存/返回 Base64 图像。
 * 参数: ctx - 工具工厂上下文，必须提供 workspace root-scoped 二进制写入能力
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 成功，CC_ERR_INVALID_ARGUMENT 空输出指针，CC_ERR_OUT_OF_MEMORY 内存分配失败
 */
cc_result_t camera_tool_create(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool);
