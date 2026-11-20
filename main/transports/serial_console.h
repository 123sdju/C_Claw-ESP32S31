#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在串口任务上下文同步接收一条有效输入，可路由启动确认或运行期消息。
 * @param[in] text 调用期间借用的 NUL 正文，最多 1023 字节，空行及超长整行不会交付，延后处理须先复制。
 * @param[in,out] user_data 注册方拥有的上下文借用，覆盖全部已接纳回调。
 * @return ESP_OK 表示接纳，其他结果由任务发布错误事件，不自动重试输入。
 * @note 输入按字节编辑并保留高位字节，不保证 UTF-8 完整性，回调不得同步等待本任务退出。
 */
typedef esp_err_t (*serial_console_submit_fn)(const char *text, void *user_data);
/**
 * @brief 启动唯一串口任务并借用可选输入 router。
 * @param[in] submit 同步 router 或 NULL，NULL 使用 Interaction 缺省提交。
 * @param[in,out] user_data 借用上下文，允许 NULL，维持至 stop 成功或任务完成。
 * @return 已有相同配对活动任务或成功创建为 true，配对冲突、关闭中、初始化/RX 清空/创建失败或即时退出为 false。
 * @pre 生命周期 owner 串行 start/stop，router 代码及依赖覆盖任务寿命。
 * @note 每次实际创建前清空 RX 积压，成功边界之后到达的字节按新输入接收，幂等 start 不清在用队列。
 */
bool serial_console_start_with_submit(serial_console_submit_fn submit, void *user_data);

/**
 * @brief 以 NULL router 启动，输入转交 Interaction，保留 UART 供后续重启。
 * @return 与 start_with_submit(NULL, NULL) 相同，已有不同配对任务时拒绝。
 * @pre 生命周期 owner 串行 start/stop，Interaction 与 EventBus 在任务使用期有效。
 */
bool serial_console_start(void);
/**
 * @brief 关闭本次输入循环并等待 callback、Interaction 与 EventBus 借用结束。
 * @param[in] timeout_ms 等待任务结束的毫秒预算，零使用五秒默认值，有限 tick 向上取整并封顶，随后同步 flush 沿驱动等待契约。
 * @return 未初始化驱动或任务结束且 RX 清空为 ESP_OK，本任务调用为 INVALID_STATE，超时为 TIMEOUT，flush 失败原样返回。
 * @pre 生命周期 owner 串行 start/stop，等待和超时后仍保持全部借用依赖。
 * @note 超长逻辑行按 CR/LF 或八百毫秒空闲整体丢弃并提示分段。停止先丢弃当前 RAM 行并等待外部借用退出，随后清空 RX。
 * 超时或清空失败保留关闭状态、句柄与配对供重试，UART 驱动复用。恢复前再次清空，成功边界后的字节属于新输入。
 * 此边界依据软件接收队列，物理 UART 在途字节没有跨代标记。原生任务栈/TCB 最终回收仍由端口完成。
 */
esp_err_t serial_console_stop(uint32_t timeout_ms);
/**
 * @brief 查询任务句柄与完成位的原子快照。
 * @return 已有句柄且尚未结束依赖使用为 true，关闭中的任务也可能为 true。
 * @note 不增加任务或依赖引用，生命周期操作仍由唯一 owner 串行组织。
 */
bool serial_console_running(void);

#ifdef __cplusplus
}
#endif
