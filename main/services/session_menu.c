#include "session_menu.h"
#include "history_view.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_ROWS (APP_EVENT_VIEW_MAX_ITEMS - 2U)
#define MENU_HISTORY_LIMIT 12U

typedef struct memory_pending_row {
    char *run_id;
    bool ready;
    bool local_submission;
    size_t retained_answer_bytes;
} memory_pending_row_t;

struct session_menu {
    session_selection_t *selection;
    recovery_service_t *recovery;
    session_memory_turns_t *memory_turns; /**< 从生命周期 owner 借用至菜单销毁。 */
    cc_session_t *sessions;
    size_t session_count, session_offset;
    bool sessions_visible;
    cc_message_t *history;
    size_t history_count;
    int older_available;
    app_history_cursor_t next_cursor;
    app_history_cursor_t visible_cursor;
    bool page_has_more;
    bool history_visible;
    char preview_id[96];
    char **run_ids;
    bool *run_from_backup;
    bool *run_from_temporary;
    char (*run_input_previews)[96];
    size_t run_count, run_offset;
    bool runs_visible;
    memory_pending_row_t *memory_pending;
    size_t memory_count, memory_offset;
    size_t memory_queued, memory_queue_bytes;
    bool memory_visible;
    enum { READY_NONE, READY_NEW, READY_HISTORY } pending_ready;
    bool ready_history_needs_refresh;
};

/**
 * @brief 释放菜单独有的记忆任务行及 run_id 副本，使旧编号视图失效。
 * @param[in,out] menu 非空菜单，持有待清理的任务行数组。
 * @post 数组释放并置空，任务数、偏移及可见标志清零。
 */
static void memory_rows_free(session_menu_t *menu)
{
    for (size_t i=0;i<menu->memory_count;i++) free(menu->memory_pending[i].run_id);
    free(menu->memory_pending); menu->memory_pending=NULL;
    menu->memory_count=0; menu->memory_offset=0; menu->memory_visible=false;
}

/**
 * @brief 逐项清理历史消息并释放拥有的页数组。
 * @param[in,out] menu 非空菜单，持有历史快照及分页状态。
 * @post 消息数组已释放，数量、分页游标、更多标志和历史可见标志清零。
 */
static void history_free(session_menu_t *menu)
{
    for (size_t i=0;i<menu->history_count;i++) cc_message_cleanup(&menu->history[i]);
    free(menu->history); menu->history=NULL; menu->history_count=0;
    menu->older_available=0; menu->page_has_more=false;
    menu->history_visible=false;
    memset(&menu->next_cursor,0,sizeof(menu->next_cursor));
    memset(&menu->visible_cursor,0,sizeof(menu->visible_cursor));
}

/**
 * @brief 释放从 recovery 接管的运行 ID 及菜单独有的来源和预览数组。
 * @param[in,out] menu 非空菜单，持有运行列表快照。
 * @post 数组释放并置空，数量、偏移和可见标志清零，后续选择须先重绘任务列表。
 */
static void runs_free(session_menu_t *menu)
{
    for (size_t i=0;i<menu->run_count;i++) free(menu->run_ids[i]);
    free(menu->run_ids); free(menu->run_from_backup);
    free(menu->run_from_temporary); free(menu->run_input_previews);
    menu->run_from_temporary=NULL; menu->run_input_previews=NULL;
    menu->run_ids=NULL; menu->run_from_backup=NULL; menu->run_count=0; menu->run_offset=0;
    menu->runs_visible=false;
}

/**
 * @brief 发布错误正文或错误码说明，并释放结果拥有的诊断资源。
 * @param[in] source 事件所属入口。
 * @param[in,out] rc 非空拥有结果，由本函数调用 cc_result_free 清理。
 * @post 已尝试发布错误事件，rc 的拥有资源已释放。
 */
static void publish_error(app_event_source_t source, cc_result_t *rc)
{
    app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
                      rc->message ? rc->message : cc_error_string(rc->code));
    cc_result_free(rc);
}

/**
 * @brief 将共享页面替换失败转为诊断事件。
 * @param[in] source 诊断所属入口。
 * @param[in] err 页面操作返回的 ESP 错误码。
 * @post err 非 ESP_OK 时发布显示错误，已完成的选择状态继续由 selection 记录。
 */
static void publish_display_error(app_event_source_t source, esp_err_t err)
{
    if (err != ESP_OK)
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
                          "页面显示失败，请重试当前命令。会话状态不会因显示失败而回滚。");
}

/**
 * @brief 为 qsort 按会话 ID 的字节顺序比较两项。
 * @param[in] a 非空 cc_session_t 元素借用地址。
 * @param[in] b 非空 cc_session_t 元素借用地址。
 * @return 原样返回 strcmp 的负、零或正比较值，缺失 ID 按空串比较。
 */
static int session_compare(const void *a, const void *b)
{
    const cc_session_t *left=a,*right=b;
    return strcmp(left->id ? left->id : "",right->id ? right->id : "");
}

/**
 * @brief 为有效 UTF-8 字符串选择预算内的完整字符前缀。
 * @param[in] text 非空 NUL 结尾字符串，编码有效性由上游保证，调用期间借用。
 * @param[in] maximum 允许保留的最大正文字节数，不计 NUL。
 * @return 未超预算时返回全文字节数，超预算时从截断处退回字符起点并返回前缀字节数。
 */
static size_t safe_prefix(const char *text, size_t maximum)
{
    size_t length=strlen(text);
    if (length<=maximum) return length;
    while (maximum && ((unsigned char)text[maximum] & 0xc0U)==0x80U) maximum--;
    return maximum;
}

/**
 * @brief 解析非零十进制序号并转换为零基索引，允许尾部空格和制表符。
 * @param[in] text 借用输入，可为 NULL，数值解析使用 strtoul。
 * @param[out] out_index 必填，成功时填写零基索引，失败保持原值。
 * @return 数值及尾部合法返回真，空输入、越界或格式错误返回假，列表范围由命令分支核对。
 */
