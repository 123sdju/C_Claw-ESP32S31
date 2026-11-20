#include "recovery_console.h"
#include "history_view.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONSOLE_ROWS (APP_EVENT_VIEW_MAX_ITEMS - 2U)

struct recovery_console {
    recovery_service_t *recovery;
    cc_pending_operation_t *operations;
    size_t count, offset;
    bool loaded;
    bool list_visible;
    cc_message_t *detail;
    app_history_cursor_t next;
    bool more;
};

/** @brief 为格式化取得非空文本借用。
 * @param[in] text 可为 NULL 的调用期文本。
 * @return 原指针或进程期空串，无释放责任。
 */
static const char *safe(const char *text) { return text ? text : ""; }

/** @brief 清理详情消息并复位分页游标。
 * @param[in,out] console 必填 owner 独占对象，detail 可为空。
 * @post 消费详情所有权，后续 more 失效。
 */
static void detail_free(recovery_console_t *console)
{
    cc_message_destroy(console->detail); console->detail=NULL;
    memset(&console->next,0,sizeof(console->next)); console->more=false;
}

/** @brief 发布并消费一份底层错误诊断。
 * @param[in] source 产品事件来源。
 * @param[in,out] rc 必填独占结果，消息在同步发布期间借用，随后 result_free。
 */
static void report_result(app_event_source_t source, cc_result_t *rc)
{
    app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
                      rc->message ? rc->message : cc_error_string(rc->code));
    cc_result_free(rc);
}

/** @brief 显示失败时发布可重试提示。
 * @param[in] source 事件来源。
 * @param[in] err 显示返回码，ESP_OK 时跳过。
 */
static void report_display(app_event_source_t source, esp_err_t err)
{
    if (err!=ESP_OK)
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"待核对操作页面显示失败，请重试。");
}

/** @brief 计算可放入预算的 UTF-8 前缀长度。
 * @param[in] text 必填 NUL 文本借用。
 * @param[in] maximum 字节预算，零允许。
 * @return 不跨越续字节的前缀字节数，原文保持。
 */
static size_t utf8_prefix(const char *text, size_t maximum)
{
    size_t len=strlen(text);
    if (len<=maximum) return len;
    while (maximum && ((unsigned char)text[maximum] & 0xc0U)==0x80U) maximum--;
    return maximum;
}

/** @brief 将人读的一基序号转换为数组下标。
 * @param[in] text 可为 NULL，接受十进制和尾部空格/制表，拒绝零、符号及溢出。
 * @param[out] out 必填下标槽，仅成功时写入。
 * @return 完整合法序号为 true，无分配及所有权交接。
 */
static bool parse_index(const char *text, size_t *out)
{
    if (!text || !*text || *text=='-' || *text=='+') return false;
    errno=0; char *end=NULL; unsigned long number=strtoul(text,&end,10);
    while (end && (*end==' ' || *end=='\t')) end++;
    if (errno || !end || *end || !number || number>SIZE_MAX) return false;
    *out=(size_t)number-1U; return true;
}

/** @brief 将当前操作快照页复制给同步显示入口。
 * @param[in,out] console 必填 owner 对象，operations/count/offset 已合法装配。
 * @return 显示入口结果，成功释放旧详情并标记列表可见，失败保持列表不可选择。
 * @pre 行栈缓冲只活到同步调用返回，显示入口负责复制。
 */
static esp_err_t render_list(recovery_console_t *console)
{
    console->list_visible=false;
    char rows[CONSOLE_ROWS][224], footer[160];
    app_event_view_item_t items[APP_EVENT_VIEW_MAX_ITEMS]; size_t n=0;
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,
        "待核对工具：/pending view 序号 查看；/pending next|prev 翻页。"};
    for (size_t i=console->offset;i<console->count && n<=CONSOLE_ROWS;i++) {
        const cc_pending_operation_t *op=&console->operations[i];
        const char *id=safe(op->operation_id), *name=safe(op->call.name);
        size_t id_len=utf8_prefix(id,75U), name_len=utf8_prefix(name,80U);
        snprintf(rows[n-1U],sizeof(rows[0]),"%zu. %.*s%s | %.*s%s%s",i+1U,
                 (int)name_len,name,name[name_len] ? "…" : "",(int)id_len,id,id[id_len] ? "…" : "",
                 op->in_progress ? " (执行中)" : op->reconciling ? " (核对中)" : "");
        items[n]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,rows[n-1U]}; n++;
    }
    snprintf(footer,sizeof(footer),"共 %zu 条；核对后用 /resolve 操作ID executed 说明 或 not-executed。",console->count);
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,footer};
    esp_err_t err=app_event_bus_replace_conversation(items,n);
    if (err==ESP_OK) { detail_free(console); console->list_visible=true; }
    return err;
}

