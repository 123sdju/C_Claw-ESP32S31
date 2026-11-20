#include "memory_console.h"
#include "history_view.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMORY_ROWS (APP_EVENT_VIEW_MAX_ITEMS - 2U)

struct memory_console {
    cc_managed_memory_editor_t *editor;
    recovery_service_t *recovery;
    session_memory_turns_t *turns;
    cc_memory_plan_t state;
    uint64_t revision;
    size_t offset, selected;
    bool loaded, list_visible, detail_visible;
    cc_message_t *detail;
    app_history_cursor_t next;
    bool more;
    char *reset_snapshot_id;
    uint64_t reset_revision;
};

/**
 * @brief 清理详情页拥有的消息及游标，所选记忆计划由 console 的 state 另行持有。
 * @param[in,out] console 非空控制台，释放其 detail 并复位详情分页状态。
 * @post 详情消息已释放，详情可见标志、分页游标和 more 状态已清理。
 */
static void detail_free(memory_console_t *console)
{
    cc_message_destroy(console->detail); console->detail=NULL;
    console->detail_visible=false; console->more=false;
    memset(&console->next,0,sizeof(console->next));
}

/**
 * @brief 同步视图被覆盖或版本失效时释放计划/详情，清除可见性与列表 revision。
 *
 * 重置重试用的快照 ID 单独保留，由 reset_revision 核对其适用范围。
 * @param[in,out] console 待失效控制台，可为 NULL。
 * @post 计划与详情已释放，页面标志失效，重置快照关联继续由 revision 管理。
 */
void memory_console_invalidate_view(memory_console_t *console)
{
    if (!console) return;
    cc_memory_plan_cleanup(&console->state); detail_free(console);
    console->loaded=false; console->list_visible=false; console->revision=0; console->offset=0;
}

/**
 * @brief 将错误结果发布为事件并释放其拥有的诊断资源。
 * @param[in] source 命令或诊断所属入口，用于事件归因。
 * @param[in,out] rc 非空错误结果，有正文时发布正文，否则发布错误码对应说明，随后调用 cc_result_free。
 * @post 已尝试发布错误事件，结果拥有的资源已释放。
 */
static void report_error(app_event_source_t source, cc_result_t *rc)
{
    app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
                      rc->message ? rc->message : cc_error_string(rc->code));
    cc_result_free(rc);
}

/**
 * @brief 将显示失败转为诊断事件，实际记忆提交状态由修改路径独立报告。
 * @param[in] source 命令或诊断所属入口，用于事件归因。
 * @param[in] err 页面替换或渲染返回的 ESP 错误码。
 * @post 显示失败时发布诊断事件，成功时结束调用。
 */
static void report_display(app_event_source_t source, esp_err_t err)
{
    if (err!=ESP_OK) app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"记忆页面显示失败，请重试。");
}

/**
 * @brief 对有效 UTF-8 C 字符串选择预算内字符边界，编码有效性由上游保证。
 * @param[in] text 上游保证有效的 UTF-8 C 字符串，调用期间借用。
 * @param[in] maximum 允许保留的最大 UTF-8 正文字节数。
 * @return 预算内完整 UTF-8 字符前缀的字节数。
 */
static size_t safe_prefix(const char *text, size_t maximum)
{
    size_t length=strlen(text);
    if (length<=maximum) return length;
    while (maximum && ((unsigned char)text[maximum] & 0xc0U)==0x80U) maximum--;
    return maximum;
}

/**
 * @brief 解析非零十进制序号并返回零基索引，out_rest 借用尾部正文起点。
 *
 * 具体命令继续验证正文、页内可见性和索引范围。
 * @param[in] text NUL 结尾的序号及可选正文，调用期间借用，可为 NULL。
 * @param[out] out 必填，成功时写入零基索引。
 * @param[out] out_rest 可选输出，指向输入内剩余正文的借用地址。
 * @return 序号合法时为真并填写输出，失败时为假，调用者只在成功后读取输出。
 */