static bool parse_index(const char *text, size_t *out_index)
{
    if (!text || !*text || *text=='-' || *text=='+') return false;
    errno=0; char *end=NULL; unsigned long number=strtoul(text,&end,10);
    while (end && (*end==' ' || *end=='\t')) end++;
    if (errno || !end || *end || !number || number>SIZE_MAX) return false;
    *out_index=(size_t)number-1U; return true;
}

/**
 * @brief 从拥有的会话快照构建当前页，成功显示后开放会话编号选择。
 *
 * 行正文位于本次栈中，replace_conversation 在调用内接管其内容副本。
 * @param[in,out] menu 非空菜单，使用当前列表偏移，成功后释放旧历史并清预览 ID。
 * @return 返回共享视图替换结果，失败保持 sessions_visible=false，列表快照继续由菜单持有。
 */
static esp_err_t render_sessions(session_menu_t *menu)
{
    menu->sessions_visible=false;
    char rows[MENU_ROWS][160];
    app_event_view_item_t items[APP_EVENT_VIEW_MAX_ITEMS];
    size_t n=0;
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,
        "请选择 /new 开始新会话，或输入 /view 序号 查看历史；/sessions next|prev 翻页。"};
    for (size_t i=menu->session_offset;i<menu->session_count && n<=MENU_ROWS;i++) {
        const char *id=menu->sessions[i].id ? menu->sessions[i].id : "";
        size_t len=safe_prefix(id,112U);
        snprintf(rows[n-1U],sizeof(rows[0]),"%zu. %.*s%s",i+1U,(int)len,id,id[len] ? "…" : "");
        items[n]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,rows[n-1U]}; n++;
    }
    char footer[128];
    snprintf(footer,sizeof(footer),"历史会话 %zu 条，当前从第 %zu 条显示。",menu->session_count,
             menu->session_count ? menu->session_offset+1U : 0U);
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,footer};
    esp_err_t err=app_event_bus_replace_conversation(items,n);
    if (err==ESP_OK) {
        history_free(menu);
        menu->preview_id[0]='\0';
        menu->sessions_visible=true;
        menu->memory_visible=false;
    }
    return err;
}

/**
 * @brief 读取并按 ID 排序会话快照，接管成功结果后显示第一页。
 * @param[in,out] menu 非空菜单，降级时显示历史或账本修复提示，正常读取成功后替换拥有列表。
 * @return 读取失败发布错误并返回 ESP_FAIL，其他返回共享视图替换结果。
 * 读取失败保留旧快照，新快照显示失败时仍由菜单持有供重绘。
 */
static esp_err_t reload_sessions(session_menu_t *menu)
{
    if (recovery_service_history_damaged(menu->recovery)) {
        app_event_view_item_t items[]={
            {APP_EVENT_KIND_ERROR,"历史记录或恢复账本不完整，无法确认未完成工具操作；所有工具已禁用。"},
            {APP_EVENT_KIND_STATUS,"输入 /new 开始纯文本新会话；修复当前 U 盘的数据并重启后恢复历史和工具。"}
        };
        menu->sessions_visible=false;
        return app_event_bus_replace_conversation(items,sizeof(items)/sizeof(items[0]));
    }
    cc_session_t *sessions=NULL; size_t count=0;
    cc_result_t rc=session_selection_list_sessions(menu->selection,&sessions,&count);
    if (rc.code != CC_OK) { publish_error(APP_EVENT_SOURCE_RUNTIME,&rc); return ESP_FAIL; }
    cc_result_free(&rc);
    if (count>1U) qsort(sessions,count,sizeof(*sessions),session_compare);
    cc_session_free_array(menu->sessions,menu->session_count);
    menu->sessions=sessions; menu->session_count=count; menu->session_offset=0;
    return render_sessions(menu);
}

/**
 * @brief 从拥有的历史消息按条数和字节预算构建共享显示页。
 *
 * 快照替换成功后发布当前/下一游标和可见标志，再清理临时 page。
 * @param[in,out] menu 非空菜单，借用其消息，入口清历史可见标志，成功后更新游标及页状态。
 * @param[in] cursor 本页在消息数组中的起点，按 history_view 的游标契约解释。
 * @return 返回分页构建或共享视图替换结果，失败保留消息和原游标供重试。
 */
static esp_err_t render_history(session_menu_t *menu, app_history_cursor_t cursor)
{
    menu->history_visible=false;
    app_history_page_t page={0};
    esp_err_t err=app_history_page_build(menu->history,menu->history_count,cursor,MENU_ROWS,
                                         APP_EVENT_VIEW_MAX_BYTES-1024U,&page);
    if (err != ESP_OK) return err;
    app_event_view_item_t items[APP_EVENT_VIEW_MAX_ITEMS];
    char header[256], footer[160];
    bool chatting=session_selection_phase(menu->selection)==SESSION_SELECTION_CHATTING;
    snprintf(header,sizeof(header),chatting ? "已恢复会话 %s。" :
             "正在查看 %s；/resume 继续此会话，/sessions 返回列表。",menu->preview_id);
    snprintf(footer,sizeof(footer),"%s",chatting ? "输入消息继续对话。" :
             (page.has_more || menu->older_available ? "输入 /more 查看下一页。" : "已到此会话历史末页。"));
    size_t n=0; items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,header};
    for (size_t i=0;i<page.count;i++) items[n++]=page.items[i];
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,footer};
    err=app_event_bus_replace_conversation(items,n);
    if (err == ESP_OK) {
        menu->visible_cursor=cursor; menu->next_cursor=page.next;
        menu->page_has_more=page.has_more; menu->history_visible=true;
        menu->sessions_visible=false; menu->memory_visible=false;
    }
    app_history_page_cleanup(&page);
    return err;
}

