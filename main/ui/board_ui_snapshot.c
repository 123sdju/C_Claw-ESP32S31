#include "board_ui_snapshot.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cc/util/cc_string_builder.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define UI_SNAPSHOT_MAX_LINES 80U
#define UI_SNAPSHOT_LINE_MAX 512U

typedef struct {
    char *lines[UI_SNAPSHOT_MAX_LINES]; /**< 环拥有的 PSRAM 行，各槽独立回收。 */
    size_t count; /**< 有效行数，不超过 80。 */
    size_t start; /**< 最早有效行的槽索引，由快照锁保护。 */
} board_ui_snapshot_ring_t;

static board_ui_snapshot_ring_t s_conversation;
static board_ui_snapshot_ring_t s_tool;
static board_ui_snapshot_ring_t s_boot;
static board_ui_snapshot_ring_t s_draw;
static SemaphoreHandle_t s_mutex;
static char s_status[1024];

/**
 * @brief 将单行复制到 PSRAM，正文最多保留 512 字节并避开 UTF-8 续字节截点。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串，有效 UTF-8 输入保持完整字符前缀。
 * @return 拥有的 NUL 字符串，使用 heap_caps_free 归还，PSRAM 不足返回 NULL。
 */
static char *snapshot_strdup(const char *text)
{
    size_t len = text ? strlen(text) : 0;
    if (len > UI_SNAPSHOT_LINE_MAX) {
        len = UI_SNAPSHOT_LINE_MAX;
        while (len && ((unsigned char)text[len] & 0xc0U) == 0x80U) len--;
    }
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) {
        return NULL;
    }
    if (len > 0 && text) {
        memcpy(copy, text, len);
    }
    copy[len] = '\0';
    return copy;
}

/**
 * @brief 获取已经发布的进程期快照锁。
 * @return 初始化已完成且成功取得互斥锁为 true，未初始化或原生获取失败为 false。
 * @note 使用 acquire 与 init 的 release 配对，锁发布后不销毁或替换。
 */
static bool snapshot_lock(void)
{
    SemaphoreHandle_t mutex = __atomic_load_n(&s_mutex, __ATOMIC_ACQUIRE);
    return mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

/**
 * @brief 释放本任务已经取得的快照锁。
 * @post 本次临界区结束，锁对象继续由进程期快照子系统持有。
 * @pre 本任务成功执行 snapshot_lock，锁自发布后不再销毁。
 */
static void snapshot_unlock(void)
{
    xSemaphoreGive(__atomic_load_n(&s_mutex, __ATOMIC_RELAXED));
}

/**
 * @brief 释放一个显示环的全部行并复位索引。
 * @param[in,out] ring 本模块独占环，可 NULL，行必须属于 heap_caps 分配器。
 * @post 所有行槽和计数归零，NULL 输入无副作用。
 * @pre 调用任务持有快照锁。
 */
static void ring_clear_locked(board_ui_snapshot_ring_t *ring)
{
    if (!ring) {
        return;
    }
    for (size_t i = 0; i < UI_SNAPSHOT_MAX_LINES; i++) {
        if (ring->lines[i]) {
            heap_caps_free(ring->lines[i]);
            ring->lines[i] = NULL;
        }
    }
    ring->count = 0;
    ring->start = 0;
}

/**
 * @brief 先复制新行再锁内提交，满 80 行时替换最早一行。
 * @param[in,out] ring 本模块环，NULL 忽略，环及锁均为进程期对象。
 * @param[in] line 调用期 NUL 文本借用，NULL 忽略，复制按单行预算截取。
 * @post 成功安装新副本，未初始化、OOM 或取锁失败保持原环，临时副本全部回收。
 */
static void ring_append(board_ui_snapshot_ring_t *ring, const char *line)
{
    if (!ring || !line) {
        return;
    }
    char *copy = snapshot_strdup(line);
    if (!copy) {
        return;
    }

    if (!snapshot_lock()) {
        heap_caps_free(copy);
        return;
    }
    size_t index = 0;
    if (ring->count < UI_SNAPSHOT_MAX_LINES) {
        index = (ring->start + ring->count) % UI_SNAPSHOT_MAX_LINES;
        ring->count++;
    } else {
        index = ring->start;
        if (ring->lines[index]) {
            heap_caps_free(ring->lines[index]);
        }
        ring->start = (ring->start + 1) % UI_SNAPSHOT_MAX_LINES;
    }
    ring->lines[index] = copy;
    snapshot_unlock();
}

/**
 * @brief 先复制流式完整文本再替换末行，空环安装第一行。
 * @param[in,out] ring 本模块环，可 NULL。
 * @param[in] line 调用期文本借用，NULL 忽略，按单行预算复制。
 * @post 提交前失败保持原环，成功后归还原末行，调用结束不保留输入借用。
 */
static void ring_replace_last(board_ui_snapshot_ring_t *ring, const char *line)
{
    if (!ring || !line) {
        return;
    }
    char *copy = snapshot_strdup(line);
    if (!copy) {
        return;
    }

    if (!snapshot_lock()) {
        heap_caps_free(copy);
        return;
    }
    if (ring->count == 0) {
        ring->lines[0] = copy;
        ring->count = 1;
        ring->start = 0;
        snapshot_unlock();
        return;
    }

    size_t index = (ring->start + ring->count - 1) % UI_SNAPSHOT_MAX_LINES;
    if (ring->lines[index]) {
        heap_caps_free(ring->lines[index]);
    }
    ring->lines[index] = copy;
    snapshot_unlock();
}

/**
 * @brief 消费构造器单次操作的拥有诊断，sticky 错误仍由构造器保留。
 * @param[in] result 按值接收完整结果所有权，包括成功时的动态消息。
 * @post 诊断消息已归还，调用者继续通过 take 判断整次构造是否成功。
 */
static void snapshot_consume_result(cc_result_t result)
{
    cc_result_free(&result);
}

/**
 * @brief 写入带双引号的 JSON 字符串并转义控制字节、引号和反斜杠。
 * @param[in,out] sb 本次调用独占且已初始化的构造器，失败记录为 sticky 错误。
 * @param[in] text 调用期稳定 NUL 文本借用，NULL 作为空串，非控制字节原样复制。
 * @post 诊断均已消费，字符串完整性由最终 take 的结果判断。
 */
static void json_escape_append(cc_string_builder_t *sb, const char *text)
{
    snapshot_consume_result(cc_string_builder_append_char(sb, '"'));
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '\\': snapshot_consume_result(cc_string_builder_append(sb, "\\\\")); break;
        case '"': snapshot_consume_result(cc_string_builder_append(sb, "\\\"")); break;
        case '\n': snapshot_consume_result(cc_string_builder_append(sb, "\\n")); break;
        case '\r': snapshot_consume_result(cc_string_builder_append(sb, "\\r")); break;
        case '\t': snapshot_consume_result(cc_string_builder_append(sb, "\\t")); break;
        default:
            if (*p < 0x20) {
                snapshot_consume_result(cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p));
            } else {
                snapshot_consume_result(cc_string_builder_append_char(sb, (char)*p));
            }
            break;
        }
    }
    snapshot_consume_result(cc_string_builder_append_char(sb, '"'));
}