static bool parse_index(const char *text, size_t *out, const char **out_rest)
{
    if (!text || !*text || *text=='-' || *text=='+') return false;
    errno=0; char *end=NULL; unsigned long number=strtoul(text,&end,10);
    if (errno || !end || end==text || !number || number>SIZE_MAX || (*end && *end!=' ' && *end!='\t')) return false;
    while (*end==' ' || *end=='\t') end++;
    *out=(size_t)number-1U; if (out_rest) *out_rest=end;
    return true;
}

/**
 * @brief 限定当前页可见项或正在显示的单条详情，state 在 owner 串行调用间保持。
 * @param[in] console 非空控制台，借用其计划及列表、详情可见状态。
 * @param[in] index 计划记录的零基索引，本函数同时检查记录范围与页面可见性。
 * @return 索引属于当前可见列表页或详情时为真，否则为假。
 */
static bool visible_index(const memory_console_t *console, size_t index)
{
    return index<console->state.count &&
        ((console->list_visible && index>=console->offset && index-console->offset<MEMORY_ROWS) ||
         (console->detail_visible && index==console->selected));
}

/**
 * @brief 按拥有的计划构建列表页，成功复制到同步显示后才设置 list_visible。
 *
 * 行数据在本次栈中，成功后清理旧详情，失败保留计划供重试。
 * @param[in,out] console 非空控制台，借用 state 生成行，成功时释放旧详情并显示列表。
 * @return 返回共享对话视图替换结果，ESP_OK 后列表才标记为可见。
 */
static esp_err_t render_list(memory_console_t *console)
{
    console->list_visible=false;
    char rows[MEMORY_ROWS][192], footer[128];
    app_event_view_item_t items[APP_EVENT_VIEW_MAX_ITEMS]; size_t n=0;
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,
        "长期记忆：/memory view 序号 查看；/memories next|prev 翻页。"};
    for (size_t i=console->offset;i<console->state.count && n<=MEMORY_ROWS;i++) {
        const cc_memory_plan_record_t *record=&console->state.records[i];
        size_t id_len=safe_prefix(record->id,90U), category_len=safe_prefix(record->category,42U);
        snprintf(rows[n-1U],sizeof(rows[0]),"%zu. %.*s%s | %.*s%s | %u",i+1U,
                 (int)id_len,record->id,record->id[id_len] ? "…" : "",
                 (int)category_len,record->category,record->category[category_len] ? "…" : "",record->importance);
        items[n]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,rows[n-1U]}; n++;
    }
    snprintf(footer,sizeof(footer),"受管理记忆 %zu 条；编辑、删除或 /memory reset 重置。",console->state.count);
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,footer};
    esp_err_t err=app_event_bus_replace_conversation(items,n);
    if (err==ESP_OK) { detail_free(console); console->list_visible=true; }
    return err;
}

/**
 * @brief 从 editor 取得拥有的当前计划/revision，接管成功结果后重绘第一页。
 *
 * 读取失败保留旧 state，显示失败保留新 state 但保持列表不可见。
 * @param[in,out] console 非空控制台，读取成功时替换拥有的计划并复位页偏移与详情。
 * @param[in] source 命令或诊断所属入口，用于事件归因。
 * @return 返回读取或列表渲染结果，读取失败为 ESP_FAIL，渲染错误向上传递。
 */
static esp_err_t reload(memory_console_t *console, app_event_source_t source)
{
    cc_memory_plan_t state={0}; uint64_t revision=0;
    cc_result_t rc=cc_managed_memory_editor_list(console->editor,&state,&revision);
    if (rc.code!=CC_OK) { report_error(source,&rc); return ESP_FAIL; }
    cc_result_free(&rc); cc_memory_plan_cleanup(&console->state); detail_free(console);
    console->state=state; console->revision=revision; console->offset=0;
    console->loaded=true; console->list_visible=false; console->detail_visible=false;
    return render_list(console);
}

