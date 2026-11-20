#include "storage_nvs_service.h"
#include "esp_err.h"
#include "nvs_flash.h"

/**
 * @brief 初始化默认 NVS 分区，保留已有凭据和设备状态。
 * @return 原生成功为 CC_OK，其余为携带原生错误名的 CC_ERR_STORAGE，诊断由调用者释放。
 * @note 可重试原生初始化，NO_FREE_PAGES 或版本不符也直接返回错误。修复或迁移由
 *       生命周期 owner 决定，本函数不擦除分区，也不接管全局 NVS 的反初始化责任。
 */
cc_result_t storage_nvs_service_check(void)
{
    esp_err_t err=nvs_flash_init();
    return err==ESP_OK ? cc_result_ok() :
        cc_result_errf(CC_ERR_STORAGE,"Cannot initialize device NVS: %s",esp_err_to_name(err));
}
