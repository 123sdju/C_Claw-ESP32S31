#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "app_event_bus.h"
#include "cc/app/cc_runtime_builder.h"
#include "recovery_service.h"
#include "manual_lua_service.h"
#include "cc/adapters/cc_managed_memory_editor.h"
#include "session_memory_turns.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INTERACTION_SERVICE_STOPPED = 0,
    INTERACTION_SERVICE_STARTING,
    INTERACTION_SERVICE_IDLE,
    INTERACTION_SERVICE_QUEUED,
    INTERACTION_SERVICE_RUNNING,
    INTERACTION_SERVICE_CLEARING,
    INTERACTION_SERVICE_PAUSED_MAINTENANCE,
    INTERACTION_SERVICE_CANCELLING,
    INTERACTION_SERVICE_FAILED,
} interaction_service_lifecycle_t;

typedef struct {
    interaction_service_lifecycle_t state;
    bool started;
    bool busy;
    unsigned generation;
    int pending;
    int queue_depth;
    int max_input_bytes;
    bool thinking_enabled;
    bool stream_supported;
    uint32_t queue_high_water;
    uint32_t late_reply_count;
    uint32_t cancel_latency_ms;
    uint64_t owner_heartbeat_ms;
    char session_id[96];
} interaction_service_status_t;

/**
 * @brief 启动基础交互 owner，并通过 START 命令创建 controller。
 * @param[in] io 调用期间有效的 IO 配置，复制会话基础名，非正队列深度/输入限额取 8/1024。
 * @param[in] builder 已装配的借用 builder，须存活到服务停止且销毁成功。
 * @return ESP_OK 表示 START 已完成，其余为参数、状态、分配、入队或等待错误。
 * @pre 生命周期 owner 串行调用 start/stop/destroy，销毁后禁止借旧资源发起调用。
 * @note 等待超时后已入队 START 仍可能完成，调用者继续保管借用依赖并执行停止。
 *       手动 Lua 依赖恢复服务提供当前盘账本。
 */
esp_err_t interaction_service_start(const app_runtime_io_config_t *io, cc_runtime_builder_t *builder);
/**
 * @brief 装配当前盘恢复服务并启动交互及会话选择菜单。
 * @param[in] io 调用期有效并在入队前复制的 IO 配置。
 * @param[in] builder 恢复服务所属的借用 builder。
 * @param[in] recovery 必填当前盘恢复服务，调用者已完成其装配及挂载核对。
 * @param[in] manual_lua_policy 已定义的手动 Lua 策略，PENDING 在执行时继续要求明确策略。
 * @param[in] memory_editor 可为 NULL 的借用记忆编辑器，用于装配用户记忆入口。
 * @return 成功或身份不匹配、参数、分配、入队及等待错误。
 * @pre owner 串行生命周期调用，全部借用依赖及存储覆盖服务停止、销毁和未决 START。
 */
esp_err_t interaction_service_start_with_recovery(const app_runtime_io_config_t *io,
                                                  cc_runtime_builder_t *builder,
                                                  recovery_service_t *recovery,
                                                  manual_lua_policy_t manual_lua_policy,
                                                  cc_managed_memory_editor_t *memory_editor);
/**
 * @brief 装配恢复、受管理记忆和会话菜单后启动单一交互 owner。
 * @param[in] io 调用期间有效的 IO 配置，动态文本被复制。
 * @param[in] builder 与恢复、记忆服务一致的借用 builder。
 * @param[in] recovery 必填当前盘恢复服务，须已完成当前挂载装配。
 * @param[in] manual_lua_policy 已定义的手动 Lua 策略。
 * @param[in] memory_turns 必填受管理记忆桥接，须匹配 builder/recovery 且提供 editor。
 * @return 成功或依赖身份、参数、分配、入队、等待错误，超时保留已接纳命令。
 * @pre owner 串行生命周期调用，并保管全部借用依赖至停止和销毁成功。
 */
esp_err_t interaction_service_start_with_managed_memory(const app_runtime_io_config_t *io,
    cc_runtime_builder_t *builder, recovery_service_t *recovery,
    manual_lua_policy_t manual_lua_policy, session_memory_turns_t *memory_turns);