/**
 * @brief 在 CHOOSING 阶段载入一页历史，核对分页锚点后替换旧消息。
 * @param[in,out] menu 非空菜单，读取成功且锚点有效后接管消息并从页首渲染。
 * @param[in] session_id 借用目标 ID，先复制到固定缓冲区，调用方已核对最多 95 字节。
 * @param[in] before_id 可选借用分页锚点，NULL 读取最近一页，可引用旧历史消息 ID。
 * @return 读取或分页锚点无效返回 ESP_FAIL，其余返回渲染结果。
 * 读取失败保留旧页，锚点缺失时清理新消息，显示失败保留新页供 /more 重试。
 */
static esp_err_t load_history(session_menu_t *menu, const char *session_id, const char *before_id)
{
    char selected_id[sizeof(menu->preview_id)];
    snprintf(selected_id,sizeof(selected_id),"%s",session_id);
    cc_message_t *messages=NULL; size_t count=0; int more=0;
    cc_result_t rc=session_selection_preview(menu->selection,selected_id,before_id,MENU_HISTORY_LIMIT,
                                              &messages,&count,&more);
    if (rc.code != CC_OK) { publish_error(APP_EVENT_SOURCE_RUNTIME,&rc); return ESP_FAIL; }
    cc_result_free(&rc);
    if (more && (!count || !messages[0].id || !messages[0].id[0])) {
        for (size_t i=0;i<count;i++) cc_message_cleanup(&messages[i]);
        free(messages);
        app_event_publish(APP_EVENT_KIND_ERROR,APP_EVENT_SOURCE_RUNTIME,CC_LOG_ERROR,"历史记录缺少分页标识。");
        return ESP_FAIL;
    }
    history_free(menu);
    menu->history=messages; menu->history_count=count; menu->older_available=more;
    snprintf(menu->preview_id,sizeof(menu->preview_id),"%s",selected_id);
    menu->page_has_more=true; /* 显示失败时允许通过 /more 从初始游标重试。 */
    return render_history(menu,(app_history_cursor_t){0,0});
}

/**
 * @brief 显示所选会话的中断任务页及临时或备份来源提示。
 * @param[in,out] menu 非空菜单，使用运行快照和当前偏移，入口清运行可见标志。
 * @return 返回共享视图替换结果，成功后开放缓存运行编号，继续或放弃由后续命令执行。
 */
static esp_err_t render_runs(session_menu_t *menu)
{
    menu->runs_visible=false;
    char rows[MENU_ROWS][256], footer[128];
    app_event_view_item_t items[APP_EVENT_VIEW_MAX_ITEMS]; size_t n=0;
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,
        "中断任务：/continue 序号 继续，/abandon 序号 放弃；临时记录先核对输入，再用 /recover 序号 确认恢复。/sessions 返回列表。"};
    for (size_t i=menu->run_offset;i<menu->run_count && n<=MENU_ROWS;i++) {
        if (menu->run_from_temporary[i])
            snprintf(rows[n-1U],sizeof(rows[0]),"%zu. %s (需要核对的临时记录) 输入：%s",
                     i+1U,menu->run_ids[i],menu->run_input_previews[i]);
        else snprintf(rows[n-1U],sizeof(rows[0]),"%zu. %s%s",i+1U,menu->run_ids[i],
                      menu->run_from_backup[i] ? " (仅备份，不能自动继续)" : "");
        items[n]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,rows[n-1U]}; n++;
    }
    snprintf(footer,sizeof(footer),"中断任务 %zu 条；未知工具操作需单独核对。",menu->run_count);
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,footer};
    esp_err_t err=app_event_bus_replace_conversation(items,n);
    if (err==ESP_OK) { menu->runs_visible=true; menu->memory_visible=false; }
    return err;
}

/**
 * @brief 读取全部恢复运行并筛选当前所选会话，转移匹配运行 ID 到新快照。
 * @param[in,out] menu 非空菜单，匹配结果接管成功后释放旧快照并重绘第一页。
 * @return 元数据读取失败发布错误并返回 ESP_FAIL，新数组分配失败返回 ESP_ERR_NO_MEM，其余返回渲染结果。
 * 失败准备阶段清理本次临时数组，保留旧运行快照。
 */
static esp_err_t reload_runs(session_menu_t *menu)
{
    recovery_run_info_t *runs=NULL; size_t count=0;
    cc_result_t rc=recovery_service_list_runs(menu->recovery,&runs,&count);
    if (rc.code != CC_OK) { publish_error(APP_EVENT_SOURCE_RUNTIME,&rc); return ESP_FAIL; }
    cc_result_free(&rc);
    char **ids=count ? calloc(count,sizeof(*ids)) : NULL;
    bool *from_backup=count ? calloc(count,sizeof(*from_backup)) : NULL;
    bool *from_temporary=count ? calloc(count,sizeof(*from_temporary)) : NULL;
    char (*previews)[96]=count ? calloc(count,sizeof(*previews)) : NULL;
    if (count && (!ids || !from_backup || !from_temporary || !previews)) {
        free(ids); free(from_backup); free(from_temporary); free(previews);
        recovery_run_info_array_free(runs,count); return ESP_ERR_NO_MEM;
    }
    size_t matched=0;
    for (size_t i=0;i<count;i++) {
        if (strcmp(runs[i].session_id,session_selection_session_id(menu->selection))==0) {
            ids[matched]=runs[i].run_id; runs[i].run_id=NULL;
            from_backup[matched]=runs[i].recovered_from_backup;
            from_temporary[matched]=runs[i].recovered_from_temporary;
            memcpy(previews[matched],runs[i].input_preview,sizeof(previews[matched]));
            matched++;
        }
    }
    recovery_run_info_array_free(runs,count);
    runs_free(menu); menu->run_ids=ids; menu->run_from_backup=from_backup; menu->run_count=matched;
    menu->run_from_temporary=from_temporary; menu->run_input_previews=previews;
    return render_runs(menu);
}