/** @brief 从当前盘重读拥有的操作快照并显示首页。
 * @param[in,out] console 必填 owner 对象，借用 recovery 存活。
 * @param[in] source 读取失败事件来源。
 * @return 读取失败发布原诊断后返回 ESP_FAIL，否则透传显示结果。
 * @post 读取成功才替换并释放旧数组，显示失败仍保留新快照供重试。
 */
static esp_err_t reload(recovery_console_t *console, app_event_source_t source)
{
    cc_pending_operation_t *ops=NULL; size_t count=0;
    cc_result_t rc=recovery_service_list_operations(console->recovery,&ops,&count);
    if (rc.code!=CC_OK) { report_result(source,&rc); return ESP_FAIL; }
    cc_result_free(&rc);
    cc_pending_operation_array_free(console->operations,console->count);
    console->operations=ops; console->count=count; console->offset=0;
    console->loaded=true; console->list_visible=false;
    return render_list(console);
}

/** @brief 脱敏详情后提交一页显示。
 * @param[in,out] console 必填 owner 对象，detail 已装配且由对象拥有。
 * @param[in] start 当前详情脱敏后的游标。
 * @return 页构建或显示错误，成功才推进 next/more 并使列表失效。
 * @post 临时页面正文在同步显示复制后 cleanup。
 */
static esp_err_t render_detail(recovery_console_t *console, app_history_cursor_t start)
{
    app_history_page_t page={0};
    esp_err_t err=app_history_page_build(console->detail,1,start,CONSOLE_ROWS,
                                         APP_EVENT_VIEW_MAX_BYTES-1024U,&page);
    if (err!=ESP_OK) return err;
    app_event_view_item_t items[APP_EVENT_VIEW_MAX_ITEMS]; size_t n=0;
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,"核对操作详情；确认结果前请检查外部系统。"};
    for (size_t i=0;i<page.count;i++) items[n++]=page.items[i];
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,
        page.has_more ? "输入 /pending more 查看后续内容。" :
                        "使用 /resolve 操作ID executed 说明 或 /resolve 操作ID not-executed。"};
    err=app_event_bus_replace_conversation(items,n);
    if (err==ESP_OK) { console->next=page.next; console->more=page.has_more; console->list_visible=false; }
    app_history_page_cleanup(&page); return err;
}

/** @brief 为指定操作创建独有详情消息并显示首屏。
 * @param[in,out] console 必填串行对象，已有操作快照。
 * @param[in] index 已由命令分支校验的当前可见页下标。
 * @param[in] source 消息创建失败事件来源。
 * @return 分配、格式化、消息创建或显示结果，首屏失败保留详情供 more 重试。
 */
static esp_err_t show_detail(recovery_console_t *console, size_t index, app_event_source_t source)
{
    const cc_pending_operation_t *op=&console->operations[index];
    const char *fmt="操作ID: %s\n会话: %s\n工具: %s\n调用ID: %s\n参数: %s";
    int length=snprintf(NULL,0,fmt,safe(op->operation_id),safe(op->session_id),safe(op->call.name),
                        safe(op->call.id),safe(op->call.arguments_json));
    if (length<0) return ESP_FAIL;
    char *body=malloc((size_t)length+1U);
    if (!body) return ESP_ERR_NO_MEM;
    snprintf(body,(size_t)length+1U,fmt,safe(op->operation_id),safe(op->session_id),safe(op->call.name),
             safe(op->call.id),safe(op->call.arguments_json));
    cc_message_t *message=NULL;
    cc_result_t rc=cc_message_create_text("pending-detail","pending",CC_ROLE_TOOL,body,NULL,&message);
    free(body);
    if (rc.code!=CC_OK) { report_result(source,&rc); return ESP_FAIL; }
    cc_result_free(&rc);
    detail_free(console); console->detail=message;
    console->more=true; /* 首屏失败后通过 /pending more 重试。 */
    return render_detail(console,(app_history_cursor_t){0,0});
}

/** @brief 创建绑定当前盘恢复服务的 owner 控制台。
 * @param[in] recovery 必填当前盘借用，寿命覆盖控制台。
 * @param[out] out_console 必填新槽，成功交付拥有对象，参数失败保留原槽，分配失败写 NULL。
 * @return CC_OK、INVALID_ARGUMENT 或 OUT_OF_MEMORY，结果诊断由调用者释放。
 * @pre 所有对象 API 由交互 owner 串行调用。
 */
cc_result_t recovery_console_create(recovery_service_t *recovery, recovery_console_t **out_console)
{
    if (!recovery || !out_console || !recovery_service_is_current(recovery))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid recovery console dependencies");
    *out_console=calloc(1,sizeof(**out_console));
    if (!*out_console) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate recovery console");
    (*out_console)->recovery=recovery; return cc_result_ok();
}

/** @brief 消费控制台及其快照和详情。
 * @param[in,out] console 可为 NULL 的拥有对象，函数不清调用者变量。
 * @pre 入口关闭且全部调用退出，借用 recovery 由外层另行管理。
 */
void recovery_console_destroy(recovery_console_t *console)
{
    if (!console) return;
    cc_pending_operation_array_free(console->operations,console->count);
    detail_free(console); free(console);
}