/**
 * @brief 按最早到最新顺序将环编码成 JSON 字符串数组。
 * @param[in,out] sb 独占构造器，分配错误保持 sticky 状态供最终 take 检查。
 * @param[in] ring 稳定环借用，NULL 编码为空数组。
 * @post 操作结果诊断已消费，不转移任何行的所有权。
 * @pre 调用者持快照锁覆盖完整遍历和字符串复制。
 */
static void ring_json_append(cc_string_builder_t *sb, board_ui_snapshot_ring_t *ring)
{
    snapshot_consume_result(cc_string_builder_append_char(sb, '['));
    if (ring) {
        for (size_t i = 0; i < ring->count; i++) {
            size_t index = (ring->start + i) % UI_SNAPSHOT_MAX_LINES;
            if (i > 0) {
                snapshot_consume_result(cc_string_builder_append_char(sb, ','));
            }
            json_escape_append(sb, ring->lines[index] ? ring->lines[index] : "");
        }
    }
    snapshot_consume_result(cc_string_builder_append_char(sb, ']'));
}

/**
 * @brief 创建并发布进程期快照互斥锁，已初始化时直接复用。
 * @return 锁已可用为 true，分配失败为 false，失败后允许 owner 重试。
 * @pre 单一初始化 owner 串行调用，其他任务可调用快照接口，尚未发布锁时安全跳过或返回 NULL。
 */
bool board_ui_snapshot_init(void)
{
    if (__atomic_load_n(&s_mutex, __ATOMIC_ACQUIRE)) return true;
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!mutex) return false;
    __atomic_store_n(&s_mutex, mutex, __ATOMIC_RELEASE);
    return true;
}

/**
 * @brief 释放对话显示快照的全部行。
 * @post 已初始化时锁内清空对话环，取锁失败或尚未初始化时跳过。
 */
void board_ui_snapshot_clear_conversation(void)
{
    if (!snapshot_lock()) return;
    ring_clear_locked(&s_conversation);
    snapshot_unlock();
}

/**
 * @brief 释放启动日志快照的全部行。
 * @post 已初始化时锁内清空启动环，否则安全跳过。
 */
void board_ui_snapshot_clear_boot(void)
{
    if (!snapshot_lock()) return;
    ring_clear_locked(&s_boot);
    snapshot_unlock();
}

/**
 * @brief 释放工具输出快照的全部行。
 * @post 已初始化时锁内清空工具环，否则安全跳过。
 */
void board_ui_snapshot_clear_tool(void)
{
    if (!snapshot_lock()) return;
    ring_clear_locked(&s_tool);
    snapshot_unlock();
}

