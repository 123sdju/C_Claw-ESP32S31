#pragma once

#include "session_selection.h"
#include "session_memory_turns.h"
#include "app_event_bus.h"

/**
 * @brief 由同一 interaction owner 串行使用的会话选择与记忆任务菜单。
 *
 * 菜单拥有 selection、历史消息及列表快照，借用 controller、builder、recovery 和可选 turns。
 * 依赖寿命须覆盖菜单，所有调用、视图切换及销毁由外层协调。
 * 流回调期间保持菜单和借用依赖有效，回调须避免重入修改菜单、selection 或 turns。
 */
typedef struct session_menu session_menu_t;

/**
 * @brief 创建菜单和会话选择状态，等待用户选择新会话或历史会话。
 * @param[in] controller 非空借用控制器，须与 builder 和 recovery 属于同一装配。
 * @param[in] builder 非空借用 builder，存活至菜单销毁。
 * @param[in] recovery 非空借用恢复服务，存活至菜单销毁。
 * @param[out] out_menu 必填，入口置 NULL，成功取得拥有对象，使用 session_menu_destroy 释放。
 * @return 返回分配或 selection 创建结果，成功返回 CC_OK，失败清理本次菜单。
 */
cc_result_t session_menu_create(cc_runtime_controller_t *controller, cc_runtime_builder_t *builder,
                                recovery_service_t *recovery, session_menu_t **out_menu);
/**
 * @brief 创建同时支持对话记忆交接的菜单。
 * @param[in] controller 同一装配的非空借用控制器。
 * @param[in] builder 同一装配的非空借用 builder。
 * @param[in] recovery 同一装配的非空借用恢复服务。
 * @param[in] turns 非空记忆任务对象，须属于上述 builder/recovery，借用至菜单销毁。
 * @param[out] out_menu 必填，入口置 NULL，成功取得拥有菜单。
 * @return 装配不符返回 CC_ERR_INVALID_ARGUMENT，其余返回基础菜单创建结果。
 */
cc_result_t session_menu_create_with_memory(cc_runtime_controller_t *controller,
    cc_runtime_builder_t *builder, recovery_service_t *recovery,
    session_memory_turns_t *turns, session_menu_t **out_menu);
/**
 * @brief 释放菜单拥有的全部列表、历史、记忆任务快照及 selection。
 * @pre owner 已关闭入口并排空菜单调用及回调，借用依赖仍存活。
 * @param[in,out] menu 待销毁菜单，可为 NULL。
 * @post 菜单及其拥有对象已释放，外层持有的 controller、builder、recovery 和 turns 继续由各自 owner 管理。
 */
void session_menu_destroy(session_menu_t *menu);
/**
 * @brief 读取并显示会话选择第一页，恢复数据降级时展示纯对话提示。
 * @param[in,out] menu 待展示菜单，更新列表快照与视图状态。
 * @return NULL 返回 ESP_ERR_INVALID_ARG，读取失败返回 ESP_FAIL，其他返回共享视图替换结果。
 */
esp_err_t session_menu_present(session_menu_t *menu);
/**
 * @brief 查询普通对话是否仍等待选择或已完成选择的页面重绘。
 * @param[in] menu 待查询菜单，可为 NULL。
 * @return pending_ready 尚存或 selection 尚未 CHATTING 时为真，NULL 为假。
 */
bool session_menu_waiting(const session_menu_t *menu);
/**
 * @brief 共享视图被覆盖时清除菜单各页的可见标志。
 * @param[in,out] menu 待失效菜单，可为 NULL。
 * @post 会话、历史、运行及记忆任务页均标为不可见，拥有快照和分页游标继续保留供重绘。
 */
void session_menu_invalidate_view(session_menu_t *menu);
/**
 * @brief 等待选择期间处理新建、历史预览、恢复和中断任务命令。
 * @param[in,out] menu 菜单对象，可为 NULL，选择及视图状态按命令更新。
 * @param[in] text NUL 结尾输入，调用期间借用，可为 NULL。
 * @param[in] source 诊断和状态事件所属入口。
 * @param[in] on_stream 继续中断任务时使用的可选同步流回调。
 * @param[in,out] stream_user_data 回调上下文，调用期间借用，可为 NULL。
 * @return 真表示输入已消费，包括命令失败或等待选择时的普通输入。
 * 空参数或菜单已就绪时为假。pending_ready 重绘成功后消费 /new、/resume、/runs、/continue 和 /abandon，
 * 其余输入返回假，由外层继续聊天路径。
 * 错误、部分完成及记忆待交接状态通过事件报告，已完成选择的显示失败可重绘重试。
 */
bool session_menu_handle(session_menu_t *menu, const char *text, app_event_source_t source,
                         cc_runtime_controller_stream_fn on_stream, void *stream_user_data);
/**
 * @brief 独立处理记忆任务列表、翻页、核对重试及本机缺失任务放弃。
 * @param[in,out] menu 借用 turns 的菜单，可为 NULL，操作前按快照 ID 重新核对任务。
 * @param[in] text 借用命令，可为 NULL，编号须在当前可见记忆页中。
 * @param[in] source 状态和错误事件所属入口。
 * @return 命令匹配即返回真，成功或错误经事件报告，参数缺失、无 turns 或未匹配返回假。
 */
bool session_menu_handle_memory(session_menu_t *menu, const char *text, app_event_source_t source);
/**
 * @brief 可见记忆任务页的数据变化时重新读取并刷新。
 * @param[in,out] menu 待核对菜单，可为 NULL。
 * @post 页面可见且快照变化时尝试重载并回到第一页，数据稳定时保留页码。
 * 查询或显示错误经事件报告，无 turns 或页面不可见时直接返回。
 */
void session_menu_refresh_memory_view(session_menu_t *menu);
/**
 * @brief 菜单就绪时通过 turns 同步提交普通对话。
 * @param[in,out] menu 非空且具有 turns 的菜单，须完成选择及 ready 页面展示。
 * @param[in] text 非空原输入，调用期间借用，须满足 turns 的旧答复别名约束。
 * @param[in] on_stream 可选同步流回调。
 * @param[in,out] stream_user_data 回调上下文，调用期间借用，可为 NULL。
 * @param[out] out_result 必填独立输出，入口清零，字符串借自 turns，借用至其下一次修改调用。
 * @return 输出为空返回 CC_ERR_INVALID_ARGUMENT，菜单未就绪返回 CC_ERR_INVALID_STATE，其余返回 turns 提交结果。
 * 失败时仍检查 completed 和 awaiting_resolution，进入中断状态时另行尝试刷新任务页。
 */
cc_result_t session_menu_submit_turn(session_menu_t *menu, const char *text,
    cc_runtime_controller_stream_fn on_stream, void *stream_user_data,
    session_memory_turn_result_t *out_result);
