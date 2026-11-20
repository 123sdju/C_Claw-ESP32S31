#pragma once

#include "cc/app/cc_runtime_features.h"

/**
 * @brief 创建可采集、保存并返回 JPEG 图像的工具。
 * @param[in] ctx 工厂上下文借用，要求 root-scoped 二进制写入，端口存活覆盖工具期。
 * @param[out] out_tool 必填独占空槽，成功取得实例拥有权，用 vtable destroy 回收。
 * @return CC_OK，参数/能力无效为参数错误，分配失败 OOM，诊断由 cc_result_free 回收。
 * @note 创建后文件端口仍由调用方拥有，销毁须等待在途调用完成。
 */
cc_result_t camera_tool_create(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool);
