#pragma once

#include "recovery_service.h"
#include "startup_gate.h"

/**
 * @brief 读取启动配置前检查设备 NVS，失败呈现 NVS 修复提示。
 * @return gate 已占用返回 CC_ERR_INVALID_STATE，其余返回 NVS 检查结果。
 * @pre 由生命周期 owner 在 OFFLINE 阶段串行调用，使用内部 RAM 栈，串口与事件依赖可用。
 * @note 底层检查保管现有 NVS 内容，成功后 gate 继续处于 OFFLINE，后续由 owner 装配服务。
 */
cc_result_t startup_recovery_check_nvs(void);

/**
 * @brief 将一个恢复服务呈现到 Web/串口共享的启动 gate。
 * @param[in] service 拥有者持续保管的恢复服务，成功后本模块借用至 reset。
 * @return 成功登记并发布阶段为 CC_OK，空参数为 CC_ERR_INVALID_ARGUMENT，gate 已占用为 CC_ERR_INVALID_STATE。
 * @pre 生命周期 owner 串行操作本模块，销毁 service 或呈现下一挂载前先 reset。
 * @note 网络、运行时和交互的实际启动由外层完成，mark_running 最后发布开放阶段。
 */
cc_result_t startup_recovery_present(recovery_service_t *service);
/**
 * @brief 从同一挂载代数的只读配置暂存创建并呈现恢复服务。
 * @param[in] stage 调用者拥有的配置暂存，成功后保持存活至 release_stage 或 service 销毁。
 * @param[in] mount_generation 打开 stage 前捕获并在打开后核实的非零挂载代数。
 * @param[out] out_failure 可选输出，先置 OTHER，准备过程可改为具体失败类别。
 * @param[out] out_service stage 与输出地址有效后先置空，成功取得拥有的 service，调用者负责销毁。
 * @return CC_OK 表示准备及呈现完成，参数、gate 状态或准备失败返回对应错误。
 * @pre 外层排除卸载、重挂载和配置写入，NVS 操作使用内部 RAM 栈。
 * @note reset 只解除呈现关系，service 对 stage 的借用继续按 release_stage 或销毁结束。
 */
cc_result_t startup_recovery_prepare_staged(app_config_stage_t *stage, uint64_t mount_generation,
    recovery_preflight_failure_t *out_failure, recovery_service_t **out_service);
/**
 * @brief 校验当前呈现服务的配置暂存与原挂载代数。
 * @param[in,out] service 仍借用 stage 的当前服务，失败可转为 FAILED。
 * @return 身份或暂存关系无效为 CC_ERR_INVALID_STATE，校验结果由恢复服务返回。
 * @note 创建存储使用者前和耗时启动步骤后调用，stage 的解析配置可以已转移，原始快照仍须存活。
 */
cc_result_t startup_recovery_validate_staged(recovery_service_t *service);
/**
 * @brief 核对当前挂载与候选快照后，把选中配置修复为主配置。
 * @param[in,out] service 已呈现的 READY_TO_ATTACH 服务，gate 必须为 LOADING。
 * @return 阶段无效为 CC_ERR_INVALID_STATE，其余返回修复结果，失败可关闭 gate。
 * @pre 配置转移和工作区创建尚未开始，外层排除介质变化及配置写入。
 */
cc_result_t startup_recovery_repair_config(recovery_service_t *service);
/**
 * @brief 配置修复完成后转移解析结果，在转移前后核对暂存和挂载。
 * @param[in,out] service 当前呈现的 READY_TO_ATTACH 服务，配置修复标志须已完成。
 * @param[in,out] stage service 借用的同一暂存，成功 take 会消费其中的解析配置。
 * @param[in,out] out_config 独立且已初始化的目标，成功释放旧内容并接管新配置，失败保管旧内容。
 * @return 参数或目标别名无效为 CC_ERR_INVALID_ARGUMENT，阶段未就绪为 CC_ERR_INVALID_STATE，
 *         转移或校验失败返回对应错误，成功为 CC_OK。
 * @pre 已停止 stage 配置地址的借用者，启动 Web 使用独立稳定的 I/O 快照或已经停止。
 * @note 转移后复核失败会清理本次解析结果，此时 stage 的解析配置可能已被消费。
 */
cc_result_t startup_recovery_take_staged_config(recovery_service_t *service,
    app_config_stage_t *stage, app_config_t *out_config);
/**
 * @brief 在 builder 创建前按已核实暂存准备工作区。
 * @param[in,out] service 当前呈现的 READY_TO_ATTACH 服务，gate 必须为 LOADING。
 * @return 阶段无效为 CC_ERR_INVALID_STATE，其余返回工作区准备结果，失败可关闭 gate。
 */
cc_result_t startup_recovery_prepare_workspace(recovery_service_t *service);
/**
 * @brief 在恢复附加与交互就绪后，校验并解除 service 的 stage 借用。
 * @param[in,out] service 当前呈现且仍借用 stage 的 READY 或 HISTORY_DAMAGED 服务。
 * @return 成功为 CC_OK，前置状态或校验失败返回相应错误。
 * @post 成功后调用者可销毁 stage，运行期存储使用者继续核对挂载有效性。
 * @pre gate 为 LOADING，普通文本入口仍受启动 gate 控制。
 */
cc_result_t startup_recovery_release_stage(recovery_service_t *service);
/**
 * @brief 预检未创建 service 时呈现一般失败。
 * @param[in] failure 只接受 OTHER，NVS 检查有专门的修复呈现路径。
 * @return gate 空闲且类别匹配为 CC_OK，其余为 CC_ERR_INVALID_STATE。
 * @post 成功后 gate 为 FAILED，发布启动错误提示。
 */
cc_result_t startup_recovery_present_failure(recovery_preflight_failure_t failure);
/**
 * @brief 关闭启动 gate 并解除本模块持有的 service 借用。
 * @post 阶段为 OFFLINE，内部借用指针为空，service 和 stage 的所有权仍在外层。
 * @pre 生命周期 owner 串行执行，各服务在途请求和销毁按自身屏障处理。
 */
void startup_recovery_reset(void);
/**
 * @brief 把恢复资源附加到当前 builder 并发布对应启动阶段。
 * @param[in,out] service 当前呈现的 READY_TO_ATTACH 服务，配置和工作区准备已完成。
 * @param[in,out] builder 尚未开放输入的运行时组合根，附加后借用 service 的恢复资源。
 * @return 参数无效为 CC_ERR_INVALID_ARGUMENT，阶段或身份无效为 CC_ERR_INVALID_STATE，其余返回附加结果。
 * @note 已附加的 builder 先结束借用并销毁，随后外层释放 service。
 */
cc_result_t startup_recovery_attach(recovery_service_t *service, cc_runtime_builder_t *builder);
/**
 * @brief 最后核对当前恢复装配并开放启动 gate。
 * @param[in] service 已呈现且成功解除 stage 借用的恢复服务。
 * @return 成功为 CC_OK，参数无效为 CC_ERR_INVALID_ARGUMENT，身份或 stage 关系无效为 CC_ERR_INVALID_STATE，
 *         阶段或挂载核对失败为 CC_ERR_STORAGE 并发布 FAILED。
 * @pre 交互已经就绪，所有启动存储使用者受控，暂存已成功释放。
 */
cc_result_t startup_recovery_mark_running(recovery_service_t *service);
