#pragma once

#include "cc/core/cc_result.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 尝试初始化默认 NVS，已有凭据和设备状态持续保留。
 * @return 成功为 CC_OK，原生初始化失败为 CC_ERR_STORAGE，拥有诊断由调用者 cc_result_free。
 * @note 原生初始化可重复调用，分区无空间或版本不符时也保留错误及数据，修复由外层负责。
 */
cc_result_t storage_nvs_service_check(void);

#ifdef __cplusplus
}
#endif