/**
 * @brief 分配菜单并创建其拥有的 selection，依赖寿命由外层 owner 保证。
 * @param[in] controller 非空借用控制器，须与其他依赖同属一套装配。
 * @param[in] builder 非空借用 builder，存活至菜单销毁。
 * @param[in] recovery 非空借用恢复服务，存活至菜单销毁。
 * @param[out] out_menu 必填，入口置 NULL，成功取得拥有对象，由 session_menu_destroy 释放。
 * @return 分配失败返回 CC_ERR_OUT_OF_MEMORY，selection 创建错误向上传递，成功返回 CC_OK。
 */
cc_result_t session_menu_create(cc_runtime_controller_t *controller, cc_runtime_builder_t *builder,
                                recovery_service_t *recovery, session_menu_t **out_menu)
{
    if (!out_menu) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid session menu output");
    *out_menu=NULL;
    session_menu_t *menu=calloc(1,sizeof(*menu));
    if (!menu) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate session menu");
    cc_result_t rc=session_selection_create(controller,builder,recovery,&menu->selection);
    if (rc.code != CC_OK) { free(menu); return rc; }
    menu->recovery=recovery; *out_menu=menu; return cc_result_ok();
}

/**
 * @brief 核对 turns 装配身份后创建带记忆任务入口的菜单。
 * @param[in] controller 非空借用控制器，由基础创建入口核对装配。
 * @param[in] builder 与 turns 相同的借用 builder。
 * @param[in] recovery 与 turns 相同的借用恢复服务。
 * @param[in] turns 非空借用记忆任务对象，存活至菜单销毁。
 * @param[out] out_menu 必填，入口置 NULL，成功取得拥有菜单。
 * @return turns 身份不匹配返回 CC_ERR_INVALID_ARGUMENT，其余返回基础菜单创建结果。
 */
cc_result_t session_menu_create_with_memory(cc_runtime_controller_t *controller,
    cc_runtime_builder_t *builder, recovery_service_t *recovery,
    session_memory_turns_t *turns, session_menu_t **out_menu)
{
    if (!out_menu) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid session menu output");
    *out_menu=NULL;
    if (!session_memory_turns_uses(turns,builder,recovery))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Memory turn owner belongs to another runtime");
    cc_result_t rc=session_menu_create(controller,builder,recovery,out_menu);
    if (rc.code==CC_OK) (*out_menu)->memory_turns=turns;
    return rc;
}

/**
 * @brief 清理菜单拥有的会话、历史、运行、记忆任务快照及 selection。
 * @pre 外层关闭入口并排空菜单调用及回调，借用依赖继续存活。
 * @param[in,out] menu 待销毁菜单，可为 NULL。
 * @post 菜单及其拥有资源已释放，借用的 recovery、controller、builder 和 turns 由各自 owner 管理。
 */
void session_menu_destroy(session_menu_t *menu)
{
    if (!menu) return;
    cc_session_free_array(menu->sessions,menu->session_count);
    history_free(menu); runs_free(menu); memory_rows_free(menu);
    session_selection_destroy(menu->selection); free(menu);
}

/**
 * @brief 加载并展示会话选择第一页。
 * @param[in,out] menu 待展示菜单，成功读取后接管新列表，可为 NULL。
 * @return NULL 返回 ESP_ERR_INVALID_ARG，其余返回 reload_sessions 的读取或显示结果。
 */
esp_err_t session_menu_present(session_menu_t *menu)
{ return menu ? reload_sessions(menu) : ESP_ERR_INVALID_ARG; }

/**
 * @brief 查询普通聊天是否仍等待选择阶段或 ready 结果显示。
 * @param[in] menu 借用菜单，可为 NULL。
 * @return pending_ready 非空或 selection 尚未 CHATTING 时为真，NULL 为假。
 */
bool session_menu_waiting(const session_menu_t *menu)
{ return menu && (menu->pending_ready!=READY_NONE ||
                  session_selection_phase(menu->selection)!=SESSION_SELECTION_CHATTING); }

/**
 * @brief 共享显示被其他页面覆盖时清除菜单页面可见标志。
 * @param[in,out] menu 待失效菜单，可为 NULL。
 * @post 四类页面均不可见，缓存、预览 ID 和游标保留，须重新展示后恢复相关可见条件。
 */
void session_menu_invalidate_view(session_menu_t *menu)
{
    if (!menu) return;
    menu->sessions_visible=false; menu->history_visible=false; menu->runs_visible=false;
    menu->memory_visible=false;
}

/**
 * @brief 显示尚未交接的记忆任务快照、FIFO 数量和队列计费字节。
 * @param[in,out] menu 非空菜单，借用拥有的行生成文本，入口清记忆页可见标志。
 * @return 返回共享视图替换结果，成功后仅记忆页可见，后续编号操作继续按 ID 核对 turns 当前任务。
 */
static esp_err_t render_memory_pending(session_menu_t *menu)
{
    menu->memory_visible=false;
    char rows[MENU_ROWS][192], footer[160];
    app_event_view_item_t items[APP_EVENT_VIEW_MAX_ITEMS]; size_t n=0;
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,
        "记忆任务：/memory-retry 序号 核对；/memory-abandon 序号 放弃缺失任务；/memory-pending next|prev 翻页。"};
    for (size_t i=menu->memory_offset;i<menu->memory_count && n<=MENU_ROWS;i++) {
        const memory_pending_row_t *row=&menu->memory_pending[i];
        size_t length=safe_prefix(row->run_id,80U);
        snprintf(rows[n-1U],sizeof(rows[0]),"%zu. %.*s%s | %s | 答复占用 %zu 字节",
                 i+1U,(int)length,row->run_id,row->run_id[length] ? "…" : "",
                 row->ready ? "待排队" : row->local_submission ? "本机待核对" : "历史待核对",
                 row->retained_answer_bytes);
        items[n]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,rows[n-1U]}; n++;
    }
    snprintf(footer,sizeof(footer),"未排队 %zu 条，待评估 %zu 条，队列占用 %zu 字节；结果未知不代表工具未执行。",
             menu->memory_count,menu->memory_queued,menu->memory_queue_bytes);
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,footer};
    esp_err_t err=app_event_bus_replace_conversation(items,n);
    if (err==ESP_OK) {
        menu->sessions_visible=false; menu->history_visible=false; menu->runs_visible=false;
        menu->memory_visible=true;
    }
    return err;
}

