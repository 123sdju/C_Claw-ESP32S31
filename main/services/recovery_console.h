#pragma once

#include "app_event_bus.h"
#include "recovery_service.h"

typedef struct recovery_console recovery_console_t;

/** @brief 创建待核对操作控制台。
 * @param[in] recovery 必填当前盘恢复服务借用，寿命覆盖对象。
 * @param[out] out_console 必填新槽，成功交付拥有对象，参数错误保留原槽，OOM 写 NULL。
 * @return CC_OK 或参数、挂载及分配错误，诊断由调用者释放。
 * @pre 对象所有 API 由同一 owner 串行调用。
 */
cc_result_t recovery_console_create(recovery_service_t *recovery, recovery_console_t **out_console);
/** @brief 释放控制台、操作快照和详情。
 * @param[in,out] console 可为 NULL 的拥有对象，调用者须先关入口并等待全部调用退出。
 * @post 借用 recovery 继续由外层保管，调用者变量自行失效。
 */
void recovery_console_destroy(recovery_console_t *console);
/** @brief 判断命令前缀。
 * @param[in] text 可为 NULL 的调用期 NUL 文本。
 * @return /pending、/resolve 及其空格参数形式为 true。
 */
bool recovery_console_handles(const char *text);
/** @brief 其他页面替换或清屏时丢弃缓存视图。
 * @param[in,out] console 可为 NULL 的串行对象，释放快照并使旧选择失效。
 */
void recovery_console_invalidate_view(recovery_console_t *console);
/** @brief 查看当前盘账本或记录人工核对结果。
 * @param[in,out] console 可为 NULL 的 owner 对象。
 * @param[in] text 可为 NULL 的调用期文本，非所属命令跳过。
 * @param[in] source 显示及错误事件来源。
 * @post 详情先脱敏再分页，失败发布事件，核对命令本身不执行工具。
 * @pre recovery 覆盖调用期，禁止并发销毁或操作对象。
 */
void recovery_console_handle(recovery_console_t *console, const char *text, app_event_source_t source);
