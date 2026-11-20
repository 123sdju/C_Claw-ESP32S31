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

/** @brief 串行确认运行镜像有效并刷新回滚状态。
 * @return 原生确认结果或锁分配失败的 ESP_ERR_NO_MEM。
 * @pre lifecycle owner 已完成健康判断并建立 otadata 写入所需的离线窗口。
 */
esp_err_t ota_manager_mark_app_valid(void);
/** @brief 锁内刷新 running、boot、target 的缓存元数据。
 * @post 读取失败字段为空或 unknown，锁分配失败时保持原缓存，无输出所有权转移。
 */
void ota_manager_refresh_status(void);
/** @brief 复制上传元数据并建立尚未打开原生 writer 的事务。
 * @param[in] expected_size 非零 BIN 字节数，须不超过目标 APP 分区容量。
 * @param[in] upload_name 可空同步 NUL 名称借用，净化后截断复制，仅用于显示。
 * @param[in] browser_time 可空同步 NUL 时间借用，过滤复制，finish 后写 NVS。
 * @return 成功、NO_MEM、INVALID_STATE、NOT_FOUND 或 INVALID_SIZE。
 * @note 每次调用串行，外层事务 owner 须排除块间重排和其他事务。
 * 尚存 writer 时包括 ERROR 状态均拒绝覆盖，先 cancel 释放句柄。
 */
esp_err_t ota_manager_begin(size_t expected_size, const char *upload_name, const char *browser_time);
/** @brief 首个非空块校验魔数并创建 writer，再同步写入目标分区。
 * @param[in] data 必填 len 字节稳定借用，零长度也要求非 NULL。
 * @param[in] len 本块字节数，零长度无原生副作用，须不超过声明剩余量。
 * @return 原生写入结果、NO_MEM、状态/容量错误或无效魔数。
 * @pre 事务 owner 保证块顺序及离线窗口，失败后仍可能拥有 writer 和 flash 前缀。
 */
esp_err_t ota_manager_write(const void *data, size_t len);
/** @brief 检查总量、消费 writer 并提交下次启动分区。
 * @return 原生结果或 NO_MEM，成功转 PENDING_REBOOT，时间 NVS 写失败仅记录日志。
 * @post esp_ota_end 返回即消费句柄，boot 提交失败仍保留已写镜像。
 */
esp_err_t ota_manager_finish(void);
/** @brief 串行 abort 尚存 writer 并清空 RAM 事务后刷新分区信息。
 * @post 保留 flash 镜像及已提交 boot 选择，锁分配失败时保持状态。
 */
void ota_manager_cancel(void);
/** @brief 串行选择指定 APP 分区作为 boot 并刷新缓存。
 * @param[in] label 同步 NUL 标签借用，NULL/空或未知返回 NOT_FOUND。
 * @return 成功、NO_MEM、活动事务的 INVALID_STATE 或原生错误。
 * @pre owner 已建立 otadata 离线写入窗口，成功选择无法通过 cancel 撤销。
 */
esp_err_t ota_manager_set_boot_partition(const char *label);
/** @brief 采样下一个更新分区的容量和标签。
 * @param[out] out_size 可空独占字节数槽，无目标写零。
 * @param[out] out_label 可空独占标签槽，非零容量时截断并终止，无目标为空串。
 * @param[in] label_size 标签字节容量，含 NUL，零时跳过。
 * @return 成功或 NOT_FOUND，全部输出按值，不建立事务租约。
 */
esp_err_t ota_manager_get_next_update_partition_info(size_t *out_size, char *out_label, size_t label_size);
/** @brief 枚举全部 APP 分区并按容量复制独立元数据。
 * @param[out] out_partitions 可空独占数组，NULL 只统计总数。
 * @param[in] max_partitions 可写元素容量，零时不填写。
 * @return 实际分区总数，可大于容量，有效填写数为二者较小值。
 * @note 原生迭代器本层释放，各分区独立采样，摘要为 ELF app_desc 字段。
 */
size_t ota_manager_get_partitions(ota_manager_partition_info_t *out_partitions, size_t max_partitions);
/** @brief 调用 esp_restart 复位设备，正常不返回。
 * @pre lifecycle owner 已执行所有必要关闭和排空屏障。
 */
void ota_manager_reboot(void);
/** @brief 锁内复制一致 RAM 快照，字符串按值保存。
 * @param[out] out_status 可空独占槽，非空先清零，锁创建失败交付 ERROR 诊断。
 * @note 分区元数据来自最近 refresh 或状态变更，本函数无堆输出。
 */
void ota_manager_get_status(ota_manager_status_t *out_status);
/** @brief 将管理器状态映射为诊断文本。
 * @param[in] state 任意枚举值。
 * @return 进程期常量借用，未知值为 unknown，可并发调用，无释放责任。
 */
const char *ota_manager_state_string(ota_manager_state_t state);
/** @brief 借回最近错误文本，无错误时为常量 none。
 * @return 内部借用或锁失败的常量诊断，调用者不得释放。
 * @pre 外层 owner 在保留文本期间排除 begin、cancel 和其他错误更新。
 * @note 需要可并发保留的错误文本时使用 get_status 的按值 error 字段。
 */
const char *ota_manager_last_error(void);