/**
 * @brief 串行复制 turns 中未交接任务的 ID 和状态，并取得 FIFO 占用。
 * @param[in,out] menu 非空且具有 turns 的菜单，新行全部构造成功后替换拥有快照并重绘第一页。
 * @return 队列状态失败返回 ESP_FAIL，行分配或任务读取失败统一返回 ESP_ERR_NO_MEM，其他返回显示结果。
 * 构造失败清理新副本并保留旧快照，任务 ID 复制后独立于 turns 的借用寿命。
 */
static esp_err_t reload_memory_pending(session_menu_t *menu)
{
    size_t queued=0, reserved=0, bytes=0;
    cc_result_t status=session_memory_turns_queue_status(menu->memory_turns,&queued,&reserved,&bytes);
    if (status.code!=CC_OK) { cc_result_free(&status); return ESP_FAIL; }
    cc_result_free(&status);
    size_t count=session_memory_turns_count(menu->memory_turns);
    memory_pending_row_t *rows=count ? calloc(count,sizeof(*rows)) : NULL;
    if (count && !rows) return ESP_ERR_NO_MEM;
    for (size_t i=0;i<count;i++) {
        session_memory_pending_turn_t pending={0};
        cc_result_t rc=session_memory_turns_pending_at(menu->memory_turns,i,&pending);
        if (rc.code==CC_OK && pending.run_id) {
            size_t length=strlen(pending.run_id)+1U;
            rows[i].run_id=malloc(length);
            if (rows[i].run_id) memcpy(rows[i].run_id,pending.run_id,length);
        }
        bool good=rc.code==CC_OK && rows[i].run_id;
        cc_result_free(&rc);
        if (!good) {
            for (size_t j=0;j<count;j++) free(rows[j].run_id);
            free(rows); return ESP_ERR_NO_MEM;
        }
        rows[i].ready=pending.ready_to_commit; rows[i].local_submission=pending.local_submission;
        rows[i].retained_answer_bytes=pending.retained_answer_bytes;
    }
    memory_rows_free(menu); menu->memory_pending=rows; menu->memory_count=count;
    menu->memory_queued=queued; menu->memory_queue_bytes=bytes;
    return render_memory_pending(menu);
}

/**
 * @brief 仅在可见记忆页的数据发生变化时重新读取，稳定轮询保留页码。
 * @param[in,out] menu 待核对菜单，可为 NULL，调用期间由同一 owner 串行访问 turns。
 * @post 有变化时尝试重载第一页，查询和显示失败经事件报告，无 turns 或页面不可见时直接返回。
 */
void session_menu_refresh_memory_view(session_menu_t *menu)
{
    if (!menu || !menu->memory_turns || !menu->memory_visible) return;
    size_t queued=0,reserved=0,bytes=0;
    cc_result_t rc=session_memory_turns_queue_status(menu->memory_turns,&queued,&reserved,&bytes);
    if (rc.code!=CC_OK) { publish_error(APP_EVENT_SOURCE_RUNTIME,&rc); return; }
    cc_result_free(&rc);
    size_t count=session_memory_turns_count(menu->memory_turns);
    bool changed=count!=menu->memory_count || queued!=menu->memory_queued || bytes!=menu->memory_queue_bytes;
    for (size_t i=0;!changed && i<count;i++) {
        session_memory_pending_turn_t pending={0};
        rc=session_memory_turns_pending_at(menu->memory_turns,i,&pending);
        changed=rc.code!=CC_OK || !pending.run_id ||
            strcmp(pending.run_id,menu->memory_pending[i].run_id)!=0 ||
            pending.ready_to_commit!=menu->memory_pending[i].ready ||
            pending.local_submission!=menu->memory_pending[i].local_submission ||
            pending.retained_answer_bytes!=menu->memory_pending[i].retained_answer_bytes;
        cc_result_free(&rc);
    }
    if (changed) publish_display_error(APP_EVENT_SOURCE_RUNTIME,reload_memory_pending(menu));
}

/**
 * @brief 处理记忆任务列表、翻页、核对交接和本机缺失任务放弃。
 *
 * 编号须属于当前可见页，再按 run_id 重新查 owner。操作结果与显示刷新分别
 * 报告，inspection 的拥有答案在此释放，turn result 的借用字段归 turns。
 * @param[in,out] menu 借用 turns 的菜单，可为 NULL，成功或部分成功后重读快照。
 * @param[in] text 借用命令文本，可为 NULL，调用期间保持有效。
 * @param[in] source 状态和错误事件所属入口。
 * @return 命令匹配返回真，参数缺失、无 turns 或未匹配返回假。部分完成及未交接预留通过事件报告。
 */
