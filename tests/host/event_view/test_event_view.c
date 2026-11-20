#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Include the production bus to initialize its private admission gate without
 * starting firmware tasks. Display and ring remain separate production files. */
#include "../../../main/services/events/app_event_bus.c"
#include "board_conversation.h"
#include "board_ui_snapshot.h"
#include "cc/util/cc_json.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "check failed at %d\n", __LINE__); return 1; } } while (0)

typedef struct fake_queue {
    size_t capacity, item_size, start, count;
    unsigned char *data;
} fake_queue_t;
static int allocations;
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; void *p=malloc(n); if (p) allocations++; return p; }
void *heap_caps_calloc(size_t n, size_t size, unsigned caps) { (void)caps; void *p=calloc(n,size); if (p) allocations++; return p; }
void heap_caps_free(void *p) { if (p) { allocations--; free(p); } }
QueueHandle_t xQueueCreate(UBaseType_t count, size_t size)
{
    fake_queue_t *q=calloc(1,sizeof(*q)); if (!q) return NULL;
    q->data=calloc(count,size); if (!q->data) { free(q); return NULL; }
    q->capacity=count; q->item_size=size; return q;
}
BaseType_t xQueueSend(QueueHandle_t handle, const void *item, TickType_t wait)
{
    (void)wait; fake_queue_t *q=handle; if (q->count==q->capacity) return pdFALSE;
    memcpy(q->data+((q->start+q->count)%q->capacity)*q->item_size,item,q->item_size);
    q->count++; return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void *item, TickType_t wait)
{
    (void)wait; fake_queue_t *q=handle; if (!q->count) return pdFALSE;
    memcpy(item,q->data+q->start*q->item_size,q->item_size);
    q->start=(q->start+1)%q->capacity; q->count--; return pdTRUE;
}
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t handle) { return (UBaseType_t)((fake_queue_t *)handle)->count; }
void vQueueDelete(QueueHandle_t handle) { fake_queue_t *q=handle; free(q->data); free(q); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t w) { (void)m; (void)w; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t m) { (void)m; return pdTRUE; }
void vSemaphoreDelete(SemaphoreHandle_t m) { (void)m; }
EventGroupHandle_t xEventGroupCreate(void) { return (void *)2; }
EventBits_t xEventGroupSetBits(EventGroupHandle_t g, EventBits_t b) { (void)g; return b; }
EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b) { (void)g; return b; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t g, EventBits_t b, BaseType_t c, BaseType_t a, TickType_t w)
{ (void)g; (void)c; (void)a; (void)w; return b; }
void vEventGroupDelete(EventGroupHandle_t g) { (void)g; }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (void *)3; }
TickType_t xTaskGetTickCount(void) { return 1; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }

/* Ordinary publication routes are disabled in this fixture. */
app_event_sink_mask_t app_event_route_for_kind_locked(app_event_kind_t kind) { (void)kind; return 0; }
const char *app_event_log_level_string(cc_log_level_t level) { (void)level; return "info"; }
bool app_event_route_to_serial(app_event_kind_t k, cc_log_level_t l, app_event_sink_mask_t r) { (void)k; (void)l; (void)r; return false; }
bool app_event_route_to_display(app_event_kind_t k, app_event_sink_mask_t r) { (void)k; (void)r; return false; }
bool app_event_route_to_usb_log(app_event_kind_t k, cc_log_level_t l, app_event_sink_mask_t r) { (void)k; (void)l; (void)r; return false; }
void app_event_usb_enqueue(app_event_envelope_t *e, uint32_t s, app_event_kind_t k, cc_log_level_t l)
{ (void)e; (void)s; (void)k; (void)l; }

