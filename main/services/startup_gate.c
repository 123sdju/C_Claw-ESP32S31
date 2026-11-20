#include "startup_gate.h"
#include "interaction_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 生命周期 owner 发布阶段，入口任务通过原子读取决定请求路由。 */
static int s_phase = STARTUP_GATE_OFFLINE;

/**
 * @brief 以 release 语义发布启动阶段，供入口任务读取。
 * @param[in] phase 由生命周期 owner 决定的新阶段，本函数直接保存该值。
 * @post 后续 acquire 读取可观察到该阶段及发布前的初始化写入。
 * @note 业务转换条件由 owner 检查，服务入口和销毁屏障仍由各服务协调。
 */
void startup_gate_set_phase(startup_gate_phase_t phase)
{
    __atomic_store_n(&s_phase, (int)phase, __ATOMIC_RELEASE);
}

/**
 * @brief 以 acquire 语义取得启动阶段快照。
 * @return 本次读取的阶段，后续调用可能观察到 owner 发布的新值。
 */
startup_gate_phase_t startup_gate_phase(void)
{
    return (startup_gate_phase_t)__atomic_load_n(&s_phase, __ATOMIC_ACQUIRE);
}

/**
 * @brief 取得指定阶段的中文提示。
 * @param[in] phase 要显示的阶段，OFFLINE 及未识别值使用通用启动提示。
 * @return 固件静态字符串的借用指针，生命周期覆盖进程，调用者保留其只读属性。
 */
const char *startup_gate_message(startup_gate_phase_t phase)
{
    switch (phase) {
    case STARTUP_GATE_LOADING: return "正在读取 U 盘配置并检查恢复记录。";
    case STARTUP_GATE_NVS_REPAIR: return "设备 NVS 存储不可用；未执行自动擦除，依赖它的操作已暂停，请修复设备存储后重启。";
    case STARTUP_GATE_USB_DISCONNECTED: return "U 盘已断开，依赖存储的操作已暂停。";
    case STARTUP_GATE_USB_RELOADING: return "U 盘已插入，正在重启以加载当前盘的配置和数据。";
    case STARTUP_GATE_FAILED: return "存储检查失败，相关操作已暂停，请检查设备和 U 盘。";
    case STARTUP_GATE_RUNNING: return "启动完成。";
    default: return "正在启动。";
    }
}

/**
 * @brief 把阶段映射到状态 JSON 使用的稳定名称。
 * @param[in] phase 阶段快照，OFFLINE 及未识别值映射为 starting。
 * @return 静态名称的借用指针。
 */
static const char *phase_name(startup_gate_phase_t phase)
{
    switch (phase) {
    case STARTUP_GATE_LOADING: return "loading";
    case STARTUP_GATE_NVS_REPAIR: return "nvs_repair";
    case STARTUP_GATE_USB_DISCONNECTED: return "usb_disconnected";
    case STARTUP_GATE_USB_RELOADING: return "usb_reloading";
    case STARTUP_GATE_FAILED: return "failed";
    case STARTUP_GATE_RUNNING: return "running";
    default: return "starting";
    }
}

/**
 * @brief 用同一次阶段快照生成状态 JSON。
 * @return 包含 phase/message 的拥有字符串，调用者 free，分配失败返回 NULL。
 * @note 内容来自本文件的固定名称和提示，阶段读取后发生的变化由下次查询反映。
 */
char *startup_gate_status_json(void)
{
    startup_gate_phase_t phase = startup_gate_phase();
    const char *name = phase_name(phase), *message = startup_gate_message(phase);
    size_t bytes = strlen(name) + strlen(message) + 32U;
    char *json = malloc(bytes);
    if (json) snprintf(json, bytes, "{\"phase\":\"%s\",\"message\":\"%s\"}", name, message);
    return json;
}

/**
 * @brief 根据阶段把文本交给交互服务，或处理启动期间的清屏请求。
 * @param[in] source 请求入口，用于交互提交与提示事件归因。
 * @param[in] text 非空 C 字符串，调用期间借用。
 * @return RUNNING 时返回交互服务结果，其他阶段 /clear 成功返回 ESP_OK，
 *         其余文本发布当前提示后返回 ESP_ERR_INVALID_STATE，空文本为 ESP_ERR_INVALID_ARG。
 * @note 阶段判断使用一次快照，交互服务自身继续校验接纳与关闭状态。
 */
esp_err_t startup_gate_submit(app_event_source_t source, const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    startup_gate_phase_t phase = startup_gate_phase();
    if (phase == STARTUP_GATE_RUNNING) return interaction_service_submit_text(source, text);
    if (strcmp(text, "/clear") == 0) {
        app_event_bus_clear_conversation();
        return ESP_OK;
    }
    app_event_publish(APP_EVENT_KIND_STATUS, source, CC_LOG_WARN, startup_gate_message(phase));
    return ESP_ERR_INVALID_STATE;
}