bool session_menu_handle_memory(session_menu_t *menu, const char *text, app_event_source_t source)
{
    if (!menu || !menu->memory_turns || !text) return false;
    bool list=strcmp(text,"/memory-pending")==0;
    bool next=strcmp(text,"/memory-pending next")==0;
    bool prev=strcmp(text,"/memory-pending prev")==0;
    bool retry=strncmp(text,"/memory-retry ",14)==0;
    bool abandon=strncmp(text,"/memory-abandon ",16)==0;
    if (!list && !next && !prev && !retry && !abandon) return false;
    if (list) { publish_display_error(source,reload_memory_pending(menu)); return true; }
    if (next || prev) {
        if (!menu->memory_visible) { publish_display_error(source,reload_memory_pending(menu)); return true; }
        if (next && menu->memory_offset+MENU_ROWS<menu->memory_count) menu->memory_offset+=MENU_ROWS;
        if (prev && menu->memory_offset>=MENU_ROWS) menu->memory_offset-=MENU_ROWS;
        publish_display_error(source,render_memory_pending(menu)); return true;
    }
    size_t index=0;
    if (!menu->memory_visible || !parse_index(text+(abandon ? 16 : 14),&index) || index>=menu->memory_count ||
        index<menu->memory_offset || index-menu->memory_offset>=MENU_ROWS) {
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"请先查看当前记忆任务列表并选择可见序号。");
        return true;
    }
    const char *run_id=menu->memory_pending[index].run_id;
    session_memory_pending_turn_t pending={0}; bool found=false;
    for (size_t i=0;i<session_memory_turns_count(menu->memory_turns);i++) {
        cc_result_t scan=session_memory_turns_pending_at(menu->memory_turns,i,&pending);
        found=scan.code==CC_OK && pending.run_id && strcmp(pending.run_id,run_id)==0;
        cc_result_free(&scan); if (found) break;
    }
    if (!found) {
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"记忆任务已变化，请重新查看列表。");
        menu->memory_visible=false; return true;
    }
    session_memory_turn_inspection_t inspection={0};
    session_memory_turn_result_t result={0};
    cc_result_t rc=abandon ? session_memory_turns_abandon_missing(menu->memory_turns,menu->selection,run_id,&result) :
        pending.ready_to_commit ? session_memory_turns_retry_commit(menu->memory_turns,run_id) :
            session_memory_turns_reconcile(menu->memory_turns,menu->selection,run_id,&inspection);
    size_t remaining=session_memory_turns_count(menu->memory_turns);
    bool still_pending=false;
    for (size_t i=0;i<remaining;i++) {
        cc_result_t scan=session_memory_turns_pending_at(menu->memory_turns,i,&pending);
        still_pending=scan.code==CC_OK && pending.run_id && strcmp(pending.run_id,run_id)==0;
        cc_result_free(&scan); if (still_pending) break;
    }
    free(inspection.assistant_text);
    publish_display_error(source,reload_memory_pending(menu));
    if (rc.code!=CC_OK) {
        if (abandon && result.abandoned) app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_WARN,
            "任务已放弃，但记忆排队或会话状态刷新未完成；请核对后重试。");
        if (abandon && result.awaiting_resolution) app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_WARN,
            "原输入仍保留在内存中，待核对的工具不会因此解除禁用。");
        publish_error(source,&rc);
    }
    else {
        cc_result_free(&rc);
        app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,
            !still_pending ? (abandon ? "任务已放弃，输入将评估记忆；待核对的工具仍保持禁用。" :
                                       "记忆任务已进入待评估队列。") :
            inspection.state==SESSION_MEMORY_NO_DURABLE_EVIDENCE ?
                "未找到检查点或最终答复；证据不足，可核对后选择放弃。" :
                "请返回中断任务，选择继续或放弃。" );
    }
    return true;
}

/**
 * @brief 将已完成的新建或历史恢复选择提交为 ready 页面。
 * @param[in,out] menu 非空菜单，历史需要更新时先读取最近消息并选择末页游标。
 * @return 无待显示结果返回 ESP_OK，读取或锚点无效返回 ESP_FAIL，其他返回分页或共享视图结果。
 * 显示成功后清 pending_ready 并释放暂存列表和历史，失败保留待显示状态供重试，业务结果由 selection 保管。
 */
static esp_err_t render_ready(session_menu_t *menu)
{
    esp_err_t err;
    if (menu->pending_ready==READY_NEW) {
        app_event_view_item_t items[]={
            {APP_EVENT_KIND_STATUS,"新会话已开启。"},
            {APP_EVENT_KIND_ERROR,"恢复数据待修复，当前仅支持纯文本对话；所有工具已禁用。"}
        };
        err=app_event_bus_replace_conversation(items,recovery_service_history_damaged(menu->recovery) ? 2U : 1U);
    } else if (menu->pending_ready==READY_HISTORY) {
        if (menu->ready_history_needs_refresh) {
            cc_message_t *messages=NULL; size_t count=0; int more=0;
            cc_result_t rc=session_selection_current_history(menu->selection,MENU_HISTORY_LIMIT,
                                                             &messages,&count,&more);
            if (rc.code!=CC_OK) { publish_error(APP_EVENT_SOURCE_RUNTIME,&rc); return ESP_FAIL; }
            cc_result_free(&rc);
            if (more && (!count || !messages[0].id || !messages[0].id[0])) {
                for (size_t i=0;i<count;i++) cc_message_cleanup(&messages[i]);
                free(messages); return ESP_FAIL;
            }
            history_free(menu);
            menu->history=messages; menu->history_count=count; menu->older_available=more;
            err=app_history_tail_cursor(menu->history,menu->history_count,MENU_ROWS,
                                        APP_EVENT_VIEW_MAX_BYTES-1024U,&menu->visible_cursor);
            if (err!=ESP_OK) return err;
            menu->ready_history_needs_refresh=false;
        }
        err=render_history(menu,menu->visible_cursor);
    } else return ESP_OK;
    if (err==ESP_OK) {
        menu->pending_ready=READY_NONE;
        history_free(menu); runs_free(menu);
        cc_session_free_array(menu->sessions,menu->session_count);
        menu->sessions=NULL; menu->session_count=0;
    }
    return err;
}

/**
 * @brief 选择等待期间解释新建、历史预览、续行、临时记录确认及任务放弃。
 *
 * 错误和提示通过应用事件发布。若先前选择
 * 已完成但显示失败，先重试 ready 视图，成功后消费 /new、/resume、/runs、/continue 和 /abandon，
 * 其余输入返回 false 交给聊天路径。中断任务按 run_id 调用 selection/turns，
 * 完成标志、记忆待处理与返回错误分别报告。基础恢复答案在此 free，
 * memory turns 返回的答案借用至相应 owner 下一次修改调用。
 * @param[in,out] menu 菜单对象，可为 NULL，同一交互 owner 串行调用。
 * @param[in] text 借用输入，可为 NULL，调用期间保持有效。
 * @param[in] source 状态和错误事件所属入口。
 * @param[in] on_stream 继续运行时的可选同步流回调，须保持菜单和 selection 稳定。
 * @param[in,out] stream_user_data 借用回调上下文，可为 NULL。
 * @return 空参数或已就绪返回假，等待选择期间消费输入返回真，ready 重绘成功后按上述命令集合决定是否消费。
 * 命令错误、业务部分成功及页面失败都通过事件报告。
 */