typedef struct board_line { board_conversation_role_t role; char *text; } board_line_t;
static board_line_t lines[32];
static size_t line_count;
static int fragment_active, inject_view;
static char boot_text[512];
static int boot_clears;
static bool fake_ui_ready=true;
bool board_ui_ready(void) { return fake_ui_ready; }
void board_ui_boot_log_append(board_conversation_role_t role,const char *text)
{
    (void)role; snprintf(boot_text,sizeof(boot_text),"%s",text ? text : "");
    board_ui_snapshot_append_boot(text ? text : "");
}
void board_ui_boot_log_clear(void)
{ boot_text[0]='\0'; boot_clears++; board_ui_snapshot_clear_boot(); }
static const app_event_view_item_t injected[]={{APP_EVENT_KIND_USER,"replacement"}};
void board_conversation_clear(void)
{
    for (size_t i=0;i<line_count;i++) free(lines[i].text);
    memset(lines,0,sizeof(lines)); line_count=0; fragment_active=0;
}
void board_conversation_finish_fragment(void) { fragment_active=0; }
void board_conversation_append(board_conversation_role_t role, const char *text)
{
    if (line_count>=32) abort();
    lines[line_count].role=role; lines[line_count].text=strdup(text ? text : "");
    if (!lines[line_count].text) abort();
    line_count++; fragment_active=0;
}
void board_conversation_append_fragment(board_conversation_role_t role, const char *text)
{
    if (!fragment_active) { board_conversation_append(role,text); fragment_active=1; }
    else {
        char *old=lines[line_count-1].text; size_t a=strlen(old),b=strlen(text);
        char *next=realloc(old,a+b+1); if (!next) abort();
        memcpy(next+a,text,b+1); lines[line_count-1].text=next;
    }
    if (inject_view) {
        inject_view=0;
        if (app_event_bus_replace_conversation(injected,1)!=ESP_OK) abort();
    }
}

