#pragma once

#include "app_config.h"
#include "cc/adapters/cc_recovery_bootstrap.h"
#include "cc/app/cc_operation_journal.h"
#include "cc/app/cc_storage_binding.h"
#include "cc/app/cc_runtime_controller.h"

/**
 * @brief 当前介质的恢复装配所有者。
 *
 * 服务拥有文件系统句柄、路径副本及可选 bootstrap，持续借用 builder 和启动期间的配置暂存。
 * owner 串行执行准备、修复、附加、用户核对和卸载，并排空依赖使用者。
 * 已附加的 builder 先销毁，随后销毁服务，最后释放仍被借用的配置暂存。
 * 明确历史/账本损坏时保留原文件，以必需但未附加的恢复 bundle 关闭模型及手动工具，
 * 允许新纯文本对话。外部修复后重新加载装配，受限实例继续保持原限制。
 */
typedef struct recovery_service recovery_service_t;
typedef enum recovery_service_state {
    RECOVERY_SERVICE_READY = 1,       /**< 完整恢复装配已附加，仍须检查实时挂载。 */
    RECOVERY_SERVICE_FAILED,          /**< 准备/附加失败或原介质已变化。 */
    RECOVERY_SERVICE_READY_TO_ATTACH, /**< 已完成只读预检，尚待修复、目录准备或附加。 */
    RECOVERY_SERVICE_HISTORY_DAMAGED  /**< 历史或账本明确损坏的纯对话限制阶段，名称保留接口兼容。 */
} recovery_service_state_t;
typedef enum recovery_preflight_failure {
    RECOVERY_PREFLIGHT_NONE = 0, /**< 预检成功。 */
    RECOVERY_PREFLIGHT_OTHER    /**< 参数、挂载、路径、能力或资源等预检错误。 */
} recovery_preflight_failure_t;

typedef struct recovery_run_info {
    char *run_id;                  /**< 列表拥有的运行 ID。 */
    char *session_id;              /**< 列表拥有的原会话 ID。 */
    unsigned step;                /**< 检查点保存的模型步骤序号。 */
    cc_run_checkpoint_phase_t phase; /**< 检查点的执行阶段。 */
    size_t completed_calls;        /**< 已完成的计划工具调用数。 */
    size_t planned_calls;          /**< 已规划工具调用总数。 */
    bool recovered_from_backup;    /**< 当前可用检查点来自备份。 */
    bool recovered_from_temporary; /**< 当前检查点来自尚待确认的临时文件。 */
    char input_preview[96];        /**< 最多 95 字节的 NUL 结尾输入预览，截断点避开 UTF-8 延续字节。 */
} recovery_run_info_t;

/**
 * @brief 在应用配置前校验分段会话后端、正的操作阈值及介质内路径隔离。
 * @param[in] config 调用期间借用的运行配置。
 * @param[in] io 调用期间借用的产品 I/O 配置。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，后端、介质根或 FAT 路径校验错误向上传递。
 */
cc_result_t recovery_service_validate_configuration(const cc_config_t *config,
                                                     const app_runtime_io_config_t *io);
/**
 * @brief 预检当前挂载并构造准备阶段服务。
 * @param[in] config 同步借用的运行配置，后续所需路径复制到服务。
 * @param[in] io 同步借用的 I/O 配置，普通操作条数阈值被复制。
 * @param[out] out_service 必填，先置 NULL，成功接收拥有的 READY_TO_ATTACH 服务。
 * @return 原样返回详细准备结果，失败清理本次资源，成功由调用者销毁服务。
 */
cc_result_t recovery_service_prepare(const cc_config_t *config, const app_runtime_io_config_t *io,
                                     recovery_service_t **out_service);
/**
 * @brief 预检挂载、路径和文件系统能力，输出启动展示所需失败分类。
 * @param[in] config 同步借用的运行配置。
 * @param[in] io 同步借用的 I/O 配置，max_pending_operations 须大于零。
 * @param[out] out_failure 可选，先置 OTHER，准备成功时置 NONE。
 * @param[out] out_service 必填，先置 NULL，成功接收拥有的准备阶段服务。
 * @return 成功为 CC_OK，参数、路径、挂载、能力和分配错误向上传递，创建期间换盘也失败并清理资源。
 */
