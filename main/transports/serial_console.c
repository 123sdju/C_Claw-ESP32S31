#include "serial_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_event_bus.h"
#include "interaction_service.h"
#include "startup_gate.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define serial_console_LINE_BYTES 1024 /**< 行缓冲容量，包含末尾 NUL，正文最多 1023 字节。 */
#define serial_console_STACK_BYTES 4096 /**< IDF 任务栈字节数。 */
#define serial_console_IDLE_SUBMIT_MS 800 /**< 已接收逻辑行的空闲提交或拒绝间隔，单位毫秒。 */
#define serial_console_STOP_POLL_MS 100 /**< UART 接收及交互空闲检查的分段毫秒预算。 */
#define serial_console_IDLE_WAIT_MS 30000 /**< 已提交输入等待交互空闲的总毫秒预算。 */
#define serial_console_DEFAULT_STOP_MS 5000 /**< stop 零参数采用的毫秒预算。 */

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define serial_console_STACK_IN_PSRAM 1
#else
#define serial_console_STACK_IN_PSRAM 0
#endif

static const char *TAG = "serial_console";
static TaskHandle_t s_serial_console_task;
static bool s_serial_console_ready;
static serial_console_submit_fn s_serial_submit;
static void *s_serial_submit_data;
static int s_serial_stop_requested;
/* 任务结束 callback、Interaction 与 EventBus 借用后 release 发布，原生任务自删仍由内核完成。 */
static int s_serial_finished = 1;

/** @brief acquire 读取协作停止标志。 @return 已请求停止为 true，不取得外部资源。 */
static bool serial_console_stopping(void)
{
    return __atomic_load_n(&s_serial_stop_requested, __ATOMIC_ACQUIRE) != 0;
}

/**
 * @brief 将有限毫秒向上换算成 tick，避开无限等待哨兵及窄乘法溢出。
 * @param[in] milliseconds 毫秒数，零也返回一个 tick，公开 stop 的零默认值由入口先处理。
 * @return 至少一个 tick，过长预算封顶为 portMAX_DELAY 减一。
 */
static TickType_t serial_console_ticks(uint32_t milliseconds)
{
    uint64_t ticks = ((uint64_t)milliseconds * configTICK_RATE_HZ + 999U) / 1000U;
    if (ticks >= (uint64_t)portMAX_DELAY) return portMAX_DELAY - 1;
    return ticks ? (TickType_t)ticks : 1;
}

/**
 * @brief 按当前 IDF 能力创建任务，支持外部栈时先尝试 PSRAM 再尝试内部 RAM。
 * @param[in] task 必填任务入口，代码覆盖任务寿命。
 * @param[in] name 任务名称借用，创建期间有效，原生端口按自身规则复制。
 * @param[in] stack_bytes IDF 栈容量，单位字节。
 * @param[in,out] arg 原样交任务的借用上下文，owner 保活至任务结束使用。
 * @param[in] priority 原生任务优先级。
 * @param[out] out_handle 必填句柄槽，成功由本模块保管，失败输出沿原生 API。
 * @return pdPASS 表示已创建，其余为原生失败，不接管 arg 的销毁责任。
 */