/**
 * @brief 按显示预算把拥有的详情消息切页，成功后更新下一游标和可见标志。
 *
 * 临时 page 在本函数清理，详情原消息仍由 console 保管。
 * @param[in,out] console 非空控制台，借用 detail 构建页面，成功显示后更新游标与可见状态。
 * @param[in] cursor 详情正文的分页起点。
 * @return 返回分页构建或共享视图替换结果，成功后更新下一页游标。
 */
static esp_err_t render_detail(memory_console_t *console, app_history_cursor_t cursor)
{
    app_history_page_t page={0};
    esp_err_t err=app_history_page_build(console->detail,1,cursor,MEMORY_ROWS,
                                         APP_EVENT_VIEW_MAX_BYTES-1024U,&page);
    if (err!=ESP_OK) return err;
    app_event_view_item_t items[APP_EVENT_VIEW_MAX_ITEMS]; size_t n=0;
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,"长期记忆详情"};
    for (size_t i=0;i<page.count;i++) items[n++]=page.items[i];
    items[n++]=(app_event_view_item_t){APP_EVENT_KIND_STATUS,
        page.has_more ? "输入 /memory more 查看后续内容。" :
                        "使用 /memory edit 序号 内容 或 /memory delete 序号。"};
    err=app_event_bus_replace_conversation(items,n);
    if (err==ESP_OK) {
        console->next=page.next; console->more=page.has_more;
        console->list_visible=false; console->detail_visible=true;
    }
    app_history_page_cleanup(&page); return err;
}

/**
 * @brief 从已验证的索引生成详情消息，正文临时缓冲在复制后释放。
 *
 * 新消息由 console 接管。显示失败仍保留消息，more 为真，next 为初始游标，支持后续重试。
 * @param[in,out] console 非空控制台，成功创建消息后释放旧详情并接管新消息。
 * @param[in] index 已由调用者验证有效的计划记录零基索引，本函数不再校验范围。
 * @param[in] source 命令或诊断所属入口，用于事件归因。
 * @return 正文缓冲分配失败返回 ESP_ERR_NO_MEM，格式化或消息创建失败返回 ESP_FAIL，
 * 消息创建成功后返回详情渲染结果。
 */
static esp_err_t show_detail(memory_console_t *console, size_t index, app_event_source_t source)
{
    const cc_memory_plan_record_t *record=&console->state.records[index];
    const char *fmt="序号: %zu\nID: %s\n分类: %s\n重要度: %u\n内容: %s";
    int length=snprintf(NULL,0,fmt,index+1U,record->id,record->category,record->importance,record->text);
    if (length<0) return ESP_FAIL;
    char *body=malloc((size_t)length+1U);
    if (!body) return ESP_ERR_NO_MEM;
    snprintf(body,(size_t)length+1U,fmt,index+1U,record->id,record->category,record->importance,record->text);
    cc_message_t *message=NULL;
    cc_result_t rc=cc_message_create_text("memory-detail","memory",CC_ROLE_SYSTEM,body,NULL,&message);
    free(body);
    if (rc.code!=CC_OK) { report_error(source,&rc); return ESP_FAIL; }
    cc_result_free(&rc); detail_free(console); console->detail=message; console->selected=index; console->more=true;
    return render_detail(console,(app_history_cursor_t){0,0});
}

/**
 * @brief owner 核对 editor 使用当前 recovery 的 store 后创建控制台，依赖仅借用。
 *
 * 成功输出拥有对象，失败输出为空。
 * 后续操作由同一个交互 owner 串行调用。
 * @param[in] editor 非空编辑器，借用至 console 销毁。
 * @param[in] recovery 非空恢复服务，须与 editor 使用同一 store，借用至 console 销毁。
 * @param[out] out_console 必填，先置 NULL，成功取得拥有的控制台，使用 memory_console_destroy 释放。
 * @return CC_OK 表示创建成功，参数或依赖不符返回 CC_ERR_INVALID_ARGUMENT，分配失败返回 CC_ERR_OUT_OF_MEMORY。
 */