cc_result_t recovery_service_prepare_detailed(const cc_config_t *config, const app_runtime_io_config_t *io,
    recovery_preflight_failure_t *out_failure, recovery_service_t **out_service);
/**
 * @brief 从有效的只读配置暂存和捕获挂载代数准备服务。
 * @param[in] stage 借用至 release_staged 成功或服务销毁的暂存，外层维持其寿命并排除其他配置写入者。
 * @param[in] mount_generation owner 捕获的非零挂载代数。
 * @param[out] out_failure 可选，先置 OTHER，成功为 NONE。
 * @param[out] out_service 必填，参数有效后置 NULL，成功取得拥有服务，创建前后校验失败均清理。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，暂存不可用为 CC_ERR_INVALID_STATE，其他准备或挂载/暂存校验错误向上传递。
 */
cc_result_t recovery_service_prepare_staged(app_config_stage_t *stage, uint64_t mount_generation,
    recovery_preflight_failure_t *out_failure, recovery_service_t **out_service);
/**
 * @brief 查询配置暂存借用是否仍存在。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 服务仍持 stage 为真，其余为假。
 */
bool recovery_service_has_staged_config(const recovery_service_t *service);
/**
 * @brief 按指针比较配置暂存身份。
 * @param[in] service 借用的服务，允许 NULL。
 * @param[in] stage 待比较的暂存，允许 NULL。
 * @return 两个参数有效且暂存指针相同为真，内容和实时挂载另行校验。
 */
bool recovery_service_uses_staged_config(const recovery_service_t *service, const app_config_stage_t *stage);
/**
 * @brief 核对暂存内容及原挂载，变化后锁定服务失败。
 * @param[in,out] service owner 串行维护的服务，须仍持有暂存。
 * @return 成功为 CC_OK，缺少暂存或已失败为 CC_ERR_INVALID_STATE，配置或挂载变化为 CC_ERR_STORAGE 并锁定 FAILED。
 */
cc_result_t recovery_service_validate_staged(recovery_service_t *service);
/**
 * @brief 配置交接失败时锁定仍借用暂存的服务。
 * @param[in,out] service owner 串行维护的服务，允许 NULL。
 * @post 仍持有 stage 的服务进入 FAILED，后续须重新打开装配。
 */
void recovery_service_invalidate_staged(recovery_service_t *service);
/**
 * @brief 修复所选配置候选，修复前后核对暂存和挂载。
 * @param[in,out] service READY_TO_ATTACH 且尚无 bootstrap 的暂存服务，owner 排除其他写入者。
 * @return 成功为 CC_OK 并记录配置修复完成，阶段错误为 CC_ERR_INVALID_STATE，其余校验或修复错误向上传递，修复失败锁定服务。
 */
cc_result_t recovery_service_repair_staged_config(recovery_service_t *service);
/**
 * @brief 查询配置修复步骤的完成标志。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 服务持有暂存且已修复为真，完整附加仍需其他准备及实时有效性检查。
 */
bool recovery_service_staged_config_ready(const recovery_service_t *service);
/**
 * @brief 配置修复后准备工作区及 lua 目录，并在每次文件操作后复查暂存。
 * @param[in,out] service READY_TO_ATTACH 的暂存服务，配置已修复，owner 排除卸载和其他写入者。
 * @return 成功为 CC_OK，阶段错误为 CC_ERR_INVALID_STATE，路径构造或文件系统错误向上传递，失败可已创建部分目录，挂载/暂存变化锁定失败。
 */
cc_result_t recovery_service_prepare_staged_workspace(recovery_service_t *service);
/**
 * @brief 完成完整或受限装配后复查暂存并解除借用。
 * @param[in,out] service READY 或 HISTORY_DAMAGED 且仍持有暂存的服务。
 * @return 成功为 CC_OK 并清空 stage，阶段非法为 CC_ERR_INVALID_STATE，校验失败保持借用关系，stage 仍归外层所有。
 */
