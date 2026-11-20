#pragma once

#include "app_config.h"
#include "managed_memory_service.h"
#include "session_memory_turns.h"

typedef struct startup_managed_memory startup_managed_memory_t;

/**
 * @brief 创建默认设备的自动记忆组合对象并安装摘要缓存限制。
 * @param[in] config 调用期借用的配置，决定容量、模型及输入/输出字节预算。
 * @param[in,out] builder 运行时组合根，借用至输出对象销毁，其 store 会设置召回及摘要策略。
 * @param[in] recovery 当前恢复装配，须属于同一 builder 并覆盖输出对象寿命。
 * @param[out] out_owner 成功取得拥有句柄，失败为 NULL，使用本模块 destroy 入口释放。
 * @return CC_OK 表示对象可用，初始化错误向上传递，返回结果按 cc_result_free 清理。
 * @pre recovery 已附加且 interaction 尚未启动，外层排除挂载变化和并发存储使用。
 * @note 可核实的摘要清理失败允许延后，启动时可查询 deferred 标志并提示。
 *       失败前已经设置的召回或摘要策略可能继续作用于当前 store。
 */
cc_result_t startup_managed_memory_create(
    const app_config_t *config, cc_runtime_builder_t *builder, recovery_service_t *recovery,
    startup_managed_memory_t **out_owner);

/**
 * @brief 取得交互任务使用的记忆任务槽 owner。
 * @param[in] owner 借用的组合对象，可为 NULL。
 * @return 借用的 turns，owner 为空时返回 NULL，借用期至组合对象销毁。
 * @pre 使用返回对象的调用由 interaction owner 串行协调。
 */
session_memory_turns_t *startup_managed_memory_turns(startup_managed_memory_t *owner);

/**
 * @brief 读取创建阶段的摘要清理延后记录。
 * @param[in] owner 借用的组合对象，可为 NULL。
 * @return 创建时曾安全延后清理为一，其余为零。
 * @note 该值保留启动时结果，实时可写性及未知安装核对使用实际存储接口。
 */
int startup_managed_memory_summary_prune_deferred(const startup_managed_memory_t *owner);

/**
 * @brief 关闭当前自动记忆组合对象，按关机规则处理 RAM 待处理内容。
 * @param[in,out] owner 拥有句柄的地址，成功后置空，*owner 为 NULL 时成功返回。
 * @return CC_OK 表示对象已释放，地址无效或取消预留失败时返回错误。
 * @pre 入口已关闭，interaction 及借用者已退出，builder/recovery 仍存活。
 * @post 成功后旧句柄与借用结果结束寿命，失败时保管对象以供清理重试。
 */
cc_result_t startup_managed_memory_destroy(startup_managed_memory_t **owner);
