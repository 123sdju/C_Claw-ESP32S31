"""编译当前 UI 生产实现，核对路径事务、显示副本和候选资源回收。

模型使用真实 cJSON、SDK JSON/result/builder 实现，原生 LVGL、BSP 和
FreeRTOS 接口由逐项断言的宿主替身表示。SAN 模式沿统一环境变量启用。
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def production_function(relative_path, signature):
    """读取指定生产函数的完整原文，保留实际分支和资源操作。"""
    source = (ROOT / relative_path).read_text(encoding="utf-8")
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


def run_fixture(code, name, headers=None, sources=()):
    """在独立目录编译模型及指定生产源，失败向 unittest 传播。"""
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if os.name == "nt" or not compiler:
        raise RuntimeError("显示资源夹具需要 Linux/WSL 的本机 C 编译器")
    with tempfile.TemporaryDirectory(prefix="cclaw-display-") as folder:
        directory = Path(folder)
        for path, content in (headers or {}).items():
            target = directory / "include" / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content, encoding="utf-8")
        source = directory / "fixture.c"
        source.write_text(code, encoding="utf-8")
        command = [compiler, "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                   "-fno-builtin-malloc", "-fno-builtin-realloc", "-fno-builtin-free", "-pthread",
                   "-I", str(directory / "include"), "-I", str(ROOT / "main/ui"),
                   "-I", str(ROOT / "third_party/cclaw-sdk/cclaw/core/include"), "-I", str(ROOT / "third_party/cclaw-sdk/cclaw/ports/include"),
                   "-I", str(ROOT / "third_party/cclaw-sdk/cclaw/core/vendor/cjson"),
                   "-Wl,--wrap=malloc", "-Wl,--wrap=realloc", "-Wl,--wrap=free",
                   "-Wl,--wrap=vsnprintf", "-o", str(directory / name), str(source)]
        command += [str(ROOT / path) for path in sources]
        command += ["-lm"]
        if os.environ.get("CCLAW_TEST_SANITIZERS") == "1":
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(directory / name)], check=True, timeout=30)


COMMON = r'''
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cc/util/cc_json.h"
#include "cJSON.h"
void *__real_malloc(size_t);
void *__real_realloc(void *, size_t);
void __real_free(void *);
int __real_vsnprintf(char *, size_t, const char *, va_list);
typedef union {
    max_align_t alignment;
    struct { size_t size; } allocation;
} allocation_header_t;
static size_t allocation_calls, fail_at, fail_until, live_blocks, live_bytes, peak_bytes;
static bool fail_format;

/**
 * @brief 为故障模型分配带字节统计的独占块。
 * @param[in] size 请求字节数。
 * @return 拥有块，指定调用点或原生分配失败返回 NULL。
 */
void *__wrap_malloc(size_t size)
{
    allocation_calls++;
    if ((fail_at && allocation_calls >= fail_at && allocation_calls <= (fail_until ? fail_until : fail_at)) ||
        size > SIZE_MAX - sizeof(allocation_header_t)) return NULL;
    allocation_header_t *header = __real_malloc(sizeof(*header) + size);
    if (!header) return NULL;
    header->allocation.size = size;
    live_blocks++;
    live_bytes += size;
    if (live_bytes > peak_bytes) peak_bytes = live_bytes;
    return header + 1;
}

/**
 * @brief 归还本夹具分配的拥有块。
 * @param[in] pointer 可 NULL 的拥有指针。
 * @post 非空块及统计归还，重复释放由断言和 sanitizer 检出。
 */
void __wrap_free(void *pointer)
{
    if (!pointer) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(live_blocks && live_bytes >= header->allocation.size);
    live_blocks--;
    live_bytes -= header->allocation.size;
    __real_free(header);
}

/**
 * @brief 强制成功 realloc 搬迁，验证失败保留和源别名处理。
 * @param[in] pointer 旧拥有块，可 NULL。
 * @param[in] size 新字节容量，零时归还旧块。
 * @return 新拥有块或 NULL，失败保留旧块。
 */
void *__wrap_realloc(void *pointer, size_t size)
{
    if (!size) { __wrap_free(pointer); return NULL; }
    size_t previous = pointer ? ((allocation_header_t *)pointer - 1)->allocation.size : 0;
    void *candidate = __wrap_malloc(size);
    if (!candidate) return NULL;
    if (pointer) memcpy(candidate, pointer, previous < size ? previous : size);
    __wrap_free(pointer);
    return candidate;
}

/**
 * @brief 按开关模拟负格式化返回，不写输出缓冲。
 * @param[out] out 原生输出区。
 * @param[in] capacity 字节容量。
 * @param[in] format 调用期格式借用。
 * @param[in] args 原生参数列表借用。
 * @return 故障时负值，否则透传真实格式化结果。
 */
int __wrap_vsnprintf(char *out, size_t capacity, const char *format, va_list args)
{
    return fail_format ? -1 : __real_vsnprintf(out, capacity, format, args);
}

/**
 * @brief 完整解析并消费结果，供夹具建立稳定拥有树。
 * @param[in] text 调用期合法完整 JSON 借用。
 * @return 拥有树，断言解析成功，调用者 destroy。
 */
static cc_json_value_t *parse_json(const char *text)
{
    cc_json_value_t *tree = NULL;
    cc_result_t result = cc_json_parse_strict(text, &tree);
    assert(result.code == CC_OK && tree);
    cc_result_free(&result);
    return tree;
}
'''

JSON_TEST = r'''
#include "board_ui_json.h"

/** @brief 逐个失败点验证完整路径安装原子性及合法长路径。 @return 零表示全部断言通过。 */
int main(void)
{
    char component[260], path[600];
    memset(component, 'k', sizeof(component) - 1);
    component[sizeof(component) - 1] = 0;
    snprintf(path, sizeof(path), "long.%s.value", component);
    cc_json_value_t *root = parse_json("{\"stable\":\"old\",\"replace\":7}");
    assert(board_ui_json_set_path_string(root, path, "long-value"));
    assert(strcmp(board_ui_json_get_path_string(root, path), "long-value") == 0);
    assert(board_ui_json_get_path(root, "...") == root);
    assert(!board_ui_json_get_path(root, NULL));
    assert(!board_ui_json_set_path_string(root, "...", "bad"));
    assert(!board_ui_json_set_path_number(root, NULL, 1));
    assert(board_ui_json_set_path_string(root, "copied", board_ui_json_get_path_string(root, "stable")));
    cc_json_destroy(root);
    assert(!live_blocks);

    size_t calls = 0;
    for (size_t failure = 0; failure <= calls || !failure; failure++) {
        root = parse_json("{\"stable\":\"old\",\"replace\":7}");
        char *before = cc_json_stringify(root);
        assert(before);
        allocation_calls = 0;
        fail_at = failure;
        bool installed = board_ui_json_set_path_string(root, "replace.deep.final", "candidate");
        size_t used = allocation_calls;
        fail_at = 0;
        if (!failure) { calls = used; assert(installed && calls); }
        else {
            assert(!installed);
            char *after = cc_json_stringify(root);
            assert(after && strcmp(before, after) == 0);
            free(after);
        }
        free(before);
        cc_json_destroy(root);
        assert(!live_blocks && !live_bytes);
    }
    root = parse_json("{\"Model\":{\"name\":\"kept\"},\"flag\":true}");
    allocation_calls = 0;
    fail_at = 1;
    assert(strcmp(board_ui_json_get_path_string(root, "..model..name."), "kept") == 0);
    assert(board_ui_json_get_path_bool(root, "flag", false));
    assert(allocation_calls == 0);
    fail_at = 0;
    cc_json_destroy(root);
    assert(!live_blocks && !live_bytes);
    return 0;
}
'''

SNAPSHOT_HEADERS = {
    "esp_heap_caps.h": r'''
#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_malloc(size_t bytes, unsigned caps);
void heap_caps_free(void *pointer);
''',
    "freertos/FreeRTOS.h": r'''
#pragma once
#include <stdint.h>
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
''',
    "freertos/semphr.h": r'''
#pragma once
#include <pthread.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned wait);
int xSemaphoreGive(SemaphoreHandle_t mutex);
''',
}

SNAPSHOT_TEST = r'''
#include "freertos/semphr.h"
/** @brief 模拟能力堆，仍使用同一分配统计。 @param[in] bytes 字节数。 @param[in] caps 能力位。 @return 拥有块或 NULL。 */
void *heap_caps_malloc(size_t bytes, unsigned caps) { (void)caps; return malloc(bytes); }
/** @brief 归还能力堆拥有块。 @param[in] pointer 可 NULL 块。 @post 统计归还。 */
void heap_caps_free(void *pointer) { free(pointer); }
/** @brief 创建宿主互斥锁。 @return 拥有锁或 NULL，失败允许重试。 */
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t mutex = malloc(sizeof(*mutex));
    if (!mutex) return NULL;
    assert(!pthread_mutex_init(mutex, NULL));
    return mutex;
}
/** @brief 取得宿主锁。 @param[in,out] mutex 有效锁。 @param[in] wait 预算标记。 @return 模拟 pdTRUE。 */
int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned wait)
{ (void)wait; assert(!pthread_mutex_lock(mutex)); return 1; }
/** @brief 归还宿主锁。 @param[in,out] mutex 当前任务持有的锁。 @return 模拟 pdTRUE。 */
int xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(!pthread_mutex_unlock(mutex)); return 1; }
@SNAPSHOT_SOURCE@

/** @brief 核对未发布、环形裁剪、UTF-8 截点及逐次构造 OOM。 @return 零表示全部通过。 */
int main(void)
{
    board_ui_snapshot_append_conversation("unpublished");
    assert(!s_conversation.count && !live_blocks);
    assert(!board_ui_screen_snapshot_json(false));
    allocation_calls = 0; fail_at = 1;
    assert(!board_ui_snapshot_init());
    fail_at = 0;
    assert(board_ui_snapshot_init());
    for (unsigned i = 0; i < 90; i++) {
        char line[32]; snprintf(line, sizeof(line), "line-%u", i);
        board_ui_snapshot_append_conversation(line);
    }
    assert(s_conversation.count == 80);
    assert(strcmp(s_conversation.lines[s_conversation.start], "line-10") == 0);
    char utf8[514]; memset(utf8, 'a', 510);
    memcpy(utf8 + 510, "\xe4\xb8\xad", 4);
    board_ui_snapshot_replace_last_conversation(utf8);
    size_t last = (s_conversation.start + s_conversation.count - 1) % 80;
    assert(strlen(s_conversation.lines[last]) == 510);
    fail_format = true;
    board_ui_snapshot_append_drawf("never %d", 1);
    fail_format = false;
    assert(!s_draw.count);
    board_ui_snapshot_append_drawf("%s", utf8);
    assert(s_draw.count == 1 && strlen(s_draw.lines[s_draw.start]) == 510);
    board_ui_snapshot_append_boot("quote\" slash\\ newline\n");
    board_ui_snapshot_set_status("ready");
    size_t baseline_blocks = live_blocks, baseline_bytes = live_bytes, calls = 0;
    for (size_t failure = 0; failure <= calls || !failure; failure++) {
        allocation_calls = 0; fail_at = failure;
        char *snapshot = board_ui_screen_snapshot_json(true);
        size_t used = allocation_calls;
        fail_at = 0;
        if (!failure) { calls = used; assert(snapshot && calls); }
        else assert(!snapshot);
        if (snapshot) {
            cc_json_value_t *tree = parse_json(snapshot);
            assert(cc_json_array_size(cc_json_object_get(tree, "conversation")) == 80);
            cc_json_destroy(tree);
            free(snapshot);
        }
        assert(live_blocks == baseline_blocks && live_bytes == baseline_bytes);
    }
    board_ui_snapshot_clear_conversation(); board_ui_snapshot_clear_boot();
    board_ui_snapshot_clear_tool(); board_ui_snapshot_clear_draw();
    assert(!pthread_mutex_destroy(s_mutex)); free(s_mutex); s_mutex = NULL;
    assert(!live_blocks && !live_bytes);
    return 0;
}
'''

TEXT_PREFIX = r'''
#include "board_ui.h"
typedef uint32_t TickType_t;
typedef struct lv_obj_t lv_obj_t;
typedef struct { lv_obj_t *target; } lv_event_t;
typedef void (*delete_callback_t)(lv_event_t *);
struct lv_obj_t {
    char *text;
    void *user_data;
    bool static_text;
    void *event_storage;
    lv_obj_t *parent, *first, *next;
    delete_callback_t on_delete;
};
#define UI_CONV_MAX_BYTES (512U * 1024U)
#define UI_CONV_MAX_MESSAGES 160U
#define UI_STREAM_SCROLL_INTERVAL_MS 50U
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define LV_EVENT_DELETE 1
#define LV_SIZE_CONTENT 0
#define pdMS_TO_TICKS(ms) (ms)
static lv_obj_t container;
static lv_obj_t *s_conversation = &container, *s_last_msg_label, *s_stream_msg_label;
static board_conversation_role_t s_last_role, s_stream_role;
static bool s_ready = true, s_stream_active, s_stream_text_dirty, s_conversation_layout_pending;
static size_t s_conversation_text_bytes, s_stream_text_len, s_stream_text_capacity;
static char *s_stream_text;
static TickType_t s_last_stream_scroll_tick, now_tick = 100;
static unsigned ui_depth, snapshot_appends, snapshot_replaces;

/** @brief 进入模型的递归 UI 临界区。 @post 深度增加，无异步重入。 */
static void ui_lock(void) { ui_depth++; }
/** @brief 退出模型 UI 临界区。 @post 核对并归还一层深度。 */
static void ui_unlock(void) { assert(ui_depth); ui_depth--; }
/** @brief 返回模型 tick。 @return 当前受测试控制的 tick 值。 */
static TickType_t xTaskGetTickCount(void) { return now_tick; }
/** @brief 模拟能力分配。 @param[in] bytes 字节数。 @param[in] caps 能力位。 @return 拥有块或 NULL。 */
static void *heap_caps_malloc(size_t bytes, unsigned caps) { (void)caps; return malloc(bytes); }
/** @brief 模拟能力扩容。 @param[in] pointer 原拥有块。 @param[in] bytes 新容量。 @param[in] caps 能力位。 @return 新块或失败保留原块。 */
static void *heap_caps_realloc(void *pointer, size_t bytes, unsigned caps)
{ (void)caps; return realloc(pointer, bytes); }
/** @brief 模拟能力归还。 @param[in] pointer 可空拥有块。 @post 块及统计归还。 */
static void heap_caps_free(void *pointer) { free(pointer); }
/** @brief 模拟仅创建标签外壳和原生默认动态文本。 @param[in,out] parent 容器。 @return 容器拥有的新标签或 NULL。 */
static lv_obj_t *lv_label_create(lv_obj_t *parent)
{
    assert(ui_depth);
    lv_obj_t *label = malloc(sizeof(*label));
    if (!label) return NULL;
    memset(label, 0, sizeof(*label));
    label->text = malloc(8);
    if (label->text) memcpy(label->text, "default", 8);
    label->parent = parent;
    lv_obj_t **slot = &parent->first;
    while (*slot) slot = &(*slot)->next;
    *slot = label;
    return label;
}
/** @brief 原生 static 安装模型。 @param[in,out] label 标签。 @param[in] text 持续借用文本。 @post 原动态默认文本释放，候选指针只借用。 */
static void lv_label_set_text_static(lv_obj_t *label, const char *text)
{
    if (!label->static_text) free(label->text);
    label->text = (char *)text;
    label->static_text = true;
}
/** @brief 取得原生标签文本。 @param[in] label 稳定标签。 @return 文本借用。 */
static const char *lv_label_get_text(lv_obj_t *label) { return label->text; }
/** @brief 设置通用借用槽。 @param[in,out] label 标签。 @param[in] data 新槽值。 @post 槽已更新。 */
static void lv_obj_set_user_data(lv_obj_t *label, void *data) { label->user_data = data; }
/** @brief 读取通用槽。 @param[in] label 标签。 @return 槽值借用。 */
static void *lv_obj_get_user_data(lv_obj_t *label) { return label->user_data; }
/** @brief 读取删除事件目标。 @param[in] event 同步事件。 @return 标签借用。 */
static lv_obj_t *lv_event_get_target(lv_event_t *event) { return event->target; }
/** @brief 登记模型删除回调。 @param[in,out] label 标签。 @param[in] callback 同步回调。 @param[in] code DELETE。 @param[in] data 未使用上下文。 @return 非空表示成功。 */
static void *lv_obj_add_event_cb(lv_obj_t *label, delete_callback_t callback, int code, void *data)
{
    (void)data;
    assert(!label->event_storage);
    label->event_storage = malloc(sizeof(callback));
    if (!label->event_storage) return NULL;
    if (code == LV_EVENT_DELETE) label->on_delete = callback;
    return label->event_storage;
}
/** @brief 先同步发 DELETE 再销毁标签，核对 native static 文本规则。 @param[in] label 拥有标签。 @post 从父容器移除且所有原生拥有成员释放。 */
static void lv_obj_delete(lv_obj_t *label)
{
    assert(ui_depth);
    lv_obj_t **slot = &label->parent->first;
    while (*slot && *slot != label) slot = &(*slot)->next;
    assert(*slot == label); *slot = label->next;
    if (label->on_delete) { lv_event_t event = {label}; label->on_delete(&event); }
    while (label->first) lv_obj_delete(label->first);
    if (!label->static_text) free(label->text);
    free(label->event_storage);
    free(label);
}
/** @brief 计数容器孩子。 @param[in] parent 稳定容器。 @return 孩子数。 */
static uint32_t lv_obj_get_child_count(lv_obj_t *parent)
{ uint32_t count = 0; for (lv_obj_t *child = parent->first; child; child = child->next) count++; return count; }
/** @brief 按索引借用孩子。 @param[in] parent 容器。 @param[in] index 非负索引。 @return 孩子或 NULL。 */
static lv_obj_t *lv_obj_get_child(lv_obj_t *parent, uint32_t index)
{ lv_obj_t *child = parent->first; while (child && index--) child = child->next; return child; }
/** @brief 忽略视觉样式，核对标签已有效。 @param[in] label 标签借用。 @param[in] role 角色。 @post 仅断言有效。 */
static void apply_message_label_style(lv_obj_t *label, board_conversation_role_t role)
{ (void)role; assert(label); }
/** @brief 模拟内容高度设置。 @param[in] label 标签。 @param[in] height 高度标记。 @post 标签保持有效。 */
static void lv_obj_set_height(lv_obj_t *label, int height) { (void)height; assert(label); }
/** @brief 记录快照追加时点。 @param[in] text 调用期文本。 @post 追加次数增加。 */
void board_ui_snapshot_append_conversation(const char *text) { assert(text); snapshot_appends++; }
/** @brief 记录快照替换时点。 @param[in] text 调用期文本。 @post 替换次数增加。 */
void board_ui_snapshot_replace_last_conversation(const char *text) { assert(text); snapshot_replaces++; }
@TEXT_FUNCTIONS@

typedef enum {
    APP_EVENT_KIND_USER, APP_EVENT_KIND_ASSISTANT, APP_EVENT_KIND_THINKING,
    APP_EVENT_KIND_TOOL, APP_EVENT_KIND_ERROR, APP_EVENT_KIND_STATUS,
    APP_EVENT_KIND_RUN_STATE, APP_EVENT_KIND_LOG
} app_event_kind_t;
typedef struct { char *text; size_t len; unsigned refs; } app_event_envelope_t;
typedef struct { app_event_kind_t kind; app_event_envelope_t *envelope; } app_event_view_record_t;
typedef struct { size_t count; app_event_view_record_t items[1]; } app_event_display_view_t;
typedef struct {
    app_event_kind_t kind;
    app_event_envelope_t *envelope;
    bool clear, boot;
    app_event_display_view_t *view;
} app_event_display_msg_t;
#define APP_EVENT_TEXT_MAX 2048U
#define APP_EVENT_DISPLAY_PUMP_MAX 16U
#define pdTRUE 1
static app_event_display_msg_t messages[16];
static unsigned read_index, write_index, bus_depth;
static struct { void *display_queue; unsigned display_pending; } g_app_event_bus;
/** @brief 模拟总线锁。 @post 深度增加。 */
static void app_event_bus_lock(void) { bus_depth++; }
/** @brief 归还总线锁。 @post 核对并减少深度。 */
static void app_event_bus_unlock(void) { assert(bus_depth); bus_depth--; }
/** @brief 核对批末排空。 @post 不改变状态，lease 模型外层保持有效。 */
static void app_event_bus_notify_idle_if_drained(void) { assert(!bus_depth); }
/** @brief 非阻塞取一条模型消息。 @param[in] queue 有效队列标记。 @param[out] out 消息槽。 @param[in] wait 必须零。 @return 有消息为 pdTRUE，否则零。 */
static int xQueueReceive(void *queue, app_event_display_msg_t *out, unsigned wait)
{ assert(queue && !wait); if (read_index == write_index) return 0; *out = messages[read_index++]; return pdTRUE; }
/** @brief 归还 envelope 一次引用。 @param[in] envelope 可空引用。 @post 最后引用释放拥有文本及对象。 */
static void app_event_envelope_release(app_event_envelope_t *envelope)
{
    if (!envelope) return;
    assert(envelope->refs);
    if (!--envelope->refs) { free(envelope->text); free(envelope); }
}
/** @brief 反映真实 UI 的逐条复制入口。 @param[in] role 显示角色。 @param[in] text 调用期文本借用。 @post 进入实际 UI 实现。 */
void board_conversation_append(board_conversation_role_t role, const char *text)
{ board_ui_conversation_append(role, text); }
/** @brief 反映真实片段转发。 @param[in] role 角色。 @param[in] text 借用片段。 @post 进入实际 UI 合并实现。 */
void board_conversation_append_fragment(board_conversation_role_t role, const char *text)
{ board_ui_conversation_append_fragment(role, text); }
/** @brief 反映真实结束转发。 @post 实际 UI 结束当前片段。 */
void board_conversation_finish_fragment(void) { board_ui_conversation_finish_fragment(); }
/** @brief 本组未提交清屏，避免模型悄悄跳过相关分支。 @post 若误入则断言失败。 */
void board_conversation_clear(void) { assert(0); }
/** @brief 本组未提交启动日志。 @param[in] role 角色。 @param[in] text 文本。 @post 若误入则失败。 */
void board_ui_boot_log_append(board_conversation_role_t role, const char *text)
{ (void)role; (void)text; assert(0); }
/** @brief 本组未提交启动清屏。 @post 若误入则失败。 */
void board_ui_boot_log_clear(void) { assert(0); }
@PUMP_FUNCTIONS@

/** @brief 清理本组真实 UI 文本状态。 @post 容器、拼接缓冲及累计字节全部归零。 */
static void reset_text(void)
{
    ui_lock(); conversation_finish_fragment_locked();
    while (container.first) lv_obj_delete(container.first);
    s_conversation_text_bytes = 0; s_last_msg_label = NULL;
    ui_unlock();
    assert(!live_blocks && !live_bytes);
}

/** @brief 验证标签 OOM 事务、累计预算及超批区 UTF-8 envelope 的实际泵送。 @return 零表示通过。 */
int main(void)
{
    board_ui_conversation_append_fragment(BOARD_CONV_ASSISTANT, "old");
    board_ui_conversation_finish_fragment();
    lv_obj_t *label = container.first;
    size_t bytes = s_conversation_text_bytes, blocks = live_blocks;
    ui_lock();
    allocation_calls = 0; fail_at = 1;
    /* PSRAM 失败后普通堆成功，保留允许回退的真实语义。 */
    assert(conversation_set_text_locked(label, "fallback"));
    fail_at = 0;
    assert(strcmp(lv_label_get_text(label), "fallback") == 0);
    assert(live_blocks == blocks);
    bytes = s_conversation_text_bytes;
    allocation_calls = 0; fail_at = 1; fail_until = 2;
    assert(!conversation_set_text_locked(label, "must-not-install"));
    fail_at = fail_until = 0;
    assert(strcmp(lv_label_get_text(label), "fallback") == 0);
    assert(s_conversation_text_bytes == bytes && live_blocks == blocks);
    ui_unlock();
    reset_text();

    for (size_t failure = 1; failure <= 12; failure++) {
        allocation_calls = 0; fail_at = failure; fail_until = failure + 1;
        board_ui_conversation_append_fragment(BOARD_CONV_ASSISTANT, "allocation-matrix");
        fail_at = fail_until = 0;
        size_t retained = 0;
        for (lv_obj_t *item = container.first; item; item = item->next) {
            assert(lv_label_get_text(item));
            retained += strlen(lv_label_get_text(item)) + 1;
        }
        assert(retained == s_conversation_text_bytes);
        reset_text();
    }

    for (unsigned i = 0; i < 170; i++) board_ui_conversation_append(BOARD_CONV_USER, "message");
    assert(lv_obj_get_child_count(&container) == UI_CONV_MAX_MESSAGES);
    assert(s_conversation_text_bytes <= UI_CONV_MAX_BYTES);
    reset_text();

    peak_bytes = 0;
    size_t length = (UI_CONV_MAX_BYTES / 3 + 3) * 3;
    char *large = malloc(length + 1); assert(large);
    for (size_t i = 0; i < length; i += 3) memcpy(large + i, "\xe4\xb8\xad", 3);
    large[length] = 0;
    app_event_envelope_t *envelope = malloc(sizeof(*envelope)); assert(envelope);
    *envelope = (app_event_envelope_t){large, length, 2};
    messages[0] = (app_event_display_msg_t){.kind = APP_EVENT_KIND_ASSISTANT, .envelope = envelope};
    read_index = 0; write_index = 1;
    g_app_event_bus.display_queue = messages; g_app_event_bus.display_pending = 1;
    ui_lock(); app_event_display_pump_active(); ui_unlock();
    assert(g_app_event_bus.display_pending == 0 && envelope->refs == 1);
    assert(read_index == 1 && s_stream_text_len < UI_CONV_MAX_BYTES);
    assert((s_stream_text_len - strlen("ASSIST: ")) % 3 == 0);
    assert(s_conversation_text_bytes <= UI_CONV_MAX_BYTES);
    assert(strcmp(lv_label_get_text(container.first), s_stream_text) == 0);
    now_tick += 100;
    board_ui_conversation_append_fragment(BOARD_CONV_ASSISTANT, "\xe4\xb8\xad");
    assert(s_conversation_text_bytes <= UI_CONV_MAX_BYTES);
    assert(peak_bytes <= 4U * UI_CONV_MAX_BYTES + 512U);
    printf("model_text_peak_bytes=%zu retained_label_budget=%u\n", peak_bytes, UI_CONV_MAX_BYTES);
    app_event_envelope_release(envelope);
    /* 先归还原 producer 引用，UI 的标签和增长缓冲仍须独立有效。 */
    assert(strstr(lv_label_get_text(container.first), "ASSIST: ") == lv_label_get_text(container.first));
    reset_text();
    assert(!ui_depth && !bus_depth);
    return 0;
}
'''

DISPLAY_HEADERS = dict(SNAPSHOT_HEADERS, **{
    "esp_err.h": r'''
#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_ARG 4
const char *esp_err_to_name(esp_err_t error);
''',
    "esp_log.h": r'''
#pragma once
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
''',
    "dev_display_lcd.h": r'''
#pragma once
typedef struct { void *panel_handle; void *io_handle; } dev_display_lcd_handles_t;
''',
    "esp_board_manager_includes.h": r'''
#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t esp_board_manager_init_device_by_name(const char *name);
bool esp_board_manager_check_name(const char *name);
esp_err_t esp_board_manager_get_device_handle(const char *name, void **out);
''',
    "esp_lcd_panel_ops.h": r'''
#pragma once
#include "esp_err.h"
esp_err_t esp_lcd_panel_draw_bitmap(void *panel, int x0, int y0, int x1, int y1, const void *pixels);
''',
})

DISPLAY_TEST = r'''
#include "dev_display_lcd.h"
#include "esp_err.h"
#include "freertos/semphr.h"
static int panel_marker;
static dev_display_lcd_handles_t native_lcd = {&panel_marker, NULL};
static bool device_initialized, fail_handle;
static unsigned native_draw_calls, fail_draw_at, mutex_depth;
static struct { int x0, y0, x1, y1; const uint16_t *source; } native_draws[500];
/** @brief 创建真实宿主互斥对象。 @return 拥有锁，分配失败 NULL。 */
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t mutex = malloc(sizeof(*mutex));
    if (!mutex) return NULL;
    assert(!pthread_mutex_init(mutex, NULL)); return mutex;
}
/** @brief 模拟原生阻塞获取。 @param[in,out] mutex 有效锁。 @param[in] wait 等待标记。 @return pdTRUE。 */
int xSemaphoreTake(SemaphoreHandle_t mutex, unsigned wait)
{ (void)wait; assert(!pthread_mutex_lock(mutex)); mutex_depth++; return 1; }
/** @brief 模拟原生释放。 @param[in,out] mutex 当前任务持有锁。 @return pdTRUE。 */
int xSemaphoreGive(SemaphoreHandle_t mutex)
{ assert(mutex_depth); mutex_depth--; assert(!pthread_mutex_unlock(mutex)); return 1; }
/** @brief 保留 BSP 设备初始化幂等语义。 @param[in] name 设备名。 @return 首次成功，已有设备 INVALID_STATE。 */
esp_err_t esp_board_manager_init_device_by_name(const char *name)
{ assert(!strcmp(name, "display_lcd")); bool ready = device_initialized; device_initialized = true; return ready ? ESP_ERR_INVALID_STATE : ESP_OK; }
/** @brief 当前板定义包含面板。 @param[in] name 设备名。 @return true。 */
bool esp_board_manager_check_name(const char *name) { assert(!strcmp(name, "display_lcd")); return true; }
/** @brief 可失败的 BSP 句柄借用入口。 @param[in] name 设备名。 @param[out] out 入口清空，成功写借用。 @return 原生状态。 */
esp_err_t esp_board_manager_get_device_handle(const char *name, void **out)
{ assert(!strcmp(name, "display_lcd")); *out = fail_handle ? NULL : &native_lcd; return fail_handle ? ESP_FAIL : ESP_OK; }
/** @brief 保存原生异步提交借用，不复制像素。 @param[in] panel 面板借用。 @param[in] x0 左界。 @param[in] y0 上界。 @param[in] x1 右开界。 @param[in] y1 下开界。 @param[in] pixels 调用者持续保持的源。 @return 指定调用失败或成功。 */
esp_err_t esp_lcd_panel_draw_bitmap(void *panel, int x0, int y0, int x1, int y1, const void *pixels)
{
    assert(panel == &panel_marker && mutex_depth == 1);
    assert(x0 >= 0 && y0 >= 0 && x1 <= 800 && y1 <= 480 && x0 < x1 && y0 < y1);
    assert(native_draw_calls < 500);
    unsigned index = native_draw_calls++;
    native_draws[index].x0 = x0; native_draws[index].y0 = y0;
    native_draws[index].x1 = x1; native_draws[index].y1 = y1; native_draws[index].source = pixels;
    return native_draw_calls == fail_draw_at ? ESP_FAIL : ESP_OK;
}
/** @brief 稳定错误文本替身。 @param[in] error 状态值。 @return 常量借用。 */
const char *esp_err_to_name(esp_err_t error) { (void)error; return "model"; }
@DISPLAY_SOURCE@

/** @brief 核对部分初始化重试、参数边界、原始行跨度和中途提交失败。 @return 零表示通过。 */
int main(void)
{
    uint16_t pixels[] = {10, 11, 12, 13, 20, 21, 22, 23};
    assert(board_display_draw_rgb565(pixels, 1, 1) == ESP_ERR_INVALID_STATE);
    allocation_calls = 0; fail_at = 1;
    assert(board_display_init() == ESP_ERR_NO_MEM && !s_display_ready);
    assert(!live_blocks);
    fail_at = 0; fail_handle = true;
    assert(board_display_init() == ESP_FAIL && !s_display_ready);
    assert(live_blocks == 1);
    fail_handle = false;
    assert(board_display_init() == ESP_OK && s_display_ready);
    assert(board_display_init() == ESP_OK && live_blocks == 1);
    assert(board_display_draw_rgb565_at(NULL, 0, 0, 1, 1) == ESP_ERR_INVALID_ARG);
    assert(board_display_draw_rgb565_at(pixels, -1, 0, 1, 1) == ESP_ERR_INVALID_ARG);
    assert(board_display_draw_rgb565_at(pixels, 800, 0, 1, 1) == ESP_ERR_INVALID_ARG);
    assert(board_display_draw_rgb565_at(pixels, 0, 480, 1, 1) == ESP_ERR_INVALID_ARG);
    assert(board_display_draw_rgb565_at(pixels, 0, 0, 0, 1) == ESP_ERR_INVALID_ARG);
    assert(!native_draw_calls);
    assert(board_display_draw_rgb565_at(pixels, 798, 10, 4, 2) == ESP_OK);
    assert(native_draw_calls == 2);
    assert(native_draws[0].source == pixels && native_draws[0].source[0] == 10 && native_draws[0].source[1] == 11);
    assert(native_draws[1].source == pixels + 4 && native_draws[1].source[0] == 20 && native_draws[1].source[1] == 21);
    native_draw_calls = 0; fail_draw_at = 2;
    assert(board_display_draw_rgb565_at(pixels, 798, 10, 4, 2) == ESP_FAIL);
    assert(native_draw_calls == 2 && !mutex_depth);
    fail_draw_at = 0; native_draw_calls = 0;
    assert(board_display_draw_rgb565_at(pixels, 799, 479, INT_MAX, INT_MAX) == ESP_OK);
    assert(native_draw_calls == 1 && native_draws[0].source == pixels);
    assert(native_draws[0].x1 == 800 && native_draws[0].y1 == 480);
    native_draw_calls = 0;
    assert(board_display_draw_rgb565(pixels, 2, 2) == ESP_OK && native_draw_calls == 1);
    assert(!mutex_depth);
    assert(!pthread_mutex_destroy(s_draw_mutex)); free(s_draw_mutex); s_draw_mutex = NULL;
    assert(!live_blocks && !live_bytes);
    return 0;
}
'''

FONT_HEADERS = dict(DISPLAY_HEADERS, **{
    "esp_heap_caps.h": r'''
#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, unsigned caps);
void heap_caps_free(void *pointer);
''',
    "lvgl.h": r'''
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#define LV_FONT_SOURCE_HAN_SANS_SC_16_CJK 0
#define LV_FONT_SOURCE_HAN_SANS_SC_14_CJK 0
#define LV_USE_FS_MEMFS 1
#define LV_FS_RES_OK 0
#define LV_FS_MODE_RD 1
#define LV_FS_SEEK_SET 0
typedef int lv_fs_res_t;
typedef struct { FILE *file; } lv_fs_file_t;
typedef struct { unsigned id; uint8_t *glyphs; uint32_t size; } lv_font_t;
typedef struct lv_obj_t { const lv_font_t *font; struct lv_obj_t *first, *next; unsigned id; } lv_obj_t;
extern lv_font_t model_builtin;
#define LV_FONT_DEFAULT (&model_builtin)
lv_fs_res_t lv_fs_open(lv_fs_file_t *file, const char *path, int mode);
lv_fs_res_t lv_fs_close(lv_fs_file_t *file);
lv_fs_res_t lv_fs_read(lv_fs_file_t *file, void *out, uint32_t bytes, uint32_t *read_bytes);
lv_fs_res_t lv_fs_seek(lv_fs_file_t *file, uint32_t offset, int mode);
''',
    "lv_binfont_loader.h": r'''
#pragma once
#include "lvgl.h"
lv_font_t *lv_binfont_create_from_buffer(void *data, uint32_t size);
lv_font_t *lv_binfont_create(const char *path);
void lv_binfont_destroy(lv_font_t *font);
''',
})

FONT_TEST = r'''
#include "lvgl.h"
#include "board_ui.h"
#include <unistd.h>
lv_font_t model_builtin;
static void *raw_backing;
static unsigned font_serial, constructed_fonts, destroyed_fonts, lvgl_depth, rebound_mask;
static bool fail_buffer_constructor, require_rebound;
static lv_obj_t nodes[12];
static lv_obj_t *s_root = nodes, *s_config_modal = nodes + 2, *s_keyboard = nodes + 4;
static lv_obj_t *s_ota_layer = nodes + 6, *s_boot_layer = nodes + 8, *s_shutdown_layer = nodes + 10;
static bool s_ready = true, s_config_loaded;
static char s_provider[32], s_model[64];
static unsigned s_preview_mux, spin_depth;
#define portENTER_CRITICAL(mux) do { (void)(mux); assert(!spin_depth); spin_depth++; } while (0)
#define portEXIT_CRITICAL(mux) do { (void)(mux); assert(spin_depth == 1); spin_depth--; } while (0)
/** @brief 使用宿主文件模拟 LVGL 路径打开。 @param[out] file 句柄槽。 @param[in] path 路径借用。 @param[in] mode 只读模式。 @return 原生状态。 */
lv_fs_res_t lv_fs_open(lv_fs_file_t *file, const char *path, int mode)
{ assert(mode == LV_FS_MODE_RD); file->file = fopen(path[1] == ':' ? path + 2 : path, "rb"); return file->file ? 0 : 1; }
/** @brief 关闭模型文件。 @param[in,out] file 独占句柄。 @return 关闭状态。 */
lv_fs_res_t lv_fs_close(lv_fs_file_t *file) { int result = fclose(file->file); file->file = NULL; return result ? 1 : 0; }
/** @brief 读取原生预检字段。 @param[in,out] file 文件。 @param[out] out 至少bytes字节。 @param[in] bytes 请求量。 @param[out] read_bytes 可空实际长度槽。 @return 无I/O错误为零。 */
lv_fs_res_t lv_fs_read(lv_fs_file_t *file, void *out, uint32_t bytes, uint32_t *read_bytes)
{ size_t read = fread(out, 1, bytes, file->file); if (read_bytes) *read_bytes = (uint32_t)read; return ferror(file->file) ? 1 : 0; }
/** @brief 模拟绝对定位。 @param[in,out] file 独占文件。 @param[in] offset 字节偏移。 @param[in] mode 原生定位枚举。 @return 定位状态。 */
lv_fs_res_t lv_fs_seek(lv_fs_file_t *file, uint32_t offset, int mode)
{ assert(mode == LV_FS_SEEK_SET); return fseek(file->file, (long)offset, SEEK_SET) ? 1 : 0; }
/** @brief 模拟对齐PSRAM分配并记录原文件借用期。 @param[in] alignment 对齐要求。 @param[in] bytes 字节数。 @param[in] caps 能力位。 @return 拥有模型块或NULL，模型不测DMA物理对齐。 */
void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, unsigned caps)
{ (void)alignment; (void)caps; assert(!raw_backing); raw_backing = malloc(bytes); return raw_backing; }
/** @brief 归还候选原文件块并结束借用。 @param[in] pointer 可空拥有指针。 @post 原文件跟踪清空。 */
void heap_caps_free(void *pointer) { if (pointer == raw_backing) raw_backing = NULL; free(pointer); }
/** @brief 模拟当前native已经确认的深复制语义。 @param[in] data 文件字节借用。 @param[in] size 字节数。 @return 拥有字体及独立字节表，失败清理候选。 */
static lv_font_t *copy_font(const void *data, uint32_t size)
{
    assert(lvgl_depth && data && size);
    lv_font_t *font = malloc(sizeof(*font));
    if (!font) return NULL;
    font->glyphs = malloc(size);
    if (!font->glyphs) { free(font); return NULL; }
    memcpy(font->glyphs, data, size); font->size = size; font->id = ++font_serial;
    constructed_fonts++; return font;
}
/** @brief 模拟memfs构造的同步深复制与可控失败。 @param[in] data 调用期文件借用。 @param[in] size 字节数。 @return 拥有字体或NULL。 */
lv_font_t *lv_binfont_create_from_buffer(void *data, uint32_t size)
{ assert(data == raw_backing); return fail_buffer_constructor ? NULL : copy_font(data, size); }
/** @brief 模拟原生文件构造路径。 @param[in] path LVGL路径借用。 @return 拥有字体或NULL，临时文件副本全部回收。 */
lv_font_t *lv_binfont_create(const char *path)
{
    FILE *file = fopen(path[1] == ':' ? path + 2 : path, "rb");
    if (!file) return NULL;
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size > 0 && size <= UINT32_MAX);
    rewind(file); void *data = malloc((size_t)size);
    if (!data) { fclose(file); return NULL; }
    assert(fread(data, 1, (size_t)size, file) == (size_t)size); assert(!fclose(file));
    lv_font_t *font = copy_font(data, (uint32_t)size); free(data); return font;
}
/** @brief 退休原生拥有字体前核对全树重绑。 @param[in] font 独占退休字体。 @post 字体及表释放，计数增加。 */
void lv_binfont_destroy(lv_font_t *font)
{
    assert(lvgl_depth && font && font != &model_builtin);
    if (require_rebound) {
        assert(rebound_mask == 0xfffU);
        for (unsigned i = 0; i < 12; i++) assert(nodes[i].font != font);
    }
    free(font->glyphs); free(font); destroyed_fonts++;
}
@FONT_SOURCE@
/** @brief 模拟LVGL owner进入。 @post 递归锁深度增加。 */
static void ui_lock(void) { lvgl_depth++; }
/** @brief 模拟LVGL owner退出。 @post 递归锁深度归还。 */
static void ui_unlock(void) { assert(lvgl_depth); lvgl_depth--; }
/** @brief 安装字体借用并记录每个实际节点。 @param[in,out] obj 节点。 @param[in] font 新字体。 @param[in] selector 默认样式。 @post 节点借用更新。 */
static void lv_obj_set_style_text_font(lv_obj_t *obj, const lv_font_t *font, int selector)
{ assert(lvgl_depth && !selector); obj->font = font; rebound_mask |= 1U << obj->id; }
/** @brief 计数模型孩子。 @param[in] obj 节点。 @return 孩子数。 */
static uint32_t lv_obj_get_child_count(lv_obj_t *obj)
{ uint32_t count = 0; for (lv_obj_t *child = obj->first; child; child = child->next) count++; return count; }
/** @brief 读取模型孩子。 @param[in] obj 节点。 @param[in] index 索引。 @return 孩子借用或NULL。 */
static lv_obj_t *lv_obj_get_child(lv_obj_t *obj, uint32_t index)
{ lv_obj_t *child = obj->first; while (child && index--) child = child->next; return child; }
/** @brief 观察字体事务之后的日志，无对象持有。 @param[in] role 角色。 @param[in] text 调用期文本。 @post 校验事务已解锁。 */
void board_ui_boot_log_append(board_conversation_role_t role, const char *text)
{ (void)role; assert(text && !lvgl_depth); }
@FONT_UI_FUNCTIONS@

/** @brief 写入供实际字体预检读取的最小受支持头和三个块。 @param[in] path 独占临时路径。 @post 文件关闭且字节稳定至后续加载完成。 */
static void write_font_fixture(const char *path)
{
    FILE *file = fopen(path, "wb"); assert(file);
    ui_binfont_header_t header = {.tables_count = 3, .font_size = 16, .ascent = 12, .descent = -4,
        .bits_per_pixel = 1, .xy_bits = 1, .wh_bits = 1};
    ui_binfont_cmap_table_t cmap = {.range_start = 65, .range_length = 1, .format_type = 2};
    uint32_t head_size = 8 + sizeof(header), cmap_size = 12 + sizeof(cmap), loca_size = 14, glyf_size = 10, count = 1;
    cmap.data_offset = cmap_size;
    uint16_t offset = 8, data = 0;
    assert(fwrite(&head_size, 4, 1, file) == 1 && fwrite("head", 4, 1, file) == 1);
    assert(fwrite(&header, sizeof(header), 1, file) == 1);
    assert(fwrite(&cmap_size, 4, 1, file) == 1 && fwrite("cmap", 4, 1, file) == 1);
    assert(fwrite(&count, 4, 1, file) == 1 && fwrite(&cmap, sizeof(cmap), 1, file) == 1);
    assert(fwrite(&loca_size, 4, 1, file) == 1 && fwrite("loca", 4, 1, file) == 1);
    assert(fwrite(&count, 4, 1, file) == 1 && fwrite(&offset, 2, 1, file) == 1);
    assert(fwrite(&glyf_size, 4, 1, file) == 1 && fwrite("glyf", 4, 1, file) == 1 && fwrite(&data, 2, 1, file) == 1);
    assert(!fclose(file));
}

/** @brief 实际字体文件读取、缓存key、原文件退休及六树重绑回收。 @return 零表示通过，BSP与native字体内部OOM属于替身边界。 */
int main(void)
{
    char directory[] = "/tmp/cclaw-font-XXXXXX";
    assert(mkdtemp(directory));
    char path[160]; size_t prefix = (size_t)snprintf(path, sizeof(path), "%s/", directory);
    memset(path + prefix, 'f', sizeof(path) - prefix - 1); path[sizeof(path) - 1] = 0;
    write_font_fixture(path);
    board_ui_font_set_path(path);
    lv_font_t *retired = (lv_font_t *)(uintptr_t)1;
    lvgl_depth = 1;
    assert(board_ui_font_load_external(&retired) && !retired && !raw_backing);
    const lv_font_t *first = board_ui_font_current();
    assert(first != &model_builtin && first->glyphs[4] == 'h');
    unsigned built = constructed_fonts;
    assert(board_ui_font_load_external(&retired) && !retired && board_ui_font_current() == first);
    assert(constructed_fonts == built && strlen(s_loaded_font_path) == 161);
    for (unsigned i = 0; i < 12; i++) { nodes[i].id = i; nodes[i].font = first; }
    for (unsigned i = 0; i < 12; i += 2) nodes[i].first = nodes + i + 1;
    char second_path[160]; snprintf(second_path, sizeof(second_path), "%s/second.bin", directory);
    write_font_fixture(second_path); board_ui_font_set_path(second_path);
    lvgl_depth = 0; rebound_mask = 0; require_rebound = true;
    board_ui_set_config_loaded(true, "provider", "model");
    assert(!lvgl_depth && !spin_depth && !raw_backing && destroyed_fonts == 1);
    assert(board_ui_font_current() != first && rebound_mask == 0xfffU);
    assert(!unlink(second_path)); rebound_mask = 0;
    board_ui_set_config_loaded(true, "provider", "model");
    assert(board_ui_font_current() == &model_builtin && destroyed_fonts == 2);
    assert(!live_blocks && !live_bytes);
    require_rebound = false;

    lvgl_depth = 1; fail_buffer_constructor = true; board_ui_font_set_path(path);
    assert(board_ui_font_load_external(&retired) && !retired && !raw_backing);
    assert(board_ui_font_current()->glyphs[4] == 'h');
    board_ui_font_set_path("/missing-cclaw-font-fixture");
    assert(!board_ui_font_load_external(&retired) && retired);
    board_ui_font_release_retired(&retired); assert(!retired);
    fail_buffer_constructor = false;
    for (size_t failure = 1; failure <= 3; failure++) {
        board_ui_font_set_path(path); allocation_calls = 0; fail_at = failure;
        bool loaded = board_ui_font_load_external(&retired);
        fail_at = 0;
        assert(!retired && !raw_backing);
        assert(loaded ? board_ui_font_current() != &model_builtin : board_ui_font_current() == &model_builtin);
        board_ui_font_set_path("/missing-cclaw-font-fixture");
        assert(!board_ui_font_load_external(&retired));
        board_ui_font_release_retired(&retired);
        assert(!live_blocks && !live_bytes);
    }
    assert(!board_ui_font_load_external(NULL));
    assert(!unlink(path) && !rmdir(directory));
    assert(constructed_fonts == destroyed_fonts && !raw_backing);
    return 0;
}
'''

INIT_MODEL = r'''
typedef struct lv_timer_t { struct lv_timer_t *next; } lv_timer_t;
typedef struct { bool with_caps; } model_queue_t;
typedef model_queue_t *QueueHandle_t;
typedef struct { void *display; bool is_rgb, ppa_accel_enabled; } board_lcd_t;
typedef struct {
    struct { uint32_t magic; int cf; unsigned w, h; uint16_t stride; } header;
    size_t data_size;
    const uint8_t *data;
} lv_image_dsc_t;
#define UI_LCD_WIDTH 800
#define UI_LCD_HEIGHT 480
#define UI_PREVIEW_W 240
#define UI_PREVIEW_H 240
#define UI_CONV_X 250
#define UI_STATUS_H 34
#define UI_PANEL_PAD 6
#define UI_ROOT_BG_COLOR 0x0b1014
#define UI_CONV_BG_COLOR 0x080c10
#define UI_STATUS_SCROLL_MS 45000
#define UI_STATUS_REFRESH_MS 3000
#define UI_PREVIEW_TIMER_MS 50
#define UI_TOOL_CMD_TIMER_MS 20
#define UI_EVENT_DISPLAY_TIMER_MS 20
#define UI_TOOL_CMD_QUEUE_DEPTH 32
#define UI_TOOL_AREA_ENABLED 1
#define UI_TOOL_BG_COLOR 0x101820
#define UI_TOOL_BORDER_COLOR 0x304050
#define UI_TOOL_X 6
#define UI_TOOL_Y 326
#define UI_TOOL_W 238
#define UI_TOOL_H 148
#define UI_TOOL_INNER_PAD 4
#define UI_TOOL_TITLE_H 26
#define UI_TOOL_CANVAS_W 230
#define UI_TOOL_CANVAS_H 114
#define UI_PREVIEW_BUFFER_COUNT 5
#define UI_PREVIEW_ROW_BYTES 480
#define UI_PREVIEW_BYTES (s_preview_stride_bytes * UI_PREVIEW_H)
#define UI_PREVIEW_PSRAM_FOR_SPI_LCD 1
#define UI_PREVIEW_PSRAM_FOR_RGB_LCD 1
#define UI_PREVIEW_STRICT_PSRAM_FOR_RGB_LCD 1
#define UI_CACHE_ALIGN 64
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_DMA 8
#define MALLOC_CAP_CACHE_ALIGNED 16
#define UI_HAVE_ESP_FREERTOS_HOOKS 0
#define configSUPPORT_STATIC_ALLOCATION 1
#define LV_IMAGE_HEADER_MAGIC 0x19
#define LV_COLOR_FORMAT_RGB565 1
#define LV_EVENT_CLICKED 2
#define LV_EVENT_REFR_READY 3
#define LV_PCT(value) (value)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M 1
#define ESP_ERR_NOT_SUPPORTED 5
static const char *TAG = "init-model";
static void *s_disp;
static bool s_lcd_is_rgb, s_ppa_accel_enabled;
static lv_obj_t top_container;
static lv_obj_t *s_root, *s_status_label, *s_preview, *s_overlay, *s_tool_panel, *s_tool_area;
static lv_obj_t *s_boot_layer, *s_boot_log;
static uint8_t *s_preview_buf[UI_PREVIEW_BUFFER_COUNT];
static lv_image_dsc_t s_preview_dsc[UI_PREVIEW_BUFFER_COUNT];
static size_t s_preview_stride_bytes;
static uint8_t s_preview_front_index, s_preview_write_index;
static int s_preview_pending_index, s_preview_presenting_index;
static QueueHandle_t s_tool_cmd_queue;
static lv_timer_t *timers;
static bool fail_snapshot;
static unsigned board_init_calls, display_registrations, queue_count;
static int display_marker;

/** @brief 核对所有视觉操作只作用于有效对象。 @param[in] obj 有效借用。 @post NULL 会使测试失败。 */
static void check_object(lv_obj_t *obj) { assert(ui_depth && obj); }
/* 视觉布局和样式在此模型只检查对象有效性，字体及native内部样式分配另有边界。 */
#define lv_obj_remove_style_all(obj) check_object(obj)
#define lv_obj_set_size(obj, ...) check_object(obj)
#define lv_obj_set_pos(obj, ...) check_object(obj)
#define lv_obj_set_width(obj, ...) check_object(obj)
#define lv_obj_align(obj, ...) check_object(obj)
#define lv_obj_center(obj) check_object(obj)
#define lv_obj_set_style_bg_color(obj, ...) check_object(obj)
#define lv_obj_set_style_bg_opa(obj, ...) check_object(obj)
#define lv_obj_set_style_pad_all(obj, ...) check_object(obj)
#define lv_obj_set_style_pad_row(obj, ...) check_object(obj)
#define lv_obj_set_style_pad_top(obj, ...) check_object(obj)
#define lv_obj_set_style_pad_bottom(obj, ...) check_object(obj)
#define lv_obj_set_style_border_color(obj, ...) check_object(obj)
#define lv_obj_set_style_border_opa(obj, ...) check_object(obj)
#define lv_obj_set_style_border_width(obj, ...) check_object(obj)
#define lv_obj_set_style_radius(obj, ...) check_object(obj)
#define lv_obj_set_style_text_color(obj, ...) check_object(obj)
#define lv_obj_set_style_anim_duration(obj, ...) check_object(obj)
#define lv_obj_set_flex_flow(obj, ...) check_object(obj)
#define lv_obj_set_flex_grow(obj, ...) check_object(obj)
#define lv_obj_set_scrollbar_mode(obj, ...) check_object(obj)
#define lv_obj_set_scroll_dir(obj, ...) check_object(obj)
#define lv_obj_clear_flag(obj, ...) check_object(obj)
#define lv_label_set_long_mode(obj, ...) check_object(obj)
#define lv_color_hex(value) (value)
#define lv_label_set_text(obj, text) lv_label_set_text_static(obj, text)
/** @brief 模拟可返回NULL的原生控件工厂。 @param[in,out] parent 父容器。 @return 父容器拥有的新对象或NULL，不模拟native内部样式分配。 */
static lv_obj_t *init_object_create(lv_obj_t *parent)
{
    assert(ui_depth && parent);
    lv_obj_t *obj = malloc(sizeof(*obj)); if (!obj) return NULL;
    memset(obj, 0, sizeof(*obj)); obj->parent = parent; obj->text = ""; obj->static_text = true;
    lv_obj_t **slot = &parent->first; while (*slot) slot = &(*slot)->next; *slot = obj;
    return obj;
}
#define lv_obj_create(parent) init_object_create(parent)
#define lv_label_create(parent) init_object_create(parent)
#define lv_image_create(parent) init_object_create(parent)
#define lv_button_create(parent) init_object_create(parent)
/** @brief 读取稳定的系统顶层。 @return 借用全局顶层容器。 */
static lv_obj_t *lv_layer_top(void) { return &top_container; }
/** @brief 读取稳定的活动屏幕。 @return 借用全局屏幕容器。 */
static lv_obj_t *lv_screen_active(void) { return &container; }
/** @brief 模拟字体样式绑定。 @param[in] obj 有效对象。 @post 只检查对象，无独立资源。 */
static void apply_font(lv_obj_t *obj) { check_object(obj); }
/** @brief 返回当前目标实际配置的对齐行跨度。 @param[in] width 像素宽度。 @param[in] format RGB565格式。 @return 512字节。 */
static uint32_t lv_draw_buf_width_to_stride(unsigned width, int format)
{ assert(width == 240 && format == LV_COLOR_FORMAT_RGB565); return 512; }
/** @brief 模拟图像源借用安装。 @param[in] obj 有效图像。 @param[in] descriptor 稳定描述符。 @post 核对像素区已经有效。 */
static void lv_image_set_src(lv_obj_t *obj, const lv_image_dsc_t *descriptor)
{ check_object(obj); assert(descriptor->data && descriptor->data_size == 122880); }
/** @brief 模拟能力堆对齐申请，不测物理DMA约束。 @param[in] alignment 对齐要求。 @param[in] bytes 字节数。 @param[in] caps 能力位。 @return 拥有块或NULL。 */
static void *heap_caps_aligned_alloc(size_t alignment, size_t bytes, unsigned caps)
{ (void)alignment; (void)caps; return malloc(bytes); }
/** @brief 模拟目标缓存同步。 @param[in] data 完整缓冲。 @param[in] bytes 字节数。 @param[in] flags 方向标志。 @return ESP_OK。 */
static esp_err_t esp_cache_msync(void *data, size_t bytes, unsigned flags)
{ assert(data && bytes && flags); return ESP_OK; }
/** @brief 日志位置判定替身。 @param[in] pointer 缓冲借用。 @return true。 */
static bool esp_ptr_external_ram(const void *pointer) { assert(pointer); return true; }
/** @brief 创建并登记可检查的原生定时器。 @param[in] callback 同步回调。 @param[in] period 毫秒周期。 @param[in] data 借用上下文。 @return 拥有定时器或NULL。 */
static lv_timer_t *lv_timer_create(void (*callback)(lv_timer_t *), unsigned period, void *data)
{
    (void)data; assert(callback && period && ui_depth);
    lv_timer_t *timer = malloc(sizeof(*timer)); if (!timer) return NULL;
    timer->next = timers; timers = timer; return timer;
}
/** @brief 删除已登记定时器。 @param[in] timer 拥有定时器。 @post 从模型原生链移除并归还。 */
static void lv_timer_delete(lv_timer_t *timer)
{
    assert(ui_depth); lv_timer_t **slot = &timers;
    while (*slot && *slot != timer) slot = &(*slot)->next;
    assert(*slot == timer); *slot = timer->next; free(timer);
}
/** @brief 记录显示事件注册，只允许完整候选后提交。 @param[in] display BSP借用。 @param[in] callback 刷新回调。 @param[in] code 刷新事件。 @param[in] data 借用上下文。 @post 注册计数增加。 */
static void lv_display_add_event_cb(void *display, delete_callback_t callback, int code, void *data)
{ (void)data; assert(display == &display_marker && callback && code == LV_EVENT_REFR_READY && ui_depth); display_registrations++; }
/** @brief 模型初始化期间不主动执行定时器。 @param[in] timer 可空借用。 @post 保持状态。 */
static void status_timer_cb(lv_timer_t *timer) { (void)timer; assert(ui_depth); }
/** @brief 模型预览定时器标识。 @param[in] timer 借用。 @post 不执行消费。 */
static void preview_timer_cb(lv_timer_t *timer) { (void)timer; }
/** @brief 模型工具定时器标识。 @param[in] timer 借用。 @post 不执行消费。 */
static void tool_cmd_timer_cb(lv_timer_t *timer) { (void)timer; }
/** @brief 模型事件定时器标识。 @param[in] timer 借用。 @post 不执行消费。 */
static void event_display_timer_cb(lv_timer_t *timer) { (void)timer; }
/** @brief 模型设置按钮回调标识。 @param[in] event 借用。 @post 未调度。 */
static void settings_btn_cb(lv_event_t *event) { (void)event; assert(0); }
/** @brief 模型刷新回调标识。 @param[in] event 借用。 @post 未调度。 */
static void display_refresh_event_cb(lv_event_t *event) { (void)event; }
/** @brief 模拟已单独验证的快照初始化。 @return 指定失败时false，否则true。 */
bool board_ui_snapshot_init(void) { return !fail_snapshot; }
/** @brief 模拟BSP创建并交付持续有效display借用。 @param[out] lcd 必填输出。 @return ESP_OK。 */
static esp_err_t board_lcd_init(board_lcd_t *lcd)
{ board_init_calls++; lcd->display = &display_marker; lcd->is_rgb = true; lcd->ppa_accel_enabled = false; return ESP_OK; }
/** @brief 创建带来源标记的队列模型。 @param[in] depth 深度。 @param[in] bytes 单项字节数。 @param[in] with_caps 是否能力堆来源。 @return 拥有队列或NULL。 */
static QueueHandle_t create_queue(unsigned depth, size_t bytes, bool with_caps)
{
    assert(ui_depth && depth == UI_TOOL_CMD_QUEUE_DEPTH);
    QueueHandle_t queue = malloc(sizeof(*queue) + depth * bytes);
    if (queue) { queue->with_caps = with_caps; queue_count++; }
    return queue;
}
/** @brief 能力队列入口。 @param[in] depth 深度。 @param[in] bytes 项字节数。 @param[in] caps 能力位。 @return 拥有队列或NULL。 */
static QueueHandle_t xQueueCreateWithCaps(unsigned depth, size_t bytes, unsigned caps)
{ (void)caps; return create_queue(depth, bytes, true); }
/** @brief 普通队列入口。 @param[in] depth 深度。 @param[in] bytes 项字节数。 @return 拥有队列或NULL。 */
static QueueHandle_t xQueueCreate(unsigned depth, size_t bytes) { return create_queue(depth, bytes, false); }
/** @brief 按能力来源删除队列。 @param[in] queue 拥有队列。 @post 核对来源并归还。 */
static void vQueueDeleteWithCaps(QueueHandle_t queue)
{ assert(queue->with_caps && queue_count); queue_count--; free(queue); }
/** @brief 按普通来源删除队列。 @param[in] queue 拥有队列。 @post 核对来源并归还。 */
static void vQueueDelete(QueueHandle_t queue)
{ assert(!queue->with_caps && queue_count); queue_count--; free(queue); }
@INIT_FUNCTIONS@

static lv_obj_t *s_ota_layer, *s_ota_bar, *s_ota_detail;
static unsigned captured_progress;
/** @brief 进度算术测试借用已存在的模型对象。 @post 不创建资源。 */
static void create_ota_layer_locked(void) { assert(ui_depth && s_ota_layer); }
/** @brief 捕获实际进度函数交付的整数百分比。 @param[in] bar 有效对象。 @param[in] value 0到100。 @param[in] animation 禁用动画标记。 @post 保存进度值。 */
static void lv_bar_set_value(lv_obj_t *bar, int32_t value, int animation)
{ (void)animation; assert(bar && value >= 0 && value <= 100); captured_progress = (unsigned)value; }
/* 该补充只观察算术，文本和失效操作不保留栈上的调用期借用。 */
#undef lv_label_set_text
#define lv_label_set_text(obj, text) do { check_object(obj); assert(text); } while (0)
#define lv_obj_invalidate(obj) check_object(obj)
#define LV_PART_INDICATOR 1
#define LV_ANIM_OFF 0
@OTA_FUNCTION@

/** @brief 仅供模型销毁一次成功UI，真实产品保持进程期对象。 @post 全部模型资源归还，BSP借用保留以检验下一次重试复用。 */
static void cleanup_initialized_model(void)
{
    s_ready = false; ui_lock();
    while (timers) lv_timer_delete(timers);
    if (s_tool_cmd_queue) {
        if (s_tool_cmd_queue->with_caps) vQueueDeleteWithCaps(s_tool_cmd_queue);
        else vQueueDelete(s_tool_cmd_queue);
        s_tool_cmd_queue = NULL;
    }
    while (container.first) lv_obj_delete(container.first);
    while (top_container.first) lv_obj_delete(top_container.first);
    s_root = s_status_label = s_preview = s_conversation = s_overlay = NULL;
    s_tool_panel = s_tool_area = s_boot_layer = s_boot_log = NULL;
    for (unsigned i = 0; i < UI_PREVIEW_BUFFER_COUNT; i++) {
        heap_caps_free(s_preview_buf[i]); s_preview_buf[i] = NULL;
    }
    ui_unlock(); assert(!queue_count && !live_blocks && !live_bytes);
}

/** @brief 逐可检查分配点故障并重试实际初始化函数。 @return 零表示失败回收、降级和BSP复用均通过。 */
int main(void)
{
    s_ready = false; fail_snapshot = true;
    assert(board_ui_init() == ESP_ERR_NO_MEM && !board_init_calls);
    fail_snapshot = false;
    size_t calls = 0;
    for (size_t failure = 0; failure <= calls || !failure; failure++) {
        allocation_calls = 0; fail_at = failure; display_registrations = 0;
        esp_err_t result = board_ui_init();
        size_t used = allocation_calls;
        fail_at = 0;
        if (!failure) { calls = used; assert(result == ESP_OK && calls); }
        if (result == ESP_OK) {
            assert(s_ready && display_registrations == 1 && timers && s_tool_cmd_queue);
            cleanup_initialized_model();
        } else {
            assert(result == ESP_ERR_NO_MEM && !s_ready && !display_registrations);
            assert(!timers && !queue_count && !container.first && !top_container.first);
            for (unsigned i = 0; i < UI_PREVIEW_BUFFER_COUNT; i++) assert(!s_preview_buf[i]);
            assert(!live_blocks && !live_bytes && !ui_depth);
            assert(board_ui_init() == ESP_OK && s_ready && display_registrations == 1);
            cleanup_initialized_model();
        }
        assert(board_init_calls == 1 && !ui_depth);
    }
    ui_lock(); s_ota_layer = &top_container; s_ota_bar = s_ota_detail = &container;
    ota_progress_update_locked(SIZE_MAX - 1U, SIZE_MAX, "Writing"); assert(captured_progress == 99);
    ota_progress_update_locked(SIZE_MAX, SIZE_MAX, "Writing"); assert(captured_progress == 100);
    ota_progress_update_locked(SIZE_MAX, 0, "Writing"); assert(captured_progress == 0);
    ota_progress_update_locked(1, 3, "Writing"); assert(captured_progress == 33);
    ota_progress_update_locked(2, 3, "Writing"); assert(captured_progress == 66);
    ui_unlock();
    printf("checked_UI_allocation_sites=%zu BSP_display_creations=%u\n", calls, board_init_calls);
    return 0;
}
'''


class DisplayResourceContracts(unittest.TestCase):
    def test_json_paths_and_transactional_oom(self):
        """完整生产 JSON 包装和 UI 路径实现验证长键及逐分配撤销。"""
        run_fixture(COMMON + JSON_TEST, "json-paths", sources=(
            "third_party/cclaw-sdk/cclaw/core/vendor/cjson/cJSON.c", "third_party/cclaw-sdk/cclaw/core/src/core/cc_result.c",
            "third_party/cclaw-sdk/cclaw/core/src/util/cc_json.c", "main/ui/board_ui_json.c"))

    def test_snapshot_publication_and_oom(self):
        """实际快照整文件验证锁发布、文本预算及失败结果拥有资源回收。"""
        snapshot = '#include "' + (ROOT / "main/ui/board_ui_snapshot.c").as_posix() + '"'
        code = COMMON + SNAPSHOT_TEST.replace("@SNAPSHOT_SOURCE@", snapshot)
        run_fixture(code, "snapshot", SNAPSHOT_HEADERS, sources=(
            "third_party/cclaw-sdk/cclaw/core/vendor/cjson/cJSON.c", "third_party/cclaw-sdk/cclaw/core/src/core/cc_result.c",
            "third_party/cclaw-sdk/cclaw/core/src/util/cc_json.c", "third_party/cclaw-sdk/cclaw/core/src/util/cc_string_builder.c"))

    def test_text_ownership_and_oversized_stream(self):
        """实际标签事务和显示泵验证超批区正文、UTF-8 截点及 pending/引用回收。"""
        ui_functions = "\n".join(production_function("main/ui/board_ui.c", signature) for signature in (
            "static const char *role_prefix(", "static void conversation_label_delete_cb(",
            "static lv_obj_t *conversation_create_label_locked(", "static bool conversation_set_text_locked(",
            "static void conversation_prune_old_messages_locked(", "static bool conversation_buffer_append_locked(",
            "static void conversation_flush_fragment_locked(", "static void conversation_finish_fragment_locked(",
            "void board_ui_conversation_append(", "void board_ui_conversation_append_fragment(",
            "void board_ui_conversation_finish_fragment("))
        pump_path = os.environ.get("CCLAW_DISPLAY_ORIGINAL_PUMP", "main/services/events/app_event_display_sink.c")
        pump_functions = "\n".join(production_function(pump_path, signature)
                                   for signature in (
            "static board_conversation_role_t role_for_event(", "static bool is_stream_event(",
            "static void display_view_free(", "static void display_emit_view(",
            "static void display_emit_direct(", "static void display_complete_one(",
            "static void display_flush_stream(", "static void app_event_display_pump_active("))
        code = COMMON + TEXT_PREFIX.replace("@TEXT_FUNCTIONS@", ui_functions).replace("@PUMP_FUNCTIONS@", pump_functions)
        run_fixture(code, "text-and-pump", {"esp_err.h": "#pragma once\ntypedef int esp_err_t;\n"}, sources=(
            "third_party/cclaw-sdk/cclaw/core/vendor/cjson/cJSON.c", "third_party/cclaw-sdk/cclaw/core/src/core/cc_result.c",
            "third_party/cclaw-sdk/cclaw/core/src/util/cc_json.c"))

    def test_direct_lcd_retry_parameters_and_stride(self):
        """整份实际直接绘制实现验证重试、整数边界、裁剪跨度和源借用。"""
        source = '#include "' + (ROOT / "main/ui/board_display.c").as_posix() + '"'
        run_fixture(COMMON + DISPLAY_TEST.replace("@DISPLAY_SOURCE@", source), "direct-lcd", DISPLAY_HEADERS,
                    sources=("third_party/cclaw-sdk/cclaw/core/vendor/cjson/cJSON.c", "third_party/cclaw-sdk/cclaw/core/src/core/cc_result.c",
                             "third_party/cclaw-sdk/cclaw/core/src/util/cc_json.c"))

    def test_font_file_copy_and_retirement(self):
        """实际字体整文件及UI重绑函数，核对完整缓存key、深复制边界与退休顺序。"""
        source = '#include "' + (ROOT / "main/ui/board_ui_font.c").as_posix() + '"'
        functions = "\n".join(production_function("main/ui/board_ui.c", signature) for signature in (
            "static void apply_font(", "static void apply_font_tree(", "void board_ui_set_config_loaded("))
        code = COMMON + FONT_TEST.replace("@FONT_SOURCE@", source).replace("@FONT_UI_FUNCTIONS@", functions)
        run_fixture(code, "font-retirement", FONT_HEADERS, sources=(
            "third_party/cclaw-sdk/cclaw/core/vendor/cjson/cJSON.c", "third_party/cclaw-sdk/cclaw/core/src/core/cc_result.c",
            "third_party/cclaw-sdk/cclaw/core/src/util/cc_json.c"))

    def test_ui_initialization_failure_cleanup_and_retry(self):
        """实际初始化及九个创建函数，逐可检查分配点核对资源回收、降级与BSP复用。"""
        prefix = TEXT_PREFIX.split("@TEXT_FUNCTIONS@")[0]
        ui_source = (ROOT / "main/ui/board_ui.c").read_text(encoding="utf-8")
        type_start = ui_source.index("typedef enum {\n    UI_TOOL_CMD_CLEAR,")
        type_end = ui_source.index("} tool_cmd_t;", type_start) + len("} tool_cmd_t;")
        queue_types = ui_source[type_start:type_end]
        functions = "\n".join(production_function("main/ui/board_ui.c", signature) for signature in (
            "static void format_bytes(", "static bool create_status_bar(", "static uint8_t *preview_buffer_alloc(",
            "static bool create_preview(", "static bool create_conversation(", "static bool create_overlay(",
            "static bool create_tool_area(", "static bool create_boot_log_layer(",
            "static bool create_settings_button(", "static bool tool_cmd_queue_ensure(bool *out_with_caps)\n{",
            "esp_err_t board_ui_init("))
        progress = production_function("main/ui/board_ui.c", "static void ota_progress_update_locked(")
        model = INIT_MODEL.replace("@INIT_FUNCTIONS@", functions).replace("@OTA_FUNCTION@", progress)
        code = COMMON + prefix + queue_types + model
        run_fixture(code, "ui-init", DISPLAY_HEADERS, sources=(
            "third_party/cclaw-sdk/cclaw/core/vendor/cjson/cJSON.c", "third_party/cclaw-sdk/cclaw/core/src/core/cc_result.c",
            "third_party/cclaw-sdk/cclaw/core/src/util/cc_json.c"))


if __name__ == "__main__":
    unittest.main()
