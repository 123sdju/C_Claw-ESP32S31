#pragma once

#include "app_event_bus.h"
#include "esp_err.h"

/** 生命周期 owner 发布的入口路由阶段，查询与提交任务读取其原子快照。 */
typedef enum {
    STARTUP_GATE_OFFLINE = 0,
    STARTUP_GATE_LOADING,
    STARTUP_GATE_FAILED,
    STARTUP_GATE_RUNNING,
    STARTUP_GATE_NVS_REPAIR,
    STARTUP_GATE_USB_DISCONNECTED,
    STARTUP_GATE_USB_RELOADING
} startup_gate_phase_t;

/**
 * @brief 发布新的启动阶段。
 * @param[in] phase 生命周期 owner 已决定的阶段，直接以 release 语义保存。
 * @post 后续入口可通过 acquire 读取观察到本次发布。
 * @note 阶段发布与服务接纳、排空及销毁分别按各自的生命周期契约协调。
 */
void startup_gate_set_phase(startup_gate_phase_t phase);
/**
 * @brief 读取当前启动阶段。
 * @return 以 acquire 语义取得的一次阶段快照。
 */
startup_gate_phase_t startup_gate_phase(void);
/**
 * @brief 将请求路由到已运行的交互服务，或显示启动状态。
 * @param[in] source 文本来自的交互入口。
 * @param[in] text 调用期间借用的非空文本。
 * @return RUNNING 时返回交互提交结果，其他阶段 /clear 返回 ESP_OK，
 *         其余请求为 ESP_ERR_INVALID_STATE，空文本为 ESP_ERR_INVALID_ARG。
 */
esp_err_t startup_gate_submit(app_event_source_t source, const char *text);
/**
 * @brief 序列化同一次阶段快照的名称和提示。
 * @return 拥有的 JSON 文本，调用者 free，分配失败为 NULL。
 */
char *startup_gate_status_json(void);
/**
 * @brief 取得指定阶段的中文提示。
 * @param[in] phase 要查询的阶段，未识别值使用通用启动提示。
 * @return 只读静态字符串的借用指针，生命周期覆盖进程。
 */
const char *startup_gate_message(startup_gate_phase_t phase);