static BaseType_t serial_console_create_task(TaskFunction_t task,
                                          const char *name,
                                          uint32_t stack_bytes,
                                          void *arg,
                                          UBaseType_t priority,
                                          TaskHandle_t *out_handle)
{
#if serial_console_STACK_IN_PSRAM
    BaseType_t ok = xTaskCreateWithCaps(task,
                                        name,
                                        stack_bytes,
                                        arg,
                                        priority,
                                        out_handle,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) return ok;
    return xTaskCreateWithCaps(task,
                               name,
                               stack_bytes,
                               arg,
                               priority,
                               out_handle,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(task, name, stack_bytes, arg, priority, out_handle);
#endif
}

/**
 * @brief 按创建分支匹配 WithCaps 或普通任务的自删除入口。
 * @pre 仅由当前串口任务在结束全部外部借用并发布完成位后调用。
 * @post 当前任务请求原生自删，正常情况下不再返回，TCB 和栈回收由内核或端口完成。
 */
static void serial_console_delete_self(void)
{
#if serial_console_STACK_IN_PSRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

/**
 * @brief 按 sdkconfig 建立 8N1、无流控的 UART 接收驱动并记住就绪状态。
 * @return 已就绪或配置/安装成功为 ESP_OK，其他错误原样返回，部分初始化可由后续 start 重试。
 * @pre 生命周期 owner 串行调用 start/stop，已安装驱动的 INVALID_STATE 沿现有兼容视为可复用。
 * @note stop 保留 UART 驱动，重启只重建轮询任务，不在此销毁原生端口。
 */
static esp_err_t init_serial_console_once(void)
{
    if (s_serial_console_ready) {
        return ESP_OK;
    }

    const uart_port_t uart_num = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(uart_num, &uart_config);
    if (err != ESP_OK) {
        return err;
    }

    err = uart_driver_install(uart_num, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_serial_console_ready = true;
    return ESP_OK;
}

/**
 * @brief 按 CR、LF 或有输入后的八百毫秒空闲结束一个 UART 逻辑行。
 * @param[out] line 必填可写行缓冲，成功返回 NUL 文本，超长行置空供任务跳过，false 时内容不可提交。
 * @param[in] line_size 缓冲字节容量，包含 NUL，零返回 false。
 * @return 已结束一行返回 true，参数无效或协作停止返回 false。任一次超容量后整行丢弃并提示用户分段发送。
 * @note 未溢出时退格按字节删除，其他低于 0x20 的控制字节忽略，不验证 UTF-8。
 * 接收期间每段检查停止，溢出后的字节继续消费到原行边界，不交给后续命令。
 */
static bool serial_read_line(char *line, size_t line_size)
{
    if (!line || line_size == 0) return false;
    const uart_port_t uart_num = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    size_t len = 0;
    bool overflow = false;
    TickType_t last_input = xTaskGetTickCount();
    while (!serial_console_stopping()) {
        uint8_t ch = 0;
        int n = uart_read_bytes(uart_num, &ch, 1, serial_console_ticks(serial_console_STOP_POLL_MS));
        if (serial_console_stopping()) return false;
        if (n <= 0) {
            if ((len > 0 || overflow) && (TickType_t)(xTaskGetTickCount() - last_input) >=
                           serial_console_ticks(serial_console_IDLE_SUBMIT_MS)) {
                line[overflow ? 0 : len] = '\0';
                printf("\r\n");
                if (overflow) printf("[warn] input exceeds %u bytes; entire line rejected. Please send shorter segments.\r\n", (unsigned)(line_size - 1));
                fflush(stdout);
                return true;
            }
            continue;
        }
        last_input = xTaskGetTickCount();
        if (ch == '\r' || ch == '\n') {
            line[overflow ? 0 : len] = '\0';
            printf("\r\n");
            if (overflow) printf("[warn] input exceeds %u bytes; entire line rejected. Please send shorter segments.\r\n", (unsigned)(line_size - 1));
            fflush(stdout);
            return true;
        }
        if (overflow) continue;
        if (ch == 0x08 || ch == 0x7f) {
            if (len > 0) {
                len--;
                printf("\b \b");
                fflush(stdout);
            }
            continue;
        }
        if (ch < 0x20) continue;
        if (len + 1 < line_size) {
            line[len++] = (char)ch;
            putchar((int)ch);
            fflush(stdout);
        } else {
            overflow = true;
        }
    }
    return false;
}

/**
 * @brief 拥有串口行缓冲并同步借出每条有效输入，协作停止后发布依赖使用结束。
 * @param[in,out] arg 创建时为 NULL，当前忽略，不接管外部资源。
 * @post 行缓冲已释放，完成位 release 可见后不再使用 router、Interaction 和 EventBus，随后请求原生自删。
 * @note 优先调用自定义 router，否则 /clear 单独提交、其他文本按原文提交。
 * 空行和被拒绝超长行均跳过。成功提交后最多等待交互空闲三十秒，启动 router 可在 Interaction 创建前直接返回。
 * 首次访问外部依赖前等待创建者发布任务句柄，使回调内 stop 能识别当前任务。
 */
static void serial_console_task(void *arg)
{
    (void)arg;
    while (!__atomic_load_n(&s_serial_console_task, __ATOMIC_ACQUIRE))
        vTaskDelay(serial_console_ticks(1));
    char *line = heap_caps_malloc(serial_console_LINE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) line = heap_caps_malloc(serial_console_LINE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!line) {
        ESP_LOGE(TAG, "serial line buffer alloc failed");
        __atomic_store_n(&s_serial_finished, 1, __ATOMIC_RELEASE);
        serial_console_delete_self();
        return;
    }

    printf("\r\nC-Claw serial console ready on UART%d.\r\n", CONFIG_ESP_CONSOLE_UART_NUM);
    printf("Commands: /help /heap /clear /lua <code> /luafile <path> /thinking on|off /debug on|off /stream on|off /profile on|off|sample|level basic|trace|interval <ms>\r\n");
    fflush(stdout);
    app_event_publish(APP_EVENT_KIND_STATUS, APP_EVENT_SOURCE_SERIAL, CC_LOG_INFO, "Serial console ready");

    while (!serial_console_stopping()) {
        printf("\r\ncclaw> ");
        fflush(stdout);
        if (!serial_read_line(line, serial_console_LINE_BYTES)) continue;
        if (serial_console_stopping()) break;
        if (line[0] == '\0') continue;
        esp_err_t err;
        if (s_serial_submit) {
            err = s_serial_submit(line, s_serial_submit_data);
        } else if (strcmp(line, "/clear") == 0) {
            err = interaction_service_submit_clear(APP_EVENT_SOURCE_SERIAL);
        } else {
            err = interaction_service_submit_text(APP_EVENT_SOURCE_SERIAL, line);
        }
        if (err != ESP_OK) {
            app_event_publishf(APP_EVENT_KIND_ERROR,
                               APP_EVENT_SOURCE_SERIAL,
                               CC_LOG_ERROR,
                               "submit failed: %s",
                               esp_err_to_name(err));
        } else if (!s_serial_submit || startup_gate_phase() == STARTUP_GATE_RUNNING) {
            /* 启动 router 可在 Interaction 创建前接纳确认命令，gate 的 acquire 读取配对初始化发布。 */
            TickType_t start = xTaskGetTickCount();
            while (!serial_console_stopping() &&
                   !interaction_service_wait_idle(serial_console_STOP_POLL_MS) &&
                   (TickType_t)(xTaskGetTickCount() - start) < serial_console_ticks(serial_console_IDLE_WAIT_MS)) {}
        }
    }
    free(line);
    __atomic_store_n(&s_serial_finished, 1, __ATOMIC_RELEASE);
    serial_console_delete_self();
}

/**
 * @brief 使用缺省 Interaction 提交方式启动串口轮询任务。
 * @return 返回 start_with_submit(NULL, NULL) 的结果，已有不同 router 的活动任务会拒绝。
 * @pre 生命周期 owner 串行 start/stop，Interaction 与 EventBus 寿命覆盖任务使用期。
 */
bool serial_console_start(void)
{
    return serial_console_start_with_submit(NULL, NULL);
}

/**
 * @brief 初始化 UART 并建立唯一串口任务，或确认在用 router 配对完全相同。
 * @param[in] submit 可选同步 router，NULL 使用默认 Interaction 提交，其代码覆盖任务使用期。
 * @param[in,out] user_data 原样借给 router 的上下文，NULL 可用，维持至 stop 成功或任务完成。
 * @return 创建并仍活动或已有相同配对活动任务为 true，初始化、RX 清空、创建、即时退出或配对冲突为 false。
 * @pre 生命周期 owner 串行调用 start/stop，回调内禁止重入 start。
 * @note 新任务接纳前先清空 RX，清空失败保留关闭状态，既有相同活动任务的幂等 start 不清队列。
 * 栈为 4096 字节、优先级四，创建失败保留可复用 UART，完成位同步任务末次依赖访问。
 */
bool serial_console_start_with_submit(serial_console_submit_fn submit, void *user_data)
{
    TaskHandle_t existing = __atomic_load_n(&s_serial_console_task, __ATOMIC_ACQUIRE);
    if (existing && !__atomic_load_n(&s_serial_finished, __ATOMIC_ACQUIRE))
        return !serial_console_stopping() && s_serial_submit == submit && s_serial_submit_data == user_data;
    __atomic_store_n(&s_serial_stop_requested, 1, __ATOMIC_RELEASE);
    esp_err_t err = init_serial_console_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "serial console init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = uart_flush_input((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "serial console RX flush failed: %s", esp_err_to_name(err));
        return false;
    }
    __atomic_store_n(&s_serial_console_task, NULL, __ATOMIC_RELEASE);
    s_serial_submit = submit;
    s_serial_submit_data = user_data;
    __atomic_store_n(&s_serial_stop_requested, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_serial_finished, 0, __ATOMIC_RELEASE);
    TaskHandle_t created = NULL;
    BaseType_t ok = serial_console_create_task(serial_console_task,
                                            "serial_console",
                                            serial_console_STACK_BYTES,
                                            NULL,
                                            4,
                                            &created);
    if (ok != pdPASS) {
        __atomic_store_n(&s_serial_finished, 1, __ATOMIC_RELEASE);
        s_serial_submit = NULL;
        s_serial_submit_data = NULL;
        ESP_LOGE(TAG, "failed to create serial console task");
        return false;
    }
    __atomic_store_n(&s_serial_console_task, created, __ATOMIC_RELEASE);
    if (__atomic_load_n(&s_serial_finished, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_serial_console_task, NULL, __ATOMIC_RELEASE);
        s_serial_submit = NULL;
        s_serial_submit_data = NULL;
        return false;
    }
    ESP_LOGI(TAG, "Serial console started");
    return true;
}

/**
 * @brief 请求串口任务协作退出，并等待其结束所有外部依赖借用。
 * @param[in] timeout_ms 等待任务结束的有限毫秒预算，零使用五秒默认值，换算向上取整并封顶到有限 tick。
 * @return 驱动未初始化或任务结束且 RX 清空为 ESP_OK，任务内调用为 INVALID_STATE，超时为 TIMEOUT，flush 失败返回原生错误。
 * @pre 生命周期 owner 串行 start/stop，等待期间保持 router、Interaction 与 EventBus 存活。
 * @note 先结束 RAM 行和外部借用，再清空当前 RX 积压，成功后才清除句柄与 router 配对。
 * 超时或 flush 失败保留关闭状态及原配对供重试，原生 TCB/栈最终回收仍由内核完成，UART 驱动继续保留。
 * flush 在任务退出后同步调用，其内部等待沿 UART 驱动契约。
 */
esp_err_t serial_console_stop(uint32_t timeout_ms)
{
    TaskHandle_t task = __atomic_load_n(&s_serial_console_task, __ATOMIC_ACQUIRE);
    if (!task && !s_serial_console_ready) return ESP_OK;
    if (!__atomic_load_n(&s_serial_finished, __ATOMIC_ACQUIRE) &&
        task == xTaskGetCurrentTaskHandle()) return ESP_ERR_INVALID_STATE;
    __atomic_store_n(&s_serial_stop_requested, 1, __ATOMIC_RELEASE);
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = serial_console_ticks(timeout_ms ? timeout_ms : serial_console_DEFAULT_STOP_MS);
    while (!__atomic_load_n(&s_serial_finished, __ATOMIC_ACQUIRE)) {
        if ((TickType_t)(xTaskGetTickCount() - start) >= timeout) return ESP_ERR_TIMEOUT;
        vTaskDelay(serial_console_ticks(1));
    }
    esp_err_t err = uart_flush_input((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM);
    if (err != ESP_OK) return err;
    __atomic_store_n(&s_serial_console_task, NULL, __ATOMIC_RELEASE);
    s_serial_submit = NULL;
    s_serial_submit_data = NULL;
    return ESP_OK;
}

/**
 * @brief 取得句柄存在且依赖使用尚未结束的原子快照。
 * @return 两项读取均表明活动为 true，其他为 false，可包含已请求停止但尚未退出的任务。
 * @note 不取得任务租约，结果仅供状态判断，不能据此释放其他 owner 的资源。
 */
bool serial_console_running(void)
{
    return __atomic_load_n(&s_serial_console_task, __ATOMIC_ACQUIRE) != NULL &&
           !__atomic_load_n(&s_serial_finished, __ATOMIC_ACQUIRE);
}