cc_result_t recovery_service_release_staged(recovery_service_t *service);
/**
 * @brief 在原挂载上核对运行时路径并附加恢复装配，明确损坏时进入全部工具关闭的纯对话模式。
 * @pre owner 排空所有输入和手动工具入口，暂存服务已完成配置修复和工作区准备。
 * @param[in,out] service 准备阶段服务，成功后拥有 bootstrap 或保留明确损坏原因。
 * @param[in,out] builder 后续持续借用的 builder，须先于服务销毁。
 * @return 成功或明确损坏降级为 CC_OK，其余错误向上传递，附加后换盘可能已安装 bundle 且状态为 FAILED，调用者须检查 is_current。
 */
cc_result_t recovery_service_attach(recovery_service_t *service, cc_runtime_builder_t *builder);
/**
 * @brief 先要求 builder 恢复，再组合非暂存 prepare/attach。
 * @pre owner 排空输入和全部工具入口，串行执行本次装配。
 * @param[in,out] builder 新 builder，成功后由服务借用，须先于服务销毁。
 * @param[in] config 同步借用的运行配置。
 * @param[in] io 同步借用的产品 I/O 配置。
 * @param[out] out_service 必填，builder/输出有效后先置 NULL，成功取得拥有服务。
 * @return 成功为 CC_OK，准备或附加错误向上传递并清理本次服务，已设置的恢复要求继续保持，成功后还须检查状态及挂载。
 */
cc_result_t recovery_service_open(cc_runtime_builder_t *builder, const cc_config_t *config,
                                  const app_runtime_io_config_t *io, recovery_service_t **out_service);
/**
 * @brief 查询可观察阶段，完整或受限服务换盘时报告 FAILED。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 空对象或就绪阶段挂载失效为 FAILED，其余返回保存阶段。
 */
recovery_service_state_t recovery_service_state(const recovery_service_t *service);
/**
 * @brief 复制当前完整恢复装配的全部操作，受限模式报告损坏诊断。
 * @param[in] service 借用的服务，须在原挂载上可用。
 * @param[out] out_items 必填且无待释放内容，两个输出有效后置 NULL，成功数组用 cc_pending_operation_array_free 清理。
 * @param[out] out_count 必填，两个输出有效后置零，成功取得条数。
 * @return 成功为 CC_OK，输出非法为 CC_ERR_INVALID_ARGUMENT，受限模式或读取后换盘为 CC_ERR_STORAGE，未就绪为 CC_ERR_INVALID_STATE，其他复制错误向上传递。
 */
cc_result_t recovery_service_list_operations(const recovery_service_t *service,
                                             cc_pending_operation_t **out_items, size_t *out_count);
/**
 * @brief 将用户外部核实结论持久化到原操作，随后完成账本核对。
 * @param[in] service 借用的当前完整装配服务，owner 串行执行核对命令。
 * @param[in] operation_id 同步借用的非空操作 ID。
 * @param[in] disposition 须为 EXECUTED 或 NOT_EXECUTED。
 * @param[in] observation 同步借用的观察正文，EXECUTED 时须非空，另一种结论允许 NULL。
 * @return 成功为 CC_OK，参数非法、目标缺失、活动状态或存储错误向上传递，末次换盘错误时结果可能已经提交，重试前核对原盘现场。
 */
cc_result_t recovery_service_resolve_operation(const recovery_service_t *service,
                                               const char *operation_id,
                                               cc_tool_recovery_disposition_t disposition,
                                               const char *observation);
/**
 * @brief 列出中断运行的阶段、工具进度及输入预览，任一坏检查点使整次查询失败。
 * @param[in] service 借用的当前完整装配服务。
 * @param[out] out_runs 必填且无待释放内容，两个输出有效后置 NULL，成功数组用 recovery_run_info_array_free 清理。
 * @param[out] out_count 必填，两个输出有效后置零，成功取得条数。
 * @return 成功为 CC_OK，受限模式及读取后换盘为 CC_ERR_STORAGE，其他状态、分配和检查点错误向上传递，失败清理部分列表。
 */
