"""用实际对话函数核对批量流式刷新、字节裁剪、结束与清空。"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_event_ring_ownership import run_c_test as run_plain_c_test
from test_display_resource_contracts import production_function


def run_c_test(source):
    """沿用普通平台入口，SAN 模式实际插桩当前对话函数夹具。"""
    if os.environ.get("CCLAW_TEST_SANITIZERS") != "1":
        run_plain_c_test(source)
        return
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if os.name == "nt" or not compiler:
        raise RuntimeError("对话 sanitizer 夹具需要 Linux/WSL 的本机 C 编译器")
    with tempfile.TemporaryDirectory(prefix="cclaw-conversation-san-") as folder:
        directory = Path(folder)
        fixture = directory / "fixture.c"
        binary = directory / "fixture"
        fixture.write_text(source, encoding="utf-8")
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                        "-o", str(binary), str(fixture)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define UI_CONV_MAX_MESSAGES 3U
#define UI_CONV_MAX_BYTES 512U
#define UI_STREAM_SCROLL_INTERVAL_MS 50U
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define LV_ANIM_OFF 0
#define LV_SIZE_CONTENT 0
#define LV_EVENT_DELETE 1
#define pdMS_TO_TICKS(n) (n)
typedef unsigned TickType_t;
typedef int board_conversation_role_t;
typedef struct object lv_obj_t;
typedef struct { lv_obj_t *target; } lv_event_t;
typedef void (*delete_callback_t)(lv_event_t *);
struct object {
    char *text;
    void *user_data;
    bool static_text;
    delete_callback_t on_delete;
    lv_obj_t *parent, *children[16];
    unsigned count;
};
static lv_obj_t panel;
static lv_obj_t *s_conversation=&panel, *s_last_msg_label, *s_stream_msg_label;
static board_conversation_role_t s_last_role, s_stream_role;
static bool s_ready=true, s_stream_active, s_stream_text_dirty, s_conversation_layout_pending;
static size_t s_conversation_text_bytes, s_stream_text_len, s_stream_text_capacity;
static char *s_stream_text;
static TickType_t s_last_stream_scroll_tick, tick=1;
static unsigned label_updates;
static char snapshot[512];
static void ui_lock(void) {}
static void ui_unlock(void) {}
static TickType_t xTaskGetTickCount(void) { return tick; }
static void *heap_caps_realloc(void *p,size_t n,unsigned caps) { (void)caps; return realloc(p,n); }
/** @brief 模拟能力堆正文分配。 @param[in] n 字节数。 @param[in] caps 能力位。 @return 拥有块或 NULL。 */
static void *heap_caps_malloc(size_t n,unsigned caps) { (void)caps; return malloc(n); }
/** @brief 归还正文拥有块。 @param[in] p 可空拥有指针。 @post 原块归还。 */
static void heap_caps_free(void *p) { free(p); }
/** @brief 更新标签拥有槽。 @param[in,out] p 标签。 @param[in] data 新槽值。 @post 槽已替换。 */
static void lv_obj_set_user_data(lv_obj_t *p,void *data) { p->user_data=data; }
/** @brief 读取标签拥有槽。 @param[in] p 标签。 @return 调用期槽借用。 */
static void *lv_obj_get_user_data(lv_obj_t *p) { return p->user_data; }
/** @brief 读取同步删除目标。 @param[in] e 事件。 @return 标签借用。 */
static lv_obj_t *lv_event_get_target(lv_event_t *e) { return e->target; }
/** @brief 登记 DELETE 回调。 @param[in,out] p 标签。 @param[in] callback 回调。 @param[in] code 事件类型。 @param[in] data 未使用上下文。 @return 非空表示成功。 */
static void *lv_obj_add_event_cb(lv_obj_t *p,delete_callback_t callback,int code,void *data) {
    (void)data; assert(code==LV_EVENT_DELETE); p->on_delete=callback; return p;
}
static unsigned lv_obj_get_child_count(lv_obj_t *p) { return p->count; }
static lv_obj_t *lv_obj_get_child(lv_obj_t *p,int index) { return p->children[index]; }
static const char *lv_label_get_text(lv_obj_t *p) { return p->text; }
static void lv_label_set_text(lv_obj_t *p,const char *text) {
    size_t length=strlen(text)+1U;
    char *copy=malloc(length); assert(copy); memcpy(copy,text,length);
    if (!p->static_text) free(p->text);
    p->text=copy; p->static_text=false; label_updates++;
}
/** @brief 安装持续借用文本并模拟原生动态文本退休。 @param[in,out] p 标签。 @param[in] text 新借用。 @post 仅原动态文本释放，更新计数增加。 */
static void lv_label_set_text_static(lv_obj_t *p,const char *text) {
    if (!p->static_text) free(p->text);
    p->text=(char *)text; p->static_text=true; label_updates++;
}
static lv_obj_t *lv_label_create(lv_obj_t *parent) {
    lv_obj_t *label=calloc(1,sizeof(*label)); assert(label && parent->count<16);
    label->parent=parent; parent->children[parent->count++]=label;
    lv_label_set_text(label,"Text"); return label;
}
static void lv_obj_delete(lv_obj_t *label) {
    lv_obj_t *parent=label->parent; unsigned i=0;
    while (parent->children[i]!=label) i++;
    memmove(parent->children+i,parent->children+i+1,(parent->count-i-1U)*sizeof(label));
    parent->count--;
    if (label->on_delete) { lv_event_t event={label}; label->on_delete(&event); }
    if (!label->static_text) free(label->text);
    free(label);
}
static void lv_obj_clean(lv_obj_t *p) { while (p->count) lv_obj_delete(p->children[0]); }
static void lv_obj_invalidate(lv_obj_t *p) { (void)p; }
static void lv_obj_set_height(lv_obj_t *p,int height) { (void)p; (void)height; }
static void apply_message_label_style(lv_obj_t *p,int role) { (void)p; (void)role; }
static const char *role_prefix(int role) { (void)role; return "assistant"; }
static void board_ui_snapshot_append_conversation(const char *text) { snprintf(snapshot,sizeof(snapshot),"%s",text); }
static void board_ui_snapshot_replace_last_conversation(const char *text) { board_ui_snapshot_append_conversation(text); }
static void board_ui_snapshot_clear_conversation(void) { snapshot[0]='\0'; }
static void board_ui_snapshot_clear_tool(void) {}
static void board_ui_snapshot_clear_draw(void) {}
static void conversation_overlay_clear_locked(void) {}
static void tool_area_clear_locked(void) {}
'''

TESTS = r'''
static void verify_budget(void) {
    size_t actual=0;
    for (unsigned i=0;i<panel.count;i++) actual+=strlen(panel.children[i]->text)+1U;
    assert(actual==s_conversation_text_bytes && actual<=UI_CONV_MAX_BYTES);
    assert(panel.count<=UI_CONV_MAX_MESSAGES);
}
int main(void) {
    board_ui_conversation_append_fragment(0,"a");
    unsigned first_updates=label_updates;
    for (unsigned i=0;i<100;i++) board_ui_conversation_append_fragment(0,"b");
    assert(label_updates==first_updates);
    board_ui_conversation_finish_fragment();
    assert(strlen(panel.children[0]->text)==112U && !s_stream_text);
    verify_budget();
    char text[251]; memset(text,'x',250); text[250]='\0';
    for (unsigned i=0;i<6;i++) {
        tick+=60;
        board_ui_conversation_append_fragment(0,text);
        board_ui_conversation_finish_fragment();
        verify_budget();
    }
    assert(panel.count==1U);
    board_ui_conversation_append_fragment(0,"pending");
    board_ui_conversation_append_fragment(0," final");
    board_ui_conversation_clear();
    assert(!snapshot[0] && !s_stream_text && !panel.count && !s_conversation_text_bytes);
    board_ui_conversation_append(0,"after clear");
    verify_budget();
    board_ui_conversation_clear();
    puts("LCD stream batching, byte budget, final flush and clear: PASS");
    return 0;
}
'''


class UIConversationMemory(unittest.TestCase):
    def test_stream_batch_budget_finish_and_clear(self):
        """保留原三条/512 字节缩小预算场景，通过真实函数签名提取当前实现。"""
        signatures = (
            "static void conversation_label_delete_cb(",
            "static lv_obj_t *conversation_create_label_locked(",
            "static bool conversation_set_text_locked(",
            "static void conversation_prune_old_messages_locked(",
            "static bool conversation_buffer_append_locked(",
            "static void conversation_flush_fragment_locked(",
            "static void conversation_finish_fragment_locked(",
            "void board_ui_conversation_clear(", "void board_ui_conversation_append(",
            "void board_ui_conversation_append_fragment(", "void board_ui_conversation_finish_fragment(")
        code = "\n".join(production_function("main/ui/board_ui.c", signature) for signature in signatures)
        run_c_test(PREFIX + code + TESTS)


if __name__ == "__main__":
    unittest.main()