int main(void)
{
    g_app_event_bus.mutex=(void *)1; g_app_event_bus.idle_events=(void *)2;
    g_app_event_bus.lifecycle=APP_EVENT_BUS_CONFIGURED;
    g_app_event_bus.display_enabled=true; g_app_event_bus.serial_enabled=true;
    s_publish_order_mutex=(void *)1; s_access_open=true;
    CHECK(app_event_display_init()==ESP_OK && board_ui_snapshot_init());

    /* A failed LCD initialization cannot leave an undrainable display queue
     * or prevent the Web/serial history view from being published. */
    fake_ui_ready=false;
    app_event_publish_boot_status(APP_EVENT_KIND_STATUS,APP_EVENT_SOURCE_SYSTEM,CC_LOG_INFO,"no LCD");
    app_event_view_item_t no_lcd={APP_EVENT_KIND_STATUS,"view without LCD"};
    CHECK(app_event_bus_replace_conversation(&no_lcd,1)==ESP_OK &&
          uxQueueMessagesWaiting(g_app_event_bus.display_queue)==0 && !g_app_event_bus.display_pending);
    app_event_bus_clear_conversation();
    CHECK(uxQueueMessagesWaiting(g_app_event_bus.display_queue)==0 && !g_app_event_bus.display_pending);
    fake_ui_ready=true;

    /* Startup status is delivered before normal display routes are enabled.
     * A clear removes the old status and the next status follows it. */
    g_app_event_bus.display_enabled=false;
    app_event_publish_boot_status(APP_EVENT_KIND_STATUS,APP_EVENT_SOURCE_SYSTEM,CC_LOG_INFO,"before clear");
    app_event_bus_clear_conversation();
    app_event_publish_boot_status(APP_EVENT_KIND_ERROR,APP_EVENT_SOURCE_SYSTEM,CC_LOG_ERROR,"after clear");
    CHECK(uxQueueMessagesWaiting(g_app_event_bus.display_queue)==2);
    app_event_display_pump();
    CHECK(boot_clears==1 && strcmp(boot_text,"after clear")==0 && g_app_event_bus.display_pending==0);
    char *screen=board_ui_screen_snapshot_json(true);
    CHECK(screen && strstr(screen,"after clear") && !strstr(screen,"before clear")); free(screen);
    g_app_event_bus.display_enabled=true;

    char cut[2054]; memset(cut,'x',2047); memcpy(cut+2047,"\xe6\xb5\x8b",3); cut[2050]='\0';
    app_event_envelope_t *truncated=app_event_envelope_create(cut);
    CHECK(truncated && truncated->len==2047 && truncated->text[2047]=='\0');
    app_event_envelope_release(truncated);
    app_event_publishf(APP_EVENT_KIND_STATUS,APP_EVENT_SOURCE_SYSTEM,CC_LOG_INFO,"%s",cut);
    char *formatted=app_event_bus_json(0,120,APP_EVENT_KIND_MASK_ALL);
    CHECK(formatted && !strchr(formatted,(unsigned char)0xe6)); free(formatted);

    for (unsigned i=0;i<APP_EVENT_DISPLAY_QUEUE_DEPTH;i++) {
        app_event_envelope_t *e=app_event_envelope_create("old backlog"); CHECK(e);
        app_event_display_emit(APP_EVENT_KIND_ASSISTANT,e); app_event_envelope_release(e);
    }
    CHECK(uxQueueMessagesWaiting(g_app_event_bus.display_queue)==APP_EVENT_DISPLAY_QUEUE_DEPTH);
    char long_text[6001]; for (size_t i=0;i<6000;i+=3) memcpy(long_text+i,"\xe6\xb5\x8b",3);
    long_text[6000]='\0';
    app_event_view_item_t page[]={
        {APP_EVENT_KIND_USER,"hello"}, {APP_EVENT_KIND_ASSISTANT,long_text}, {APP_EVENT_KIND_STATUS,"session 1"}
    };
    FILE *serial=tmpfile(); CHECK(serial); int saved=dup(fileno(stdout)); CHECK(saved>=0);
    CHECK(dup2(fileno(serial),fileno(stdout))>=0);
    CHECK(app_event_bus_replace_conversation(page,3)==ESP_OK);
    fflush(stdout); CHECK(dup2(saved,fileno(stdout))>=0); close(saved);
    CHECK(uxQueueMessagesWaiting(g_app_event_bus.display_queue)==1 && g_app_event_bus.display_pending==1);
    app_event_display_pump();
    CHECK(line_count==3 && lines[0].role==BOARD_CONV_USER && strcmp(lines[0].text,"hello")==0 &&
          lines[1].role==BOARD_CONV_ASSISTANT && strcmp(lines[1].text,long_text)==0 &&
          strcmp(lines[2].text,"session 1")==0 && g_app_event_bus.display_pending==0);
    fseek(serial,0,SEEK_END); long serial_size=ftell(serial); CHECK(serial_size>6000);
    rewind(serial); char *serial_text=calloc((size_t)serial_size+1,1); CHECK(serial_text);
    CHECK(fread(serial_text,1,(size_t)serial_size,serial)==(size_t)serial_size);
    CHECK(strstr(serial_text,"hello") && strstr(serial_text,long_text)); free(serial_text); fclose(serial);
    char *json=app_event_bus_json(0,120,APP_EVENT_KIND_MASK_ALL);
    CHECK(json && strstr(json,"hello") && strstr(json,long_text) && strstr(json,"session 1")); free(json);

    char *oversized=malloc(APP_EVENT_VIEW_MAX_BYTES+1U); CHECK(oversized);
    memset(oversized,'z',APP_EVENT_VIEW_MAX_BYTES); oversized[APP_EVENT_VIEW_MAX_BYTES]='\0';
    app_event_view_item_t too_large={APP_EVENT_KIND_USER,oversized};
    uint32_t generation=g_app_event_bus.clear_generation;
    CHECK(app_event_bus_replace_conversation(&too_large,1)==ESP_ERR_INVALID_SIZE &&
          g_app_event_bus.clear_generation==generation && line_count==3);
    free(oversized);

    /* A late Web client must still receive the selected history page after
     * ordinary events have overwritten every page record in the ring. */
    uint32_t page_seq=g_app_event_bus.view_latest_seq;
    for (unsigned i=0;i<APP_EVENT_RECENT_MAX+3U;i++)
        app_event_publish(APP_EVENT_KIND_STATUS,APP_EVENT_SOURCE_SYSTEM,CC_LOG_INFO,"later event");
    json=app_event_bus_json(0,4,APP_EVENT_KIND_MASK_ALL); CHECK(json);
    cc_json_value_t *root=NULL; cc_result_t parse=cc_json_parse_strict(json,&root);
    CHECK(parse.code==CC_OK && root); cc_result_free(&parse); free(json);
    cc_json_value_t *view=cc_json_object_get(root,"view");
    CHECK(cc_json_is_array(view) && cc_json_array_size(view)==3 &&
          cc_json_int_value(cc_json_object_get(root,"view_seq"))==(int)page_seq &&
          strcmp(cc_json_string_value(cc_json_object_get(cc_json_array_get(view,1),"text")),long_text)==0 &&
          cc_json_array_size(cc_json_object_get(root,"events"))==4);
    cc_json_destroy(root);
    json=app_event_bus_json(page_seq+1U,4,APP_EVENT_KIND_MASK(APP_EVENT_KIND_USER)); CHECK(json);
    root=NULL; parse=cc_json_parse_strict(json,&root); CHECK(parse.code==CC_OK && root); cc_result_free(&parse); free(json);
    view=cc_json_object_get(root,"view");
    CHECK(cc_json_bool_value(cc_json_object_get(root,"gap")) && cc_json_is_array(view) &&
          cc_json_array_size(view)==1 &&
          strcmp(cc_json_string_value(cc_json_object_get(cc_json_array_get(view,0),"text")),"hello")==0);
    cc_json_destroy(root);
    json=app_event_bus_json(g_app_event_bus.next_seq,4,APP_EVENT_KIND_MASK_ALL); CHECK(json);
    root=NULL; parse=cc_json_parse_strict(json,&root); CHECK(parse.code==CC_OK && root); cc_result_free(&parse); free(json);
    CHECK(cc_json_is_null(cc_json_object_get(root,"view"))); cc_json_destroy(root);

    /* A view arriving while a stream batch is being rendered remains one
     * queue item; the next pump replaces rather than loses the snapshot. */
    app_event_envelope_t *old=app_event_envelope_create("old"); CHECK(old);
    app_event_display_emit(APP_EVENT_KIND_ASSISTANT,old); app_event_envelope_release(old);
    inject_view=1; app_event_display_pump();
    CHECK(uxQueueMessagesWaiting(g_app_event_bus.display_queue)==1);
    app_event_display_pump();
    CHECK(line_count==1 && strcmp(lines[0].text,"replacement")==0 && g_app_event_bus.display_pending==0);

    CHECK(app_event_bus_replace_conversation(NULL,0)==ESP_OK);
    app_event_display_pump(); CHECK(line_count==0 && g_app_event_bus.display_pending==0);
    app_event_bus_clear_conversation();
    app_event_display_pump();
    CHECK(boot_clears==2 && !boot_text[0]);
    screen=board_ui_screen_snapshot_json(true);
    CHECK(screen && !strstr(screen,"after clear")); free(screen);
    json=app_event_bus_json(0,4,APP_EVENT_KIND_MASK_ALL); CHECK(json);
    root=NULL; parse=cc_json_parse_strict(json,&root); CHECK(parse.code==CC_OK && root); cc_result_free(&parse); free(json);
    CHECK(cc_json_is_null(cc_json_object_get(root,"view")) && !g_app_event_bus.view_active);
    cc_json_destroy(root);
    app_event_ring_clear_locked();
    char full_event[APP_EVENT_TEXT_MAX];
    memset(full_event, 'b', sizeof(full_event) - 1U);
    full_event[sizeof(full_event) - 1U] = '\0';
    for (unsigned i = 0; i < APP_EVENT_RECENT_MAX; i++) {
        app_event_publish(APP_EVENT_KIND_STATUS, APP_EVENT_SOURCE_SYSTEM, CC_LOG_INFO, full_event);
    }
    CHECK(g_app_event_bus.recent_bytes <= APP_EVENT_RECENT_MAX_BYTES &&
          g_app_event_bus.recent_count < APP_EVENT_RECENT_MAX &&
          g_app_event_bus.recent_count > 0 && g_app_event_bus.ring_overwrites > 0);
    /* 序号耗尽清空近期 ring 和固定页，清屏代数允许 MAX 回零，旧游标收到刷新提示。 */
    app_event_view_item_t rollover_page[] = {
        {APP_EVENT_KIND_USER, "before rollover"},
        {APP_EVENT_KIND_ASSISTANT, "old selected page"},
    };
    CHECK(app_event_bus_replace_conversation(rollover_page, 2) == ESP_OK);
    app_event_display_pump();
    g_app_event_bus.next_seq = UINT32_MAX;
    g_app_event_bus.clear_generation = UINT32_MAX;
    g_app_event_bus.published = UINT32_MAX;
    app_event_publish(APP_EVENT_KIND_STATUS, APP_EVENT_SOURCE_SYSTEM, CC_LOG_INFO, "new sequence");
    CHECK(g_app_event_bus.next_seq == 1 && g_app_event_bus.clear_generation == 0 &&
          g_app_event_bus.published == UINT32_MAX && !g_app_event_bus.view_active &&
          g_app_event_bus.recent_count == 1);
    json = app_event_bus_json(UINT32_MAX, 4, APP_EVENT_KIND_MASK_ALL);
    CHECK(json);
    root = NULL; parse = cc_json_parse_strict(json, &root);
    CHECK(parse.code == CC_OK && root); cc_result_free(&parse); free(json);
    CHECK(cc_json_bool_value(cc_json_object_get(root, "cursor_reset")) &&
          cc_json_int_value(cc_json_object_get(root, "clear_generation")) == 0 &&
          cc_json_array_size(cc_json_object_get(root, "events")) == 1 &&
          cc_json_is_null(cc_json_object_get(root, "view")));
    cc_json_destroy(root);
    app_event_display_pump();
    CHECK(line_count == 0);

    /* 整页安装预先换代，避免页中途清空已提交引用。 */
    g_app_event_bus.next_seq = UINT32_MAX - 1U;
    CHECK(app_event_bus_replace_conversation(rollover_page, 2) == ESP_OK);
    CHECK(g_app_event_bus.next_seq == 2 && g_app_event_bus.view_count == 2 &&
          g_app_event_bus.view[0].seq == 1 && g_app_event_bus.view[1].seq == 2);
    app_event_display_pump();
    CHECK(line_count == 2);
    char overlap[16] = "abcdef";
    app_event_copy_text(overlap, sizeof(overlap), overlap);
    CHECK(strcmp(overlap, "abcdef") == 0);
    app_event_copy_text(overlap, sizeof(overlap), overlap + 2);
    CHECK(strcmp(overlap, "cdef") == 0);
    char *null_copy = app_event_strdup_len(NULL, SIZE_MAX);
    CHECK(null_copy && !null_copy[0]); heap_caps_free(null_copy);
    uint32_t saturated = UINT32_MAX;
    app_event_counter_increment(&saturated);
    CHECK(saturated == UINT32_MAX);
    app_event_ring_clear_locked(); heap_caps_free(g_app_event_bus.recent);
    app_event_display_discard_all(); vQueueDelete(g_app_event_bus.display_queue);
    board_conversation_clear(); CHECK(allocations==0);
    return 0;
}