/** @brief 显示被替换或清空后使旧序号与详情失效。
 * @param[in,out] console 可为 NULL 的 owner 对象。
 * @post 释放操作和详情快照，loaded/list_visible/more 归 false，账本保持。
 */
void recovery_console_invalidate_view(recovery_console_t *console)
{
    if (!console) return;
    cc_pending_operation_array_free(console->operations,console->count);
    console->operations=NULL; console->count=0; console->offset=0;
    console->loaded=false; console->list_visible=false; detail_free(console);
}

/** @brief 判断文本是否属于待核对操作命令。
 * @param[in] text 可为 NULL 的调用期 NUL 文本。
 * @return 精确 /pending、/resolve 或其空格参数前缀为 true。
 */
bool recovery_console_handles(const char *text)
{
    return text && (strcmp(text,"/pending")==0 || strncmp(text,"/pending ",9)==0 ||
                    strcmp(text,"/resolve")==0 || strncmp(text,"/resolve ",9)==0);
}

/** @brief 解析人工核对结果并写当前盘操作账本。
 * @param[in,out] console 必填串行对象，当前盘已由上层核对。
 * @param[in] args 必填调用期参数，ID 小于 96 B，executed 要求非空说明。
 * @param[in] source 诊断及成功事件来源。
 * @post 调用恢复服务记录 disposition，成功后重读显示；命令本身不调用工具。
 */
static void resolve(recovery_console_t *console, const char *args, app_event_source_t source)
{
    while (*args==' ' || *args=='\t') args++;
    const char *end=args; while (*end && *end!=' ' && *end!='\t') end++;
    size_t id_len=(size_t)(end-args);
    if (!id_len || id_len>=96U) goto usage;
    char id[96]; memcpy(id,args,id_len); id[id_len]='\0';
    while (*end==' ' || *end=='\t') end++;
    cc_tool_recovery_disposition_t disposition;
    const char *observation=NULL;
    if (strcmp(end,"not-executed")==0) disposition=CC_TOOL_RECOVERY_NOT_EXECUTED;
    else if (strncmp(end,"executed ",9)==0) {
        disposition=CC_TOOL_RECOVERY_EXECUTED; observation=end+9;
        while (*observation==' ' || *observation=='\t') observation++;
        if (!*observation) goto usage;
    } else goto usage;
    cc_result_t rc=recovery_service_resolve_operation(console->recovery,id,disposition,observation);
    if (rc.code!=CC_OK) { report_result(source,&rc); return; }
    cc_result_free(&rc);
    app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,"已记录核对结果；该命令没有执行工具。");
    report_display(source,reload(console,source));
    return;
usage:
    app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
        "用法：/resolve 操作ID executed 核对说明，或 /resolve 操作ID not-executed。");
}

/** @brief 在 owner 中解释列表、详情翻页与人工核对命令。
 * @param[in,out] console 可为 NULL 的串行对象，NULL 跳过。
 * @param[in] text 可为 NULL 的调用期文本，非所属命令跳过。
 * @param[in] source 所有显示错误及结果事件来源。
 * @post 挂载失效清视图并提示，view 必须属于当前可见页，more 成功才推进游标。
 * @pre recovery 由外层保管至全部调用退出，函数以事件表达失败。
 */
void recovery_console_handle(recovery_console_t *console, const char *text, app_event_source_t source)
{
    if (!console || !recovery_console_handles(text)) return;
    if (!recovery_service_is_current(console->recovery)) {
        recovery_console_invalidate_view(console);
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"恢复存储不可用，核对操作已暂停。");
        return;
    }
    if (strcmp(text,"/pending")==0) { report_display(source,reload(console,source)); return; }
    if (strcmp(text,"/pending next")==0 || strcmp(text,"/pending prev")==0) {
        if (!console->loaded) { report_display(source,reload(console,source)); return; }
        if (strcmp(text,"/pending next")==0 && console->offset+CONSOLE_ROWS<console->count) console->offset+=CONSOLE_ROWS;
        else if (strcmp(text,"/pending prev")==0 && console->offset>=CONSOLE_ROWS) console->offset-=CONSOLE_ROWS;
        report_display(source,render_list(console)); return;
    }
    if (strncmp(text,"/pending view ",14)==0) {
        size_t index=0;
        if (!console->list_visible || !parse_index(text+14,&index) || index<console->offset ||
            index>=console->count || index-console->offset>=CONSOLE_ROWS)
            app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"请从当前待核对列表选择有效序号。");
        else report_display(source,show_detail(console,index,source));
        return;
    }
    if (strcmp(text,"/pending more")==0 && console->detail && console->more) {
        report_display(source,render_detail(console,console->next)); return;
    }
    if (strncmp(text,"/resolve ",9)==0) { resolve(console,text+9,source); return; }
    app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,
        "输入 /pending 查看待核对操作，或 /resolve 操作ID executed 说明 / not-executed。");
}
