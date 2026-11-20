#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "cc/app/cc_runtime_builder.h"
#include "cc/app/cc_cache_metrics.h"
#include "cc/util/cc_string_builder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** @brief 在任务注册前创建 mutex、事件组和缓存指标，成功复位 stopping。
 * @return ESP_OK 或 NO_MEM，部分资源保留供 destroy。
 * @pre 生命周期 owner 独占初始化/销毁，复用时 sampler 已停止。
 */
esp_err_t app_profile_init(void);
/** @brief 禁用采样并等待 sampler 最后 mutex Give 尾部完成，保留 SDK 订阅。
 * @param[in] timeout_ms 事件与尾部等待共用毫秒数，零立即检查，mutex 获取沿端口阻塞。
 * @return 无任务或退出交接完成为 ESP_OK，未完成为 TIMEOUT，可重试。
 * @pre 已关闭并排空配置/命令创建入口，当前在 sampler 之外，排除并发 init/destroy。
 * @note 正预算向上取整为有限 tick，必要时按原生上限分段等待，不使用无限等待哨兵。
 */
esp_err_t app_profile_stop(uint32_t timeout_ms);
/** @brief 停止 sampler 后释放事件组、锁和缓存指标，清空 owner。
 * @param[in] timeout_ms 转交 stop 的毫秒预算。
 * @return stop 错误、仍有 runtime 订阅时 INVALID_STATE，完整清理 ESP_OK。
 * @pre 先成功退订并排空注册、配置、采样和状态查询等所有使用者。
 */
esp_err_t app_profile_destroy(uint32_t timeout_ms);

/** @brief BASIC 保留关键事件，TRACE 还处理节流的流式事件。 */
typedef enum {
    APP_PROFILE_LEVEL_BASIC = 0,
    APP_PROFILE_LEVEL_TRACE,
} app_profile_level_t;

/** @brief 按值跨度，字符串借用到每次 span_end 返回，无动态拥有资源。 */
typedef struct {
    bool enabled; /**< 起点 profile 开关快照。 */
    uint64_t start_us; /**< 单调起始微秒。 */
    const char *event; /**< 可选事件名借用。 */
    const char *phase; /**< 可选阶段名借用。 */
    const char *session_id; /**< 可选会话名借用。 */
    const char *run_id; /**< 可选运行名借用。 */
} app_profile_span_t;

/** @brief 锁内复制配置，启用边沿重置基线，再按需创建 sampler。
 * @param[in] config 同步借用配置，NULL 使用禁用/BASIC/零周期，不保留指针。
 * @return 未初始化 INVALID_STATE，配置写入 ESP_OK，任务创建失败仅计 dropped。
 * @note stopping 阻止新建 sampler，重新启动由 owner init 交接。
 */
esp_err_t app_profile_apply_config(const app_profile_config_t *config);
/** @brief 将后续缓存指标关联到 provider。
 * @param[in] provider 必填同步借用名，指标对象复制所需内容。
 * @return NULL 为 INVALID_ARG，成功 ESP_OK，指标不可用或更新失败 ESP_FAIL。
 */
esp_err_t app_profile_set_cache_provider(const char *provider);
/** @brief 锁内更新开关，启用边沿重置基线并尝试启动 sampler。
 * @param[in] enabled true 启用，false 停用采样，已有任务继续等待配置。
 * @post 配置已更新，创建失败仅计 dropped，stopping 继续阻止新任务。
 * @pre 已初始化且 owner 排除并发销毁。
 */
void app_profile_set_enabled(bool enabled);
/** @brief 锁内读取 profile 开关。
 * @return 当前 enabled 值，owner 保持模块有效。
 */
bool app_profile_enabled(void);
/** @brief 锁内设置采样级别。
 * @param[in] level TRACE 保留，其他值归 BASIC。
 * @post 后续事件采用新级别，现有流式计数保持。
 */
void app_profile_set_level(app_profile_level_t level);
/** @brief 锁内读取采样级别。
 * @return 当前 BASIC 或 TRACE。
 */
app_profile_level_t app_profile_level(void);
/** @brief 更新周期并按需创建任务，当前等待不被立即打断。
 * @param[in] interval_ms 毫秒间隔，负值归零，零停用周期采样。
 * @post sampler 下次唤醒使用新配置，stopping 阻止创建。
 */
