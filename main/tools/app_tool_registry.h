#pragma once

#include <stddef.h>

#include "cc/app/cc_runtime_features.h"

/**
 * @brief 查询摄像头、Lua 和屏幕的应用工具工厂描述符。
 * @param[out] out_count 可选元素数输出，允许 NULL，当前固定数组包含六项。
 * @return 进程期有效的只读借用数组，按返回的元素数遍历。
 * @note 查询可并发执行，实例创建与资源清理由运行时 generation 装配流程负责。
 */
const cc_tool_descriptor_t *app_tool_registry_descriptors(size_t *out_count);
