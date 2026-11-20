#pragma once

#include "cc/app/cc_runtime_features.h"

/**
 * @brief 获取合并 SDK 和应用工具、执行只读记忆策略后的 feature 集合。
 * @return 进程期有效的只读借用指针，由内部持有。首次工具数组分配失败时，
 *         集合保留其他 feature，工具列表为空，并将此结果用于后续调用。
 * @note 首次调用同步合并描述符，竞争者自旋等待，完成后以 acquire/release 发布。
 *       调用者可并发读取，builder 和 generation 可在整个存活期借用集合及其数组。
 */
const cc_runtime_feature_set_t *cclaw_runtime_features(void);