bool session_menu_handle(session_menu_t *menu, const char *text, app_event_source_t source,
                         cc_runtime_controller_stream_fn on_stream, void *stream_user_data)
{
    if (!menu || !text || !session_menu_waiting(menu)) return false;
    menu->memory_visible=false;
    if (menu->pending_ready!=READY_NONE) {
        esp_err_t err=render_ready(menu);
        publish_display_error(source,err);
        if (err!=ESP_OK) return true;
        /* 下列选择命令的显示重试在此消费，其余输入继续交给聊天路径。 */
        return strcmp(text,"/new")==0 || strcmp(text,"/resume")==0 || strcmp(text,"/runs")==0 ||
               strncmp(text,"/continue ",10)==0 || strncmp(text,"/abandon ",9)==0;
    }
    session_selection_phase_t phase=session_selection_phase(menu->selection);
    if (phase==SESSION_SELECTION_CHOOSING) {
        if (recovery_service_history_damaged(menu->recovery) && strcmp(text,"/new")!=0) {
            publish_display_error(source,reload_sessions(menu));
            return true;
        }
        if (strcmp(text,"/sessions")==0) { publish_display_error(source,reload_sessions(menu)); return true; }
        if (strcmp(text,"/sessions next")==0 || strcmp(text,"/sessions prev")==0) {
            if (strcmp(text,"/sessions next")==0 && menu->session_offset+MENU_ROWS<menu->session_count)
                menu->session_offset+=MENU_ROWS;
            else if (strcmp(text,"/sessions prev")==0 && menu->session_offset>=MENU_ROWS)
                menu->session_offset-=MENU_ROWS;
            publish_display_error(source,render_sessions(menu)); return true;
        }
        if (strcmp(text,"/new")==0) {
            cc_result_t rc=session_selection_choose_new(menu->selection);
            if (rc.code!=CC_OK) publish_error(source,&rc);
            else { cc_result_free(&rc); menu->pending_ready=READY_NEW;
                   publish_display_error(source,render_ready(menu)); }
            return true;
        }
        if (strncmp(text,"/view ",6)==0) {
            size_t index=0;
            if (!menu->sessions_visible || !parse_index(text+6,&index) || index>=menu->session_count ||
                !menu->sessions[index].id || strlen(menu->sessions[index].id)>=sizeof(menu->preview_id))
                app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"会话序号无效。");
            else publish_display_error(source,load_history(menu,menu->sessions[index].id,NULL));
            return true;
        }
        if (strcmp(text,"/more")==0 && menu->preview_id[0]) {
            if (menu->page_has_more) publish_display_error(source,render_history(menu,menu->next_cursor));
            else if (menu->older_available && menu->history_count)
                publish_display_error(source,load_history(menu,menu->preview_id,menu->history[0].id));
            else app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,"没有更多历史记录。");
            return true;
        }
        if (strcmp(text,"/resume")==0 && menu->preview_id[0] && menu->history_visible) {
            cc_result_t rc=session_selection_choose_existing(menu->selection,menu->preview_id);
            if (rc.code!=CC_OK) publish_error(source,&rc);
            else {
                cc_result_free(&rc);
                cc_session_free_array(menu->sessions,menu->session_count);
                menu->sessions=NULL; menu->session_count=0;
                if (session_selection_phase(menu->selection)==SESSION_SELECTION_INTERRUPTED)
                    publish_display_error(source,reload_runs(menu));
                else { menu->pending_ready=READY_HISTORY; menu->ready_history_needs_refresh=true;
                       publish_display_error(source,render_ready(menu)); }
            }
            return true;
        }
        app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,"请先选择 /new 或使用 /view 序号 查看历史会话。");
        return true;
    }
    if (phase==SESSION_SELECTION_INTERRUPTED) {
        if (strcmp(text,"/sessions")==0 || strcmp(text,"/new")==0) {
            bool start_new=strcmp(text,"/new")==0;
            cc_result_t rc=session_selection_defer_interrupted(menu->selection);
            if (rc.code==CC_OK && start_new) rc=session_selection_choose_new(menu->selection);
            if (rc.code!=CC_OK) {
                publish_error(source,&rc);
                if (session_selection_phase(menu->selection)==SESSION_SELECTION_CHOOSING)
                    publish_display_error(source,reload_sessions(menu));
            } else {
                cc_result_free(&rc);
                runs_free(menu);
                if (start_new) {
                    menu->pending_ready=READY_NEW;
                    publish_display_error(source,render_ready(menu));
                } else publish_display_error(source,reload_sessions(menu));
                app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,
                    "中断任务已保留，待核对的工具仍保持禁用。");
            }
            return true;
        }
        if (strcmp(text,"/runs")==0) {
            cc_result_t rc=session_selection_refresh_runs(menu->selection);
            if (rc.code!=CC_OK) publish_error(source,&rc);
            else {
                cc_result_free(&rc);
                if (session_selection_phase(menu->selection)==SESSION_SELECTION_CHATTING) {
                    menu->pending_ready=READY_HISTORY; menu->ready_history_needs_refresh=true;
                    publish_display_error(source,render_ready(menu));
                } else publish_display_error(source,reload_runs(menu));
            }
            return true;
        }
        if (strcmp(text,"/runs next")==0 || strcmp(text,"/runs prev")==0) {
            if (strcmp(text,"/runs next")==0 && menu->run_offset+MENU_ROWS<menu->run_count) menu->run_offset+=MENU_ROWS;
            else if (strcmp(text,"/runs prev")==0 && menu->run_offset>=MENU_ROWS) menu->run_offset-=MENU_ROWS;
            publish_display_error(source,render_runs(menu)); return true;
        }
        if (strncmp(text,"/recover ",9)==0) {
            size_t index=0;
            if (!menu->runs_visible || !parse_index(text+9,&index) || index>=menu->run_count ||
                !menu->run_from_temporary[index]) {
                app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"需要核对的临时记录序号无效。");
                return true;
            }
            cc_result_t rc=recovery_service_confirm_temporary_run(menu->recovery,menu->run_ids[index]);
            if (rc.code!=CC_OK) publish_error(source,&rc);
            else {
                cc_result_free(&rc);
                publish_display_error(source,reload_runs(menu));
                app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,
                    "临时记录已确认恢复。可使用 /continue 序号 继续，或 /abandon 序号 放弃。");
            }
            return true;
        }
        bool resume=strncmp(text,"/continue ",10)==0, abandon=strncmp(text,"/abandon ",9)==0;
        if (resume || abandon) {
            size_t index=0;
            if (!menu->runs_visible || !parse_index(text+(resume ? 10 : 9),&index) || index>=menu->run_count) {
                app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"中断任务序号无效。"); return true;
            }
            cc_result_t rc; char *owned_response=NULL; const char *response=NULL;
            bool completed=false, abandoned=false;
            bool awaiting_memory=false;
            if (menu->memory_turns) {
                session_memory_turn_result_t result={0};
                if (resume) rc=session_memory_turns_resume(menu->memory_turns,menu->selection,
                    menu->run_ids[index],on_stream,stream_user_data,&result);
                else rc=session_memory_turns_abandon(menu->memory_turns,menu->selection,
                    menu->run_ids[index],&result);
                response=result.response; completed=result.completed; abandoned=result.abandoned;
                awaiting_memory=result.awaiting_resolution;
            } else if (resume) {
                rc=session_selection_resume_run_ex(menu->selection,menu->run_ids[index],
                                                   on_stream,stream_user_data,&owned_response,&completed);
                response=owned_response;
            } else rc=session_selection_abandon_run(menu->selection,menu->run_ids[index]);
            if (rc.code!=CC_OK) {
                if ((completed || abandoned) && session_selection_phase(menu->selection)==SESSION_SELECTION_CHATTING) {
                    menu->pending_ready=READY_HISTORY; menu->ready_history_needs_refresh=true;
                    publish_display_error(source,render_ready(menu));
                } else if ((completed || abandoned) && session_selection_phase(menu->selection)==SESSION_SELECTION_INTERRUPTED) {
                    publish_display_error(source,reload_runs(menu));
                }
                if (completed) {
                    if (response && response[0])
                        app_event_publish(APP_EVENT_KIND_ASSISTANT,APP_EVENT_SOURCE_RUNTIME,CC_LOG_INFO,response);
                    app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,
                                      "任务已完成，但记忆排队或会话状态刷新失败；请核对后重试。");
                }
                if (abandoned) app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,
                    "任务已放弃，但记忆排队或会话状态刷新失败；请核对后重试。");
                if (awaiting_memory) app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_WARN,
                    "该任务的记忆评估仍待处理；原始输入与答复已保留在内存中。");
                publish_error(source,&rc);
            }
            else {
                cc_result_free(&rc);
                if (session_selection_phase(menu->selection)==SESSION_SELECTION_INTERRUPTED) {
                    esp_err_t display=reload_runs(menu);
                    publish_display_error(source,display);
                    /* 任务快照替换会覆盖此前流式内容，完成答复在新快照成功后再次发布。 */
                    if (display==ESP_OK && response && response[0])
                        app_event_publish(APP_EVENT_KIND_ASSISTANT,APP_EVENT_SOURCE_RUNTIME,CC_LOG_INFO,response);
                } else {
                    menu->pending_ready=READY_HISTORY; menu->ready_history_needs_refresh=true;
                    publish_display_error(source,render_ready(menu));
                    app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,"待核对的工具仍保持禁用。");
                }
            }
            free(owned_response);
            return true;
        }
        app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,"请先选择继续或放弃中断任务。");
        return true;
    }
    app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"存储不可用，会话选择已暂停。");
    return true;
}