cc_result_t memory_console_create(cc_managed_memory_editor_t *editor, recovery_service_t *recovery,
                                  memory_console_t **out_console)
{
    if (!out_console) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory console output");
    *out_console=NULL;
    if (!editor || !recovery ||
        !cc_managed_memory_editor_uses_store(editor,recovery_service_borrow_memory_store(recovery)))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid memory console dependencies");
    memory_console_t *console=calloc(1,sizeof(*console));
    if (!console) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot allocate memory console");
    console->editor=editor; console->recovery=recovery; *out_console=console; return cc_result_ok();
}

/**
 * @brief 验证 turns 借出的 editor 与参数相同，再创建支持队列排空检查的重置入口。
 * @param[in] editor 非空编辑器，借用至 console 销毁。
 * @param[in] recovery 与 editor 使用同一 store 的恢复服务，借用至 console 销毁。
 * @param[in] turns 非空任务槽对象，须借出同一 editor，借用至 console 销毁。
 * @param[out] out_console 必填拥有对象输出。turns 检查失败时保持原值，通过检查后由 create 清零。
 * @return 返回依赖检查或创建结果，成功后将 turns 的借用存入控制台。
 */
cc_result_t memory_console_create_with_turns(cc_managed_memory_editor_t *editor,
    recovery_service_t *recovery, session_memory_turns_t *turns, memory_console_t **out_console)
{
    if (!turns || session_memory_turns_editor(turns)!=editor)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Memory queue and editor do not match");
    cc_result_t rc=memory_console_create(editor,recovery,out_console);
    if (rc.code==CC_OK) (*out_console)->turns=turns;
    return rc;
}

/**
 * @brief owner 关闭入口并排空调用后释放计划、详情、重试快照 ID 与对象本身。
 * @pre 交互入口已关闭，owner 的控制台调用已退出。
 * @param[in,out] console 待销毁对象，可为 NULL。
 * @post 控制台拥有的页面、详情及重置快照 ID 已释放，控制台寿命结束。
 */
void memory_console_destroy(memory_console_t *console)
{
    if (!console) return;
    memory_console_invalidate_view(console); free(console->reset_snapshot_id); free(console);
}

/**
 * @brief 仅在当前列表可见、受管理集合非空且 FIFO/预留均已排空时请求重置。
 *
 * 保管已创建的快照 ID，revision 匹配时供重试复用。
 * 成功后刷新列表。
 * 安装与显示结果分别报告，写入结果未知仍应结合存储恢复状态核对。
 * @param[in,out] console 非空控制台，须已显示当前列表，持有的快照 ID 按列表 revision 复用。
 * @param[in] source 命令或诊断所属入口，用于事件归因。
 * @post 重置、核对要求或拒绝原因通过事件报告，成功后尝试刷新列表。
 */
