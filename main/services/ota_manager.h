#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    OTA_MANAGER_IDLE = 0,
    OTA_MANAGER_UPLOADING,
    OTA_MANAGER_PENDING_REBOOT,
    OTA_MANAGER_ERROR,
} ota_manager_state_t;

typedef struct {
    ota_manager_state_t state;
    size_t bytes_written;
    size_t expected_size;
    size_t target_size;
    char upload_name[96];
    char running_label[24];
    char running_app_name[33];
    char running_version[33];
    char running_sha256[65];
    char running_uploaded_at[40];
    char running_ota_state[24];
    char target_label[24];
    char target_app_name[33];
    char target_version[33];
    char target_sha256[65];
    char target_uploaded_at[40];
    char boot_label[24];
    char boot_app_name[33];
    char boot_version[33];
    char boot_sha256[65];
    char boot_uploaded_at[40];
    char error[128];
    bool rollback_pending;
} ota_manager_status_t;

typedef struct {
    char label[24];
    char app_name[33];
    char version[33];
    char sha256[65];
    char uploaded_at[40];
    size_t size;
    bool running;
    bool boot;
    bool selected;
} ota_manager_partition_info_t;

/*
 * 将当前运行分区标记为有效/取消回滚。在 OTA 后验证 app 稳定时调用。
 */
void ota_manager_mark_app_valid(void);
/*
 * 刷新分区状态信息（running/boot/target 分区标签、版本、SHA256、OTA 状态等）。
 */
void ota_manager_refresh_status(void);
/*
 * 开始 OTA 写入：选择目标分区、校验镜像大小、设置状态为 UPLOADING。
 * 参数: expected_size - 预期固件大小（字节）
 * 参数: upload_name   - 上传文件名（用于显示和 NVS 存储）
 * 参数: browser_time  - 浏览器上传时间（存 NVS，用于分区 uploaded_at）
 * 返回: ESP_OK 成功，ESP_ERR_NOT_FOUND 无可用分区，ESP_ERR_INVALID_SIZE 大小超限
 */
esp_err_t ota_manager_begin(size_t expected_size, const char *upload_name, const char *browser_time);
/*
 * 写入 OTA 数据块。首次写入时校验 ESP 镜像魔数字节。
 * 参数: data - 数据缓冲区
 * 参数: len  - 写入字节数
 * 返回: ESP_OK 成功
 */
esp_err_t ota_manager_write(const void *data, size_t len);
/*
 * 完成 OTA 写入：调用 esp_ota_end、设置启动分区、记录 uploaded_at 到 NVS。
 * 返回: ESP_OK 成功，状态变为 PENDING_REBOOT
 */
esp_err_t ota_manager_finish(void);
/*
 * 取消 OTA 操作：如有打开的更新句柄则 abort，重置所有状态为 IDLE。
 */
void ota_manager_cancel(void);
/*
 * 设置指定标签的分区为下次启动分区。
 * 参数: label - 分区标签
 * 返回: ESP_OK 成功，ESP_ERR_NOT_FOUND 分区不存在
 */
esp_err_t ota_manager_set_boot_partition(const char *label);
/*
 * 获取下一个可用于 OTA 更新的分区信息（大小和标签）。
 * 参数: out_size    - 输出分区大小
 * 参数: out_label   - 输出分区标签缓冲区
 * 参数: label_size  - 标签缓冲区大小
 * 返回: ESP_OK 成功，ESP_ERR_NOT_FOUND 无可用分区
 */
esp_err_t ota_manager_get_next_update_partition_info(size_t *out_size, char *out_label, size_t label_size);
/*
 * 枚举所有 APP 类型分区，填充分区信息数组（标签、app 名、版本、SHA256、上传时间等）。
 * 参数: out_partitions - 输出数组
 * 参数: max_partitions - 数组容量
 * 返回: 实际分区数量（可能大于 max_partitions）
 */
size_t ota_manager_get_partitions(ota_manager_partition_info_t *out_partitions, size_t max_partitions);
/*
 * 重启设备（调用 esp_restart）。
 */
void ota_manager_reboot(void);
/*
 * 获取 OTA 管理器当前状态快照。
 * 参数: out_status - 输出状态结构体指针
 */
void ota_manager_get_status(ota_manager_status_t *out_status);
/*
 * 将 OTA 状态枚举转换为可读字符串。
 * 参数: state - 状态枚举
 * 返回: 对应的字符串（"idle"/"uploading"/"pending_reboot"/"error"/"unknown"）
 */
const char *ota_manager_state_string(ota_manager_state_t state);
/*
 * 获取上次错误描述（无错误时返回 "none"）。
 * 返回: 错误字符串
 */
const char *ota_manager_last_error(void);