/**
 * @brief 菜单就绪且有 turns 时提交普通对话，进入中断状态时另行刷新任务页。
 * @param[in,out] menu 非空且就绪的菜单，提交前关闭记忆任务页可见标志。
 * @param[in] text 非空用户原输入，调用期间借用，须满足 turns 对旧答复的别名约束。
 * @param[in] on_stream 可选同步流回调，须遵守菜单及 turns 的回调约束。
 * @param[in,out] stream_user_data 借用回调上下文，可为 NULL。
 * @param[out] out_result 必填独立输出，入口清零，字符串借自 turns 至其下一次修改调用。
 * @return 输出缺失返回参数错误，菜单未就绪返回状态错误，其余原样返回 turns 提交结果。
 * 即使失败也检查 completed 和 awaiting_resolution，显示错误通过独立事件报告。
 */
cc_result_t session_menu_submit_turn(session_menu_t *menu, const char *text,
    cc_runtime_controller_stream_fn on_stream, void *stream_user_data,
    session_memory_turn_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid chat output");
    memset(out_result,0,sizeof(*out_result));
    if (!menu || !menu->memory_turns || session_menu_waiting(menu))
        return cc_result_error(CC_ERR_INVALID_STATE,"Session is not ready for a memory-managed turn");
    menu->memory_visible=false;
    cc_result_t rc=session_memory_turns_submit(menu->memory_turns,menu->selection,text,
                                               on_stream,stream_user_data,out_result);
    if (session_selection_phase(menu->selection)==SESSION_SELECTION_INTERRUPTED)
        publish_display_error(APP_EVENT_SOURCE_RUNTIME,reload_runs(menu));
    return rc;
}
