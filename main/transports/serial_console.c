#include "serial_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_event_bus.h"
#include "interaction_service.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define serial_console_LINE_BYTES 1024
#define serial_console_STACK_BYTES 4096
#define serial_console_IDLE_SUBMIT_MS 800

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define serial_console_STACK_IN_PSRAM 1
#else
#define serial_console_STACK_IN_PSRAM 0
#endif

static const char *TAG = "serial_console";
static TaskHandle_t s_serial_console_task;
static bool s_serial_console_ready;

/*
 * 创建 FreeRTOS 任务，优先使用 PSRAM 作为任务栈，失败时回退到内部 RAM。
 * 当 PSRAM 启用且 IDF 配置支持时，调用 xTaskCreateWithCaps 以 MALLOC_CAP_SPIRAM 分配栈；
 * 若 PSRAM 分配失败则回退到内部 RAM 重试；否则直接使用标准 xTaskCreate。
 * 参数: task  - 任务入口函数指针
 *       name  - 任务名称（用于调试）
 *       stack_bytes - 栈大小（字节）
 *       arg   - 传递给任务函数的参数
 *       priority - 任务优先级
 *       out_handle - 输出任务句柄
 * 返回: pdPASS 表示成功，其他值表示失败
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

/*
 * 删除当前 FreeRTOS 任务自身。若任务栈由 PSRAM 分配则使用带 Caps 的删除 API。
 * 仅在任务内部调用，调用后当前任务不再执行。
 */
static void serial_console_delete_self(void)
{
#if serial_console_STACK_IN_PSRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

/*
 * 初始化 ESP 串口控制台硬件，仅执行一次。根据 sdkconfig 的 UART 端口号和波特率
 * 配置 UART 参数（8N1，无流控），安装 UART 驱动并分配 1024 字节接收缓冲区。
 * 若已初始化过（s_serial_console_ready 为 true），直接返回 ESP_OK。
 * 返回: ESP_OK 表示成功，其他 ESP 错误码表示初始化失败
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

/*
 * 从 UART 串口读取一行输入，支持退格编辑和行缓冲。
 * 逐字节轮询 UART：回车或换行时返回完整行；退格键删除前一字符；
 * 控制字符（&lt;0x20）被忽略；若缓冲区满则标记溢出并丢弃后续字符。
 * 首字节读取无超时限制（portMAX_DELAY），缓冲区有内容后空闲 800ms
 * 未收到新字节则自动提交当前行（用于空行检测）。
 * 参数: line      - 输出缓冲区指针
 *       line_size - 缓冲区大小（字节），含末尾 '\0'
 * 返回: true 表示读取到一行数据（可能为空行），false 表示参数无效
 */
static bool serial_read_line(char *line, size_t line_size)
{
    if (!line || line_size == 0) return false;
    const uart_port_t uart_num = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    size_t len = 0;
    bool overflow = false;
    while (true) {
        uint8_t ch = 0;
        TickType_t wait_ticks = len > 0 ? pdMS_TO_TICKS(serial_console_IDLE_SUBMIT_MS) : portMAX_DELAY;
        int n = uart_read_bytes(uart_num, &ch, 1, wait_ticks);
        if (n <= 0) {
            if (len > 0) {
                line[len] = '\0';
                printf("\r\n");
                if (overflow) printf("[warn] line truncated to %u bytes\r\n", (unsigned)(line_size - 1));
                fflush(stdout);
                return true;
            }
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            line[len] = '\0';
            printf("\r\n");
            if (overflow) printf("[warn] line truncated to %u bytes\r\n", (unsigned)(line_size - 1));
            fflush(stdout);
            return true;
        }
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
}

/*
 * 串口控制台主任务函数。在 FreeRTOS 任务中运行，分配 1024 字节行缓冲区（优先 PSRAM），
 * 打印启动提示和命令列表，然后进入无限循环：显示 "cclaw&gt; " 提示符，通过 serial_read_line
 * 读取用户输入行；空行跳过；"/clear" 命令调用 interaction_service_submit_clear，
 * 其他文本调用 interaction_service_submit_text 提交到交互服务处理。
 * 提交失败时发布错误事件；成功后等待交互服务空闲。
 * 参数: arg - 未使用（传入 NULL）
 */
static void serial_console_task(void *arg)
{
    (void)arg;
    char *line = heap_caps_malloc(serial_console_LINE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) line = heap_caps_malloc(serial_console_LINE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!line) {
        ESP_LOGE(TAG, "serial line buffer alloc failed");
        s_serial_console_task = NULL;
        serial_console_delete_self();
        return;
    }

    printf("\r\nC-Claw serial console ready on UART%d.\r\n", CONFIG_ESP_CONSOLE_UART_NUM);
    printf("Commands: /help /heap /clear /lua <code> /luafile <path> /thinking on|off /debug on|off /stream on|off /profile on|off|sample|level basic|trace|interval <ms>\r\n");
    fflush(stdout);
    app_event_publish(APP_EVENT_KIND_STATUS, APP_EVENT_SOURCE_SERIAL, CC_LOG_INFO, "Serial console ready");

    while (true) {
        printf("\r\ncclaw> ");
        fflush(stdout);
        if (!serial_read_line(line, serial_console_LINE_BYTES)) continue;
        if (line[0] == '\0') continue;
        esp_err_t err;
        if (strcmp(line, "/clear") == 0) {
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
        } else {
            interaction_service_wait_idle(0);
        }
    }
}

/*
 * 启动串口控制台。先调用 init_serial_console_once 初始化 UART 硬件，然后创建
 * 名为 "serial_console" 的 FreeRTOS 任务（栈 4096 字节，优先级 4）。
 * 若任务已在运行则直接返回成功，防止重复创建。
 * 返回: true 表示启动成功或已在运行，false 表示初始化或任务创建失败
 */
bool serial_console_start(void)
{
    if (s_serial_console_task) return true;
    esp_err_t err = init_serial_console_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "serial console init failed: %s", esp_err_to_name(err));
        return false;
    }
    BaseType_t ok = serial_console_create_task(serial_console_task,
                                            "serial_console",
                                            serial_console_STACK_BYTES,
                                            NULL,
                                            4,
                                            &s_serial_console_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create serial console task");
        s_serial_console_task = NULL;
        return false;
    }
    ESP_LOGI(TAG, "Serial console started");
    return true;
}

/*
 * 查询串口控制台任务是否正在运行。
 * 返回: true 表示任务句柄非空（在运行中），false 表示未启动或已退出
 */
bool serial_console_running(void)
{
    return s_serial_console_task != NULL;
}