cc_result_t recovery_service_list_runs(const recovery_service_t *service,
                                      recovery_run_info_t **out_runs, size_t *out_count);
/**
 * @brief 释放运行列表的 ID、会话 ID 和数组。
 * @param[in,out] runs 拥有的列表，允许 NULL，前 count 条可为有效或零初始化记录。
 * @param[in] count 需清理的槽数，须在数组范围内。
 * @post 列表资源已释放，调用者持有的指针失效。
 */
void recovery_run_info_array_free(recovery_run_info_t *runs, size_t count);
/**
 * @brief 放弃选定运行检查点，工具结果的人工核对另行处理。
 * @param[in] service 借用的当前服务，owner 串行协调运行生命周期。
 * @param[in] run_id 同步借用的非空运行 ID。
 * @return 成功为 CC_OK，参数、状态、租约和检查点错误向上传递，末次换盘错误时检查点可能已删除，输入保留由 selection/turns 组合入口负责。
 */
cc_result_t recovery_service_abandon_run(const recovery_service_t *service, const char *run_id);
/**
 * @brief 用户确认临时输入后安装选定临时检查点。
 * @param[in] service 借用的当前完整装配服务，owner 串行执行。
 * @param[in] run_id 同步借用的非空运行 ID。
 * @return 成功为 CC_OK，状态或输入不可用为 CC_ERR_INVALID_STATE，检查点及换盘错误向上传递，末次错误时确认可能已完成。
 */
cc_result_t recovery_service_confirm_temporary_run(const recovery_service_t *service, const char *run_id);
/**
 * @brief 查询 READY 或受限装配是否仍对应创建时挂载。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 阶段可用且原挂载代数一致为真，能力限制由调用者另行检查。
 */
bool recovery_service_is_current(const recovery_service_t *service);
/**
 * @brief 查询当前服务是否为历史或账本明确损坏后的受限模式。
 * @param[in] service 借用的服务，允许 NULL。
 * @return HISTORY_DAMAGED 且原挂载仍当前为真，陈旧或完整装配为假。
 */
bool recovery_service_history_damaged(const recovery_service_t *service);
/**
 * @brief 返回适合观测的静态模式名。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 借用静态字符串 ready、history_damaged、journal_damaged 或 unavailable，ready 仍可能有同名操作或策略限制。
 */
const char *recovery_service_mode_name(const recovery_service_t *service);
/**
 * @brief 核对当前服务与指定 builder 的装配身份。
 * @param[in] service 借用的服务，允许 NULL。
 * @param[in] builder 待比较的 builder，允许 NULL。
 * @return 当前服务可用且指针一致为真，其余为假。
 */
bool recovery_service_uses_builder(const recovery_service_t *service, const cc_runtime_builder_t *builder);
/**
 * @brief 核对 controller 与当前服务是否依赖同一 builder。
 * @param[in] service 借用的服务，允许 NULL。
 * @param[in] controller 借用的 controller，允许 NULL。
 * @return 服务当前可用且依赖匹配为真，其余为假。
 */
bool recovery_service_uses_controller(const recovery_service_t *service, const cc_runtime_controller_t *controller);
/**
 * @brief 借出当前 builder 管理的记忆存储。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 借用指针，有效期受 builder 及原挂载限制，服务不可用或存储缺失返回 NULL。
 */
cc_memory_store_t *recovery_service_borrow_memory_store(const recovery_service_t *service);
/**
 * @brief 释放 bootstrap、文件系统句柄、路径副本和服务。
 * @pre owner 关闭入口并排空调用，已附加的 builder 已销毁，仍借用的 stage 有效。
 * @param[in,out] service 待销毁服务，允许 NULL，stage 自身由外层释放。
 * @post 服务及其子对象寿命结束，磁盘文件保留供修复及重新加载。
 */
void recovery_service_destroy(recovery_service_t *service);
