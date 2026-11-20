#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
/** @brief 返回模型错误名称占位。 @param[in] err 未使用的错误码。 @return 静态借用字符串error，无需释放。 */
static inline const char *esp_err_to_name(esp_err_t err) { (void)err; return "error"; }