static void reset_memory(memory_console_t *console, app_event_source_t source)
{
    if (!console->loaded || !console->list_visible) {
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"请先输入 /memories 查看当前记忆。");
        return;
    }
    if (!console->state.count) {
        app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,"当前没有需要重置的长期记忆。");
        return;
    }
    if (!console->turns) {
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"记忆队列不可用，暂时不能重置。");
        return;
    }
    size_t ready=0,reserved=0;
    cc_result_t status=session_memory_turns_queue_status(console->turns,&ready,&reserved,NULL);
    if (status.code!=CC_OK) { report_error(source,&status); return; }
    cc_result_free(&status);
    if (ready || reserved) {
        app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_WARN,
            "仍有待处理的对话记忆；请先完成处理，再重置长期记忆。");
        return;
    }
    if (console->reset_snapshot_id && console->reset_revision!=console->revision) {
        free(console->reset_snapshot_id);
        console->reset_snapshot_id=NULL;
    }
    char *snapshot_id=NULL; int installed=0;
    cc_result_t rc=cc_managed_memory_editor_reset(console->editor,
        cc_managed_memory_editor_epoch(console->editor),console->revision,
        console->reset_snapshot_id,&snapshot_id,&installed);
    if (snapshot_id) {
        free(console->reset_snapshot_id);
        console->reset_snapshot_id=snapshot_id;
        console->reset_revision=console->revision;
    }
    if (rc.code==CC_OK && !recovery_service_is_current(console->recovery)) {
        cc_result_free(&rc);
        memory_console_invalidate_view(console);
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
            "U 盘在记忆重置期间发生变化，请恢复原介质并核对结果。");
        return;
    }
    if (rc.code!=CC_OK) {
        bool stale=rc.code==CC_ERR_INVALID_STATE || installed;
        bool storage_error=rc.code==CC_ERR_STORAGE || rc.code==CC_ERR_IO;
        report_error(source,&rc);
        if (stale) memory_console_invalidate_view(console);
        if (installed) app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
            "记忆可能已重置但同步结果未知，请恢复存储并核对后再操作。");
        else if (storage_error) app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_WARN,
            "本次记忆重置未获成功确认，请检查 U 盘并核对当前记忆，再处理重试。");
        return;
    }
    cc_result_free(&rc);
    free(console->reset_snapshot_id); console->reset_snapshot_id=NULL;
    memory_console_invalidate_view(console);
    if (reload(console,source)!=ESP_OK)
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"记忆已重置，但页面刷新失败；请重新输入 /memories。");
    else app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,"记忆已重置，重置前的历史版本已保存。");
}

/**
 * @brief 识别本控制台命令前缀，具体子命令与参数由 handle 校验。
 * @param[in] text 待识别的 NUL 结尾文本，可为 NULL，调用期间借用。
 * @return 文本属于受支持的命令前缀时为真，详细参数继续交给 handle 校验。
 */
bool memory_console_handles(const char *text)
{
    return text && (strcmp(text,"/memories")==0 || strncmp(text,"/memories ",10)==0 ||
                    strcmp(text,"/memory")==0 || strncmp(text,"/memory ",8)==0);
}

/**
 * @brief 按可见索引取得稳定记录 ID，以当前 editor epoch/列表 revision 修改或删除。
 *
 * 保留类别与重要度，编辑时间取调用时间和原时间的较大值。
 * 版本冲突、报告已安装或介质变化时使视图失效，成功提交后单独重读显示。
 * @param[in,out] console 非空控制台，使用可见记录 ID 和当前列表 revision 执行变更。
 * @param[in] args 编辑或删除命令的序号及正文，调用期间借用。
 * @param[in] deleting 为真时要求序号后无正文并删除，为假时要求非空正文并编辑。
 * @param[in] source 命令或诊断所属入口，用于事件归因。
 * @param[in] updated_at 编辑时须非负，实际使用其与原记录时间的较大值，删除时忽略。
 * @post 编辑或删除结果经事件报告，提交与显示刷新分别处理。
 */
