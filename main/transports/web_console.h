#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "esp_err.h"

typedef struct {
    const app_runtime_io_config_t *io; /**< 仅 start/promote 调用期间借用的稳定 IO，模块复制 web 与 max_input_bytes 标量。 */
    /**
     * @brief 为配置 GET 生成独立脱敏 JSON。
     * @param[in,out] user_data 借用业务上下文，owner 保活到最终 destroy。
     * @return free 可释放的拥有正文，NULL 表示不可用或分配失败，由 handler 发送错误。
     */
    char *(*config_json)(void *user_data);
    /**
     * @brief 同步应用配置并填写逐阶段报告。
     * @param[in] json 调用期间借用的 NUL 结尾配置正文。
     * @param[out] out_result 必需报告槽，由回调初始化并填写，handler 在调用后读取。
     * @param[in,out] user_data 借用业务上下文，回调自行同步配置事务。
     * @return ESP_OK 表示成功，其他错误和报告交由配置 handler 形成响应。
     */
    esp_err_t (*config_applied)(const char *json,
                                app_config_apply_result_t *out_result,
                                void *user_data);
    /**
     * @brief 生成设备状态 JSON。
     * @param[in,out] user_data 借用业务上下文，覆盖回调执行期。
     * @return free 可释放的拥有正文，NULL 触发 HTTP 500，handler 发送后释放。
     */
    char *(*status_json)(void *user_data);
    /**
     * @brief 生成当前事件视图与游标响应。
     * @param[in] since 请求起始序列号。
     * @param[in] limit 请求显式条数或原子读取的当前默认值，回调负责实际范围规则。
     * @param[in] kind_mask 请求事件种类掩码。
     * @param[in,out] user_data 借用业务上下文，覆盖回调执行期。
     * @return free 可释放的拥有 JSON，NULL 触发 HTTP 500，handler 发送后释放。
     */
    char *(*events_json)(uint32_t since, int limit, uint32_t kind_mask, void *user_data);
    /**
     * @brief 同步接收一条 Web 输入，排队等延后处理需在返回前复制正文。
     * @param[in] text 调用期间借用的 NUL 结尾正文，handler 返回后释放缓冲。
     * @param[in,out] user_data 借用业务上下文。
     * @return ESP_OK 形成 queued 成功响应，其他错误形成 HTTP 400。
     */
    esp_err_t (*message_submitted)(const char *text, void *user_data);
    /**
     * @brief 由运行时 owner 判断路径能否经通用文件 API 读取。
     * @param[in] path 调用期间借用的已检查路径。
     * @param[in,out] user_data 借用业务上下文，回调自行同步当前受保护路径。
     * @return true 允许，false 拒绝，未设置时通用读取失败关闭。
     */
    bool (*file_read_allowed)(const char *path, void *user_data);
    /**
     * @brief 由配置 owner 判断当前路径能否经通用文件 API 修改。
     * @param[in] path 调用期间借用的已检查路径。
     * @param[in,out] user_data 借用业务上下文，回调自行同步当前存储和日志路径。
     * @return true 允许，false 拒绝，未设置时通用修改失败关闭。
     */
    bool (*file_mutation_allowed)(const char *path, void *user_data);
    void *user_data; /**< 业务借用上下文，stop 后仍保留供 restart，最终 destroy 成功后才能释放。 */
    bool startup_only; /**< 存储身份确认前仅公开启动页面、状态、事件与输入。 */
} web_console_context_t;

/**
 * @brief 启动 HTTP 控制台，按 startup_only 注册受限或完整路由。
 * @param[in] context 调用期间稳定借用的上下文，NULL 使用默认无回调上下文；IO 标量复制，回调和 user_data 继续借用。
 * @return 成功或相同完整上下文已运行时为 ESP_OK，切换忙、上下文冲突、OTA/HTTP 启动或路由注册失败返回对应错误。
 * @note 失败停止部分 server 也失败时保留句柄，应用路由保持拒绝，调用者可重试 destroy。
 *       IO 及源配置在返回后可释放，已保存的回调和 user_data 须覆盖后续 restart 至最终 destroy。
 */
esp_err_t web_console_start(const web_console_context_t *context);
/**
 * @brief 在依赖就绪后将启动控制台提升为完整控制台。
 * @param[in] context 必需非 startup_only 的稳定借用上下文，复制和寿命规则同 start。
 * @param[out] out_startup_restored 可选结果，进入先置 false，停止失败保留旧启动 server 或恢复成功时置 true。
 * @return 完整启动成功为 ESP_OK，参数、切换、停止或启动失败原样返回，恢复失败仍返回最初启动错误。
 * @note 尝试前保存旧 IO 和当前热限制，完整启动失败且无 server 时恢复两者；部分 server 无法停止时保留拒绝状态。
 */
esp_err_t web_console_promote_startup(const web_console_context_t *context, bool *out_startup_restored);
/**
 * @brief 停止 HTTP server 并等待已接纳 handler 退出，保留上下文和热限制供 restart。
 * @return 无 server 或停止成功为 ESP_OK，切换忙为 INVALID_STATE，原生停止错误保留 server 供重试。
 * @pre 调用方不得在该 HTTP server 的 handler 内同步等待自身退出。
 */
esp_err_t web_console_stop(void);
/**
 * @brief 使用最近启动的自有 IO 快照和借用回调恢复 server，保留已发布热限制。
 * @return 无 IO 或切换忙为 INVALID_STATE，其余遵循 start 的结果；完整 server 已运行时重新放行应用路由。
 */
esp_err_t web_console_restart(void);
/**
 * @brief 停止后清除保存的上下文和 IO，恢复默认热限制。
 * @return 成功为 ESP_OK，切换忙或停止失败时保留上下文和 server，允许随后重试。
 * @pre owner 已关闭新的配置发布入口，且遵循 stop 的调用线程约束。
 */
esp_err_t web_console_destroy(void);

/**
 * @brief 原子发布配置 owner 已采用版本的两个 Web 热限制。
 * @param[in] event_limit 默认事件条数，直接保存已验证的配置值。
 * @param[in] max_input_bytes 最大输入字节数，直接保存已验证的配置值。
 * @post 后续请求读取各自当前限制，调用无需 Web 切换锁，也不等待 handler。
 * @pre 配置 owner 串行发布成功或已安装的 RAM 版本，最终 destroy 前关闭发布入口。
 */
void web_console_publish_limits(int event_limit, int max_input_bytes);
