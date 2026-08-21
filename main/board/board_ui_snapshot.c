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
    char *lines[UI_SNAPSHOT_MAX_LINES];
    size_t count;
    size_t start;
} board_ui_snapshot_ring_t;

static board_ui_snapshot_ring_t s_conversation;
static board_ui_snapshot_ring_t s_tool;
static board_ui_snapshot_ring_t s_boot;
static board_ui_snapshot_ring_t s_draw;
static SemaphoreHandle_t s_mutex;
static char s_status[1024];

/*
 * 安全复制字符串到 PSRAM，最大长度为 UI_SNAPSHOT_LINE_MAX。
 * 参数: text - 源字符串，可为 NULL
 * 返回: 新分配字符串的指针，分配失败返回 NULL
 */
static char *snapshot_strdup(const char *text)
{
    size_t len = text ? strlen(text) : 0;
    if (len > UI_SNAPSHOT_LINE_MAX) {
        len = UI_SNAPSHOT_LINE_MAX;
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

/*
 * 获取快照互斥锁（阻塞等待）。
 */
static void snapshot_lock(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

/*
 * 释放快照互斥锁。
 */
static void snapshot_unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

/*
 * 释放环形缓冲区中所有行并重置计数器和起始位置（调用方需持有锁）。
 * 参数: ring - 指向环形缓冲区的指针
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

/*
 * 将一行文本追加到环形缓冲区末尾，满时覆盖最旧行。
 * 参数: ring - 环形缓冲区指针
 *       line - 要追加的文本，NULL 则忽略
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

    snapshot_lock();
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

/*
 * 替换环形缓冲区中最后一行文本（用于流式更新合并）。
 * 参数: ring - 环形缓冲区指针
 *       line - 替换后的完整文本，NULL 则忽略
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

    snapshot_lock();
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

/*
 * 将字符串追加到字符串构建器，并对控制字符和引号做 JSON 转义（两端加引号）。
 * 参数: sb   - 字符串构建器
 *       text - 原始文本
 */
static void json_escape_append(cc_string_builder_t *sb, const char *text)
{
    cc_string_builder_append_char(sb, '"');
    for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; p++) {
        switch (*p) {
        case '\\': cc_string_builder_append(sb, "\\\\"); break;
        case '"': cc_string_builder_append(sb, "\\\""); break;
        case '\n': cc_string_builder_append(sb, "\\n"); break;
        case '\r': cc_string_builder_append(sb, "\\r"); break;
        case '\t': cc_string_builder_append(sb, "\\t"); break;
        default:
            if (*p < 0x20) {
                cc_string_builder_appendf(sb, "\\u%04x", (unsigned)*p);
            } else {
                cc_string_builder_append_char(sb, (char)*p);
            }
            break;
        }
    }
    cc_string_builder_append_char(sb, '"');
}

/*
 * 将环形缓冲区内容追加到 JSON 字符串构建器，格式为 JSON 字符串数组。
 * 参数: sb   - 字符串构建器
 *       ring - 环形缓冲区指针
 */
static void ring_json_append(cc_string_builder_t *sb, board_ui_snapshot_ring_t *ring)
{
    cc_string_builder_append_char(sb, '[');
    if (ring) {
        for (size_t i = 0; i < ring->count; i++) {
            size_t index = (ring->start + i) % UI_SNAPSHOT_MAX_LINES;
            if (i > 0) {
                cc_string_builder_append_char(sb, ',');
            }
            json_escape_append(sb, ring->lines[index] ? ring->lines[index] : "");
        }
    }
    cc_string_builder_append_char(sb, ']');
}

/*
 * 初始化快照子系统，创建互斥锁。
 * 返回: true 成功，false 内存不足
 */
bool board_ui_snapshot_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    return s_mutex != NULL;
}

/*
 * 清空 conversation 环形缓冲区。
 */
void board_ui_snapshot_clear_conversation(void)
{
    snapshot_lock();
    ring_clear_locked(&s_conversation);
    snapshot_unlock();
}

/*
 * 清空 tool 环形缓冲区。
 */
void board_ui_snapshot_clear_tool(void)
{
    snapshot_lock();
    ring_clear_locked(&s_tool);
    snapshot_unlock();
}

/*
 * 清空 draw 环形缓冲区。
 */
void board_ui_snapshot_clear_draw(void)
{
    snapshot_lock();
    ring_clear_locked(&s_draw);
    snapshot_unlock();
}

/*
 * 更新状态栏文本快照（线程安全）。
 * 参数: text - 状态栏文本内容，NULL 则为空字符串
 */
void board_ui_snapshot_set_status(const char *text)
{
    snapshot_lock();
    snprintf(s_status, sizeof(s_status), "%s", text ? text : "");
    snapshot_unlock();
}

/*
 * 向 boot 环形缓冲区追加一行引导日志。
 * 参数: line - 日志行内容
 */
void board_ui_snapshot_append_boot(const char *line)
{
    ring_append(&s_boot, line);
}

/*
 * 向 conversation 环形缓冲区追加一行消息。
 * 参数: line - 消息内容
 */
void board_ui_snapshot_append_conversation(const char *line)
{
    ring_append(&s_conversation, line);
}

/*
 * 替换 conversation 环形缓冲区最后一行（用于流式更新合并）。
 * 参数: line - 替换后的完整文本
 */
void board_ui_snapshot_replace_last_conversation(const char *line)
{
    ring_replace_last(&s_conversation, line);
}

/*
 * 向 tool 环形缓冲区追加一行工具输出。
 * 参数: line - 工具输出内容
 */
void board_ui_snapshot_append_tool(const char *line)
{
    ring_append(&s_tool, line);
}

/*
 * 以 printf 格式向 draw 环形缓冲区追加绘制日志。
 * 参数: fmt - printf 格式串
 *       ... - 可变参数
 */
void board_ui_snapshot_append_drawf(const char *fmt, ...)
{
    char line[UI_SNAPSHOT_LINE_MAX + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    ring_append(&s_draw, line);
}

/*
 * 生成包含所有环形缓冲区内容和状态栏的 JSON 快照，调用者负责释放返回字符串。
 * 参数: ready - UI 就绪标志
 * 返回: JSON 字符串（需调用 free 释放），分配失败返回 NULL
 */
char *board_ui_screen_snapshot_json(bool ready)
{
    cc_string_builder_t sb;
    if (cc_string_builder_init(&sb).code != CC_OK) {
        return NULL;
    }

    snapshot_lock();
    cc_string_builder_append(&sb, "{\"ready\":");
    cc_string_builder_append(&sb, ready ? "true" : "false");
    cc_string_builder_append(&sb, ",\"status\":");
    json_escape_append(&sb, s_status);
    cc_string_builder_append(&sb, ",\"boot\":");
    ring_json_append(&sb, &s_boot);
    cc_string_builder_append(&sb, ",\"conversation\":");
    ring_json_append(&sb, &s_conversation);
    cc_string_builder_append(&sb, ",\"tool\":");
    ring_json_append(&sb, &s_tool);
    cc_string_builder_append(&sb, ",\"draw\":");
    ring_json_append(&sb, &s_draw);
    cc_string_builder_append_char(&sb, '}');
    snapshot_unlock();

    return cc_string_builder_take(&sb);
}
