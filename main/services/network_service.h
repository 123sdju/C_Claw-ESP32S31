#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "network_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 按值取得的网络诊断快照，Wi-Fi 与服务两部分分别取样。 */
typedef struct {
    bool enabled;                    /* WiFi 是否启用 */
    bool started;                    /* WiFi 驱动是否已启动 */
    bool connected;                  /* STA 是否已连接 */
    bool ap_enabled;                 /* SoftAP 是否启用 */
    bool ap_started;                 /* SoftAP 是否已启动 */
    bool mdns_enabled;               /* mDNS 是否启用 */
    bool keepalive;                  /* 保活/自动重连是否激活 */
    bool time_synced;                /* SNTP 时间是否已同步 */
    int retry_count;                 /* 当前重连计数 */
    int max_retry;                   /* 配置的最大重试次数 */
    uint16_t last_disconnect_reason; /* 上次断开的原因码 */
    char ssid[33];                   /* 当前 STA SSID */
    char ap_ssid[33];                /* SoftAP 热点名称。 */
    char sta_ip[16];                 /* STA 获得的 IP 地址 */
    char ap_ip[16];                  /* SoftAP IP 地址 */
    char mdns_hostname[32];          /* mDNS 主机名 */
    uint32_t command_high_water; /**< 已观察到的命令队列最大深度。 */
    uint32_t wifi_event_coalesced; /**< 饱和累计的最新状态覆盖次数，短锁失败可少计。 */
    uint32_t config_coalesced; /**< 饱和累计的尚未取走配置被覆盖次数。 */
    uint64_t owner_heartbeat_ms; /**< owner 最近一次取出队列提示时的平台单调毫秒。 */
    bool stopping; /**< 已接纳 STOP 且尚未确认该轮关闭结果。 */
} network_service_status_t;

/**
 * @brief 由唯一 lifecycle owner 逐步建立队列、QueueSet、锁、事件组和 Wi-Fi 回调注册。
 * @return 成功为 ESP_OK，分配或注册失败返回对应错误，已经创建的资源保留供重试或 destroy。
 * @note 不启动任务，初始化/销毁串行，注册成功后回调可向长期队列发布状态。
 */
esp_err_t network_service_init(void);
/**
 * @brief 设置后续 mDNS 配置应用使用的 HTTP 广告端口。
 * @param[in] port 端口号，零采用八十。
 * @return 成功、未初始化的 INVALID_STATE 或原生锁失败的 TIMEOUT。
 * @note 锁等待无独立预算，更新本字段不立即重建广告，外层排除并发销毁。
 */
esp_err_t network_service_set_web_port(uint16_t port);

/**
 * @brief 在已初始化资源上启动内部 RAM 栈 owner 并等待配置应用。
 * @param[in] config 必填完整稳定配置，接纳时按值复制，各字符串在数组范围内终止。
 * @param[in] timeout_ms 任务启动、提交和确认共用的毫秒预算，零仅作立即检查。
 * @return 成功为 ESP_OK，创建、参数、状态、队列或等待错误分别返回，Wi-Fi 应用失败为 ESP_FAIL。
 * @note mDNS 尽力完成，错误写日志。超时可能已有配置接纳并迟到生效，调用者不再被借用。
 */
esp_err_t network_service_start_from_config(const network_config_t *config, uint32_t timeout_ms);
/**
 * @brief 向运行中的 owner 发布最新配置快照并等待对应 generation 的确认。
 * @param[in] config 必填完整稳定配置，接纳时复制，字符串须在各数组范围内终止。
 * @param[in] timeout_ms 提交锁、状态锁和确认等待共用的毫秒预算，零不阻塞。
 * @return 应用成功为 ESP_OK，停止中/无 owner 为 INVALID_STATE，参数、队列或等待失败返回实际错误。
 * @note 超时不会撤销已接纳配置，迟到回复只写服务字段，外层事务负责核对或再次提交回滚配置。
 */
esp_err_t network_service_apply_wifi_config(const network_config_t *config, uint32_t timeout_ms);
/**
 * @brief 使用最后成功应用的配置快照重新启动 owner 和网络。
 * @param[in] timeout_ms 复制、启动和确认共用的毫秒预算，零不阻塞。
 * @return 无保存配置为 INVALID_STATE，其余为状态、锁、创建和应用结果。
 */
esp_err_t network_service_resume(uint32_t timeout_ms);
/**
 * @brief 请求 Wi-Fi 驱动关闭并等待网络 owner 结束全部动态资源访问。
 * @param[in] timeout_ms 总毫秒预算，零采用五千毫秒。
 * @return 成功停机为 true，队列、等待、驱动关闭错误或 owner 自等待为 false。
 * @note 失败保持请求及资源可重试，关闭错误保留 owner 处理下一次 STOP。
 */
bool network_service_stop(uint32_t timeout_ms);
/**
 * @brief 确认 owner 完成后关闭 stop 准入、排空调用和回调并释放动态 RTOS 资源。
 * @param[in] timeout_ms 状态检查及两类排空共用的毫秒预算，零仅检查。
 * @return 成功 ESP_OK，owner 活动或队列成员移除失败为 INVALID_STATE，尾部/排空未完成为 TIMEOUT，
 *         其余为回调注销错误。
 * @pre lifecycle owner 已停止服务并关闭其他 API 入口，排空 start/apply/resume/status/web-port
 *      等借用，串行协调销毁与重建。已登记的 stop 和 Wi-Fi 回调由本函数排空。
 * @note 失败保留尚未释放资源和关闭准入供重试，回调排空后丢弃末次队列事件再移除成员。
 *       Wi-Fi 进程期原生资源持续保留。
 */
esp_err_t network_service_destroy(uint32_t timeout_ms);
/**
 * @brief 提交重启模式关闭，停止 mDNS/HTTP 并关闭 Wi-Fi 保活。
 * @param[in] timeout_ms 总毫秒预算，零采用五千毫秒。
 * @return owner 最终完成为 true，状态或等待失败为 false，设备重启由外层执行。
 */
bool network_service_prepare_for_reboot(uint32_t timeout_ms);
/**
 * @brief 以五秒窗口等待 owner 完成网络及 Wi-Fi 驱动关闭。
 * @return 成功 ESP_OK，已记录原生错误优先，其余未完成为 TIMEOUT。
 * @note 原生调用可能继续执行到后续检查点，失败保留资源和 owner 供重试。
 */
esp_err_t network_service_shutdown_for_ota(void);
/**
 * @brief 获取两层锁分别保护的 Wi-Fi/网络值快照。
 * @param[out] out_status 可空输出槽，无动态拥有字段。
 * @post 有效槽得到完整值记录，服务状态锁五十毫秒超时时网络字段保守置零并保留日历判断。
 * @note 两层取样不代表同一时刻，调用及其动态锁借用由外层与 destroy 排空协调。
 */
void network_service_get_status(network_service_status_t *out_status);

#ifdef __cplusplus
}
#endif