/**
 * @brief 关闭新 API 登记，取消当前工作并等待已接纳 API、owner 和退出通知尾部。
 * @param[in] timeout_ms 本次全部等待共用的有限毫秒预算，零取默认三十秒。
 * @return 已停止或完整结束为 ESP_OK，自 owner 调用为 INVALID_STATE，其余为入队、关闭或超时错误。
 * @pre 生命周期 owner 先关闭外部入口并串行调用，借用依赖保持到停止、销毁成功。
 * @note 超时保留动态资源及已有 SHUTDOWN，请求重试会等待原命令。controller 关闭失败
 *       保留 owner 以便再次关闭。之前接纳的命令继续按队列顺序排空。
 */
esp_err_t interaction_service_stop(uint32_t timeout_ms);
/**
 * @brief 在 owner 最终完成且 API 登记排空后释放服务原语。
 * @return ESP_OK 表示资源清理完成，尚有 owner、controller 或在途 API 时返回 INVALID_STATE。
 * @pre 生命周期 owner 串行调用，外部入口已经关闭，销毁后禁止使用旧句柄及借用。
 */
esp_err_t interaction_service_destroy(void);
/**
 * @brief 通过 owner 队列同步热应用最大输入字节数。
 * @param[in] io 调用期间有效的配置，其 session 字符串在提交前复制。
 * @return 成功或参数、关闭、分配、入队及等待错误，超时不撤销已接纳命令。
 * @note 队列容量与会话身份由下一次启动装配，当前 owner 自调用会被拒绝。
 */
esp_err_t interaction_service_apply_config(const app_runtime_io_config_t *io);
/**
 * @brief 带外触发当前 controller 和独立记忆请求的取消源。
 * @param[in] source 取消状态事件的来源。
 * @return 取消已传播或当前空闲为 ESP_OK，入口关闭/未装配为 INVALID_STATE，controller 错误为 ESP_FAIL。
 * @note 状态收敛命令排队失败仍返回已传播的取消结果，本调用登记覆盖全部指针借用。
 */
esp_err_t interaction_service_request_cancel(app_event_source_t source);
/**
 * @brief 同步切换交互维护暂停，并在进入时先取消独立记忆请求。
 * @param[in] paused true 进入暂停，false 由 owner 解除。
 * @param[in] reason 可为 NULL 的调用期借用文本，入队前复制，NULL 按空串。
 * @return 成功或关闭、自等待、分配、入队及等待错误。
 * @note 进入暂停可能先关闭输入门或发出取消，再因后续分配/入队失败返回。
 *       已接纳命令超时后仍可能完成，调用者可据状态重试解除。
 */
esp_err_t interaction_service_set_maintenance_paused(bool paused, const char *reason);
/**
 * @brief 限长并复制文本，零等待提交给交互 owner。
 * @param[in] source 后续显示与状态事件来源。
 * @param[in] text 必填调用期有效的 NUL 文本，正文须非空且在当前最大字节数内。
 * @return ESP_OK 仅表示已接纳，尺寸无效为 INVALID_SIZE，关闭为 INVALID_STATE，分配/队满另报。
 * @note /clear 编码为清屏命令，其余本地命令及普通文本均由 owner 分流，失败不接管原文。
 */
esp_err_t interaction_service_submit_text(app_event_source_t source, const char *text);
/**
 * @brief 零等待提交 LCD/Web 视图清屏请求。
 * @param[in] source 清屏结果事件来源。
 * @return 接纳为 ESP_OK，关闭为 INVALID_STATE，队满为 TIMEOUT。
 * @note owner 执行时仍核对维护门，清屏只更新显示视图。
 */
esp_err_t interaction_service_submit_clear(app_event_source_t source);
/**
 * @brief 等待物理队列为空且服务状态属于空闲集合。
 * @param[in] timeout_ms 有限毫秒预算，零取默认三十秒。
 * @return 观察到空闲为 true，超时或 owner 自等待为 false，关闭入口时返回最终完成快照。
 * @note FAILED、STOPPED、维护暂停在队列为空时也属于空闲，调用者另行判断健康。
 */
bool interaction_service_wait_idle(uint32_t timeout_ms);
/**
 * @brief 复制服务状态并合并 controller 运行身份及当前代的能力。
 * @param[out] out_status 可选调用者输出，先清零，入口关闭时交付零快照。
 * @post 输出独立标量及会话 ID 副本，无动态释放责任，多个阶段的快照用于观测。
 */
void interaction_service_get_status(interaction_service_status_t *out_status);
/**
 * @brief 将生命周期枚举转换成稳定观测文本。
 * @param[in] state 待转换枚举，接受未知数值。
 * @return 进程期只读字符串借用，未知值返回 unknown，调用者无释放责任。
 */
const char *interaction_service_state_string(interaction_service_lifecycle_t state);

#ifdef __cplusplus
}
#endif
