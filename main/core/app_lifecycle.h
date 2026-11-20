#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief 从 app_main 装配启动依赖、恢复门禁和交互，成功后常驻处理 USB 生命周期。
 * @pre 由唯一启动 owner 调用一次，配置事务、USB guard 和各全局服务均处于启动前状态。
 * @post 成功启动后保持常驻，换盘走受控重启；早期初始化、NVS 前检或关键服务失败可返回。
 * @note 启动分支分别保留诊断或执行分阶段回滚，回滚首个屏障失败时后续依赖继续存活。
 * 交互 RUNNING 先于 Web/Camera/OTA 的最终收尾，boot_complete 发布后重启准备入口才可接纳。
 */
void app_lifecycle_run(void);

/**
 * @brief 关闭输入和 USB 操作接纳，按十三个依赖步骤排空重启所需服务。
 * @param[in] timeout_ms 各步骤原样取得的毫秒预算，零采用 5000，整体时长还包含多步及端口清理。
 * @return 启动尚未移交为 ESP_ERR_INVALID_STATE，shutdown 锁不可用为 ESP_ERR_TIMEOUT，其余返回首个失败步骤或 ESP_OK。
 * @pre 当前任务位于待停止服务的回调之外，且未持有 USB owner guard；多个请求由 shutdown mutex 串行。
 * @note 首个失败即停止推进，已关闭入口和剩余依赖保留供重试，是否实际重启由调用者决定。
 */
esp_err_t app_lifecycle_prepare_for_reboot(uint32_t timeout_ms);
