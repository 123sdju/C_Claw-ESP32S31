#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief 进程期传输适配器，init 复制表，回调和 user_data 由外层持有。
 * @note send 回调必须同步消费字符串，query_value 返回 heap_caps_free 可释放副本。
 * alloc_io 可空，非空必须返回 INTERNAL|8BIT 且 heap_caps_free 可释放的独占缓冲。
 * recv 至多写入 length 字节，返回实际长度或负错误，request_timeout_code 必须负值。
 * stop/start_web 可阻塞并须支持恢复重试，同表 init 幂等，安装后的表不可替换。
 */
typedef struct {
    char *(*alloc_io)(size_t size); /**< 可空分配器，size 字节，独占结果或 NULL。 */
    esp_err_t (*send_text)(void *request, const char *text, const char *type); /**< 必填，同步借用请求、NUL 正文和类型，返回发送结果。 */
    esp_err_t (*send_error)(void *request, int code, const char *message); /**< 必填，同步借用请求和消息，code 为协议码，成功发送错误也可返回 ESP_OK。 */
    char *(*query_value)(void *request, const char *key); /**< 必填，请求和 NUL key 同步借用，返回可释放副本或 NULL。 */
    int (*recv)(void *request, char *buffer, int length); /**< 必填，借用请求并写入 length 字节槽，返回字节数或负错误。 */
    void (*json_escape_append)(void *builder, const char *text); /**< 兼容表项，当前实现使用内部 sticky builder 转义，无调用。 */
    esp_err_t (*stop_web)(void *user_data); /**< 必填，同步借用 user_data，返回停止结果，失败可能部分停止。 */
    esp_err_t (*start_web)(void *user_data); /**< 必填，同步借用 user_data，返回恢复结果，须可重复调用。 */
    int request_timeout_code;
    int bad_request_code;
    int server_error_code;
    size_t io_chunk;
    void *user_data;
} ota_service_transport_t;

/** @brief 安装不可变传输表、两个进程期 mutex 和 8192 字节栈 watchdog。
 * @param[in] transport 必填同步表借用，函数指针和 user_data 须保持进程期有效。
 * @return 成功、INVALID_ARG、NO_MEM 或已安装不同表的 INVALID_STATE。
 * @note io_chunk 零使用 4096，须不超过 INT_MAX，并发首次初始化只发布一个锁。
 */
esp_err_t ota_service_init(const ota_service_transport_t *transport);
/** @brief 健康检查后，在维护离线窗口将运行镜像标记 VALID 并恢复服务。
 * @return 初始化未完成/操作冲突/恢复未完成为 INVALID_STATE，其余原生结果。
 * @pre startup owner 已完成健康判断，调用者允许阻塞。
 * @post ESP_OK 要求确认和恢复成功，恢复失败时镜像可能已 VALID，详见状态。
 */
esp_err_t ota_service_confirm_running_app(void);
/** @brief 同步发送服务、管理器、分区、资源及恢复状态 JSON。
 * @param[in,out] request 同步请求借用，空值语义由适配器定义。
 * @return 未初始化为 INVALID_STATE，其余发送器结果。
 * @note 来源分别加锁采样，构造器/JSON 本层释放，发送器不得保留文本。
 */
esp_err_t ota_service_status(void *request);
/** @brief 校验 name/size/sha256/time，等待空闲并建立六十秒准备租约。
 * @param[in,out] request 同步请求借用，参数副本本层释放。
 * @return 操作入场冲突为 ESP_FAIL，其余发送器结果，错误响应也可返回 ESP_OK。
 * @post 失败恢复部分维护状态，恢复失败由 watchdog 重试。
 */
esp_err_t ota_service_prepare(void *request);
/** @brief 接收 PSRAM 镜像并验证 SHA-256、布局，异步交接离线写入任务。
 * @param[in,out] request 同步请求借用，须与 prepare 元数据匹配，任务不保留请求。
 * @param[in] content_len 正镜像字节数，非正值拒绝。
 * @return 入场冲突为 ESP_FAIL，其余发送器结果，接受后 worker 可独立完成并重启。
 * @note 整体接收上限 180 秒，空闲上限 30 秒，连续超时上限五次。
 */
esp_err_t ota_service_upload(void *request, int content_len);
/** @brief 从 label 参数请求选择 boot，需写入时启动离线选择任务。
 * @param[in,out] request 同步请求借用，参数副本本层释放。
 * @return 入场冲突为 ESP_FAIL，其余发送器结果，异步接受详情通过 status 查询。
 * @post 原有准备租约先恢复，任务拥有独立 job，接受后发送失败仍继续。
 */
esp_err_t ota_service_select(void *request);
/** @brief 创建延迟约 1.2 秒的复位任务并请求 lifecycle 排空。
 * @param[in,out] request 同步请求借用。
 * @return 入场冲突为 ESP_FAIL，其余发送器结果，任务启动后响应失败仍继续。
 */
esp_err_t ota_service_reboot(void *request);
/** @brief 串行取消准备/管理器事务并尝试恢复相机与服务。
 * @param[in,out] request 同步请求借用。
 * @return 异步 flash/select/reboot 或恢复冲突为 ESP_FAIL，其余发送器结果。
 * @post 已写镜像和 boot 选择保留，恢复失败仍由 watchdog 承接。
 */
esp_err_t ota_service_cancel(void *request);