void app_profile_set_interval_ms(int interval_ms);
/** @brief 将采样级别映射为稳定名称。
 * @param[in] level TRACE 以外均按 BASIC。
 * @return 静态只读 trace/basic 借用，无需释放。
 */
const char *app_profile_level_string(app_profile_level_t level);

/** @brief 在八个水位槽中注册任务，同句柄或与已存名字精确匹配时替换，满表跳过。
 * @param[in] name 必填非空名，复制最多 23 字节并保证 NUL 结尾。
 * @param[in] handle 必填存活句柄借用，删除任务前须注销。
 * @post 合法输入尽力安装，不转移任务拥有权，无效参数跳过。
 */
void app_profile_register_task(const char *name, TaskHandle_t handle);
/** @brief 等待锁内栈水位读取结束并移除所有匹配槽。
 * @param[in] handle 尚未删除的任务借用，NULL 跳过。
 * @post 匹配槽清空，重复调用安全，外层随后可删除任务。
 */
void app_profile_unregister_task(TaskHandle_t handle);
/** @brief 取得开关快照与起始微秒，按值记录跨度。
 * @param[in] event 可选事件名借用，持续到 span_end 返回。
 * @param[in] phase 可选阶段名借用，持续到 span_end 返回。
 * @param[in] session_id 可选会话标识借用，持续到 span_end 返回。
 * @param[in] run_id 可选运行标识借用，持续到 span_end 返回。
 * @return 无拥有资源的跨度，字符串保留原借用。
 */
app_profile_span_t app_profile_span_begin(const char *event,
                                          const char *phase,
                                          const char *session_id,
                                          const char *run_id);
/** @brief 计算跨度耗时并按当前准入尽力发布，复用最近 CPU 窗口。
 * @param[in] span 可选跨度借用，NULL 或起点未启用跳过，不修改跨度。
 * @param[in] status 可选同步借用状态，NULL 省略字段。
 * @post 临时正文清理，重复调用可重复发布，当前 enabled/维护状态仍决定准入。
 */
void app_profile_span_end(app_profile_span_t *span, const char *status);
/** @brief 采集无耗时和会话身份的点，按普通或强制准入发布。
 * @param[in] event 同步借用名，NULL 使用 sample。
 * @param[in] phase 同步借用阶段，NULL 使用 point。
 * @param[in] force true 绕过 enabled 与维护检查。
 * @post 构造成功时更新状态缓存并尽力发布，临时正文释放。
 * @pre periodic/interval 特殊身份只由唯一 sampler 调用以推进 CPU 基线。
 */
void app_profile_sample(const char *event, const char *phase, bool force);
/** @brief 观察缓存指标，再按 BASIC 白名单或 TRACE 节流转为 profile 快照。
 * @param[in] event_type 可选同步借用名，TRACE 缺名使用 unknown。
 * @param[in] event_json 可选同步借用 JSON，NULL 按空对象，解析失败仍可发布基础诊断。
 * @post 解析树与复制正文释放，缓存指标独立于 enabled，事件发布在 profile 锁外执行。
 */
void app_profile_sdk_event(const char *event_type, const char *event_json);
/** @brief 建立到 builder 长期事件总线的唯一 SDK 订阅。
 * @param[in] builder 可选组合根借用，NULL 跳过，owner 保活到 sampler 与退订结束。
 * @post 成功保管订阅句柄，失败清空标记和 builder 并累计 dropped，无独立错误返回。
 * @pre owner 串行协调订阅/退订/销毁，当前位于该订阅回调之外。
 */
void app_profile_subscribe_runtime(cc_runtime_builder_t *builder);
/** @brief 撤销订阅并等待 SDK 回调结束，成功结束 builder 借用。
 * @param[in] timeout_ms 毫秒预算，超过 INT_MAX 饱和，零立即检查。
 * @return 未订阅/成功/已不存在 ESP_OK，代际不可取 INVALID_STATE，超时 TIMEOUT，其余 ESP_FAIL。
 * @pre owner 串行交接，builder 存活且当前位于该回调之外，sampler 另由 stop 排空。
 */
esp_err_t app_profile_unsubscribe_runtime(uint32_t timeout_ms);
/** @brief 锁内追加配置、计数、最近快照及缓存指标。
 * @param[in,out] sb 调用者已初始化的 builder，NULL 跳过，保留 sticky 错误状态。
 * @post 构造结果消息与临时缓存正文清理，调用者通过最终 take 检查完整性。
 */
void app_profile_append_status_json(cc_string_builder_t *sb);
