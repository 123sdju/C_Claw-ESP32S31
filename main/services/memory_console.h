#pragma once

#include "app_event_bus.h"
#include "cc/adapters/cc_managed_memory_editor.h"
#include "recovery_service.h"
#include "session_memory_turns.h"

/**
 * @brief 由交互 owner 串行使用的记忆管理控制台。
 *
 * 控制台没有内部锁。借用的 editor、recovery 以及可选 turns 须覆盖控制台寿命，
 * 所有调用及共享视图的切换由外层协调。
 */
typedef struct memory_console memory_console_t;

/**
 * @brief owner 核对 editor 使用当前 recovery 的 store 后创建控制台，依赖仅借用。
 *
 * 成功输出拥有对象，失败输出为空。
 * 后续操作由同一个交互 owner 串行调用。
 *
 * @param[in] editor 非空编辑器，借用至 console 销毁。
 * @param[in] recovery 非空恢复服务，须与 editor 使用同一 store，借用至 console 销毁。
 * @param[out] out_console 必填，先置 NULL，成功取得拥有的控制台，使用 memory_console_destroy 释放。
 * @return CC_OK 表示创建成功，参数或依赖不符返回 CC_ERR_INVALID_ARGUMENT，分配失败返回 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t memory_console_create(cc_managed_memory_editor_t *editor, recovery_service_t *recovery,
                                  memory_console_t **out_console);
/**
 * @brief 验证 turns 借出的 editor 与参数相同，再创建支持队列排空检查的重置入口。
 *
 * @param[in] editor 非空编辑器，借用至 console 销毁。
 * @param[in] recovery 与 editor 使用同一 store 的恢复服务，借用至 console 销毁。
 * @param[in] turns 非空任务槽对象，须提供同一 editor，借用至 console 销毁。
 * @param[out] out_console 必填，成功取得拥有对象，使用 memory_console_destroy 释放。
 * turns 缺失或 editor 不匹配时保持输出原值，通过依赖检查后由 create 清零输出。
 * @return 返回依赖检查或创建结果，CC_OK 表示创建成功。
 */
cc_result_t memory_console_create_with_turns(cc_managed_memory_editor_t *editor,
    recovery_service_t *recovery, session_memory_turns_t *turns, memory_console_t **out_console);
/**
 * @brief owner 关闭入口并排空调用后释放计划、详情、重试快照 ID 与对象本身。
 *
 * @pre 交互入口已关闭，owner 的控制台调用已退出。
 *
 * @param[in,out] console 待销毁对象，可为 NULL。
 * @post 控制台拥有的页面、详情及重置快照 ID 已释放，控制台寿命结束。
 */
void memory_console_destroy(memory_console_t *console);
/**
 * @brief 识别本控制台命令前缀，具体子命令与参数由 handle 校验。
 *
 * @param[in] text 待识别的 NUL 结尾文本，可为 NULL，调用期间借用。
 * @return 文本属于受支持的命令前缀时为真，详细参数继续交给 handle 校验。
 */
bool memory_console_handles(const char *text);
/**
 * @brief 同步视图被覆盖或版本失效时释放计划/详情，清除可见性与列表 revision。
 *
 * 重置重试用的快照 ID 单独保留，由 reset_revision 核对其适用范围。
 *
 * @param[in,out] console 待失效对象，可为 NULL。
 * @post 计划与详情已释放，页面标志失效，重置快照关联继续由 revision 管理。
 */
void memory_console_invalidate_view(memory_console_t *console);
/**
 * @brief 当前挂载有效时解释列表、详情、编辑、删除及重置命令。
 *
 * text 调用期借用，状态和错误通过应用事件发布，计划及详情归 console。
 * 所有命令在 interaction owner 串行执行，updated_at 由应用提供。
 *
 * 命令作用于受管理条目，编辑和删除要求当前可见索引。
 * @param[in,out] console 控制台对象，可为 NULL，所有处理由交互 owner 串行调用。
 * @param[in] text NUL 结尾命令，调用期间借用，可为 NULL，前缀不匹配时直接返回。
 * @param[in] source 命令或诊断所属入口，用于事件归因。
 * @param[in] updated_at 应用提供的编辑时间，编辑时须非负，并取其与原记录时间的较大值。
 * 删除、查询和重置忽略此参数。
 * @post 有效命令通过共享视图和事件发布结果，并更新页面与版本状态。
 * console 为空或命令前缀不匹配时直接返回，介质失效时清理视图并发布错误。
 */
void memory_console_handle(memory_console_t *console, const char *text,
                           app_event_source_t source, int64_t updated_at);
