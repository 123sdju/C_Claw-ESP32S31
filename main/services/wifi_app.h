#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "network_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 短状态锁保护的应用快照，原生事件与驱动调用异步推进。 */
typedef struct {
    bool enabled; /**< 最近候选配置中 STA 的启用状态。 */
    bool started; /**< 当前跟踪的原生 driver 启动状态。 */
    bool connected; /**< 最近 GOT_IP/断开事件跟踪的 STA 状态。 */
    bool ap_enabled; /**< 最近候选配置中 SoftAP 的启用状态。 */
    bool ap_started; /**< 最近事件及已确认驱动操作跟踪的 AP 状态。 */
    bool keepalive; /**< 允许尽力自动重连，单次 timer 提交仍可能失败并记录诊断。 */
    int retry_count; /**< 饱和于 INT_MAX 的断开重试计数，连接成功后清零。 */
    int max_retry; /**< 首轮失败标志的阈值，达到后后台保活仍可继续。 */
    uint16_t last_disconnect_reason; /**< 最近原生断开原因，未知为零。 */
    char ssid[33]; /**< 最近候选配置的 STA SSID，独立终止文本。 */
    char ap_ssid[33]; /**< 最近成功安装的 SoftAP SSID。 */
    char sta_ip[16]; /**< 最近跟踪的 STA IPv4 文本，未连接时为空。 */
    char ap_ip[16]; /**< 已配置的 AP IPv4 文本。 */
} wifi_app_status_t;

/** @brief 本模块交给上层网络 owner 的状态变更种类。 */
typedef enum {
    WIFI_APP_EVENT_CONNECTED,
    WIFI_APP_EVENT_DISCONNECTED,
    WIFI_APP_EVENT_RECONNECTING,
    WIFI_APP_EVENT_DISABLED,
} wifi_app_event_t;

/**
 * @brief 同步消费一条 Wi-Fi 状态通知，跨回调保留内容时由接收者复制。
 * @param[in] event 按值事件编号。
 * @param[in] status 回调期状态借用，业务派发提供独立快照，直接 dispatch 也允许 NULL。
 * @param[in,out] user_data 注册者拥有的上下文借用，覆盖所有已登记回调。
 * @post 返回后结束本次状态借用，接收者自行同步并发上下文，保持短回调且不等待自己排空。
 */
typedef void (*wifi_app_event_callback_t)(wifi_app_event_t event,
                                          const wifi_app_status_t *status,
                                          void *user_data);

/**
 * @brief 在唯一空闲回调槽中成对注册函数和上下文。
 * @param[in] callback 必填同步函数。
 * @param[in,out] user_data 可空借用上下文，覆盖已登记的全部调用。
 * @return 成功 ESP_OK，空函数 INVALID_ARG，已有注册或正在注销为 INVALID_STATE。
 * @note 注册与最终释放由 Network lifecycle owner 协调，业务通知在驱动操作锁外执行。
 */
esp_err_t wifi_app_set_event_callback(wifi_app_event_callback_t callback, void *user_data);
/**
 * @brief 关闭新回调接纳并等待已取得注册快照的调用返回。
 * @param[in] timeout_ms 有限总毫秒预算，零只检查当前状态。
 * @return 已排空 ESP_OK，未完成 TIMEOUT，等待者计数饱和 INVALID_STATE。
 * @note 超时保持接纳关闭，末个回调归还后自动回收槽位，owner 保管旧 userdata 并完成
 *       销毁重试后释放。回调内部不能等待自己的借用结束。
 */
esp_err_t wifi_app_clear_event_callback(uint32_t timeout_ms);
/**
 * @brief 在进程期操作锁内初始化并应用 STA/AP 配置，归锁后通知上层。
 * @param[in] config 必填稳定配置，先按值复制，所用字符串须在各自数组内终止。
 * @return STA 在连接窗口内成功、仅 AP 配置成功或明确全部关闭为 true，其他为 false。
 * @note 非正连接预算采用二万毫秒，非正重试阈值采用五次，AP 参数沿既有默认归一化。
 *       安装失败允许同参重试，单纯连接超时保留已安装配置并尽力保活。操作锁和原生
 *       驱动调用的耗时单独于连接窗口，失败可能已有部分驱动变更。
 */
bool wifi_app_connect_from_config(const network_config_t *config);
/**
 * @brief 等待已接纳驱动序列后关闭重连请求，供外层重启准备。
 * @post 操作锁可用时清 keepalive、重试和 FAIL 位，driver 和进程期资源保留。
 * @note timer Stop 为零等待命令，失败写诊断，迟到 callback 在操作锁后复核关闭状态。
 */
void wifi_app_prepare_for_reboot(void);
/**
 * @brief 串行关闭保活并尝试 disconnect、stop、deinit driver。
 * @return 成功 ESP_OK，操作锁初始化失败 NO_MEM，其余保留首个原生错误。
 * @note 已完成步骤保持，未确认关闭的状态保留供重试。成功 deinit 后原生 RAM 配置
 *       缓存失效，相同配置也会重新安装。netif、事件组、handler、timer 和同步锁沿
 *       进程期保留，所有驱动序列产生的回调在归锁后派发。
 */
esp_err_t wifi_app_shutdown_for_ota(void);
/**
 * @brief 在进程期状态锁内读取异步跟踪的 STA 连接标志。
 * @return 当前快照，查询不建立 driver 或任务租约。
 */
bool wifi_app_is_connected(void);
/**
 * @brief 取得完整独立的 Wi-Fi 应用值快照。
 * @param[out] out_status 可空调用者槽，非空先清零再填充。
 * @post 无动态拥有字段，所有内部字符串已复制，原生状态可在返回后继续变化。
 */
void wifi_app_get_status(wifi_app_status_t *out_status);

#ifdef __cplusplus
}
#endif