/**
 * @brief 释放绘制命令快照的全部行。
 * @post 已初始化时锁内清空绘制环，否则安全跳过。
 */
void board_ui_snapshot_clear_draw(void)
{
    if (!snapshot_lock()) return;
    ring_clear_locked(&s_draw);
    snapshot_unlock();
}

/**
 * @brief 锁内复制状态栏文本，最多保留 1023 字节。
 * @param[in] text 调用期 NUL 文本借用，NULL 作为空串。
 * @post 初始化完成且取锁成功时更新状态并 NUL 终止，否则保持原值。
 */
void board_ui_snapshot_set_status(const char *text)
{
    if (!snapshot_lock()) return;
    snprintf(s_status, sizeof(s_status), "%s", text ? text : "");
    snapshot_unlock();
}

/**
 * @brief 复制并追加一行启动日志显示文本。
 * @param[in] line 调用期文本借用，NULL 忽略，最多保留 512 字节。
 * @post 成功更新最多 80 行的启动环，分配或同步失败保持原内容。
 */
void board_ui_snapshot_append_boot(const char *line)
{
    ring_append(&s_boot, line);
}

/**
 * @brief 复制并追加一行对话显示文本。
 * @param[in] line 调用期文本借用，NULL 忽略，最多保留 512 字节。
 * @post 成功更新最多 80 行的对话环，分配或同步失败保持原内容。
 */
void board_ui_snapshot_append_conversation(const char *line)
{
    ring_append(&s_conversation, line);
}

/**
 * @brief 用流式合并后的完整文本替换对话末行，空环时安装首行。
 * @param[in] line 调用期文本借用，NULL 忽略，最多保留 512 字节。
 * @post 新行复制成功才替换旧行，失败保持已显示的快照。
 */
void board_ui_snapshot_replace_last_conversation(const char *line)
{
    ring_replace_last(&s_conversation, line);
}

/**
 * @brief 复制并追加一行工具输出快照。
 * @param[in] line 调用期文本借用，NULL 忽略，最多保留 512 字节。
 * @post 成功更新最多 80 行的工具环，分配或同步失败保持原内容。
 */
void board_ui_snapshot_append_tool(const char *line)
{
    ring_append(&s_tool, line);
}

/**
 * @brief 将一条绘制诊断格式化到栈缓冲后复制进绘制环。
 * @param[in] fmt 调用期 printf 格式借用，NULL 忽略。
 * @param[in] ... 与格式匹配的按值参数及调用期字符串借用。
 * @post 格式化失败不追加，成功最多保留 512 字节并遵循环的分配失败规则。
 */
void board_ui_snapshot_append_drawf(const char *fmt, ...)
{
    if (!fmt) return;
    /* 多保留三个字节供行预算截点识别 UTF-8 续字节，最终副本仍受 512 字节限制。 */
    char line[UI_SNAPSHOT_LINE_MAX + 4];
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (written >= 0) ring_append(&s_draw, line);
}

/**
 * @brief 在一个锁定区间编码四个显示环及状态栏，交付独立 JSON 副本。
 * @param[in] ready 调用者按值提供的 UI 就绪状态，快照锁不改变该标志。
 * @return 拥有的完整 NUL JSON，调用者 free，未初始化、同步或任一构造分配失败为 NULL。
 * @note 锁覆盖整次编码，环中行不会在借用期间被替换或释放，返回后与 UI 内容独立。
 */
char *board_ui_screen_snapshot_json(bool ready)
{
    cc_string_builder_t sb;
    cc_result_t result = cc_string_builder_init(&sb);
    bool initialized = result.code == CC_OK;
    cc_result_free(&result);
    if (!initialized) {
        return NULL;
    }

    if (!snapshot_lock()) {
        cc_string_builder_deinit(&sb);
        return NULL;
    }
    snapshot_consume_result(cc_string_builder_append(&sb, "{\"ready\":"));
    snapshot_consume_result(cc_string_builder_append(&sb, ready ? "true" : "false"));
    snapshot_consume_result(cc_string_builder_append(&sb, ",\"status\":"));
    json_escape_append(&sb, s_status);
    snapshot_consume_result(cc_string_builder_append(&sb, ",\"boot\":"));
    ring_json_append(&sb, &s_boot);
    snapshot_consume_result(cc_string_builder_append(&sb, ",\"conversation\":"));
    ring_json_append(&sb, &s_conversation);
    snapshot_consume_result(cc_string_builder_append(&sb, ",\"tool\":"));
    ring_json_append(&sb, &s_tool);
    snapshot_consume_result(cc_string_builder_append(&sb, ",\"draw\":"));
    ring_json_append(&sb, &s_draw);
    snapshot_consume_result(cc_string_builder_append_char(&sb, '}'));
    snapshot_unlock();

    return cc_string_builder_take(&sb);
}