static void change(memory_console_t *console, const char *args, bool deleting,
                   app_event_source_t source, int64_t updated_at)
{
    size_t index=0; const char *rest=NULL;
    if (!parse_index(args,&index,&rest) || !visible_index(console,index) ||
        (deleting ? *rest!='\0' : !*rest || updated_at<0)) {
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
                          "请使用当前页面可见的序号和有效内容。");
        return;
    }
    const cc_memory_plan_record_t *record=&console->state.records[index];
    const char *epoch=cc_managed_memory_editor_epoch(console->editor); int installed=0;
    cc_result_t rc=deleting ? cc_managed_memory_editor_delete(console->editor,epoch,console->revision,record->id,&installed) :
        cc_managed_memory_editor_update(console->editor,epoch,console->revision,record->id,rest,record->category,
                                        record->importance,updated_at<record->updated_at ? record->updated_at : updated_at,&installed);
    if (rc.code==CC_OK && !recovery_service_is_current(console->recovery)) {
        cc_result_free(&rc); memory_console_invalidate_view(console);
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
            "U 盘在记忆变更期间发生变化，请恢复原介质并核对结果。");
        return;
    }
    if (rc.code!=CC_OK) {
        bool stale=rc.code==CC_ERR_INVALID_STATE || installed;
        report_error(source,&rc);
        if (stale) memory_console_invalidate_view(console);
        if (installed) app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
            "记忆可能已写入但同步结果未知，请恢复存储并核对后再操作。");
        return;
    }
    cc_result_free(&rc);
    memory_console_invalidate_view(console);
    esp_err_t err=reload(console,source);
    if (err!=ESP_OK) app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,
        "记忆变更已完成，但页面刷新失败；请重新输入 /memories。");
    else app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,deleting ? "记忆已删除。" : "记忆已修改。");
}

/**
 * @brief 当前挂载有效时解释列表、详情、编辑、删除及重置命令。
 *
 * text 调用期借用，状态和错误通过应用事件发布，计划及详情归 console。
 * 所有命令在 interaction owner 串行执行，updated_at 由应用提供。
 * @param[in,out] console 控制台对象，可为 NULL，所有处理由交互 owner 串行调用。
 * @param[in] text NUL 结尾命令，调用期间借用，可为 NULL，前缀不匹配时直接返回。
 * @param[in] source 命令或诊断所属入口，用于事件归因。
 * @param[in] updated_at 编辑时提供的时间，须非负，其他命令忽略此参数。
 * @post 有效命令经共享视图和事件报告结果，按页面与版本推进状态。
 * 空对象或不匹配的前缀直接返回，介质失效时清理视图并发布错误。
 */
void memory_console_handle(memory_console_t *console, const char *text,
                           app_event_source_t source, int64_t updated_at)
{
    if (!console || !memory_console_handles(text)) return;
    if (!recovery_service_is_current(console->recovery)) {
        memory_console_invalidate_view(console);
        app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"恢复存储不可用，记忆管理已暂停。");
        return;
    }
    if (strcmp(text,"/memories")==0) { report_display(source,reload(console,source)); return; }
    if (strcmp(text,"/memories next")==0 || strcmp(text,"/memories prev")==0) {
        if (!console->loaded) { report_display(source,reload(console,source)); return; }
        if (strcmp(text,"/memories next")==0 && console->offset+MEMORY_ROWS<console->state.count) console->offset+=MEMORY_ROWS;
        else if (strcmp(text,"/memories prev")==0 && console->offset>=MEMORY_ROWS) console->offset-=MEMORY_ROWS;
        report_display(source,render_list(console)); return;
    }
    if (strncmp(text,"/memory view ",13)==0) {
        size_t index=0; const char *rest=NULL;
        if (!console->list_visible || !parse_index(text+13,&index,&rest) || *rest || !visible_index(console,index))
            app_event_publish(APP_EVENT_KIND_ERROR,source,CC_LOG_ERROR,"请从当前记忆列表选择有效序号。");
        else report_display(source,show_detail(console,index,source));
        return;
    }
    if (strcmp(text,"/memory more")==0 && console->detail && console->more) {
        report_display(source,render_detail(console,console->next)); return;
    }
    if (strncmp(text,"/memory edit ",13)==0) { change(console,text+13,false,source,updated_at); return; }
    if (strncmp(text,"/memory delete ",15)==0) { change(console,text+15,true,source,updated_at); return; }
    if (strcmp(text,"/memory reset")==0) { reset_memory(console,source); return; }
    app_event_publish(APP_EVENT_KIND_STATUS,source,CC_LOG_INFO,
        "输入 /memories 查看，或使用 /memory view|edit|delete 序号、/memory reset。");
}
