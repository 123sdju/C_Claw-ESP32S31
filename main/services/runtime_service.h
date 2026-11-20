#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cc/app/cc_runtime_builder.h"
#include "cc/util/cc_config.h"

/**
 * @brief 记录 SDK 版本，并创建本地文本消息验证摘要和清理链路。
 * @param[in] config 调用期间借用的可选配置，仅用于演示正文的 provider/model 名称。
 * @post 临时消息、摘要与错误结果均释放，成功或失败情况写入日志。
 */
void runtime_service_run_self_test(const cc_config_t *config);
/**
 * @brief 以普通模式创建 builder，已有实例则复用并应用 thinking。
 * @param[in] config 必填调用期配置，新实例据此装配，已有实例继续使用原装配配置。
 * @return 成功返回 true，参数或创建失败返回 false 并记录原因。
 * @pre 生命周期 owner 串行协调创建和停止，保证全局引用交接。
 */
bool runtime_service_start(const cc_config_t *config);
/**
 * @brief 创建工具派发关闭的新 builder，准备附加恢复组件。
 * @param[in] config 必填调用期配置。
 * @return 准备成功为 true，已有 builder 或准备失败为 false，清理失败保留实例供 stop 重试。
 * @pre owner 独占启动装配，附加 recovery bundle 后再激活观测和输入。
 */
bool runtime_service_prepare_recovery(const cc_config_t *config);
/**
 * @brief 将已经完成恢复装配的 builder 交给 profile 建立运行事件订阅。
 * @pre 生命周期 owner 保证装配完成并排除并发停止。
 * @post 存在 builder 时调用 profile 订阅入口，无实例时直接返回。
 */
void runtime_service_activate_observability(void);
/**
 * @brief 停止 profile、排空订阅，再按剩余预算销毁 builder。
 * @param[in] timeout_ms 毫秒预算，后续阶段使用扣除已用单调时间后的剩余值。
 * @return 成功清空全局引用，失败返回拥有结果并保留未完成对象供重试。
 * @pre owner 关闭外部入口并协调其他调用退出，串行执行 stop。
 */
cc_result_t runtime_service_stop(uint32_t timeout_ms);
/**
 * @brief 借出应用当前持有的 builder 组合根。
 * @return 返回借用对象，未创建或已停止为 NULL，由本服务 stop 销毁。
 * @pre 调用者通过生命周期协调保持 builder 覆盖借用期，代内资源另取 generation 租约。
 */
cc_runtime_builder_t *runtime_service_builder(void);

/**
 * @brief 将连接池重置请求交给当前 builder 的 HTTP 端口。
 * @return 未启动为 CC_ERR_INVALID_STATE，其他返回端口结果，由调用者释放诊断。
 * @pre 生命周期保持 builder 有效，网络 owner 协调在途 HTTP 操作。
 */
cc_result_t runtime_service_reset_http_connections(void);
/**
 * @brief 在当前 generation 租约内更新 thinking 标志。
 * @param[in] thinking_mode true 开启，false 关闭。
 * @pre 生命周期保证全局 builder 引用及调用期对象有效。
 * @post 可获取当前代时更新标志并归还租约，无实例或准入拒绝时结束。
 */
void runtime_service_set_thinking(bool thinking_mode);
