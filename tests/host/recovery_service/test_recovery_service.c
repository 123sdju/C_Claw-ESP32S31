#include "recovery_service.h"
#include "session_selection.h"
#include "session_memory_turns.h"
#include "session_menu.h"
#include "recovery_console.h"
#include "manual_lua_service.h"
#include "memory_console.h"
#include "managed_memory_service.h"
#include "startup_managed_memory.h"
#include "cclaw_runtime_features.h"
#include "startup_recovery.h"
#include "board_ui.h"
#include "cc/app/cc_runtime_features.h"
#include "cc/adapters/cc_json_segmented_history.h"
#include "cc/adapters/cc_json_file_session_store.h"
#include "cc/app/cc_cache_metrics.h"
#include "cc/util/cc_json.h"
#include "storage_nvs_service.h"
#include "cc/core/cc_id.h"
#include "cc/core/cc_observability.h"
#include "cc/internal/cc_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

extern cc_result_t cc_json_segmented_store_create(const char *,size_t,const char *,size_t,cc_session_store_t *);
extern cc_result_t cc_json_file_store_create(const char *,cc_session_store_t *);
extern cc_result_t __real_cc_filesystem_get_default(cc_filesystem_t *);
static int wrap_stage_filesystem;
static unsigned mutate_on_stage_read, stage_reads;
static const char *stage_mutation_path;
static const char *workspace_mkdir_path, *mutate_during_workspace_mkdir;
static int fail_workspace_mkdir, change_mount_during_workspace_mkdir;
static int change_mount_during_config_replace;
static const char *session_replace_fail_path;
static const char *session_read_fail_path;
static int session_replace_fail_mode;
static int memory_primary_read_fail;
static const char *journal_read_fault_path;
static cc_error_code_t journal_read_fault_code;
static int change_mount_during_journal_read;
static int media_write_fault, media_replace_fault, media_sync_fault, media_read_fault;
static char media_last_temporary[512];
static uint64_t mount_generation=1;
static cc_filesystem_vtable_t stage_vtable;
static const cc_filesystem_vtable_t *stage_base_vtable;
/**
 * @brief 注入记忆主文件回读错误或账本读取错误/换盘，其他读取转交真实文件系统。
 * @param[in] self 借用的底层文件系统实例。
 * @param[in] path 同步借用路径，按记忆主文件或账本故障路径精确匹配，可为 NULL 并由底层校验。
 * @param[out] out 有效输出槽成功取得拥有文本，注入失败时置空，空地址交由底层校验。
 * @return 记忆故障为 CC_ERR_IO，账本故障保留配置的错误码，其余返回底层结果，账本换盘标志在读取后一次性触发。
 */
static cc_result_t staged_read_text(void *self,const char *path,char **out)
{
    if (!path || !out) return stage_base_vtable->read_text(self,path,out);
    if (journal_read_fault_path && strcmp(path,journal_read_fault_path)==0) {
        if (journal_read_fault_code!=CC_OK) {
            *out=NULL;
            return cc_result_error(journal_read_fault_code,"injected journal read failure");
        }
        if (change_mount_during_journal_read) {
            change_mount_during_journal_read=0;
            mount_generation++;
        }
    }
    if (memory_primary_read_fail && session_replace_fail_path &&
        strcmp(path,session_replace_fail_path)==0) {
        *out=NULL;
        return cc_result_error(CC_ERR_IO,"injected unavailable memory primary");
    }
    return stage_base_vtable->read_text(self,path,out);
}
static cc_result_t staged_read_bytes(void *self,const char *path,void **data,size_t *size)
{
    if (media_read_fault && strstr(path,"/media/")) {
        media_read_fault=0;
        return cc_result_error(CC_ERR_IO,"injected media read failure");
    }
    if (session_read_fail_path && strcmp(path,session_read_fail_path)==0)
        return cc_result_error(CC_ERR_IO,"injected session read failure");
    if (stage_mutation_path && strcmp(path,stage_mutation_path)==0 &&
        mutate_on_stage_read && ++stage_reads==mutate_on_stage_read) {
        FILE *file=fopen(path,"ab");
        if (file) { fputc(' ',file); fclose(file); }
    }
    return stage_base_vtable->read_bytes(self,path,data,size);
}
static cc_result_t staged_write_bytes(void *self,const char *path,const void *data,size_t size)
{
    size_t n=strlen(path);
    int media_tmp=strstr(path,"/media/") && n>=4U && strcmp(path+n-4U,".tmp")==0;
    if (media_tmp) snprintf(media_last_temporary,sizeof(media_last_temporary),"%s",path);
    if (media_tmp && media_write_fault) {
        int mode=media_write_fault; media_write_fault=0;
        size_t written=mode==1 && size>2U ? 2U : size;
        cc_result_t rc=stage_base_vtable->write_bytes(self,path,data,written);
        if (rc.code!=CC_OK) return rc;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_IO,mode==1 ? "injected partial media write" : "injected lost media write response");
    }
    return stage_base_vtable->write_bytes(self,path,data,size);
}
static cc_result_t staged_sync_file(void *self,const char *path)
{
    size_t n=strlen(path);
    int media=strstr(path,"/media/")!=NULL;
    int temporary=n>=4U && strcmp(path+n-4U,".tmp")==0;
    cc_result_t rc=stage_base_vtable->sync_file(self,path);
    if (rc.code==CC_OK && media && media_sync_fault &&
        ((media_sync_fault==1 && temporary) || (media_sync_fault==2 && !temporary))) {
        media_sync_fault=0; cc_result_free(&rc);
        return cc_result_error(CC_ERR_IO,"injected media sync failure");
    }
    return rc;
}
static cc_result_t staged_make_dir(void *self,const char *path)
{
    int selected=workspace_mkdir_path && strcmp(path,workspace_mkdir_path)==0;
    if (selected && fail_workspace_mkdir)
        return cc_result_error(CC_ERR_IO,"injected workspace creation failure");
    cc_result_t rc=stage_base_vtable->make_dir(self,path);
    if (selected && rc.code==CC_OK) {
        if (mutate_during_workspace_mkdir) {
            FILE *file=fopen(mutate_during_workspace_mkdir,"ab");
            if (file) { fputc(' ',file); fclose(file); }
        }
        if (change_mount_during_workspace_mkdir) mount_generation++;
    }
    return rc;
}
static cc_result_t staged_atomic_replace(void *self,const char *from,const char *to)
{
    int media=strstr(to,"/media/")!=NULL;
    if (media && media_replace_fault==1) {
        media_replace_fault=0;
        return cc_result_error(CC_ERR_IO,"injected failure before media install");
    }
    int selected=session_replace_fail_path && strcmp(to,session_replace_fail_path)==0;
    if (selected && session_replace_fail_mode==1) {
        session_replace_fail_mode=0;
        return cc_result_error(CC_ERR_IO,"injected failure before session install");
    }
    cc_result_t rc=stage_base_vtable->atomic_replace(self,from,to);
    if (media && media_replace_fault==2 && rc.code==CC_OK) {
        media_replace_fault=0; cc_result_free(&rc);
        return cc_result_error(CC_ERR_IO,"injected failure after media install");
    }
    if (selected && (session_replace_fail_mode==2 || session_replace_fail_mode==3) && rc.code==CC_OK) {
        if (session_replace_fail_mode==3) memory_primary_read_fail=1;
        session_replace_fail_mode=0;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_IO,"injected failure after session install");
    }
    if (rc.code==CC_OK && change_mount_during_config_replace) mount_generation++;
    return rc;
}
cc_result_t __wrap_cc_filesystem_get_default(cc_filesystem_t *out)
{
    cc_result_t rc=__real_cc_filesystem_get_default(out);
    /* This fixture contains no symlinks; this does not test FAT behavior. */
    if (rc.code==CC_OK) {
        out->capabilities |= CC_FS_CAP_NO_SYMLINKS;
        if (wrap_stage_filesystem) {
            stage_base_vtable=out->vtable;
            stage_vtable=*out->vtable;
            stage_vtable.read_text=staged_read_text;
            stage_vtable.read_bytes=staged_read_bytes;
            stage_vtable.write_bytes=staged_write_bytes;
            stage_vtable.sync_file=staged_sync_file;
            stage_vtable.make_dir=staged_make_dir;
            stage_vtable.atomic_replace=staged_atomic_replace;
            out->vtable=&stage_vtable;
        }
    }
    return rc;
}
static int mounted=1;
static int identity_open_fail;
static unsigned identity_open_calls;
static unsigned checks_until_mount_change;
static int change_after_abandon;
static int fail_before_abandon;
static int fail_before_controller_submit;
extern cc_result_t __real_cc_runtime_controller_submit_text_with_run_id(cc_runtime_controller_t *,
    const char *,const char *,const char *,cc_runtime_controller_stream_fn,void *,char **);
cc_result_t __wrap_cc_runtime_controller_submit_text_with_run_id(cc_runtime_controller_t *controller,
    const char *session_id,const char *run_id,const char *input,cc_runtime_controller_stream_fn on_stream,
    void *user_data,char **out_response)
{
    if (fail_before_controller_submit) {
        if (out_response) *out_response=NULL;
        return cc_result_error(CC_ERR_NETWORK,"injected failure before session creation");
    }
    return __real_cc_runtime_controller_submit_text_with_run_id(controller,session_id,run_id,input,
                                                                on_stream,user_data,out_response);
}
extern cc_result_t __real_cc_agent_runtime_abandon_run(cc_agent_runtime_t *,const char *);
cc_result_t __wrap_cc_agent_runtime_abandon_run(cc_agent_runtime_t *runtime,const char *run_id)
{
    if (fail_before_abandon) return cc_result_error(CC_ERR_IO,"injected failure before checkpoint deletion");
    cc_result_t rc=__real_cc_agent_runtime_abandon_run(runtime,run_id);
    if (rc.code==CC_OK && change_after_abandon) mount_generation++;
    return rc;
}
bool usb_storage_service_is_mounted(void) { return mounted!=0; }
bool usb_storage_service_current_generation(uint64_t *out)
{
    if (checks_until_mount_change && --checks_until_mount_change==0) mount_generation++;
    if (out) *out=mount_generation;
    return mounted!=0;
}
cc_result_t storage_nvs_service_check(void)
{
    identity_open_calls++;
    if (identity_open_fail) return cc_result_error(CC_ERR_STORAGE,"injected NVS initialization failure");
    return cc_result_ok();
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; if (!good) fprintf(stderr,"%s\n",rc.message ? rc.message : "error"); cc_result_free(&rc); return good; }

/* Exercise the product's no-extra-tools path too: it must still replace the
 * SDK memory factory with the read-only product factory. */
const cc_tool_descriptor_t *app_tool_registry_descriptors(size_t *count)
{
    if (count) *count=0;
    return NULL;
}
static int readonly_write_calls;
static cc_result_t readonly_test_set(void *self,const char *key,const char *value,
                                      const char *category,const char *session)
{
    (void)self; (void)key; (void)value; (void)category; (void)session;
    readonly_write_calls++;
    return cc_result_ok();
}
static int test_product_memory_tool_is_readonly(void)
{
    const cc_runtime_feature_set_t *features=cclaw_runtime_features();
    const cc_tool_descriptor_t *memory=NULL;
    for (size_t i=0;features && i<features->tool_count;i++)
        if (features->tools[i].name && strcmp(features->tools[i].name,"memory")==0)
            memory=&features->tools[i];
    CHECK(memory && memory->compiled && memory->create);
    cc_memory_store_vtable_t vtable={0}; vtable.set=readonly_test_set;
    cc_memory_store_t store={.self=&readonly_write_calls,.vtable=&vtable};
    unsigned capabilities=0;
    cc_runtime_tool_factory_ctx_t ctx={0}; ctx.memory_store=&store; ctx.out_capabilities=&capabilities;
    cc_tool_t tool={0}; CHECK(ok(memory->create(&ctx,&tool)));
    CHECK(capabilities==CC_TOOL_CAPABILITY_READ_ONLY);
    const char *schema=tool.vtable->schema_json(tool.self);
    CHECK(schema && strstr(schema,"\"recall\"") && !strstr(schema,"\"store\""));
    cc_tool_result_t result={0};
    CHECK(ok(tool.vtable->call(tool.self,
          "{\"operation\":\"store\",\"key\":\"k\",\"value\":\"v\"}",NULL,&result)) &&
          !result.ok && result.error && readonly_write_calls==0);
    cc_tool_result_cleanup(&result); tool.vtable->destroy(tool.self);
    return 0;
}
static void provider_destroy(void *self) { (void)self; }
static const char *provider_response="resumed";
static int provider_calls, provider_fail, provider_usage_mode, provider_saw_tools;
static int expect_recovered_memory, recovered_memory_seen;
static const char *expected_memory_input="old input";
static const char *expected_memory_response="resumed";
static cc_result_t provider_chat(void *self,const cc_llm_chat_request_t *request,cc_llm_response_t *out)
{
    (void)self; provider_calls++;
    provider_saw_tools=request->tools_json!=NULL;
    if (provider_usage_mode) {
        out->usage.valid=1; out->usage.prompt_tokens=100;
        out->usage.cached_tokens_valid=1; out->usage.cached_tokens=0;
    }
    if (provider_fail) return cc_result_error(CC_ERR_NETWORK,"injected provider interruption");
    if (expect_recovered_memory && request->message_count>=2) {
        char *payload=NULL;
        cc_result_t rc=cc_message_get_text_summary(&request->messages[1],&payload);
        if (rc.code==CC_OK && payload && strstr(payload,expected_memory_input) && strstr(payload,expected_memory_response))
            recovered_memory_seen=1;
        cc_result_free(&rc); free(payload);
    }
    out->finished=1; return cc_llm_response_set_text(out,provider_response);
}
static const cc_llm_provider_vtable_t provider_vt={provider_chat,NULL,provider_destroy,NULL};
static cc_result_t provider_create(const cc_config_t *c,cc_http_client_t *h,cc_llm_provider_t *out)
{ (void)c; (void)h; out->self=(void *)1; out->vtable=&provider_vt; return cc_result_ok(); }
static cc_result_t session_create(const cc_config_t *c,cc_session_store_t *out)
{ return cc_json_segmented_store_create(c->storage_path,2048,"media",1024,out); }
static cc_result_t memory_create(const cc_config_t *c,cc_memory_store_t *out)
{ return cc_memory_store_create_json_file(out,c->memory_path); }
static cc_result_t allow(void *s,const cc_tool_call_t *c,const cc_tool_context_t *x,cc_policy_decision_t *out)
{ (void)s; (void)c; (void)x; memset(out,0,sizeof(*out)); out->allowed=1; return cc_result_ok(); }
static const cc_policy_engine_vtable_t policy_vt={allow,NULL};
static cc_result_t policy_create(const cc_config_t *c,cc_policy_engine_t *out)
{ (void)c; memset(out,0,sizeof(*out)); out->vtable=&policy_vt; return cc_result_ok(); }
static int tool_calls;
static int tool_fail;
static unsigned tool_attempt_number;
static char last_tool_attempt_id[32];
static char view_text[40000];
static char boot_status_text[512];
static char last_event_text[512];
static board_conversation_role_t boot_status_role;
void app_event_publish_boot_status(app_event_kind_t kind,app_event_source_t source,cc_log_level_t level,const char *text)
{
    (void)source; (void)level;
    boot_status_role=kind==APP_EVENT_KIND_ERROR ? BOARD_CONV_ERROR : BOARD_CONV_STATUS;
    snprintf(boot_status_text,sizeof(boot_status_text),"%s",text ? text : "");
}
static int fail_next_view;
esp_err_t app_event_bus_replace_conversation(const app_event_view_item_t *items,size_t count)
{
    if (fail_next_view) { fail_next_view=0; return ESP_ERR_NO_MEM; }
    view_text[0]='\0';
    for (size_t i=0;i<count;i++) {
        size_t used=strlen(view_text), len=strlen(items[i].text ? items[i].text : "");
        if (used+len+2U>sizeof(view_text)) return ESP_ERR_INVALID_SIZE;
        memcpy(view_text+used,items[i].text ? items[i].text : "",len);
        view_text[used+len]='\n'; view_text[used+len+1U]='\0';
    }
    return ESP_OK;
}
void app_event_publish(app_event_kind_t kind,app_event_source_t source,cc_log_level_t level,const char *text)
{
    (void)kind; (void)source; (void)level;
    snprintf(last_event_text,sizeof(last_event_text),"%s",text ? text : "");
}
void app_event_bus_clear_conversation(void) { }
static int forwarded_interaction_calls;
esp_err_t interaction_service_submit_text(app_event_source_t source,const char *text)
{ (void)source; (void)text; forwarded_interaction_calls++; return ESP_OK; }
static const char *tool_name(void *self) { (void)self; return "write"; }
static const char *tool_description(void *self) { (void)self; return "test"; }
static const char *tool_schema(void *self) { (void)self; return "{}"; }
static cc_result_t tool_call(void *self,const char *args,const cc_tool_context_t *ctx,cc_tool_result_t *out)
{
    (void)self; (void)args; (void)ctx; tool_calls++; out->ok=!tool_fail;
    if (tool_fail) out->error=cc_copy_string("outcome unknown");
    else out->text=cc_copy_string("done");
    return (out->text || out->error) ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY,"oom");
}
static const cc_tool_vtable_t tool_vt={tool_name,tool_description,tool_schema,tool_call,NULL};
static const char *trusted_tool_name(void *self) { return (const char *)self; }
static const cc_tool_vtable_t trusted_tool_vt={trusted_tool_name,tool_description,tool_schema,tool_call,NULL};
static int lua_calls, lua_fail;
static char lua_argument[128];
static cc_result_t lua_stub_call(void *self,const char *args,const cc_tool_context_t *ctx,cc_tool_result_t *out)
{
    (void)ctx; cc_json_value_t *json=NULL;
    cc_result_t rc=cc_json_parse_strict(args,&json);
    if (rc.code!=CC_OK) return rc;
    const char *key=strcmp((const char *)self,"lua_run_file")==0 ? "path" : "code";
    const char *value=cc_json_string_value(cc_json_object_get(json,key));
    if (!value) { cc_json_destroy(json); return cc_result_error(CC_ERR_INVALID_ARGUMENT,"missing lua argument"); }
    snprintf(lua_argument,sizeof(lua_argument),"%s",value); lua_calls++;
    cc_json_destroy(json); out->ok=!lua_fail;
    if (lua_fail) out->error=cc_copy_string("unknown lua result");
    else out->text=cc_copy_string("lua done");
    return (out->text || out->error) ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY,"oom");
}
static const cc_tool_vtable_t lua_stub_vt={trusted_tool_name,tool_description,tool_schema,lua_stub_call,NULL};
cc_result_t lua_run_code_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,cc_tool_t *out)
{ (void)ctx; out->self=(void *)"lua_run_code"; out->vtable=&lua_stub_vt; return cc_result_ok(); }
cc_result_t lua_run_file_tool_create(const cc_runtime_tool_factory_ctx_t *ctx,cc_tool_t *out)
{ (void)ctx; out->self=(void *)"lua_run_file"; out->vtable=&lua_stub_vt; return cc_result_ok(); }
static pthread_mutex_t block_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t block_cond=PTHREAD_COND_INITIALIZER;
static int block_started, block_release;
static const char *blocking_name(void *self) { (void)self; return "blocking"; }
static cc_result_t blocking_call(void *self,const char *args,const cc_tool_context_t *ctx,cc_tool_result_t *out)
{
    (void)self; (void)args; (void)ctx;
    pthread_mutex_lock(&block_mutex); block_started=1; pthread_cond_broadcast(&block_cond);
    while (!block_release) pthread_cond_wait(&block_cond,&block_mutex);
    pthread_mutex_unlock(&block_mutex);
    out->ok=1; out->text=cc_copy_string("confirmed");
    return out->text ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY,"oom");
}
static const cc_tool_vtable_t blocking_vt={blocking_name,tool_description,tool_schema,blocking_call,NULL};
typedef struct finish_action {
    cc_runtime_controller_t *controller;
    int called;
    int cancelled;
    int change_media;
} finish_action_t;
static void on_run_finished(const char *event,const char *json,void *user)
{
    (void)event; (void)json; finish_action_t *action=user;
    cc_runtime_controller_status_t status={0};
    cc_runtime_controller_get_status(action->controller,&status);
    cc_result_t rc=cc_runtime_controller_cancel(action->controller,status.active_run_id);
    action->called++; action->cancelled=rc.code==CC_OK; cc_result_free(&rc);
    if (action->change_media) mount_generation++;
}
typedef struct manual_thread { cc_runtime_controller_t *controller; cc_result_t rc; cc_tool_result_t result; } manual_thread_t;
static void *manual_thread_run(void *arg)
{
    manual_thread_t *thread=arg;
    cc_tool_t tool={(void *)1,&blocking_vt}; cc_tool_call_t call={"blocking-call","blocking","{}"};
    thread->rc=cc_runtime_controller_execute_manual_tool(thread->controller,&call,&tool,&thread->result);
    return NULL;
}
static cc_result_t tool_create(const cc_runtime_tool_factory_ctx_t *ctx,cc_tool_t *out)
{ (void)ctx; out->self=(void *)1; out->vtable=&tool_vt; return cc_result_ok(); }
static const cc_llm_provider_descriptor_t providers[]={{"fake",1,provider_create}};
static const cc_tool_descriptor_t tools[]={{"write",NULL,1,tool_create}};
static cc_runtime_feature_set_t features={.llm_providers=providers,.llm_provider_count=1,.tools=tools,.tool_count=1,
    .create_session_store=session_create,.create_memory_store=memory_create,.create_policy_engine=policy_create};

static int attempt_tool(cc_runtime_builder_t *builder,int expected_ok)
{
    cc_runtime_generation_t *generation=NULL;
    if (!ok(cc_runtime_builder_acquire_generation(builder,&generation))) return 0;
    cc_agent_runtime_t *runtime=cc_runtime_generation_runtime(generation);
    if (!ok(cc_agent_runtime_create_session(runtime,"session","."))) { cc_runtime_generation_release(generation); return 0; }
    snprintf(last_tool_attempt_id,sizeof(last_tool_attempt_id),"test-%u",++tool_attempt_number);
    cc_tool_call_t call={last_tool_attempt_id,"write","{}"}; cc_tool_result_t result={0};
    cc_result_t rc=cc_agent_runtime_execute_manual_tool(runtime,"session",&call,&result);
    int success=(rc.code==CC_OK && result.ok==expected_ok) ||
                (!expected_ok && rc.code==CC_ERR_INVALID_STATE);
    cc_result_free(&rc);
    cc_tool_result_cleanup(&result); cc_runtime_generation_release(generation); return success;
}

static int attempt_trusted_tool(cc_runtime_builder_t *builder,const char *name,const char *call_id,int expected_ok)
{
    cc_runtime_generation_t *generation=NULL;
    if (!ok(cc_runtime_builder_acquire_generation(builder,&generation))) return 0;
    cc_agent_runtime_t *runtime=cc_runtime_generation_runtime(generation);
    if (!ok(cc_agent_runtime_create_session(runtime,"manual-session","."))) { cc_runtime_generation_release(generation); return 0; }
    cc_tool_t trusted={(void *)name,&trusted_tool_vt};
    cc_tool_call_t call={(char *)call_id,(char *)name,"{}"}; cc_tool_result_t result={0};
    int success=ok(cc_agent_runtime_execute_manual_tool_with_impl(runtime,"manual-session",&call,&trusted,&result)) && result.ok==expected_ok;
    cc_tool_result_cleanup(&result); cc_runtime_generation_release(generation); return success;
}

static int exercise_memory_console(recovery_service_t *service,cc_filesystem_t *fs,const char *memory,const char *root)
{
    char history[256]; snprintf(history,sizeof(history),"%s/memory-history",root);
    CHECK(ok(fs->vtable->make_dir(fs->self,history)));
    cc_memory_store_t *store=recovery_service_borrow_memory_store(service);
    CHECK(store && cc_json_memory_store_matches_path(store,memory));
    cc_memory_snapshot_store_t *snapshots=NULL; CHECK(ok(cc_memory_snapshot_store_open(fs,history,memory,&snapshots)));
    cc_memory_plan_change_t changes[2]={0};
    changes[0].kind=CC_MEMORY_CHANGE_ADD; changes[0].record.size=sizeof(changes[0].record);
    changes[0].record.id="m1"; changes[0].record.text="token=EXAMPLEONLY";
    changes[0].record.category="preference"; changes[0].record.importance=70; changes[0].record.updated_at=10;
    changes[1]=changes[0]; changes[1].record.id="m2"; changes[1].record.text="second memory";
    cc_memory_plan_t initial={0}, proposed={0}; uint64_t revision=0;
    CHECK(ok(cc_json_memory_managed_list(store,&initial,&revision)) && initial.count==0);
    CHECK(ok(cc_memory_plan_build(NULL,0,changes,2,8,NULL,NULL,&proposed)));
    cc_memory_plan_cleanup(&initial);
    char *snapshot_id=NULL; int installed=0;
    CHECK(ok(cc_json_memory_managed_apply(store,revision,&proposed,NULL,0,NULL,&snapshot_id,&installed)) && installed && !snapshot_id);
    cc_memory_plan_cleanup(&proposed);
    cc_managed_memory_editor_t *editor=NULL; memory_console_t *console=NULL;
    char foreign_path[256]; snprintf(foreign_path,sizeof(foreign_path),"%s/foreign-memory.json",root);
    cc_memory_store_t foreign={0}; cc_managed_memory_editor_t *foreign_editor=NULL; memory_console_t *foreign_console=NULL;
    CHECK(ok(cc_memory_store_create_json_file(&foreign,foreign_path)) &&
          ok(cc_managed_memory_editor_create(&foreign,snapshots,&foreign_editor)));
    cc_result_t foreign_rc=memory_console_create(foreign_editor,service,&foreign_console);
    CHECK(foreign_rc.code==CC_ERR_INVALID_ARGUMENT && !foreign_console); cc_result_free(&foreign_rc);
    cc_managed_memory_editor_destroy(foreign_editor); cc_memory_store_destroy(&foreign);
    CHECK(ok(cc_managed_memory_editor_create(store,snapshots,&editor)) && ok(memory_console_create(editor,service,&console)));
    CHECK(memory_console_handles("/memories") && memory_console_handles("/memory edit 1 value") && !memory_console_handles("/memory-other"));
    memory_console_handle(console,"/memories",APP_EVENT_SOURCE_WEB,20);
    CHECK(strstr(view_text,"m1") && strstr(view_text,"m2"));
    memory_console_handle(console,"/memory view 1",APP_EVENT_SOURCE_SERIAL,20);
    CHECK(strstr(view_text,"长期记忆详情") && !strstr(view_text,"EXAMPLEONLY"));
    memory_console_handle(console,"/memory edit 1 new value",APP_EVENT_SOURCE_WEB,20);
    cc_memory_entry_t entry={0};
    CHECK(ok(cc_memory_store_get(store,"m1",&entry)) && strcmp(entry.value,"new value")==0); cc_memory_entry_free(&entry);
    CHECK(ok(cc_memory_store_set(store,"internal.summary","other","context_summary",NULL)));
    memory_console_handle(console,"/memory delete 1",APP_EVENT_SOURCE_WEB,21);
    CHECK(ok(cc_memory_store_get(store,"m1",&entry))); cc_memory_entry_free(&entry);
    memory_console_handle(console,"/memories",APP_EVENT_SOURCE_SERIAL,21);
    mount_generation++;
    memory_console_handle(console,"/memory delete 1",APP_EVENT_SOURCE_WEB,21);
    mount_generation--;
    CHECK(ok(cc_memory_store_get(store,"m1",&entry))); cc_memory_entry_free(&entry);
    memory_console_handle(console,"/memory delete 1",APP_EVENT_SOURCE_WEB,21);
    CHECK(ok(cc_memory_store_get(store,"m1",&entry))); cc_memory_entry_free(&entry);
    memory_console_handle(console,"/memories",APP_EVENT_SOURCE_WEB,21);
    memory_console_handle(console,"/memory delete 1",APP_EVENT_SOURCE_WEB,21);
    cc_result_t rc=cc_memory_store_get(store,"m1",&entry); CHECK(rc.code==CC_ERR_NOT_FOUND); cc_result_free(&rc);
    cc_memory_snapshot_info_t *versions=NULL; size_t version_count=0;
    CHECK(ok(cc_memory_snapshot_list(snapshots,&versions,&version_count)) && version_count==0);
    cc_memory_snapshot_info_free_array(versions,version_count);
    memory_console_destroy(console); cc_managed_memory_editor_destroy(editor);
    cc_memory_snapshot_store_destroy(snapshots);
    return 0;
}

static int test_managed_memory_product_policy(void)
{
    cc_memory_plan_record_t records[]={
        {.id="z",.importance=10,.updated_at=5},
        {.id="a",.importance=10,.updated_at=5},
        {.id="b",.importance=10,.updated_at=2},
        {.id="c",.importance=9,.updated_at=100}
    };
    size_t selected=SIZE_MAX;
    CHECK(ok(managed_memory_select_eviction(records,4,NULL,&selected)) && selected==3);
    records[3].importance=10;
    CHECK(ok(managed_memory_select_eviction(records,4,NULL,&selected)) && selected==2);
    records[2].updated_at=5;
    CHECK(ok(managed_memory_select_eviction(records,4,NULL,&selected)) && selected==1);
    cc_result_t rc=managed_memory_select_eviction(records,0,NULL,&selected);
    CHECK(rc.code==CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);

    cc_memory_plan_t plan={0};
    plan.rewritten_existing=29;
    CHECK(managed_memory_decide_snapshot(&plan,100,NULL)==0);
    plan.rewritten_existing=30;
    CHECK(managed_memory_decide_snapshot(&plan,100,NULL)==CC_MEMORY_SNAPSHOT_REWRITE);
    plan.rewritten_existing=4;
    CHECK(managed_memory_decide_snapshot(&plan,10,NULL)==0);
    plan.rewritten_existing=5;
    CHECK(managed_memory_decide_snapshot(&plan,16,NULL)==CC_MEMORY_SNAPSHOT_REWRITE);
    CHECK(managed_memory_decide_snapshot(&plan,17,NULL)==0);
    plan.rewritten_existing=6;
    CHECK(managed_memory_decide_snapshot(&plan,17,NULL)==CC_MEMORY_SNAPSHOT_REWRITE);
    CHECK(managed_memory_decide_snapshot(&plan,0,NULL)==0);
    return 0;
}

static cc_result_t memory_evict(const cc_memory_plan_record_t *records,size_t count,void *user,size_t *out)
{ (void)records; (void)user; *out=count-1U; return cc_result_ok(); }
static cc_memory_snapshot_reason_t memory_snapshot(const cc_memory_plan_t *plan,size_t original,void *user)
{ (void)plan; (void)original; (void)user; return 0; }
static cc_result_t flip_mount_before_ack(void *user)
{ if (*(int *)user) mount_generation++; return cc_result_ok(); }

typedef struct memory_usage_observer {
    cc_cache_metrics_t *metrics;
    int extraction_events;
    int attribution_errors;
} memory_usage_observer_t;

static void observe_memory_usage(const char *type,const char *json,void *user)
{
    memory_usage_observer_t *observer=user;
    cc_result_t rc=cc_cache_metrics_observe(observer->metrics,type,json);
    if (rc.code!=CC_OK) observer->attribution_errors++;
    cc_result_free(&rc);
    cc_json_value_t *root=NULL;
    rc=cc_json_parse(json,&root);
    if (rc.code==CC_OK) {
        cc_json_value_t *attributes=cc_json_object_get(root,"attributes");
        const char *purpose=cc_json_string_value(cc_json_object_get(attributes,"purpose"));
        const char *session=cc_json_string_value(cc_json_object_get(root,"session_id"));
        const char *run=cc_json_string_value(cc_json_object_get(root,"run_id"));
        const char *turn=cc_json_string_value(cc_json_object_get(attributes,"origin_turn_id"));
        if (purpose && strcmp(purpose,"memory_extraction")==0) {
            observer->extraction_events++;
            if (!session || strcmp(session,"old-session")!=0 || !run || run[0] ||
                !turn || strcmp(turn,"turn-memory")!=0) observer->attribution_errors++;
        } else observer->attribution_errors++;
    } else observer->attribution_errors++;
    cc_result_free(&rc); cc_json_destroy(root);
}

static int check_memory_usage(cc_event_bus_t *bus,memory_usage_observer_t *observer,
                              int extractions,int reported)
{
    CHECK(ok(cc_event_bus_flush(bus,1000)));
    CHECK(!observer->attribution_errors && observer->extraction_events==extractions);
    char *snapshot=NULL; cc_json_value_t *root=NULL;
    CHECK(ok(cc_cache_metrics_snapshot(observer->metrics,&snapshot)));
    CHECK(ok(cc_json_parse(snapshot,&root))); free(snapshot);
    cc_json_value_t *memory=cc_json_object_get(root,"memory_model");
    CHECK(cc_json_int_value(cc_json_object_get(memory,"extraction_requests"))==extractions &&
          cc_json_int_value(cc_json_object_get(memory,"extraction_usage_reported"))==reported &&
          cc_json_int_value(cc_json_object_get(memory,"extraction_usage_unreported"))==extractions-reported);
    cc_json_value_t *models=cc_json_object_get(root,"models");
    if (extractions) {
        CHECK(cc_json_array_size(models)==1);
        cc_json_value_t *model=cc_json_array_get(models,0);
        CHECK(strcmp(cc_json_string_value(cc_json_object_get(model,"provider")),"fake")==0 &&
              strcmp(cc_json_string_value(cc_json_object_get(model,"model")),"fake")==0 &&
              cc_json_int_value(cc_json_object_get(model,"usage_events"))==extractions &&
              cc_json_int_value(cc_json_object_get(model,"cache_measured"))==reported &&
              cc_json_int_value(cc_json_object_get(model,"cache_unreported"))==extractions-reported &&
              cc_json_int_value(cc_json_object_get(model,"cached_tokens"))==0);
    } else CHECK(cc_json_array_size(models)==0);
    cc_json_destroy(root);
    return 0;
}

static int exercise_managed_memory(cc_runtime_builder_t *builder,recovery_service_t *recovery,const char *memory)
{
    int flip=1;
    cc_managed_memory_worker_options_t options={0}; options.size=sizeof(options);
    options.capacity=8; options.max_pending_items=1; options.max_pending_bytes=4096;
    options.select_eviction=memory_evict; options.decide_snapshot=memory_snapshot;
    options.before_ack=flip_mount_before_ack; options.before_ack_user_data=&flip;
    cc_memory_extraction_options_t extraction={0}; extraction.size=sizeof(extraction); extraction.model="fake";
    managed_memory_service_t *manager=NULL;
    cc_result_t rc=managed_memory_service_create(builder,recovery,"/wrong-memory.json",&options,&extraction,&manager);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !manager); cc_result_free(&rc);
    CHECK(ok(managed_memory_service_create(builder,recovery,memory,&options,&extraction,&manager)));
    CHECK(managed_memory_service_editor(manager)!=NULL);
    cc_runtime_generation_t *generation=NULL; cc_cache_metrics_t *metrics=NULL;
    cc_event_subscription_t subscription={0};
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&generation)) &&
          ok(cc_cache_metrics_create(&metrics)));
    cc_event_bus_t *bus=cc_agent_runtime_event_bus(cc_runtime_generation_runtime(generation));
    memory_usage_observer_t observer={.metrics=metrics};
    CHECK(bus && ok(cc_event_bus_subscribe(bus,CC_OBS_EVENT_LLM_USAGE,observe_memory_usage,
                                           &observer,NULL,&subscription)));
    cc_memory_pending_reservation_t *reservation=NULL;
    checks_until_mount_change=2;
    rc=managed_memory_service_reserve(manager,"not-started","old-session","input",128,&reservation);
    CHECK(rc.code==CC_ERR_STORAGE && !reservation); cc_result_free(&rc);
    mount_generation--; /* The fake fixture restores the earlier generation. */
    CHECK(ok(managed_memory_service_reserve(manager,"turn-memory","old-session","I like tea",128,&reservation)) && reservation);
    size_t ready=0,reserved=0;
    CHECK(ok(managed_memory_service_status_detail(manager,&ready,&reserved,NULL)) && !ready && reserved==1);
    mount_generation++;
    CHECK(ok(managed_memory_service_commit(manager,&reservation,"ok")) && !reservation);
    CHECK(ok(managed_memory_service_status_detail(manager,&ready,&reserved,NULL)) && ready==1 && !reserved);
    cc_managed_memory_worker_result_t interrupted={0}; int calls_when_unmounted=provider_calls;
    rc=managed_memory_service_process_one(manager,30,NULL,&interrupted);
    CHECK(rc.code==CC_ERR_STORAGE && provider_calls==calls_when_unmounted); cc_result_free(&rc);
    CHECK(check_memory_usage(bus,&observer,0,0)==0);
    mount_generation--; /* Only this fake can restore a previous generation. */
    rc=managed_memory_service_enqueue(manager,"overflow","old-session","other","ok");
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED); cc_result_free(&rc);
    size_t pending=0,bytes=0; CHECK(ok(managed_memory_service_status(manager,&pending,&bytes)) && pending==1 && bytes>0);
    provider_response="{\"memories\":[{\"action\":\"add\",\"target_id\":\"\",\"text\":\"likes tea\","
        "\"category\":\"preference\",\"importance\":60,\"evidence\":\"like tea\"}]}";
    int calls_before=provider_calls; cc_managed_memory_worker_result_t result={0};
    provider_usage_mode=1; provider_fail=1;
    rc=managed_memory_service_process_one(manager,30,NULL,&result);
    CHECK(rc.code==CC_ERR_NETWORK && provider_calls==calls_before+1); cc_result_free(&rc);
    CHECK(check_memory_usage(bus,&observer,1,1)==0);
    provider_fail=0;
    rc=managed_memory_service_process_one(manager,30,NULL,&result);
    CHECK(rc.code==CC_ERR_STORAGE && provider_calls==calls_before+2 && !recovery_service_is_current(recovery)); cc_result_free(&rc);
    CHECK(check_memory_usage(bus,&observer,2,2)==0);
    CHECK(ok(managed_memory_service_status(manager,&pending,&bytes)) && pending==1);
    mount_generation--; flip=0; /* The host fixture alone can restore an old generation. */
    CHECK(ok(managed_memory_service_process_one(manager,31,NULL,&result)) && provider_calls==calls_before+2);
    CHECK(check_memory_usage(bus,&observer,2,2)==0);
    CHECK(ok(managed_memory_service_status(manager,&pending,&bytes)) && pending==0 && bytes==0);
    cc_memory_plan_t state={0}; uint64_t revision=0;
    CHECK(ok(cc_managed_memory_editor_list(managed_memory_service_editor(manager),&state,&revision)) && state.count==2);
    int found=0; for (size_t i=0;i<state.count;i++) if (strcmp(state.records[i].text,"likes tea")==0) found=1;
    CHECK(found); cc_memory_plan_cleanup(&state);
    rc=managed_memory_service_recover_store(manager); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(cc_event_bus_unsubscribe(bus,subscription,1000)));
    cc_cache_metrics_destroy(metrics); cc_runtime_generation_release(generation);
    provider_usage_mode=0; provider_response="resumed"; managed_memory_service_destroy(manager); return 0;
}

static int exercise_owned_memory_turns(cc_runtime_builder_t *builder,recovery_service_t *recovery,
                                       const char *memory)
{
    cc_runtime_controller_config_t config={0}; config.size=sizeof(config);
    config.session_base="owned-memory"; config.fresh_session_on_start=1;
    cc_runtime_controller_t *controller=NULL;
    session_selection_t *selection=NULL;
    CHECK(ok(cc_runtime_controller_create(builder,&config,&controller)) &&
          ok(session_selection_create(controller,builder,recovery,&selection)) &&
          ok(session_selection_choose_new(selection)));
    cc_managed_memory_worker_options_t options={0}; options.size=sizeof(options);
    options.capacity=8; options.max_pending_items=3; options.max_pending_bytes=8192;
    options.select_eviction=memory_evict; options.decide_snapshot=memory_snapshot;
    cc_memory_extraction_options_t extraction={0}; extraction.size=sizeof(extraction); extraction.model="fake";
    managed_memory_service_t *memory_service=NULL;
    session_memory_turns_t *turns=NULL;
    CHECK(ok(managed_memory_service_create(builder,recovery,memory,&options,&extraction,&memory_service)) &&
          ok(session_memory_turns_create(memory_service,2,2,&turns)) &&
          session_memory_turns_uses(turns,builder,recovery));

    session_memory_turn_result_t result={0};
    provider_response="resumed";
    cc_result_t rc=session_memory_turns_submit(turns,selection,"first long answer",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && result.completed && result.awaiting_resolution &&
          result.response && strcmp(result.response,"resumed")==0 && result.run_id); cc_result_free(&rc);
    char *first=cc_copy_string(result.run_id); CHECK(first);
    rc=session_memory_turns_submit(turns,selection,"second long answer",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && result.completed && result.awaiting_resolution && result.run_id);
    cc_result_free(&rc); char *second=cc_copy_string(result.run_id); CHECK(second && strcmp(first,second)!=0);
    session_memory_pending_turn_t pending={0};
    CHECK(session_memory_turns_count(turns)==2 && ok(session_memory_turns_pending_at(turns,0,&pending)) &&
          pending.ready_to_commit && pending.has_answer && pending.retained_answer_bytes==strlen("resumed")+1U);
    int calls_before=provider_calls;
    rc=session_memory_turns_submit(turns,selection,"must not run",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && provider_calls==calls_before && session_memory_turns_count(turns)==2);
    cc_result_free(&rc);
    rc=session_memory_turns_destroy(turns); CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    session_memory_pump_result_t pump={0}; provider_response="{\"memories\":[]}";
    CHECK(ok(session_memory_turns_pump(turns,51,NULL,&pump)) && pump.committed==2 && pump.processed &&
          pump.remaining_ready==1 && !pump.remaining_reserved && session_memory_turns_count(turns)==0 &&
          provider_calls==calls_before+1);
    free(first); free(second);
    size_t ready=0,reserved=0;
    CHECK(ok(managed_memory_service_status_detail(memory_service,&ready,&reserved,NULL)) && ready==1 && !reserved);
    cc_managed_memory_worker_result_t processed={0};
    CHECK(ok(session_memory_turns_pump(turns,52,NULL,&pump)) && pump.processed && !pump.remaining_ready &&
          provider_calls==calls_before+2);

    /* When a completed answer cannot expand because an older queue entry uses
     * the byte budget, process that entry first and retry the same reservation. */
    const char *squeezed_input="squeezed input";
    size_t reservation_charge=64U+41U+strlen(session_selection_session_id(selection))+1U+
                              strlen(squeezed_input)+1U+1U+2U;
    size_t filler_base=64U+sizeof("filler")+sizeof("old-session")+sizeof("input")+1U;
    CHECK(reservation_charge+filler_base+3U<options.max_pending_bytes);
    size_t filler_length=options.max_pending_bytes-reservation_charge-filler_base-3U;
    char *filler=malloc(filler_length+1U); CHECK(filler);
    memset(filler,'x',filler_length); filler[filler_length]='\0';
    CHECK(ok(managed_memory_service_enqueue(memory_service,"filler","old-session","input",filler)));
    free(filler); provider_response="resumed";
    rc=session_memory_turns_submit(turns,selection,squeezed_input,NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && result.completed && result.awaiting_resolution);
    cc_result_free(&rc); calls_before=provider_calls; provider_response="{\"memories\":[]}";
    CHECK(ok(session_memory_turns_pump(turns,52,NULL,&pump)) && pump.processed && pump.committed==1 &&
          pump.remaining_ready==1 && !pump.remaining_reserved && provider_calls==calls_before+1);
    CHECK(ok(session_memory_turns_pump(turns,52,NULL,&pump)) && pump.processed && !pump.remaining_ready &&
          provider_calls==calls_before+2);

    provider_response="ok"; provider_fail=1;
    rc=session_memory_turns_submit(turns,selection,"interrupted input",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_NETWORK && result.awaiting_resolution && result.run_id &&
          session_selection_phase(selection)==SESSION_SELECTION_INTERRUPTED); cc_result_free(&rc);
    first=cc_copy_string(result.run_id); CHECK(first); provider_fail=0;
    session_memory_turn_inspection_t inspection={0};
    CHECK(ok(session_memory_turns_reconcile(turns,selection,first,&inspection)) &&
          inspection.state==SESSION_MEMORY_CHECKPOINT_ONLY && session_memory_turns_count(turns)==1);
    free(inspection.assistant_text);
    CHECK(ok(session_memory_turns_resume(turns,selection,first,NULL,NULL,&result)) && result.completed &&
          !result.awaiting_resolution && result.response && strcmp(result.response,"ok")==0 &&
          session_memory_turns_count(turns)==0);
    free(first); provider_response="{\"memories\":[]}";
    CHECK(ok(managed_memory_service_process_one(memory_service,53,NULL,&processed)));

    /* A missing checkpoint and missing answer alone must not resolve an
     * interrupted run or discard its reserved input. */
    tool_fail=1;
    CHECK(attempt_trusted_tool(builder,"owner-unknown","owner-unknown-1",0));
    tool_fail=0; cc_pending_operation_t *operations=NULL; size_t operation_count=0;
    CHECK(ok(recovery_service_list_operations(recovery,&operations,&operation_count)));
    char *unknown_operation=NULL;
    for (size_t i=0;i<operation_count;i++)
        if (strcmp(operations[i].call.name,"owner-unknown")==0)
            unknown_operation=cc_copy_string(operations[i].operation_id);
    cc_pending_operation_array_free(operations,operation_count); CHECK(unknown_operation);
    provider_response="ok"; provider_fail=1;
    rc=session_memory_turns_submit(turns,selection,"no evidence input",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_NETWORK && result.awaiting_resolution && result.run_id); cc_result_free(&rc);
    first=cc_copy_string(result.run_id); CHECK(first); provider_fail=0;
    cc_runtime_generation_t *generation=NULL; cc_run_checkpoint_t saved={0};
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&generation)) &&
          ok(cc_agent_runtime_load_interrupted_run(cc_runtime_generation_runtime(generation),first,&saved)) &&
          ok(cc_agent_runtime_abandon_run(cc_runtime_generation_runtime(generation),first)));
    char *expected_input_id=NULL;
    CHECK(ok(cc_run_checkpoint_derive_user_message_id(first,&expected_input_id)) &&
          strcmp(saved.user_message_id,expected_input_id)==0);
    cc_runtime_generation_release(generation);
    memset(&inspection,0,sizeof(inspection));
    CHECK(ok(session_memory_turns_reconcile(turns,selection,first,&inspection)) &&
          inspection.state==SESSION_MEMORY_NO_DURABLE_EVIDENCE && session_memory_turns_count(turns)==1);
    free(inspection.assistant_text);
    CHECK(ok(session_memory_turns_abandon_missing(turns,selection,first,&result)) && result.abandoned &&
          !result.awaiting_resolution && session_memory_turns_count(turns)==0 &&
          session_selection_phase(selection)==SESSION_SELECTION_CHATTING);
    int calls_before_tool=tool_calls;
    CHECK(attempt_trusted_tool(builder,"owner-unknown","owner-unknown-2",0) && tool_calls==calls_before_tool &&
          attempt_trusted_tool(builder,"owner-other","owner-other-1",1) && tool_calls==calls_before_tool+1 &&
          ok(recovery_service_resolve_operation(recovery,unknown_operation,CC_TOOL_RECOVERY_NOT_EXECUTED,NULL)));
    free(unknown_operation);
    cc_message_t *retained_history=NULL; size_t retained_count=0,retained_matches=0;
    CHECK(ok(cc_runtime_controller_load_messages(controller,session_selection_session_id(selection),100,
                                                  &retained_history,&retained_count)));
    for (size_t i=0;i<retained_count;i++) {
        if (retained_history[i].id && strcmp(retained_history[i].id,expected_input_id)==0) retained_matches++;
        cc_message_cleanup(&retained_history[i]);
    }
    free(retained_history); CHECK(retained_matches==1);
    calls_before=provider_calls; char *unexpected_answer=NULL;
    rc=cc_runtime_controller_submit_text_with_run_id(controller,session_selection_session_id(selection),
                                                      first,"no evidence input",NULL,NULL,&unexpected_answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !unexpected_answer && provider_calls==calls_before); cc_result_free(&rc);
    cc_run_checkpoint_cleanup(&saved); free(expected_input_id); free(first); provider_response="{\"memories\":[]}";
    CHECK(ok(managed_memory_service_process_one(memory_service,53,NULL,&processed)));

    /* If the final answer was committed but the reply was lost, recover it
     * from history and enqueue it without asking the conversation model again. */
    provider_response="ok"; provider_fail=1;
    rc=session_memory_turns_submit(turns,selection,"final already saved",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_NETWORK && result.awaiting_resolution && result.run_id); cc_result_free(&rc);
    first=cc_copy_string(result.run_id); CHECK(first); provider_fail=0;
    generation=NULL; CHECK(ok(cc_runtime_builder_acquire_generation(builder,&generation)));
    cc_agent_runtime_t *runtime=cc_runtime_generation_runtime(generation);
    cc_session_store_t *store=cc_agent_runtime_session_store(runtime);
    char final_id[64]; snprintf(final_id,sizeof(final_id),"%s-final",first);
    cc_message_t *final_message=NULL;
    CHECK(ok(cc_message_create_text(final_id,session_selection_session_id(selection),CC_ROLE_ASSISTANT,
                                    "ok",NULL,&final_message)));
    cc_session_record_t final_record={.type=CC_SESSION_RECORD_MESSAGE,
        .session_id=session_selection_session_id(selection),.data.message=final_message};
    CHECK(ok(store->vtable->append_records(store->self,&final_record,1)) &&
          ok(cc_agent_runtime_abandon_run(runtime,first)));
    cc_message_destroy(final_message); cc_runtime_generation_release(generation);
    rc=session_memory_turns_abandon_missing(turns,selection,first,&result);
    CHECK(rc.code==CC_ERR_INVALID_STATE && result.awaiting_resolution && session_memory_turns_count(turns)==1);
    cc_result_free(&rc);
    calls_before=provider_calls; memset(&inspection,0,sizeof(inspection));
    CHECK(ok(session_memory_turns_reconcile(turns,selection,first,&inspection)) &&
          inspection.state==SESSION_MEMORY_FINAL_ONLY && !session_memory_turns_count(turns) &&
          provider_calls==calls_before && ok(session_selection_refresh_runs(selection)));
    free(inspection.assistant_text); free(first); provider_response="{\"memories\":[]}";
    expect_recovered_memory=1; expected_memory_input="final already saved";
    expected_memory_response="\"assistant_response\":\"ok\""; recovered_memory_seen=0;
    CHECK(ok(managed_memory_service_process_one(memory_service,53,NULL,&processed)) && recovered_memory_seen);
    expect_recovered_memory=0; expected_memory_input="old input"; expected_memory_response="resumed";

    provider_response="ok"; provider_fail=1;
    rc=session_memory_turns_submit(turns,selection,"abandoned owner input",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_NETWORK && result.awaiting_resolution && result.run_id); cc_result_free(&rc);
    first=cc_copy_string(result.run_id); CHECK(first); provider_fail=0; fail_before_abandon=1;
    rc=session_memory_turns_abandon(turns,selection,first,&result);
    CHECK(rc.code==CC_ERR_IO && result.awaiting_resolution && result.response && !result.response[0]);
    cc_result_free(&rc); fail_before_abandon=0;
    CHECK(ok(session_memory_turns_abandon(turns,selection,first,&result)) && result.abandoned &&
          !result.awaiting_resolution && session_memory_turns_count(turns)==0);
    free(first); provider_response="{\"memories\":[]}"; expect_recovered_memory=1;
    expected_memory_input="abandoned owner input"; expected_memory_response="\"assistant_response\":\"\"";
    recovered_memory_seen=0;
    CHECK(ok(managed_memory_service_process_one(memory_service,54,NULL,&processed)) && recovered_memory_seen);
    expect_recovered_memory=0; expected_memory_input="old input"; expected_memory_response="resumed";

    provider_response="ok"; provider_fail=1;
    rc=session_memory_turns_submit(turns,selection,"uncertain abandoned input",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_NETWORK && result.awaiting_resolution && result.run_id); cc_result_free(&rc);
    first=cc_copy_string(result.run_id); CHECK(first); provider_fail=0; change_after_abandon=1;
    rc=session_memory_turns_abandon(turns,selection,first,&result);
    CHECK(rc.code==CC_ERR_STORAGE && result.awaiting_resolution && result.response && !result.response[0]);
    cc_result_free(&rc); change_after_abandon=0;
    mount_generation--; /* Only the host fixture can restore an old generation. */
    memset(&inspection,0,sizeof(inspection));
    CHECK(ok(session_memory_turns_reconcile(turns,selection,first,&inspection)) &&
          inspection.state==SESSION_MEMORY_NO_DURABLE_EVIDENCE && session_memory_turns_count(turns)==0);
    free(inspection.assistant_text); free(first); provider_response="{\"memories\":[]}";
    expect_recovered_memory=1; expected_memory_input="uncertain abandoned input";
    expected_memory_response="\"assistant_response\":\"\""; recovered_memory_seen=0;
    CHECK(ok(managed_memory_service_process_one(memory_service,55,NULL,&processed)) && recovered_memory_seen);
    expect_recovered_memory=0; expected_memory_input="old input"; expected_memory_response="resumed";
    session_selection_destroy(selection); CHECK(ok(cc_runtime_controller_destroy(controller,1000)));

    /* The same owner is used by ordinary chat and by the synchronized
     * continue/abandon menu, so an interrupted input is evaluated once. */
    controller=NULL; session_menu_t *menu=NULL;
    CHECK(ok(cc_runtime_controller_create(builder,&config,&controller)) &&
          ok(session_menu_create_with_memory(controller,builder,recovery,turns,&menu)) &&
          session_menu_present(menu)==ESP_OK &&
          session_menu_handle(menu,"/new",APP_EVENT_SOURCE_WEB,NULL,NULL) && !session_menu_waiting(menu));
    /* Failure before session creation still retains the accepted input. An
     * explicit abandonment creates the session and records it exactly once. */
    fail_before_controller_submit=1; calls_before=provider_calls;
    rc=session_menu_submit_turn(menu,"prestart abandoned input",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_NETWORK && result.awaiting_resolution && result.run_id &&
          !session_menu_waiting(menu) && provider_calls==calls_before); cc_result_free(&rc);
    first=cc_copy_string(result.run_id); CHECK(first); fail_before_controller_submit=0;
    cc_runtime_controller_status_t current={0}; cc_runtime_controller_get_status(controller,&current);
    CHECK(!current.session_is_persisted && session_menu_handle_memory(menu,"/memory-pending",APP_EVENT_SOURCE_WEB) &&
          session_menu_handle_memory(menu,"/memory-abandon 1",APP_EVENT_SOURCE_SERIAL) &&
          session_memory_turns_count(turns)==0);
    cc_runtime_controller_get_status(controller,&current); CHECK(current.session_is_persisted);
    retained_history=NULL; retained_count=0; retained_matches=0;
    CHECK(ok(cc_run_checkpoint_derive_user_message_id(first,&expected_input_id)) &&
          ok(cc_runtime_controller_load_messages(controller,current.session_id,100,&retained_history,&retained_count)));
    for (size_t i=0;i<retained_count;i++) {
        if (retained_history[i].id && strcmp(retained_history[i].id,expected_input_id)==0) retained_matches++;
        cc_message_cleanup(&retained_history[i]);
    }
    free(retained_history); CHECK(retained_matches==1);
    unexpected_answer=NULL;
    rc=cc_runtime_controller_submit_text_with_run_id(controller,current.session_id,first,
                                                      "prestart abandoned input",NULL,NULL,&unexpected_answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !unexpected_answer && provider_calls==calls_before); cc_result_free(&rc);
    free(expected_input_id); free(first); provider_response="{\"memories\":[]}";
    expect_recovered_memory=1; expected_memory_input="prestart abandoned input";
    expected_memory_response="\"assistant_response\":\"\""; recovered_memory_seen=0;
    CHECK(ok(managed_memory_service_process_one(memory_service,56,NULL,&processed)) && recovered_memory_seen);
    expect_recovered_memory=0; expected_memory_input="old input"; expected_memory_response="resumed";
    provider_response="resumed";
    rc=session_menu_submit_turn(menu,"menu long answer",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && result.completed && result.awaiting_resolution &&
          session_memory_turns_count(turns)==1); cc_result_free(&rc);
    CHECK(session_menu_handle_memory(menu,"/memory-pending",APP_EVENT_SOURCE_WEB) && strstr(view_text,"待排队"));
    session_menu_invalidate_view(menu);
    CHECK(session_menu_handle_memory(menu,"/memory-retry 1",APP_EVENT_SOURCE_SERIAL) &&
          session_memory_turns_count(turns)==1);
    CHECK(session_menu_handle_memory(menu,"/memory-pending",APP_EVENT_SOURCE_SERIAL) &&
          session_menu_handle_memory(menu,"/memory-retry 1",APP_EVENT_SOURCE_WEB) &&
          session_memory_turns_count(turns)==0);
    provider_response="{\"memories\":[]}";
    calls_before=provider_calls; mount_generation++;
    rc=session_memory_turns_pump(turns,56,NULL,&pump);
    CHECK(rc.code==CC_ERR_STORAGE && !pump.processed && pump.remaining_ready==1 && provider_calls==calls_before);
    cc_result_free(&rc); mount_generation--;
    CHECK(ok(session_memory_turns_pump(turns,56,NULL,&pump)) && pump.processed && !pump.remaining_ready &&
          provider_calls==calls_before+1);
    session_menu_refresh_memory_view(menu); CHECK(strstr(view_text,"待评估 0 条"));
    provider_response="ok"; provider_fail=1;
    rc=session_menu_submit_turn(menu,"menu abandoned input",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_NETWORK && result.awaiting_resolution && session_menu_waiting(menu) &&
          strstr(view_text,"中断任务")); cc_result_free(&rc); provider_fail=0;
    calls_before=provider_calls;
    CHECK(session_menu_handle_memory(menu,"/memory-pending",APP_EVENT_SOURCE_WEB) &&
          session_menu_handle_memory(menu,"/memory-abandon 1",APP_EVENT_SOURCE_SERIAL) &&
          session_memory_turns_count(turns)==1 && provider_calls==calls_before &&
          session_menu_handle(menu,"/runs",APP_EVENT_SOURCE_WEB,NULL,NULL) && strstr(view_text,"中断任务"));
    CHECK(session_menu_handle(menu,"/abandon 1",APP_EVENT_SOURCE_SERIAL,NULL,NULL) &&
          !session_menu_waiting(menu) && session_memory_turns_count(turns)==0);
    provider_response="{\"memories\":[]}"; expect_recovered_memory=1;
    expected_memory_input="menu abandoned input"; expected_memory_response="\"assistant_response\":\"\"";
    recovered_memory_seen=0;
    CHECK(ok(managed_memory_service_process_one(memory_service,56,NULL,&processed)) && recovered_memory_seen);
    expect_recovered_memory=0; expected_memory_input="old input"; expected_memory_response="resumed";
    provider_response="resumed";
    rc=session_menu_submit_turn(menu,"volatile shutdown input",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && result.completed && result.awaiting_resolution);
    cc_result_free(&rc);
    session_menu_destroy(menu);
    CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    rc=session_memory_turns_destroy(turns);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(session_memory_turns_destroy_for_shutdown(turns)));
    CHECK(ok(managed_memory_service_status_detail(memory_service,&ready,&reserved,NULL)) && !reserved);
    managed_memory_service_destroy(memory_service); return 0;
}

static int exercise_default_memory_owner(cc_runtime_builder_t *builder,recovery_service_t *recovery,
                                         const char *memory_path)
{
    app_config_t config={0};
    config.runtime.memory_path=(char *)memory_path; config.runtime.model="fake";
    config.runtime.max_input_bytes=1024; config.runtime.max_output_bytes=64;
    config.summary_cache_per_session=2; config.summary_cache_max_entries=128; config.summary_cache_max_bytes=1048576;
    config.managed_memory_capacity=100; config.managed_memory_pending_items=1;
    config.managed_memory_pending_bytes=4096;
    startup_managed_memory_t *owner=NULL;
    config.managed_memory_pending_bytes=64;
    cc_result_t rc=startup_managed_memory_create(&config,builder,recovery,&owner);
    CHECK(rc.code==CC_ERR_INVALID_ARGUMENT && !owner); cc_result_free(&rc);
    config.managed_memory_pending_bytes=4096;
    CHECK(ok(startup_managed_memory_create(&config,builder,recovery,&owner)) && owner);
    CHECK(!startup_managed_memory_summary_prune_deferred(owner));
    cc_memory_store_t *device_memory=cc_runtime_builder_borrow_memory_store(builder);
    CHECK(ok(cc_memory_store_set(device_memory,"integration-summary-1","one","context_summary","cache-test")) &&
          ok(cc_memory_store_set(device_memory,"integration-summary-2","two","context_summary","cache-test")) &&
          ok(cc_memory_store_set(device_memory,"integration-summary-3","three","context_summary","cache-test")));
    cc_memory_entry_t cached={0};
    rc=cc_memory_store_get(device_memory,"integration-summary-1",&cached);
    CHECK(rc.code==CC_ERR_NOT_FOUND); cc_result_free(&rc);
    CHECK(ok(cc_memory_store_get(device_memory,"integration-summary-2",&cached)));
    cc_memory_entry_free(&cached);
    session_memory_turns_t *turns=startup_managed_memory_turns(owner); CHECK(turns);
    cc_runtime_controller_config_t controller_config={0};
    controller_config.size=sizeof(controller_config); controller_config.session_base="default-memory";
    controller_config.fresh_session_on_start=1;
    cc_runtime_controller_t *controller=NULL; session_menu_t *menu=NULL;
    CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)) &&
          ok(session_menu_create_with_memory(controller,builder,recovery,turns,&menu)) &&
          session_menu_present(menu)==ESP_OK &&
          session_menu_handle(menu,"/new",APP_EVENT_SOURCE_WEB,NULL,NULL));
    provider_response="ack"; session_memory_turn_result_t result={0};
    CHECK(ok(session_menu_submit_turn(menu,"I prefer coffee",NULL,NULL,&result)) && result.completed);
    cc_memory_entry_t *entries=NULL; size_t entry_count=0;
    CHECK(ok(cc_memory_store_list(device_memory,NULL,0,&entries,&entry_count)));
    for (size_t i=0;i<entry_count;i++) CHECK(strncmp(entries[i].key,"active.",7)!=0);
    cc_memory_entry_free_array(entries,entry_count);
    memory_console_t *console=NULL;
    CHECK(ok(memory_console_create_with_turns(session_memory_turns_editor(turns),recovery,turns,&console)));
    cc_memory_plan_t state={0}; uint64_t revision=0;
    CHECK(ok(cc_managed_memory_editor_list(session_memory_turns_editor(turns),&state,&revision)) && state.count>0);
    size_t before_reset=state.count; cc_memory_plan_cleanup(&state);
    memory_console_handle(console,"/memories",APP_EVENT_SOURCE_WEB,60);
    memory_console_handle(console,"/memory reset",APP_EVENT_SOURCE_SERIAL,60);
    CHECK(ok(cc_managed_memory_editor_list(session_memory_turns_editor(turns),&state,&revision)) &&
          state.count==before_reset);
    cc_memory_plan_cleanup(&state);
    provider_fail=1; session_memory_pump_result_t pump={0};
    rc=session_memory_turns_pump(turns,60,NULL,&pump);
    CHECK(rc.code==CC_ERR_NETWORK && pump.remaining_ready==1); cc_result_free(&rc);
    int calls_before=provider_calls;
    rc=session_menu_submit_turn(menu,"must wait",NULL,NULL,&result);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && provider_calls==calls_before); cc_result_free(&rc);
    provider_fail=0;
    provider_response="{\"memories\":[{\"action\":\"add\",\"target_id\":\"\","
        "\"text\":\"prefers coffee\",\"category\":\"preference\",\"importance\":70,"
        "\"evidence\":\"prefer coffee\"}]}";
    CHECK(ok(session_memory_turns_pump(turns,61,NULL,&pump)) && pump.processed && !pump.remaining_ready);
    CHECK(ok(cc_managed_memory_editor_list(session_memory_turns_editor(turns),&state,&revision)));
    int found=0;
    for (size_t i=0;i<state.count;i++)
        if (strcmp(state.records[i].text,"prefers coffee")==0) found=1;
    CHECK(found); cc_memory_plan_cleanup(&state);
    memory_console_handle(console,"/memories",APP_EVENT_SOURCE_WEB,62);
    memory_console_handle(console,"/memory reset",APP_EVENT_SOURCE_SERIAL,62);
    CHECK(ok(cc_managed_memory_editor_list(session_memory_turns_editor(turns),&state,&revision)) &&
          state.count==0);
    cc_memory_plan_cleanup(&state);
    cc_filesystem_t fs={0}; CHECK(ok(__real_cc_filesystem_get_default(&fs)));
    cc_memory_snapshot_store_t *snapshots=NULL;
    CHECK(ok(cc_memory_snapshot_store_open(&fs,APP_RECOVERY_ROOT "/memory_versions",memory_path,&snapshots)));
    cc_memory_snapshot_info_t *versions=NULL; size_t version_count=0;
    CHECK(ok(cc_memory_snapshot_list(snapshots,&versions,&version_count)));
    found=0;
    for (size_t i=0;i<version_count;i++) if (versions[i].reason==CC_MEMORY_SNAPSHOT_RESET) {
        char *saved=NULL;
        CHECK(ok(cc_memory_snapshot_read(snapshots,versions[i].id,&saved)));
        if (strstr(saved,"prefers coffee")) found=1;
        free(saved);
    }
    CHECK(found);
    cc_memory_snapshot_info_free_array(versions,version_count);
    cc_memory_snapshot_store_destroy(snapshots); fs.vtable->destroy(fs.self);
    memory_console_destroy(console);
    session_menu_destroy(menu); CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    CHECK(ok(startup_managed_memory_destroy(&owner)) && !owner);
    provider_response="resumed";
    return 0;
}

static int remove_tree(cc_filesystem_t *fs,const char *path)
{
    cc_filesystem_stat_t info={.size=sizeof(info)}; if (!ok(fs->vtable->stat(fs->self,path,&info))) return 0;
    if (info.type==CC_FS_ENTRY_DIRECTORY) {
        char **names=NULL; size_t count=0; if (!ok(fs->vtable->list_dir(fs->self,path,&names,&count))) return 0;
        int good=1; for (size_t i=0;i<count;i++) { size_t n=strlen(path)+strlen(names[i])+2; char *child=malloc(n);
            if (!child) good=0; else { snprintf(child,n,"%s/%s",path,names[i]); if (!remove_tree(fs,child)) good=0; free(child); } free(names[i]); }
        free(names); if (!good) return 0;
    }
    return info.type==CC_FS_ENTRY_MISSING || ok(fs->vtable->remove(fs->self,path));
}

static int test_automatic_memory_recovery(cc_filesystem_t *fs)
{
    char workspace[256],sessions[256],memory[256],logs[256];
    snprintf(workspace,sizeof(workspace),"%s/workspace",RECOVERY_TEST_BASE);
    snprintf(sessions,sizeof(sessions),"%s/sessions",RECOVERY_TEST_BASE);
    snprintf(memory,sizeof(memory),"%s/memory.json",RECOVERY_TEST_BASE);
    snprintf(logs,sizeof(logs),"%s/logs",RECOVERY_TEST_BASE);
    cc_config_t config={0}; config.provider="fake"; config.model="fake"; config.data_dir=RECOVERY_TEST_BASE;
    config.workspace_path=workspace; config.storage_path=sessions;
    config.storage_type="json_segmented"; config.memory_path=memory;
    app_runtime_io_config_t io={0}; io.max_pending_operations=64; io.usb_log.dir=logs;
    cc_runtime_builder_t *builder=NULL; recovery_service_t *recovery=NULL;
    wrap_stage_filesystem=1;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
          ok(recovery_service_open(builder,&config,&io,&recovery)));
    cc_memory_store_t *store=cc_runtime_builder_borrow_memory_store(builder);
    CHECK(ok(cc_memory_store_set(store,"legacy-note","unmanaged legacy-token","preference","old")) &&
          ok(cc_memory_store_set(store,"summary-key","derived cache","context_summary","old")) &&
          ok(cc_memory_store_set(store,"defer-1","one","context_summary","defer")) &&
          ok(cc_memory_store_set(store,"defer-2","two","context_summary","defer")));
    session_replace_fail_path=memory; session_replace_fail_mode=2;
    cc_result_t deferred_write=cc_memory_store_set(store,"defer-3","three","context_summary","defer");
    CHECK(deferred_write.code==CC_ERR_IO); cc_result_free(&deferred_write);
    app_config_t memory_config={0}; memory_config.runtime=config;
    memory_config.runtime.max_input_bytes=1024; memory_config.runtime.max_output_bytes=128;
    memory_config.summary_cache_per_session=2; memory_config.summary_cache_max_entries=128; memory_config.summary_cache_max_bytes=1048576;
    memory_config.managed_memory_capacity=100; memory_config.managed_memory_pending_items=2;
    memory_config.managed_memory_pending_bytes=4096;
    startup_managed_memory_t *owner=NULL;
    CHECK(ok(startup_managed_memory_create(&memory_config,builder,recovery,&owner)));
    CHECK(startup_managed_memory_summary_prune_deferred(owner));
    /* No generation or worker is active yet; recovery can safely reload the
     * installed file, and the next cache write enforces the deferred limit. */
    CHECK(ok(cc_json_memory_recover_in_place(store)) &&
          ok(cc_memory_store_set(store,"defer-4","four","context_summary","defer")));
    cc_memory_entry_t deferred_entry={0};
    deferred_write=cc_memory_store_get(store,"defer-1",&deferred_entry);
    CHECK(deferred_write.code==CC_ERR_NOT_FOUND); cc_result_free(&deferred_write);
    deferred_write=cc_memory_store_get(store,"defer-2",&deferred_entry);
    CHECK(deferred_write.code==CC_ERR_NOT_FOUND); cc_result_free(&deferred_write);
    CHECK(ok(cc_memory_store_get(store,"defer-3",&deferred_entry)));
    cc_memory_entry_free(&deferred_entry);
    CHECK(ok(cc_memory_store_delete(store,"defer-3")) && ok(cc_memory_store_delete(store,"defer-4")));
    /* A known-unchanged pruning failure, such as a full disk, also leaves
     * startup usable and retries cleanup on a later summary write. */
    CHECK(ok(startup_managed_memory_destroy(&owner)) &&
          ok(cc_json_memory_set_summary_cache_limits(store,3,128,1048576)) &&
          ok(cc_memory_store_set(store,"retry-1","one","context_summary","retry")) &&
          ok(cc_memory_store_set(store,"retry-2","two","context_summary","retry")) &&
          ok(cc_memory_store_set(store,"retry-3","three","context_summary","retry")));
    session_replace_fail_mode=1;
    CHECK(ok(startup_managed_memory_create(&memory_config,builder,recovery,&owner)) &&
          startup_managed_memory_summary_prune_deferred(owner));
    CHECK(ok(cc_memory_store_get(store,"retry-1",&deferred_entry)));
    cc_memory_entry_free(&deferred_entry);
    CHECK(ok(cc_memory_store_set(store,"retry-4","four","context_summary","retry")));
    deferred_write=cc_memory_store_get(store,"retry-1",&deferred_entry);
    CHECK(deferred_write.code==CC_ERR_NOT_FOUND); cc_result_free(&deferred_write);
    CHECK(ok(cc_memory_store_delete(store,"retry-3")) && ok(cc_memory_store_delete(store,"retry-4")));
    cc_memory_entry_t *unmanaged_results=NULL; size_t unmanaged_count=0;
    CHECK(ok(cc_memory_store_search(store,"legacy-token",10,&unmanaged_results,&unmanaged_count)) &&
          unmanaged_count==0);
    cc_memory_entry_free_array(unmanaged_results,unmanaged_count);
    unmanaged_results=NULL; unmanaged_count=0;
    CHECK(ok(cc_memory_store_list(store,NULL,10,&unmanaged_results,&unmanaged_count)) && unmanaged_count==0);
    cc_memory_entry_free_array(unmanaged_results,unmanaged_count);
    cc_memory_query_t recall_query={.size=sizeof(recall_query),.query="legacy-token",.limit=10};
    cc_memory_search_result_t *recall_results=NULL; size_t recall_count=0;
    CHECK(ok(cc_memory_store_query(store,&recall_query,&recall_results,&recall_count)) && recall_count==0);
    cc_memory_search_result_free_array(recall_results,recall_count);
    cc_memory_query_t summary_query={.size=sizeof(summary_query),.category="context_summary",.limit=10};
    cc_memory_search_result_t *summary_results=NULL; size_t summary_count=0;
    CHECK(ok(cc_memory_store_query(store,&summary_query,&summary_results,&summary_count)) && summary_count==1);
    cc_memory_search_result_free_array(summary_results,summary_count);
    session_memory_turns_t *turns=startup_managed_memory_turns(owner);
    cc_runtime_controller_config_t ccfg={.size=sizeof(ccfg),.session_base="memory-recovery",.fresh_session_on_start=1};
    cc_runtime_controller_t *controller=NULL; session_menu_t *menu=NULL;
    CHECK(ok(cc_runtime_controller_create(builder,&ccfg,&controller)) &&
          ok(session_menu_create_with_memory(controller,builder,recovery,turns,&menu)) &&
          session_menu_present(menu)==ESP_OK && session_menu_handle(menu,"/new",APP_EVENT_SOURCE_WEB,NULL,NULL));
    provider_response="ack"; session_memory_turn_result_t turn={0};
    CHECK(ok(session_menu_submit_turn(menu,"I prefer coffee",NULL,NULL,&turn)) && turn.completed);
    provider_response="{\"memories\":[{\"action\":\"add\",\"target_id\":\"\","
        "\"text\":\"prefers coffee\",\"category\":\"preference\",\"importance\":70,"
        "\"evidence\":\"prefer coffee\"}]}";
    session_replace_fail_path=memory; session_replace_fail_mode=2;
    cc_runtime_generation_t *held=NULL; CHECK(ok(cc_runtime_builder_acquire_generation(builder,&held)));
    session_memory_pump_result_t pump={0};
    cc_result_t rc=session_memory_turns_pump(turns,70,NULL,&pump);
    CHECK(rc.code==CC_ERR_IO && pump.remaining_ready==1); cc_result_free(&rc);
    int calls_after_model=provider_calls, ready=1;
    CHECK(ok(cc_json_memory_write_ready(store,&ready)) && !ready);
    cc_runtime_generation_release(held);
    /* The first retry observes the latch and reloads only after leases drain. */
    rc=session_memory_turns_pump(turns,71,NULL,&pump);
    CHECK(rc.code==CC_ERR_STORAGE && pump.remaining_ready==1); cc_result_free(&rc);
    CHECK(ok(cc_json_memory_write_ready(store,&ready)) && ready && provider_calls==calls_after_model);
    /* The retained plan proves the installed result; do not invoke the model again. */
    CHECK(ok(session_memory_turns_pump(turns,72,NULL,&pump)) && pump.processed &&
          !pump.remaining_ready && provider_calls==calls_after_model);
    unmanaged_results=NULL; unmanaged_count=0;
    CHECK(ok(cc_memory_store_search(store,"legacy-token",10,&unmanaged_results,&unmanaged_count)) &&
          unmanaged_count==0); /* The restriction survives a recovery reload. */
    cc_memory_entry_free_array(unmanaged_results,unmanaged_count);
    cc_memory_entry_t retained={0};
    CHECK(ok(cc_memory_store_get(store,"legacy-note",&retained))); cc_memory_entry_free(&retained);
    cc_memory_plan_t state={0}; uint64_t revision=0;
    CHECK(ok(cc_managed_memory_editor_list(session_memory_turns_editor(turns),&state,&revision)) &&
          state.count==1 && strcmp(state.records[0].text,"prefers coffee")==0);
    cc_memory_plan_cleanup(&state);
    /* 重置可安装空受管理集合但仍返回 installed=0，控制台提示核对，
     * 解除回读故障后重新加载并确认盘上结果。 */
    memory_console_t *console=NULL;
    CHECK(ok(memory_console_create_with_turns(session_memory_turns_editor(turns),recovery,turns,&console)));
    memory_console_handle(console,"/memories",APP_EVENT_SOURCE_WEB,73);
    session_replace_fail_mode=3;
    memory_console_handle(console,"/memory reset",APP_EVENT_SOURCE_SERIAL,73);
    CHECK(strstr(last_event_text,"未获成功确认")!=NULL);
    CHECK(ok(cc_json_memory_write_ready(store,&ready)) && !ready);
    char *disk_memory=NULL;
    CHECK(ok(fs->vtable->read_text(fs->self,memory,&disk_memory)));
    CHECK(strstr(disk_memory,"prefers coffee")==NULL);
    free(disk_memory);
    memory_primary_read_fail=0;
    /* 同步夹具的 generation 租约和 worker 调用均已退出，可原位重载。 */
    CHECK(ok(cc_json_memory_recover_in_place(store)));
    CHECK(ok(cc_managed_memory_editor_list(session_memory_turns_editor(turns),&state,&revision)) && state.count==0);
    cc_memory_plan_cleanup(&state);
    memory_console_destroy(console);
    session_menu_destroy(menu); CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    CHECK(ok(startup_managed_memory_destroy(&owner)) && ok(cc_runtime_builder_destroy(builder,1000)));
    recovery_service_destroy(recovery);
    session_replace_fail_path=NULL; session_replace_fail_mode=0; wrap_stage_filesystem=0; provider_response="resumed";
    CHECK(remove_tree(fs,RECOVERY_TEST_BASE));
    return 0;
}

static int test_json_session_write_recovery(cc_filesystem_t *fs)
{
    CHECK(ok(fs->vtable->make_dir(fs->self,RECOVERY_TEST_BASE)));
    char path[256],tmp[264];
    snprintf(path,sizeof(path),"%s/sessions.json",RECOVERY_TEST_BASE);
    snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    wrap_stage_filesystem=1; session_replace_fail_path=path;
    cc_session_store_t store={0};
    CHECK(ok(cc_json_file_store_create(path,&store)) && ok(store.vtable->create_session(store.self,"s1",".")));
    session_replace_fail_mode=2;
    cc_result_t rc=store.vtable->create_session(store.self,"s2",".");
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    cc_session_t *listed=NULL; size_t count=0;
    rc=store.vtable->list_sessions(store.self,&listed,&count);
    CHECK(rc.code==CC_ERR_STORAGE && !listed && count==0); cc_result_free(&rc);
    rc=store.vtable->create_session(store.self,"s3",".");
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    store.vtable->destroy(store.self);
    memset(&store,0,sizeof(store));
    CHECK(ok(cc_json_file_store_create(path,&store)) &&
          ok(store.vtable->list_sessions(store.self,&listed,&count)) && count==2);
    cc_session_free_array(listed,count);
    session_replace_fail_mode=1;
    rc=store.vtable->create_session(store.self,"s3",".");
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    listed=NULL; count=0;
    rc=store.vtable->list_sessions(store.self,&listed,&count);
    CHECK(rc.code==CC_ERR_STORAGE && !listed && count==0); cc_result_free(&rc);
    store.vtable->destroy(store.self); memset(&store,0,sizeof(store));
    rc=cc_json_file_store_create(path,&store);
    CHECK(rc.code==CC_ERR_STORAGE && !store.self); cc_result_free(&rc);
    int tmp_exists=0;
    CHECK(ok(fs->vtable->exists(fs->self,tmp,&tmp_exists)) && tmp_exists);
    CHECK(ok(fs->vtable->remove(fs->self,tmp)) && ok(cc_json_file_store_create(path,&store)) &&
          ok(store.vtable->list_sessions(store.self,&listed,&count)) && count==2);
    cc_session_free_array(listed,count); store.vtable->destroy(store.self);
    session_replace_fail_path=NULL; wrap_stage_filesystem=0;
    CHECK(remove_tree(fs,RECOVERY_TEST_BASE));
    return 0;
}

static int test_segmented_manifest_write_recovery(cc_filesystem_t *fs)
{
    CHECK(ok(fs->vtable->make_dir(fs->self,RECOVERY_TEST_BASE)));
    char sessions[256]; snprintf(sessions,sizeof(sessions),"%s/segments",RECOVERY_TEST_BASE);
    wrap_stage_filesystem=1;
    cc_session_store_t store={0};
    CHECK(ok(cc_json_segmented_store_create(sessions,1,"media",1024,&store)) &&
          ok(store.vtable->create_session(store.self,"s1",".")));
    char **names=NULL; size_t names_count=0;
    CHECK(ok(fs->vtable->list_dir(fs->self,sessions,&names,&names_count)) && names_count==1);
    char manifest[512],tmp[520];
    CHECK(snprintf(manifest,sizeof(manifest),"%s/%s/manifest.json",sessions,names[0])>0);
    CHECK(snprintf(tmp,sizeof(tmp),"%s.tmp",manifest)>0);
    for (size_t i=0;i<names_count;i++) free(names[i]);
    free(names);
    session_replace_fail_path=manifest;
    cc_result_t rc=cc_result_ok();
    int exists=0;
    cc_message_t *one=NULL,*two=NULL,*three=NULL;
    CHECK(ok(cc_message_create_text("one","s1",CC_ROLE_USER,"one",NULL,&one)) &&
          ok(cc_message_create_text("two","s1",CC_ROLE_USER,"two",NULL,&two)) &&
          ok(cc_message_create_text("three","s1",CC_ROLE_USER,"three",NULL,&three)));
    cc_session_record_t record={.type=CC_SESSION_RECORD_MESSAGE,.session_id="s1",.data.message=one};
    CHECK(ok(store.vtable->append_records(store.self,&record,1)));
    session_replace_fail_mode=1;
    record.data.message=two;
    rc=store.vtable->append_records(store.self,&record,1);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    exists=0; CHECK(ok(fs->vtable->exists(fs->self,tmp,&exists)) && exists);
    cc_message_t *messages=NULL; size_t count=0; int more=0;
    rc=cc_json_segmented_store_load_page(&store,"s1",NULL,8,&messages,&count,&more);
    CHECK(rc.code==CC_ERR_STORAGE && !messages && count==0); cc_result_free(&rc);
    /* Simulate an operator resolving the interrupted install: the empty next
     * segment remains, can safely be reused, and no prior message is lost. */
    CHECK(ok(fs->vtable->remove(fs->self,tmp)) && ok(store.vtable->append_records(store.self,&record,1)));
    session_replace_fail_mode=2;
    record.data.message=three;
    rc=store.vtable->append_records(store.self,&record,1);
    CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
    CHECK(ok(cc_json_segmented_store_load_page(&store,"s1",NULL,8,&messages,&count,&more)) &&
          count==2 && strcmp(messages[0].id,"one")==0 && strcmp(messages[1].id,"two")==0);
    for (size_t i=0;i<count;i++) cc_message_cleanup(&messages[i]);
    free(messages);
    CHECK(ok(store.vtable->append_records(store.self,&record,1)));
    messages=NULL; count=0;
    CHECK(ok(cc_json_segmented_store_load_page(&store,"s1",NULL,8,&messages,&count,&more)) &&
          count==3 && strcmp(messages[2].id,"three")==0);
    for (size_t i=0;i<count;i++) cc_message_cleanup(&messages[i]);
    free(messages);
    cc_session_t *listed=NULL; size_t listed_count=0;
    CHECK(ok(store.vtable->list_sessions(store.self,&listed,&listed_count)) && listed_count==1 &&
          listed[0].id && strcmp(listed[0].id,"s1")==0 &&
          listed[0].status==CC_SESSION_ACTIVE);
    cc_session_free_array(listed,listed_count);
    session_replace_fail_path=NULL; wrap_stage_filesystem=0;
    cc_message_destroy(one); cc_message_destroy(two); cc_message_destroy(three);
    store.vtable->destroy(store.self);
    CHECK(remove_tree(fs,RECOVERY_TEST_BASE));
    return 0;
}

/* Simulate failures at each media staging boundary. A retry may reuse only
 * byte-identical data, never silently append a reference to a partial file. */
static int test_segmented_media_write_recovery(cc_filesystem_t *fs)
{
    CHECK(ok(fs->vtable->make_dir(fs->self,RECOVERY_TEST_BASE)));
    char root[256]; snprintf(root,sizeof(root),"%s/media-sessions",RECOVERY_TEST_BASE);
    cc_session_store_t store={0};
    wrap_stage_filesystem=1;
    CHECK(ok(cc_json_segmented_store_create(root,4096,"media",1024,&store)) &&
          ok(store.vtable->create_session(store.self,"media-test",".")));
    for (int scenario=0;scenario<12;scenario++) {
        char id[32]; snprintf(id,sizeof(id),"media-case-%d",scenario);
        cc_media_artifact_t image={.id="image",.kind=CC_MEDIA_IMAGE,.mime="image/png",.data_base64="aW1hZ2U="};
        cc_content_parts_t parts; cc_content_parts_init(&parts);
        CHECK(ok(cc_content_parts_append_artifact(&parts,&image,CC_CONTENT_PART_INPUT)));
        cc_message_t *message=NULL;
        CHECK(ok(cc_message_create_parts(id,"media-test",CC_ROLE_USER,&parts,NULL,&message)));
        cc_session_record_t record={.type=CC_SESSION_RECORD_MESSAGE,.session_id="media-test",.data.message=message};
        media_last_temporary[0]='\0';
        if (scenario==0 || scenario>=7) media_write_fault=1; /* partial temp */
        else if (scenario==1) media_write_fault=2;     /* complete temp, lost result */
        else if (scenario==2) media_replace_fault=1;   /* before install */
        else if (scenario==3) media_replace_fault=2;   /* installed, lost result */
        else if (scenario==4) media_sync_fault=1;      /* temporary sync */
        else if (scenario==5) media_sync_fault=2;      /* installed file sync */
        else media_read_fault=1;                       /* verification read */
        cc_result_t rc=store.vtable->append_records(store.self,&record,1);
        CHECK(rc.code==CC_ERR_IO && media_last_temporary[0]); cc_result_free(&rc);
        char canonical[512],target[512],failed_alternate[512]={0};
        snprintf(canonical,sizeof(canonical),"%s",media_last_temporary);
        snprintf(target,sizeof(target),"%s",canonical);
        size_t n=strlen(target); CHECK(n>4U && strcmp(target+n-4U,".tmp")==0); target[n-4U]='\0';
        cc_filesystem_stat_t temporary={.size=sizeof(temporary)}, installed={.size=sizeof(installed)};
        CHECK(ok(fs->vtable->stat(fs->self,media_last_temporary,&temporary)) &&
              ok(fs->vtable->stat(fs->self,target,&installed)));
        if (scenario==3 || scenario==5)
            CHECK(temporary.type==CC_FS_ENTRY_MISSING && installed.type==CC_FS_ENTRY_FILE && installed.byte_size==5U);
        else CHECK(temporary.type==CC_FS_ENTRY_FILE &&
                   temporary.byte_size==((scenario==0 || scenario>=7) ? 2U : 5U) &&
                   installed.type==CC_FS_ENTRY_MISSING);
        cc_message_t *history=NULL; size_t count=0;
        CHECK(ok(store.vtable->load_messages(store.self,"media-test",-1,&history,&count)) &&
              count==(size_t)scenario);
        for (size_t j=0;j<count;j++) cc_message_cleanup(&history[j]); free(history);
        if (scenario==0 || scenario>=7) {
            const char *retained_content=scenario==11 ? "wrong" : "im";
            if (scenario==11)
                CHECK(ok(fs->vtable->write_bytes(fs->self,canonical,"wrong",5U)));
            if (scenario==7) media_replace_fault=1;      /* before installing alternate */
            else if (scenario==8) media_replace_fault=2; /* installed, lost result */
            else if (scenario==9) media_sync_fault=1;    /* sync original temp */
            else if (scenario==10) media_sync_fault=2;  /* sync installed target */
            else if (scenario==11) media_read_fault=1;  /* inspect equal-sized temp */
            if (scenario>=7) {
                rc=store.vtable->append_records(store.self,&record,1);
                CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
                CHECK(ok(fs->vtable->stat(fs->self,canonical,&temporary)) &&
                      temporary.type==CC_FS_ENTRY_FILE);
                if (scenario==7) {
                    CHECK(strcmp(media_last_temporary,canonical)!=0);
                    snprintf(failed_alternate,sizeof(failed_alternate),"%s",media_last_temporary);
                    CHECK(ok(fs->vtable->stat(fs->self,failed_alternate,&temporary)) &&
                          temporary.type==CC_FS_ENTRY_FILE && temporary.byte_size==5U);
                }
            }
            CHECK(ok(store.vtable->append_records(store.self,&record,1)));
            void *retained=NULL; size_t retained_size=0;
            CHECK(ok(fs->vtable->read_bytes(fs->self,canonical,&retained,&retained_size)) &&
                  retained_size==strlen(retained_content) && memcmp(retained,retained_content,retained_size)==0);
            free(retained);
            if (failed_alternate[0]) {
                retained=NULL; retained_size=0;
                CHECK(ok(fs->vtable->read_bytes(fs->self,failed_alternate,&retained,&retained_size)) &&
                      retained_size==5U && memcmp(retained,"image",5U)==0);
                free(retained);
            }
        } else {
            CHECK(ok(store.vtable->append_records(store.self,&record,1)));
        }
        void *bytes=NULL; size_t size=0;
        CHECK(ok(fs->vtable->read_bytes(fs->self,target,&bytes,&size)) &&
              size==5U && memcmp(bytes,"image",5)==0);
        free(bytes);
        CHECK(ok(fs->vtable->stat(fs->self,canonical,&temporary)) &&
              temporary.type==((scenario==0 || scenario>=7) ? CC_FS_ENTRY_FILE : CC_FS_ENTRY_MISSING));
        cc_message_destroy(message); cc_content_parts_cleanup(&parts);
    }
    cc_message_t *history=NULL; size_t count=0;
    CHECK(ok(store.vtable->load_messages(store.self,"media-test",-1,&history,&count)) && count==12U);
    for (size_t i=0;i<count;i++) {
        CHECK(history[i].content.count==1 && history[i].content.items[0].artifact.data_base64 &&
              strcmp(history[i].content.items[0].artifact.data_base64,"aW1hZ2U=")==0);
        cc_message_cleanup(&history[i]);
    }
    free(history);
    store.vtable->destroy(store.self);
    wrap_stage_filesystem=0;
    CHECK(remove_tree(fs,RECOVERY_TEST_BASE));
    return 0;
}

static int test_staged_startup(cc_filesystem_t *fs)
{
    CHECK(ok(fs->vtable->make_dir(fs->self,RECOVERY_TEST_BASE)));
    char path[256],json[1536],marker[256],workspace[256],config_tmp[280],config_backup[280];
    snprintf(path,sizeof(path),"%s/config.json",RECOVERY_TEST_BASE);
    snprintf(config_tmp,sizeof(config_tmp),"%s.tmp",path);
    snprintf(config_backup,sizeof(config_backup),"%s.bak",path);
    snprintf(marker,sizeof(marker),"%s/storage.id",APP_RECOVERY_ROOT);
    snprintf(workspace,sizeof(workspace),"%s/workspace",RECOVERY_TEST_BASE);
    snprintf(json,sizeof(json),
        "{\"model\":{\"provider\":\"fake\",\"name\":\"fake\"},"
        "\"workspace\":{\"root\":\"%s/workspace\"},"
        "\"storage\":{\"root\":\"%s\",\"session_backend\":\"json_segmented\","
        "\"sessions\":\"%s/sessions\",\"memory\":\"%s/memory.json\",\"logs\":\"%s/logs\"}}",
        RECOVERY_TEST_BASE,RECOVERY_TEST_BASE,RECOVERY_TEST_BASE,RECOVERY_TEST_BASE,RECOVERY_TEST_BASE);
    app_config_stage_t *stage=NULL;
    recovery_service_t *service=NULL; recovery_preflight_failure_t failure=RECOVERY_PREFLIGHT_NONE;
    CHECK(ok(fs->vtable->write_text(fs->self,path,"{")) &&
          ok(fs->vtable->write_text(fs->self,config_tmp,"broken")) &&
          ok(fs->vtable->write_text(fs->self,config_backup,json)) &&
          app_config_stage_open(path,&stage)==ESP_OK && app_config_stage_source(stage)==2 &&
          ok(startup_recovery_prepare_staged(stage,mount_generation,&failure,&service)));
    /* Staging and preflight inspect data without repairing or creating paths. */
    char *primary=NULL,*tmp=NULL,*backup=NULL;
    CHECK(ok(fs->vtable->read_text(fs->self,path,&primary)) &&
          ok(fs->vtable->read_text(fs->self,config_tmp,&tmp)) &&
          ok(fs->vtable->read_text(fs->self,config_backup,&backup)) &&
          strcmp(primary,"{")==0 && strcmp(tmp,"broken")==0 && strcmp(backup,json)==0);
    free(primary); free(tmp); free(backup);
    startup_recovery_reset(); recovery_service_destroy(service); app_config_stage_destroy(stage);
    CHECK(ok(fs->vtable->remove(fs->self,config_tmp)) && ok(fs->vtable->remove(fs->self,config_backup)) &&
          ok(fs->vtable->write_text(fs->self,path,json)));
    stage=NULL; service=NULL;
    CHECK(app_config_stage_open(path,&stage)==ESP_OK);
    cc_result_t rc=startup_recovery_prepare_staged(stage,mount_generation+1U,&failure,&service);
    CHECK(rc.code==CC_ERR_STORAGE && !service && failure==RECOVERY_PREFLIGHT_OTHER &&
          startup_gate_phase()==STARTUP_GATE_OFFLINE); cc_result_free(&rc);
    CHECK(ok(startup_recovery_prepare_staged(stage,mount_generation,&failure,&service)) &&
          failure==RECOVERY_PREFLIGHT_NONE && startup_gate_phase()==STARTUP_GATE_LOADING &&
          boot_status_role==BOARD_CONV_STATUS &&
          strcmp(boot_status_text,startup_gate_message(STARTUP_GATE_LOADING))==0);
    app_config_t adopted={0};
    rc=startup_recovery_take_staged_config(service,stage,&adopted);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !adopted.loaded && app_config_stage_config(stage)); cc_result_free(&rc);
    rc=startup_recovery_prepare_workspace(service);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    cc_filesystem_stat_t info={.size=sizeof(info)};
    CHECK(ok(fs->vtable->stat(fs->self,workspace,&info)) && info.type==CC_FS_ENTRY_MISSING);
    CHECK(ok(fs->vtable->write_text(fs->self,path,"{}")));
    rc=startup_recovery_validate_staged(service);
    CHECK(rc.code==CC_ERR_STORAGE && startup_gate_phase()==STARTUP_GATE_FAILED); cc_result_free(&rc);
    CHECK(ok(fs->vtable->stat(fs->self,marker,&info)) && info.type==CC_FS_ENTRY_MISSING);
    startup_recovery_reset();
    CHECK(ok(startup_recovery_present(service)) && startup_gate_phase()==STARTUP_GATE_FAILED &&
          recovery_service_has_staged_config(service));
    rc=recovery_service_validate_staged(service);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    startup_recovery_reset(); recovery_service_destroy(service); app_config_stage_destroy(stage);

    CHECK(ok(fs->vtable->write_text(fs->self,path,json)));
    stage=NULL; service=NULL;
    CHECK(app_config_stage_open(path,&stage)==ESP_OK &&
          ok(startup_recovery_prepare_staged(stage,mount_generation,&failure,&service)));
    startup_recovery_reset();
    CHECK(ok(fs->vtable->write_text(fs->self,path,"{}")));
    rc=recovery_service_validate_staged(service);
    CHECK(rc.code==CC_ERR_STORAGE && recovery_service_state(service)==RECOVERY_SERVICE_FAILED);
    cc_result_free(&rc); recovery_service_destroy(service); app_config_stage_destroy(stage);

    /* Only the current stage may promote a valid backup. If the medium changes
     * during replacement, even an installed primary never opens the gate. */
    for (int change_media=0;change_media<=1;change_media++) {
        CHECK(ok(fs->vtable->write_text(fs->self,path,"{")) &&
              ok(fs->vtable->write_text(fs->self,config_tmp,"broken")) &&
              ok(fs->vtable->write_text(fs->self,config_backup,json)));
        stage=NULL; service=NULL; wrap_stage_filesystem=1;
        esp_err_t stage_err=app_config_stage_open(path,&stage);
        rc=stage_err==ESP_OK ? startup_recovery_prepare_staged(stage,mount_generation,&failure,&service) :
                               cc_result_error(CC_ERR_STORAGE,"stage fixture failed");
        wrap_stage_filesystem=0;
        CHECK(stage_err==ESP_OK && ok(rc) && app_config_stage_source(stage)==2 &&
              startup_gate_phase()==STARTUP_GATE_LOADING);
        rc=startup_recovery_take_staged_config(service,stage,&adopted);
        CHECK(rc.code==CC_ERR_INVALID_STATE && !adopted.loaded); cc_result_free(&rc);
        change_mount_during_config_replace=change_media;
        rc=startup_recovery_repair_config(service); change_mount_during_config_replace=0;
        if (change_media) {
            CHECK(rc.code==CC_ERR_STORAGE && startup_gate_phase()==STARTUP_GATE_FAILED &&
                  recovery_service_state(service)==RECOVERY_SERVICE_FAILED); cc_result_free(&rc);
            rc=startup_recovery_take_staged_config(service,stage,&adopted);
            CHECK(rc.code==CC_ERR_INVALID_STATE && !adopted.loaded); cc_result_free(&rc);
        } else {
            CHECK(ok(rc) && app_config_stage_source(stage)==0 &&
                  ok(startup_recovery_take_staged_config(service,stage,&adopted)) &&
                  ok(startup_recovery_prepare_workspace(service)));
        }
        char *repaired=NULL;
        CHECK(ok(fs->vtable->read_text(fs->self,path,&repaired)) && repaired && strcmp(repaired,json)==0);
        free(repaired);
        startup_recovery_reset(); recovery_service_destroy(service); app_config_stage_destroy(stage);
        app_config_destroy(&adopted);
        if (change_media) mount_generation--; /* Only this fake can restore the old mount. */
        CHECK(remove_tree(fs,workspace));
    }

    /* 创建错误保持可重试，创建期间的配置或介质变化锁定预检失败。 */
    for (int change_media=0;change_media<=1;change_media++) {
        CHECK(ok(fs->vtable->write_text(fs->self,path,json)));
        stage=NULL; service=NULL; wrap_stage_filesystem=1;
        esp_err_t stage_err=app_config_stage_open(path,&stage);
        rc=stage_err==ESP_OK ? startup_recovery_prepare_staged(stage,mount_generation,&failure,&service) :
                               cc_result_error(CC_ERR_STORAGE,"stage fixture failed");
        wrap_stage_filesystem=0;
        CHECK(stage_err==ESP_OK && ok(rc) && startup_gate_phase()==STARTUP_GATE_LOADING &&
              ok(startup_recovery_repair_config(service)));
        workspace_mkdir_path=workspace; fail_workspace_mkdir=1;
        rc=startup_recovery_prepare_workspace(service); fail_workspace_mkdir=0;
        CHECK(rc.code==CC_ERR_IO && startup_gate_phase()==STARTUP_GATE_LOADING &&
              recovery_service_state(service)==RECOVERY_SERVICE_READY_TO_ATTACH); cc_result_free(&rc);
        CHECK(ok(fs->vtable->stat(fs->self,workspace,&info)) && info.type==CC_FS_ENTRY_MISSING);
        CHECK(ok(fs->vtable->write_text(fs->self,workspace,"not a directory")));
        rc=startup_recovery_prepare_workspace(service);
        /* POSIX mkdir 对同名普通文件返回 IO，服务按端口契约透传并保留重试状态。 */
        CHECK(rc.code==CC_ERR_IO && recovery_service_state(service)==RECOVERY_SERVICE_READY_TO_ATTACH);
        cc_result_free(&rc);
        CHECK(ok(fs->vtable->stat(fs->self,workspace,&info)) && info.type==CC_FS_ENTRY_FILE &&
              ok(fs->vtable->remove(fs->self,workspace)) && ok(startup_recovery_prepare_workspace(service)));
        /* A later unsuccessful preparation invalidates the earlier success;
         * attachment cannot rely on a stale ready bit. */
        fail_workspace_mkdir=1; rc=startup_recovery_prepare_workspace(service); fail_workspace_mkdir=0;
        CHECK(rc.code==CC_ERR_IO); cc_result_free(&rc);
        cc_runtime_builder_t *premature=NULL;
        CHECK(ok(cc_runtime_builder_create(app_config_runtime(app_config_stage_config(stage)),&features,&premature)));
        rc=startup_recovery_attach(service,premature);
        CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
        CHECK(ok(cc_runtime_builder_destroy(premature,1000)));
        if (change_media) change_mount_during_workspace_mkdir=1;
        else mutate_during_workspace_mkdir=path;
        rc=startup_recovery_prepare_workspace(service);
        change_mount_during_workspace_mkdir=0; mutate_during_workspace_mkdir=NULL; workspace_mkdir_path=NULL;
        CHECK(rc.code==CC_ERR_STORAGE && startup_gate_phase()==STARTUP_GATE_FAILED &&
              recovery_service_state(service)==RECOVERY_SERVICE_FAILED); cc_result_free(&rc);
        rc=recovery_service_prepare_staged_workspace(service);
        CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
        startup_recovery_reset(); recovery_service_destroy(service); app_config_stage_destroy(stage);
        if (change_media) mount_generation--; /* Only this fake can restore the old mount. */
        CHECK(remove_tree(fs,workspace));
    }

    /* Detect changes both while the staged config is being transferred and
     * immediately after transfer; an output config never escapes on failure. */
    for (unsigned fail_read=2;fail_read<=3;fail_read++) {
        CHECK(ok(fs->vtable->write_text(fs->self,path,json)));
        stage=NULL; service=NULL; wrap_stage_filesystem=1;
        esp_err_t stage_err=app_config_stage_open(path,&stage);
        wrap_stage_filesystem=0;
        CHECK(stage_err==ESP_OK && ok(startup_recovery_prepare_staged(stage,mount_generation,&failure,&service)) &&
              ok(startup_recovery_repair_config(service)));
        CHECK(app_config_prepare(&adopted,"{\"model\":{\"name\":\"sentinel\"}}") == ESP_OK);
        stage_reads=0; mutate_on_stage_read=fail_read; stage_mutation_path=path;
        rc=startup_recovery_take_staged_config(service,stage,&adopted);
        mutate_on_stage_read=0; stage_mutation_path=NULL;
        CHECK(rc.code==CC_ERR_STORAGE && adopted.loaded && strcmp(adopted.runtime.model,"sentinel")==0 &&
              startup_gate_phase()==STARTUP_GATE_FAILED &&
              recovery_service_state(service)==RECOVERY_SERVICE_FAILED); cc_result_free(&rc);
        CHECK(ok(fs->vtable->write_text(fs->self,path,json)));
        startup_recovery_reset();
        CHECK(ok(startup_recovery_present(service)) && startup_gate_phase()==STARTUP_GATE_FAILED);
        startup_recovery_reset(); recovery_service_destroy(service); app_config_stage_destroy(stage); app_config_destroy(&adopted);
    }

    /* A config change after transfer is still detected before attachment. */
    CHECK(ok(fs->vtable->write_text(fs->self,path,json)));
    stage=NULL; service=NULL;
    CHECK(app_config_stage_open(path,&stage)==ESP_OK &&
          ok(startup_recovery_prepare_staged(stage,mount_generation,&failure,&service)) &&
          recovery_service_state(service)==RECOVERY_SERVICE_READY_TO_ATTACH);
    rc=startup_recovery_release_stage(service);
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    CHECK(ok(startup_recovery_repair_config(service)) &&
          ok(startup_recovery_take_staged_config(service,stage,&adopted)) &&
          ok(startup_recovery_validate_staged(service)));
    cc_runtime_builder_t *builder=NULL;
    CHECK(ok(cc_runtime_builder_create(app_config_runtime(&adopted),&features,&builder)));
    rc=startup_recovery_attach(service,builder);
    CHECK(rc.code==CC_ERR_INVALID_STATE && startup_gate_phase()==STARTUP_GATE_LOADING); cc_result_free(&rc);
    CHECK(ok(startup_recovery_prepare_workspace(service)));
    CHECK(ok(fs->vtable->write_text(fs->self,path,"{}")));
    rc=startup_recovery_attach(service,builder);
    CHECK(rc.code==CC_ERR_STORAGE && startup_gate_phase()==STARTUP_GATE_FAILED); cc_result_free(&rc);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000)));
    startup_recovery_reset(); recovery_service_destroy(service); app_config_stage_destroy(stage); app_config_destroy(&adopted);

    for (int change_before_run=0;change_before_run<=1;change_before_run++) {
        CHECK(ok(fs->vtable->write_text(fs->self,path,json)));
        stage=NULL; service=NULL; builder=NULL;
        CHECK(app_config_stage_open(path,&stage)==ESP_OK &&
              ok(startup_recovery_prepare_staged(stage,mount_generation,&failure,&service)) &&
              ok(startup_recovery_repair_config(service)) &&
              ok(startup_recovery_prepare_workspace(service)) &&
              ok(startup_recovery_take_staged_config(service,stage,&adopted)) &&
              ok(startup_recovery_validate_staged(service)) &&
              ok(cc_runtime_builder_create(app_config_runtime(&adopted),&features,&builder)) &&
              ok(cc_runtime_builder_require_recovery(builder)) &&
              ok(startup_recovery_attach(service,builder)));
        cc_runtime_controller_config_t boot_controller_config={0};
        boot_controller_config.size=sizeof(boot_controller_config);
        boot_controller_config.session_base="boot"; boot_controller_config.fresh_session_on_start=1;
        cc_runtime_controller_t *boot_controller=NULL; session_menu_t *boot_menu=NULL;
        CHECK(ok(cc_runtime_controller_create(builder,&boot_controller_config,&boot_controller)) &&
              ok(session_menu_create(boot_controller,builder,service,&boot_menu)) &&
              session_menu_present(boot_menu)==ESP_OK);
        int before_open=forwarded_interaction_calls;
        CHECK(startup_gate_submit(APP_EVENT_SOURCE_WEB,"ordinary input")==ESP_ERR_INVALID_STATE &&
              forwarded_interaction_calls==before_open);
        rc=startup_recovery_mark_running(service);
        CHECK(rc.code==CC_ERR_INVALID_STATE && startup_gate_phase()==STARTUP_GATE_LOADING); cc_result_free(&rc);
        CHECK(ok(startup_recovery_release_stage(service)) && !recovery_service_has_staged_config(service) &&
              startup_gate_phase()==STARTUP_GATE_LOADING);
        app_config_stage_destroy(stage); stage=NULL;
        if (change_before_run) mount_generation++;
        rc=startup_recovery_mark_running(service);
        if (change_before_run) {
            int forwarded=forwarded_interaction_calls;
            CHECK(rc.code==CC_ERR_STORAGE && startup_gate_phase()==STARTUP_GATE_FAILED &&
                  startup_gate_submit(APP_EVENT_SOURCE_WEB,"must remain closed")==ESP_ERR_INVALID_STATE &&
                  forwarded_interaction_calls==forwarded); cc_result_free(&rc);
            mount_generation--; /* Only this fixture can restore the old generation. */
        } else CHECK(ok(rc) && startup_gate_phase()==STARTUP_GATE_RUNNING);
        char lua_workspace[280]; snprintf(lua_workspace,sizeof(lua_workspace),"%s/lua",workspace);
        CHECK(ok(fs->vtable->stat(fs->self,workspace,&info)) && info.type==CC_FS_ENTRY_DIRECTORY &&
              ok(fs->vtable->stat(fs->self,lua_workspace,&info)) && info.type==CC_FS_ENTRY_DIRECTORY);
        session_menu_destroy(boot_menu); CHECK(ok(cc_runtime_controller_destroy(boot_controller,1000)));
        startup_recovery_reset(); CHECK(ok(cc_runtime_builder_destroy(builder,1000)));
        recovery_service_destroy(service); app_config_destroy(&adopted);
    }
    /* The default staged startup must also release its config and open the
     * text-only menu after positively identifying broken history. */
    CHECK(ok(fs->vtable->write_text(fs->self,path,json)));
    stage=NULL; service=NULL; builder=NULL;
    CHECK(app_config_stage_open(path,&stage)==ESP_OK &&
          ok(startup_recovery_prepare_staged(stage,mount_generation,&failure,&service)) &&
          ok(startup_recovery_repair_config(service)) &&
          ok(startup_recovery_prepare_workspace(service)) &&
          ok(startup_recovery_take_staged_config(service,stage,&adopted)) &&
          ok(cc_runtime_builder_create(app_config_runtime(&adopted),&features,&builder)) &&
          ok(cc_runtime_builder_require_recovery(builder)));
    cc_runtime_generation_t *damaged_generation=NULL;
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&damaged_generation)) &&
          ok(cc_agent_runtime_create_session(cc_runtime_generation_runtime(damaged_generation),"stage-damaged",workspace)));
    cc_runtime_generation_release(damaged_generation);
    char staged_sessions[256],staged_segment[512]; staged_segment[0]='\0';
    snprintf(staged_sessions,sizeof(staged_sessions),"%s/sessions",RECOVERY_TEST_BASE);
    char **staged_dirs=NULL; size_t staged_count=0;
    CHECK(ok(fs->vtable->list_dir(fs->self,staged_sessions,&staged_dirs,&staged_count)));
    for (size_t i=0;i<staged_count;i++) {
        if (strncmp(staged_dirs[i],"stage-damaged-",14)==0)
            snprintf(staged_segment,sizeof(staged_segment),"%s/%s/events-000001.jsonl",staged_sessions,staged_dirs[i]);
        free(staged_dirs[i]);
    }
    free(staged_dirs);
    CHECK(staged_segment[0] && ok(fs->vtable->append_bytes(fs->self,staged_segment,"{",1)) &&
          ok(startup_recovery_attach(service,builder)) && recovery_service_history_damaged(service) &&
          startup_gate_phase()==STARTUP_GATE_LOADING);
    cc_runtime_controller_config_t damaged_controller_config={0};
    damaged_controller_config.size=sizeof(damaged_controller_config);
    damaged_controller_config.session_base="damaged-boot"; damaged_controller_config.fresh_session_on_start=1;
    cc_runtime_controller_t *damaged_controller=NULL; session_menu_t *damaged_menu=NULL;
    CHECK(ok(cc_runtime_controller_create(builder,&damaged_controller_config,&damaged_controller)) &&
          ok(session_menu_create(damaged_controller,builder,service,&damaged_menu)) &&
          session_menu_present(damaged_menu)==ESP_OK && strstr(view_text,"所有工具已禁用") &&
          ok(startup_recovery_release_stage(service)));
    app_config_stage_destroy(stage); stage=NULL;
    CHECK(ok(startup_recovery_mark_running(service)) && startup_gate_phase()==STARTUP_GATE_RUNNING);
    session_menu_destroy(damaged_menu); CHECK(ok(cc_runtime_controller_destroy(damaged_controller,1000)));
    startup_recovery_reset(); CHECK(ok(cc_runtime_builder_destroy(builder,1000)));
    recovery_service_destroy(service); app_config_destroy(&adopted);
    CHECK(remove_tree(fs,RECOVERY_TEST_BASE));

    /* Complete first boot in default lifecycle order. Interaction stays closed
     * until this medium's journal, checkpoints and session menu are ready. */
    CHECK(ok(fs->vtable->make_dir(fs->self,RECOVERY_TEST_BASE)) &&
          ok(fs->vtable->write_text(fs->self,path,json)));
    stage=NULL; service=NULL; app_config_t first_config={0}; cc_runtime_builder_t *first_builder=NULL;
    CHECK(app_config_stage_open(path,&stage)==ESP_OK &&
          ok(startup_recovery_prepare_staged(stage,mount_generation,&failure,&service)) &&
          startup_gate_phase()==STARTUP_GATE_LOADING &&
          ok(fs->vtable->stat(fs->self,APP_RECOVERY_ROOT,&info)) && info.type==CC_FS_ENTRY_MISSING);
    CHECK(ok(startup_recovery_repair_config(service)) &&
          ok(startup_recovery_prepare_workspace(service)) &&
          ok(startup_recovery_take_staged_config(service,stage,&first_config)) &&
          ok(cc_runtime_builder_create(app_config_runtime(&first_config),&features,&first_builder)) &&
          ok(cc_runtime_builder_require_recovery(first_builder)) &&
          ok(startup_recovery_attach(service,first_builder)));
    cc_runtime_controller_config_t first_controller_config={0};
    first_controller_config.size=sizeof(first_controller_config);
    first_controller_config.session_base="first-boot"; first_controller_config.fresh_session_on_start=1;
    cc_runtime_controller_t *first_controller=NULL; session_menu_t *first_menu=NULL;
    CHECK(ok(cc_runtime_controller_create(first_builder,&first_controller_config,&first_controller)) &&
          ok(session_menu_create(first_controller,first_builder,service,&first_menu)) &&
          session_menu_present(first_menu)==ESP_OK);
    int forwarded_before_first=forwarded_interaction_calls;
    CHECK(startup_gate_submit(APP_EVENT_SOURCE_SERIAL,"normal message")==ESP_ERR_INVALID_STATE &&
          forwarded_interaction_calls==forwarded_before_first &&
          ok(startup_recovery_release_stage(service)));
    app_config_stage_destroy(stage); stage=NULL;
    CHECK(ok(startup_recovery_mark_running(service)) && startup_gate_phase()==STARTUP_GATE_RUNNING);
    session_menu_destroy(first_menu); CHECK(ok(cc_runtime_controller_destroy(first_controller,1000)));
    startup_recovery_reset(); CHECK(ok(cc_runtime_builder_destroy(first_builder,1000)));
    recovery_service_destroy(service); app_config_destroy(&first_config);
    CHECK(remove_tree(fs,RECOVERY_TEST_BASE));
    return 0;
}

/**
 * @brief 验证已确认账本损坏只开放新纯对话，所有工具关闭，修复后重建恢复正常。
 *
 * 分别损坏 JSON 语法、必需字段及校验值，保留原文件并检查恢复列表诊断、模型工具表、
 * 注册手动工具、显式实现工具和独立 Lua 入口。读取 I/O/OOM 及读取期间换盘均维持真实失败。
 * @param[in,out] fs 借用的真实文件系统，测试独占 RECOVERY_TEST_BASE，成功清理全部文件及实例。
 * @return 全部断言及清理通过为零，CHECK 失败为一并保留当时现场。
 */
static int test_damaged_operation_journal(cc_filesystem_t *fs)
{
    const char *original_response=provider_response;
    provider_response="journal text";
    char workspace[256],sessions[256],memory[256],logs[256];
    snprintf(workspace,sizeof(workspace),"%s/workspace",RECOVERY_TEST_BASE);
    snprintf(sessions,sizeof(sessions),"%s/sessions",RECOVERY_TEST_BASE);
    snprintf(memory,sizeof(memory),"%s/memory.json",RECOVERY_TEST_BASE);
    snprintf(logs,sizeof(logs),"%s/logs",RECOVERY_TEST_BASE);
    cc_config_t config={0}; config.provider="fake"; config.model="fake"; config.data_dir=RECOVERY_TEST_BASE;
    config.workspace_path=workspace; config.storage_path=sessions; config.storage_type="json_segmented";
    config.memory_path=memory;
    app_runtime_io_config_t io={0}; io.max_pending_operations=64; io.usb_log.dir=logs;
    cc_runtime_builder_t *builder=NULL; recovery_service_t *service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
          ok(recovery_service_open(builder,&config,&io,&service)));
    int original_tool_calls=tool_calls;
    tool_fail=1; CHECK(attempt_tool(builder,0)); tool_fail=0;
    cc_pending_operation_t *pending=NULL; size_t pending_count=0;
    CHECK(ok(recovery_service_list_operations(service,&pending,&pending_count)) && pending_count==1);
    char *operation=cc_copy_string(pending[0].operation_id); CHECK(operation);
    cc_pending_operation_array_free(pending,pending_count);
    char path[512]; snprintf(path,sizeof(path),"%s/operations/%s.json",APP_RECOVERY_ROOT,operation);
    char *saved=NULL; CHECK(ok(fs->vtable->read_text(fs->self,path,&saved)) && saved);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);
    char *bad_checksum=cc_copy_string(saved); CHECK(bad_checksum);
    char *checksum=strstr(bad_checksum,"\"crc32\":\""); CHECK(checksum);
    checksum+=strlen("\"crc32\":\""); *checksum=*checksum=='0' ? '1' : '0';
    const char *damaged[]={"{","{}",bad_checksum};
    for (size_t i=0;i<sizeof(damaged)/sizeof(damaged[0]);i++) {
        CHECK(ok(fs->vtable->write_text(fs->self,path,damaged[i])));
        builder=NULL; service=NULL;
        CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
              ok(recovery_service_open(builder,&config,&io,&service)) &&
              recovery_service_history_damaged(service) &&
              strcmp(recovery_service_mode_name(service),"journal_damaged")==0);
        pending=NULL; pending_count=99;
        cc_result_t rc=recovery_service_list_operations(service,&pending,&pending_count);
        CHECK(rc.code==CC_ERR_STORAGE && rc.message && strstr(rc.message,"journal") && !pending && !pending_count);
        cc_result_free(&rc);
        recovery_run_info_t *runs=NULL; size_t run_count=99;
        rc=recovery_service_list_runs(service,&runs,&run_count);
        CHECK(rc.code==CC_ERR_STORAGE && !runs && !run_count); cc_result_free(&rc);
        cc_runtime_controller_config_t controller_config={0}; controller_config.size=sizeof(controller_config);
        controller_config.session_base="journal-degraded"; controller_config.fresh_session_on_start=1;
        cc_runtime_controller_t *controller=NULL;
        CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)));
        provider_saw_tools=1; char *answer=NULL;
        CHECK(ok(cc_runtime_controller_submit_text(controller,"chat with damaged journal",NULL,NULL,&answer)) &&
              answer && !provider_saw_tools); free(answer);
        int blocked_calls=tool_calls, blocked_lua_calls=lua_calls;
        CHECK(attempt_tool(builder,0) && tool_calls==blocked_calls);
        cc_runtime_generation_t *generation=NULL;
        CHECK(ok(cc_runtime_builder_acquire_generation(builder,&generation)));
        cc_tool_t trusted={(void *)"manual-only",&trusted_tool_vt};
        cc_tool_call_t call={"journal-manual","manual-only","{}"}; cc_tool_result_t result={0};
        rc=cc_agent_runtime_execute_manual_tool_with_impl(cc_runtime_generation_runtime(generation),
                                                         "session",&call,&trusted,&result);
        CHECK(rc.code==CC_ERR_INVALID_STATE && tool_calls==blocked_calls); cc_result_free(&rc);
        cc_tool_result_cleanup(&result); cc_runtime_generation_release(generation);
        rc=manual_lua_service_execute(controller,service,"/lua work()",MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY,&result);
        CHECK(rc.code==CC_ERR_INVALID_STATE && lua_calls==blocked_lua_calls); cc_result_free(&rc);
        cc_tool_result_cleanup(&result);
        char *retained=NULL;
        CHECK(ok(fs->vtable->read_text(fs->self,path,&retained)) && strcmp(retained,damaged[i])==0); free(retained);
        CHECK(ok(cc_runtime_controller_destroy(controller,1000)) && ok(cc_runtime_builder_destroy(builder,1000)));
        recovery_service_destroy(service);
    }
    wrap_stage_filesystem=1; journal_read_fault_path=path;
    const cc_error_code_t read_failures[]={CC_ERR_IO,CC_ERR_OUT_OF_MEMORY,CC_ERR_STORAGE};
    for (size_t i=0;i<sizeof(read_failures)/sizeof(read_failures[0]);i++) {
        journal_read_fault_code=read_failures[i]; builder=NULL; service=NULL;
        CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
        cc_result_t rc=recovery_service_open(builder,&config,&io,&service);
        CHECK(rc.code==read_failures[i] && !service && attempt_tool(builder,0)); cc_result_free(&rc);
        CHECK(ok(cc_runtime_builder_destroy(builder,1000)));
    }
    journal_read_fault_code=CC_OK; change_mount_during_journal_read=1; builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    cc_result_t rc=recovery_service_open(builder,&config,&io,&service);
    CHECK(rc.code==CC_ERR_STORAGE && !service && !change_mount_during_journal_read && attempt_tool(builder,0));
    cc_result_free(&rc); CHECK(ok(cc_runtime_builder_destroy(builder,1000)));
    mount_generation--; /* 仅此合成挂载夹具可恢复旧代数。 */
    wrap_stage_filesystem=0; journal_read_fault_path=NULL;
    CHECK(ok(fs->vtable->write_text(fs->self,path,saved))); free(saved); free(bad_checksum);
    builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
          ok(recovery_service_open(builder,&config,&io,&service)) &&
          recovery_service_state(service)==RECOVERY_SERVICE_READY && !recovery_service_history_damaged(service));
    CHECK(ok(recovery_service_resolve_operation(service,operation,CC_TOOL_RECOVERY_NOT_EXECUTED,NULL)));
    free(operation); CHECK(attempt_tool(builder,1));
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);
    CHECK(remove_tree(fs,RECOVERY_TEST_BASE)); tool_calls=original_tool_calls; provider_response=original_response;
    return 0;
}

/**
 * @brief 外部清理最后一个中断任务后，验证 /runs 的页面重试被消费且普通输入仍可聊天。
 * @param[in] builder 借用当前已附加恢复装配的 builder，调用期间有效。
 * @param[in] recovery 借用当前恢复服务，与 builder 属于同一装配。
 * @param[in] filesystem 借用主测试文件系统，调用期间有效。
 * @param[in] runs_dir 借用当前检查点目录，主测试已创建。
 * @param[in] controller_config 借用控制器配置，使用延迟持久化的新会话模式。
 * @return 全部断言通过返回零，失败返回一。成功路径清理临时控制器、菜单和运行 ID。
 */
static int test_runs_ready_retry(cc_runtime_builder_t *builder, recovery_service_t *recovery,
    cc_filesystem_t *filesystem, const char *runs_dir,
    const cc_runtime_controller_config_t *controller_config)
{
    char *run_id = NULL;
    CHECK(ok(cc_id_generate_uuid_v4("run-", &run_id)));
    cc_run_checkpoint_store_t *checkpoints = NULL;
    CHECK(ok(cc_run_checkpoint_store_open(filesystem, runs_dir, &checkpoints)));
    cc_run_checkpoint_t checkpoint = {0};
    checkpoint.run_id = run_id;
    checkpoint.session_id = "old-session";
    checkpoint.user_message_id = "menu-ready-retry-input";
    checkpoint.user_input = "externally abandoned input";
    checkpoint.reasoning_content = "";
    checkpoint.step = 1;
    checkpoint.phase = CC_RUN_CHECKPOINT_MODEL;
    CHECK(ok(cc_run_checkpoint_save(checkpoints, &checkpoint)));
    cc_run_checkpoint_store_destroy(checkpoints);

    cc_runtime_controller_t *controller = NULL;
    session_menu_t *menu = NULL;
    CHECK(ok(cc_runtime_controller_create(builder, controller_config, &controller)));
    CHECK(ok(session_menu_create(controller, builder, recovery, &menu)));
    CHECK(session_menu_present(menu) == ESP_OK);
    CHECK(session_menu_handle(menu, "/view 1", APP_EVENT_SOURCE_WEB, NULL, NULL));
    CHECK(session_menu_handle(menu, "/resume", APP_EVENT_SOURCE_WEB, NULL, NULL));
    CHECK(session_menu_waiting(menu) && strstr(view_text, run_id));

    cc_runtime_generation_t *generation = NULL;
    CHECK(ok(cc_runtime_builder_acquire_generation(builder, &generation)));
    CHECK(ok(cc_agent_runtime_abandon_run(cc_runtime_generation_runtime(generation), run_id)));
    cc_runtime_generation_release(generation);
    int calls_before = provider_calls;
    fail_next_view = 1;
    CHECK(session_menu_handle(menu, "/runs", APP_EVENT_SOURCE_WEB, NULL, NULL));
    CHECK(!fail_next_view && session_menu_waiting(menu) && provider_calls == calls_before);
    CHECK(session_menu_handle(menu, "/runs", APP_EVENT_SOURCE_WEB, NULL, NULL));
    CHECK(!session_menu_waiting(menu) && provider_calls == calls_before && strstr(view_text, "已恢复会话"));

    /* 未被菜单消费的普通文字沿外层现有路径提交，确认命令消费修复保留普通聊天入口。 */
    const char *input = "ordinary input after ready retry";
    CHECK(!session_menu_handle(menu, input, APP_EVENT_SOURCE_WEB, NULL, NULL));
    const char *previous_response = provider_response;
    provider_response = "ready retry reply";
    char *answer = NULL;
    CHECK(ok(cc_runtime_controller_submit_text(controller, input, NULL, NULL, &answer)));
    CHECK(provider_calls == calls_before + 1 && answer && strcmp(answer, provider_response) == 0);
    free(answer);
    provider_response = previous_response;
    session_menu_destroy(menu);
    CHECK(ok(cc_runtime_controller_destroy(controller, 1000)));
    free(run_id);
    return 0;
}

/**
 * @brief 运行恢复装配、菜单、记忆与工具账本的主机集成回归。
 * @return 所有断言通过返回零，任一失败返回一。
 */
int main(void)
{
    CHECK(test_managed_memory_product_policy()==0);
    CHECK(test_product_memory_tool_is_readonly()==0);
    /* The default lifecycle checks NVS before touching the USB config. A failed check keeps input closed while
     * still allowing the serial clear command and publishing a repair state. */
    startup_recovery_reset();
    identity_open_fail=1;
    cc_result_t nvs_check=startup_recovery_check_nvs();
    CHECK(nvs_check.code==CC_ERR_STORAGE && startup_gate_phase()==STARTUP_GATE_NVS_REPAIR &&
          boot_status_role==BOARD_CONV_ERROR && strstr(boot_status_text,"修复") &&
          identity_open_calls==1 &&
          startup_gate_submit(APP_EVENT_SOURCE_SERIAL,"ordinary input")==ESP_ERR_INVALID_STATE &&
          forwarded_interaction_calls==0 && startup_gate_submit(APP_EVENT_SOURCE_SERIAL,"/clear")==ESP_OK);
    cc_result_free(&nvs_check);
    nvs_check=startup_recovery_check_nvs();
    CHECK(nvs_check.code==CC_ERR_INVALID_STATE && identity_open_calls==1); cc_result_free(&nvs_check);
    startup_recovery_reset(); identity_open_fail=0;
    CHECK(ok(startup_recovery_check_nvs()) && startup_gate_phase()==STARTUP_GATE_OFFLINE &&
          identity_open_calls==2);

    cc_filesystem_t fs={0}; CHECK(ok(__real_cc_filesystem_get_default(&fs)));
    cc_filesystem_stat_t info={.size=sizeof(info)}; CHECK(ok(fs.vtable->stat(fs.self,RECOVERY_TEST_BASE,&info)) && info.type==CC_FS_ENTRY_MISSING);
    CHECK(test_json_session_write_recovery(&fs)==0);
    CHECK(test_segmented_manifest_write_recovery(&fs)==0);
    CHECK(test_segmented_media_write_recovery(&fs)==0);
    CHECK(test_staged_startup(&fs)==0);
    CHECK(test_automatic_memory_recovery(&fs)==0);
    CHECK(test_damaged_operation_journal(&fs)==0);
    char workspace[256],sessions[256],memory[256],logs[256];
    snprintf(workspace,sizeof(workspace),"%s/workspace",RECOVERY_TEST_BASE); snprintf(sessions,sizeof(sessions),"%s/sessions",RECOVERY_TEST_BASE);
    snprintf(memory,sizeof(memory),"%s/memory.json",RECOVERY_TEST_BASE); snprintf(logs,sizeof(logs),"%s/logs",RECOVERY_TEST_BASE);
    cc_config_t config={0}; config.provider="fake"; config.model="fake"; config.data_dir=RECOVERY_TEST_BASE;
    config.workspace_path=workspace; config.storage_path=sessions; config.storage_type="json_segmented"; config.memory_path=memory;
    app_runtime_io_config_t io={0}; io.max_pending_operations=64; io.usb_log.dir=logs;
    cc_runtime_builder_t *builder=NULL; recovery_service_t *service=NULL;

    /* Preflight validates paths before builder creation without creating directories. */
    config.storage_path="/outside/sessions";
    cc_result_t invalid_paths=recovery_service_prepare(&config,&io,&service);
    CHECK(invalid_paths.code==CC_ERR_PERMISSION_DENIED && !service); cc_result_free(&invalid_paths);
    char lookalike[300],escape[300];
    snprintf(lookalike,sizeof(lookalike),"%s-other/sessions",RECOVERY_TEST_BASE);
    config.storage_path=lookalike;
    invalid_paths=recovery_service_prepare(&config,&io,&service);
    CHECK(invalid_paths.code==CC_ERR_PERMISSION_DENIED && !service); cc_result_free(&invalid_paths);
    config.storage_path=sessions;
    CHECK(ok(recovery_service_validate_configuration(&config,&io)));
    char overlapping_logs[300]; snprintf(overlapping_logs,sizeof(overlapping_logs),"%s/logs",sessions);
    io.usb_log.dir=overlapping_logs;
    invalid_paths=recovery_service_prepare(&config,&io,&service);
    CHECK(invalid_paths.code==CC_ERR_PERMISSION_DENIED && !service); cc_result_free(&invalid_paths);
    io.usb_log.dir=logs;
    /* A proposed session directory can be safe while its new live log route
     * overlaps the still-running old session directory. Both need checking. */
    char future_sessions[300]; snprintf(future_sessions,sizeof(future_sessions),"%s/future-sessions",RECOVERY_TEST_BASE);
    cc_config_t future_config=config; future_config.storage_path=future_sessions;
    io.usb_log.dir=sessions;
    CHECK(ok(recovery_service_validate_configuration(&future_config,&io)));
    invalid_paths=recovery_service_validate_configuration(&config,&io);
    CHECK(invalid_paths.code==CC_ERR_PERMISSION_DENIED); cc_result_free(&invalid_paths);
    io.usb_log.dir=logs;
    char overlapping_memory[300]; snprintf(overlapping_memory,sizeof(overlapping_memory),"%s/memory.json",sessions);
    config.memory_path=overlapping_memory;
    invalid_paths=recovery_service_prepare(&config,&io,&service);
    CHECK(invalid_paths.code==CC_ERR_PERMISSION_DENIED && !service); cc_result_free(&invalid_paths);
    config.memory_path=memory;
    snprintf(escape,sizeof(escape),"%s/../other",RECOVERY_TEST_BASE); config.data_dir=escape;
    invalid_paths=recovery_service_prepare(&config,&io,&service);
    CHECK(invalid_paths.code==CC_ERR_INVALID_ARGUMENT && !service); cc_result_free(&invalid_paths);
    config.data_dir=RECOVERY_TEST_BASE;
    recovery_preflight_failure_t failure=RECOVERY_PREFLIGHT_NONE;
    CHECK(ok(recovery_service_prepare_detailed(&config,&io,&failure,&service)) &&
          failure==RECOVERY_PREFLIGHT_NONE &&
          recovery_service_state(service)==RECOVERY_SERVICE_READY_TO_ATTACH);
    info.size=sizeof(info); CHECK(ok(fs.vtable->stat(fs.self,RECOVERY_TEST_BASE,&info)) && info.type==CC_FS_ENTRY_MISSING);
    CHECK(ok(startup_recovery_present(service)) && startup_gate_phase()==STARTUP_GATE_LOADING);
    recovery_service_t *other_preflight=NULL;
    CHECK(ok(recovery_service_prepare(&config,&io,&other_preflight)));
    cc_result_t gate_rc=startup_recovery_present(other_preflight);
    CHECK(gate_rc.code==CC_ERR_INVALID_STATE && startup_gate_phase()==STARTUP_GATE_LOADING); cc_result_free(&gate_rc);
    recovery_service_destroy(other_preflight);
    char other_sessions[256],other_workspace[256],other_memory[256];
    snprintf(other_sessions,sizeof(other_sessions),"%s/other-sessions",RECOVERY_TEST_BASE);
    snprintf(other_workspace,sizeof(other_workspace),"%s/other-workspace",RECOVERY_TEST_BASE);
    snprintf(other_memory,sizeof(other_memory),"%s/other-memory.json",RECOVERY_TEST_BASE);
    config.storage_path=other_sessions;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    invalid_paths=recovery_service_attach(service,builder);
    CHECK(invalid_paths.code==CC_ERR_INVALID_STATE && recovery_service_state(service)==RECOVERY_SERVICE_READY_TO_ATTACH);
    cc_result_free(&invalid_paths); CHECK(ok(cc_runtime_builder_destroy(builder,1000))); builder=NULL;
    config.storage_path=sessions; config.workspace_path=other_workspace;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    invalid_paths=recovery_service_attach(service,builder);
    CHECK(invalid_paths.code==CC_ERR_INVALID_STATE && recovery_service_state(service)==RECOVERY_SERVICE_READY_TO_ATTACH);
    cc_result_free(&invalid_paths); CHECK(ok(cc_runtime_builder_destroy(builder,1000))); builder=NULL;
    config.workspace_path=workspace; config.memory_path=other_memory;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    invalid_paths=recovery_service_attach(service,builder);
    CHECK(invalid_paths.code==CC_ERR_INVALID_STATE && recovery_service_state(service)==RECOVERY_SERVICE_READY_TO_ATTACH);
    cc_result_free(&invalid_paths); CHECK(ok(cc_runtime_builder_destroy(builder,1000))); builder=NULL;
    config.memory_path=memory;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    cc_runtime_generation_t *fixture_generation=NULL;
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&fixture_generation)));
    CHECK(ok(cc_agent_runtime_create_session(cc_runtime_generation_runtime(fixture_generation),"old-session",workspace)));
    cc_session_store_t *fixture_sessions=cc_agent_runtime_session_store(cc_runtime_generation_runtime(fixture_generation));
    cc_message_t *old_message=NULL;
    CHECK(ok(cc_message_create_text("old-message","old-session",CC_ROLE_USER,"old input",NULL,&old_message)));
    cc_session_record_t old_record={.type=CC_SESSION_RECORD_MESSAGE,.session_id="old-session",.data.message=old_message};
    CHECK(ok(fixture_sessions->vtable->append_records(fixture_sessions->self,&old_record,1)));
    cc_message_destroy(old_message);
    cc_runtime_generation_release(fixture_generation);
    char runs_dir[256]; snprintf(runs_dir,sizeof(runs_dir),"%s/runs",APP_RECOVERY_ROOT);
    cc_run_checkpoint_store_t *fixture_store=NULL;
    CHECK(ok(fs.vtable->make_dir(fs.self,runs_dir)) &&
          ok(fs.vtable->make_dir(fs.self,APP_RECOVERY_ROOT "/operations")));
    CHECK(ok(cc_run_checkpoint_store_open(&fs,runs_dir,&fixture_store)));
    char *run_id=NULL; CHECK(ok(cc_id_generate_uuid_v4("run-",&run_id)));
    cc_run_checkpoint_t checkpoint={0}; checkpoint.run_id=run_id; checkpoint.session_id="old-session";
    checkpoint.user_message_id="old-message"; checkpoint.user_input="old input";
    checkpoint.reasoning_content=""; checkpoint.step=1; checkpoint.phase=CC_RUN_CHECKPOINT_MODEL;
    CHECK(ok(cc_run_checkpoint_save(fixture_store,&checkpoint)));
    char *run_id2=NULL; CHECK(ok(cc_id_generate_uuid_v4("run-",&run_id2)));
    checkpoint.run_id=run_id2; checkpoint.user_message_id="old-message-2"; checkpoint.user_input="other input";
    CHECK(ok(cc_run_checkpoint_save(fixture_store,&checkpoint)));
    cc_run_checkpoint_store_destroy(fixture_store);
    recovery_run_info_t *runs=NULL; size_t run_count=0;
    cc_result_t rc=recovery_service_list_runs(service,&runs,&run_count);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !runs && run_count==0); cc_result_free(&rc);
    CHECK(ok(startup_recovery_attach(service,builder)) && recovery_service_is_current(service) &&
          startup_gate_phase()==STARTUP_GATE_LOADING);
    mount_generation++;
    gate_rc=startup_recovery_mark_running(service);
    CHECK(gate_rc.code==CC_ERR_STORAGE && startup_gate_phase()==STARTUP_GATE_FAILED); cc_result_free(&gate_rc);
    mount_generation--; startup_recovery_reset();
    CHECK(ok(startup_recovery_present(service)) && ok(startup_recovery_mark_running(service)) &&
          startup_gate_phase()==STARTUP_GATE_RUNNING);
    startup_recovery_reset();
    mount_generation++;
    CHECK(!recovery_service_is_current(service));
    mount_generation--; /* Only this host fixture can restore a previous generation. */
    cc_config_t other_config=config; other_config.workspace_path=other_workspace;
    other_config.storage_path=other_sessions; other_config.memory_path=other_memory;
    cc_runtime_builder_t *other_builder=NULL; cc_runtime_controller_t *other_controller=NULL;
    cc_runtime_controller_config_t other_controller_config={0}; other_controller_config.size=sizeof(other_controller_config);
    other_controller_config.session_base="other"; other_controller_config.fresh_session_on_start=1;
    CHECK(ok(cc_runtime_builder_create(&other_config,&features,&other_builder)) &&
          ok(cc_runtime_controller_create(other_builder,&other_controller_config,&other_controller)));
    session_selection_t *foreign_selection=NULL;
    rc=session_selection_create(other_controller,builder,service,&foreign_selection);
    CHECK(rc.code==CC_ERR_INVALID_ARGUMENT && !foreign_selection); cc_result_free(&rc);
    cc_tool_result_t foreign_lua={0};
    rc=manual_lua_service_execute(other_controller,service,"/lua work()",
                                  MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY,&foreign_lua);
    CHECK(rc.code==CC_ERR_STORAGE && lua_calls==0); cc_result_free(&rc); cc_tool_result_cleanup(&foreign_lua);
    CHECK(ok(cc_runtime_controller_destroy(other_controller,1000)) && ok(cc_runtime_builder_destroy(other_builder,1000)));
    CHECK(exercise_memory_console(service,&fs,memory,RECOVERY_TEST_BASE)==0);
    CHECK(exercise_managed_memory(builder,service,memory)==0);
    CHECK(ok(recovery_service_list_runs(service,&runs,&run_count)) && run_count==2 &&
          strcmp(runs[0].session_id,"old-session")==0 &&
          runs[0].phase==CC_RUN_CHECKPOINT_MODEL && runs[0].step==1 && !runs[0].recovered_from_backup);
    recovery_run_info_array_free(runs,run_count);
    cc_runtime_controller_config_t controller_config={0}; controller_config.size=sizeof(controller_config);
    char temporary_run_path[384], primary_run_path[384];
    snprintf(primary_run_path,sizeof(primary_run_path),"%s/%s.json",runs_dir,run_id);
    snprintf(temporary_run_path,sizeof(temporary_run_path),"%s/%s.tmp",runs_dir,run_id);
    CHECK(ok(fs.vtable->atomic_replace(fs.self,primary_run_path,temporary_run_path)));
    CHECK(ok(recovery_service_list_runs(service,&runs,&run_count)) && run_count==2);
    size_t temporary_index=0;
    for (size_t i=0;i<run_count;i++) if (strcmp(runs[i].run_id,run_id)==0) temporary_index=i;
    CHECK(runs[temporary_index].recovered_from_temporary && strcmp(runs[temporary_index].input_preview,"old input")==0);
    recovery_run_info_array_free(runs,run_count);
    controller_config.session_base="device"; controller_config.fresh_session_on_start=1;
    cc_runtime_controller_t *controller=NULL;
    CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)));
    session_menu_t *menu=NULL;
    CHECK(ok(session_menu_create(controller,builder,service,&menu)) && session_menu_present(menu)==ESP_OK &&
          strstr(view_text,"old-session") && session_menu_waiting(menu));
    CHECK(session_menu_handle(menu,"unselected text",APP_EVENT_SOURCE_WEB,NULL,NULL) && session_menu_waiting(menu));
    CHECK(session_menu_handle(menu,"/view 1",APP_EVENT_SOURCE_SERIAL,NULL,NULL) && strstr(view_text,"old input"));
    session_menu_invalidate_view(menu);
    CHECK(session_menu_handle(menu,"/resume",APP_EVENT_SOURCE_WEB,NULL,NULL) && session_menu_waiting(menu));
    CHECK(session_menu_handle(menu,"/sessions",APP_EVENT_SOURCE_WEB,NULL,NULL) && strstr(view_text,"old-session"));
    CHECK(session_menu_handle(menu,"/resume",APP_EVENT_SOURCE_WEB,NULL,NULL) && session_menu_waiting(menu) &&
          strstr(view_text,"/view")); /* Returning to the list invalidates the previous preview. */
    fail_next_view=1;
    CHECK(session_menu_handle(menu,"/view 1",APP_EVENT_SOURCE_SERIAL,NULL,NULL) && strstr(view_text,"/view"));
    CHECK(session_menu_handle(menu,"/resume",APP_EVENT_SOURCE_WEB,NULL,NULL) && session_menu_waiting(menu));
    CHECK(session_menu_handle(menu,"/more",APP_EVENT_SOURCE_WEB,NULL,NULL) && strstr(view_text,"old input"));
    CHECK(session_menu_handle(menu,"/resume",APP_EVENT_SOURCE_WEB,NULL,NULL) && strstr(view_text,"中断任务") &&
          strstr(view_text,run_id) && strstr(view_text,run_id2) && session_menu_waiting(menu));
    CHECK(strstr(view_text,"需要核对的临时记录") && strstr(view_text,"old input"));
    char temporary_command[64]; int calls_before_confirmation=provider_calls;
    snprintf(temporary_command,sizeof(temporary_command),"/continue %zu",temporary_index+1U);
    CHECK(session_menu_handle(menu,temporary_command,APP_EVENT_SOURCE_WEB,NULL,NULL) &&
          provider_calls==calls_before_confirmation);
    snprintf(temporary_command,sizeof(temporary_command),"/recover %zu",temporary_index+1U);
    CHECK(session_menu_handle(menu,temporary_command,APP_EVENT_SOURCE_SERIAL,NULL,NULL) &&
          provider_calls==calls_before_confirmation && !strstr(view_text,"需要核对的临时记录"));
    CHECK(ok(recovery_service_list_runs(service,&runs,&run_count)) && run_count==2 &&
          !runs[temporary_index].recovered_from_temporary);
    recovery_run_info_array_free(runs,run_count);
    session_menu_destroy(menu); CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    controller=NULL; CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)));
    session_selection_t *selection=NULL;
    CHECK(ok(session_selection_create(controller,builder,service,&selection)) &&
          session_selection_phase(selection)==SESSION_SELECTION_CHOOSING);
    cc_session_t *listed=NULL; size_t listed_count=0;
    CHECK(ok(session_selection_list_sessions(selection,&listed,&listed_count)) && listed_count==1 &&
          strcmp(listed[0].id,"old-session")==0);
    cc_session_free_array(listed,listed_count);
    cc_message_t *history=NULL; size_t history_count=0; int more=0;
    CHECK(ok(session_selection_preview(selection,"old-session",NULL,4,&history,&history_count,&more)) &&
          history_count==1 && strcmp(history[0].id,"old-message")==0 && !more);
    for (size_t i=0;i<history_count;i++) cc_message_cleanup(&history[i]); free(history);
    cc_runtime_controller_status_t controller_status={0};
    cc_runtime_controller_get_status(controller,&controller_status);
    CHECK(!controller_status.session_is_persisted && session_selection_phase(selection)==SESSION_SELECTION_CHOOSING);
    CHECK(ok(session_selection_choose_existing(selection,"old-session")) &&
          session_selection_phase(selection)==SESSION_SELECTION_INTERRUPTED &&
          strcmp(session_selection_session_id(selection),"old-session")==0);
    rc=session_selection_abandon_run(selection,"run-not-in-selected-session");
    CHECK(rc.code==CC_ERR_NOT_FOUND && session_selection_phase(selection)==SESSION_SELECTION_INTERRUPTED); cc_result_free(&rc);
    cc_managed_memory_worker_options_t resume_options={0}; resume_options.size=sizeof(resume_options);
    resume_options.capacity=8; resume_options.max_pending_items=1; resume_options.max_pending_bytes=4096;
    resume_options.select_eviction=memory_evict; resume_options.decide_snapshot=memory_snapshot;
    cc_memory_extraction_options_t resume_extraction={0}; resume_extraction.size=sizeof(resume_extraction);
    resume_extraction.model="fake"; managed_memory_service_t *resume_memory=NULL;
    CHECK(ok(managed_memory_service_create(builder,service,memory,&resume_options,&resume_extraction,&resume_memory)));
    CHECK(ok(managed_memory_service_enqueue(resume_memory,"already-pending","old-session","input","answer")));
    int calls_before_resume=provider_calls; session_resume_memory_result_t resumed={0};
    rc=session_selection_resume_run_with_memory(selection,run_id,resume_memory,128,NULL,NULL,&resumed);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && !resumed.completed && !resumed.response && !resumed.reservation &&
          provider_calls==calls_before_resume); cc_result_free(&rc);
    provider_response="{\"memories\":[]}"; cc_managed_memory_worker_result_t memory_result={0};
    CHECK(ok(managed_memory_service_process_one(resume_memory,40,NULL,&memory_result)));
    provider_response="resumed";
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&fixture_generation)));
    cc_event_bus_t *runtime_bus=cc_agent_runtime_event_bus(cc_runtime_generation_runtime(fixture_generation));
    finish_action_t finish={.controller=controller,.change_media=1}; cc_event_subscription_t finish_subscription={0};
    CHECK(ok(cc_event_bus_subscribe(runtime_bus,CC_OBS_EVENT_RUN_FINISHED,on_run_finished,&finish,NULL,&finish_subscription)));
    cc_runtime_generation_release(fixture_generation);
    rc=session_selection_resume_run_with_memory(selection,run_id,resume_memory,128,NULL,NULL,&resumed);
    CHECK(rc.code==CC_ERR_STORAGE && finish.called==1 && finish.cancelled && resumed.completed &&
          resumed.response && strcmp(resumed.response,"resumed")==0 && !resumed.reservation &&
          session_selection_phase(selection)==SESSION_SELECTION_UNAVAILABLE); cc_result_free(&rc); free(resumed.response);
    size_t memory_ready=0,memory_reserved=0;
    CHECK(ok(managed_memory_service_status_detail(resume_memory,&memory_ready,&memory_reserved,NULL)) &&
          memory_ready==1 && memory_reserved==0);
    CHECK(ok(cc_event_bus_unsubscribe(runtime_bus,finish_subscription,1000)));
    mount_generation--; /* The fake fixture restores the earlier generation. */
    provider_response="{\"memories\":[]}"; expect_recovered_memory=1;
    CHECK(ok(managed_memory_service_process_one(resume_memory,41,NULL,&memory_result)) && recovered_memory_seen);
    expect_recovered_memory=0; provider_response="resumed";
    session_selection_destroy(selection); CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    controller=NULL; CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)));
    selection=NULL; CHECK(ok(session_selection_create(controller,builder,service,&selection)) &&
          ok(session_selection_choose_existing(selection,"old-session")) &&
          session_selection_phase(selection)==SESSION_SELECTION_INTERRUPTED);
    /* A failed attempt retains its reservation until the owner checks the
     * checkpoint. An oversized completed answer can then be expanded and
     * committed without losing the checkpoint's original user input. */
    char *extra_run=NULL; CHECK(ok(cc_id_generate_uuid_v4("run-",&extra_run)));
    CHECK(ok(cc_run_checkpoint_store_open(&fs,runs_dir,&fixture_store)));
    checkpoint.run_id=extra_run; checkpoint.user_message_id="old-message"; checkpoint.user_input="old input";
    CHECK(ok(cc_run_checkpoint_save(fixture_store,&checkpoint)));
    cc_run_checkpoint_store_destroy(fixture_store);
    provider_fail=1; memset(&resumed,0,sizeof(resumed));
    rc=session_selection_resume_run_with_memory(selection,extra_run,resume_memory,2,NULL,NULL,&resumed);
    CHECK(rc.code==CC_ERR_NETWORK && !resumed.completed && resumed.reservation); cc_result_free(&rc);
    free(resumed.response); resumed.response=NULL; provider_fail=0;
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&fixture_generation)));
    cc_run_checkpoint_t still_interrupted={0};
    CHECK(ok(cc_agent_runtime_load_interrupted_run(cc_runtime_generation_runtime(fixture_generation),extra_run,&still_interrupted)) &&
          strcmp(still_interrupted.user_input,"old input")==0);
    cc_run_checkpoint_cleanup(&still_interrupted); cc_runtime_generation_release(fixture_generation);
    CHECK(ok(managed_memory_service_cancel_reservation(resume_memory,&resumed.reservation)));
    memset(&resumed,0,sizeof(resumed));
    rc=session_selection_resume_run_with_memory(selection,extra_run,resume_memory,2,NULL,NULL,&resumed);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && resumed.completed && resumed.reservation && resumed.response &&
          strcmp(resumed.response,"resumed")==0); cc_result_free(&rc);
    rc=managed_memory_service_expand_reservation(resume_memory,resumed.reservation,4096);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && resumed.reservation); cc_result_free(&rc);
    CHECK(ok(managed_memory_service_expand_reservation(resume_memory,resumed.reservation,128)) &&
          ok(managed_memory_service_commit(resume_memory,&resumed.reservation,resumed.response)) && !resumed.reservation);
    free(resumed.response);
    provider_response="{\"memories\":[]}"; recovered_memory_seen=0; expect_recovered_memory=1;
    CHECK(ok(managed_memory_service_process_one(resume_memory,42,NULL,&memory_result)) && recovered_memory_seen);
    expect_recovered_memory=0; provider_response="resumed";
    /* A final answer saved before the checkpoint was removed is still part of
     * the memory turn when the user chooses to abandon the checkpoint. */
    CHECK(ok(cc_run_checkpoint_store_open(&fs,runs_dir,&fixture_store)));
    CHECK(ok(cc_run_checkpoint_save(fixture_store,&checkpoint)));
    cc_run_checkpoint_store_destroy(fixture_store);
    cc_memory_pending_reservation_t *existing_reservation=NULL;
    CHECK(ok(managed_memory_service_reserve(resume_memory,extra_run,"old-session","old input",2,&existing_reservation)));
    session_memory_turn_inspection_t inspection={0};
    CHECK(ok(session_selection_inspect_reserved_memory(selection,extra_run,resume_memory,existing_reservation,&inspection)) &&
          inspection.state==SESSION_MEMORY_CHECKPOINT_AND_FINAL && !inspection.checkpoint_from_backup &&
          inspection.assistant_text && strcmp(inspection.assistant_text,"resumed")==0);
    free(inspection.assistant_text);
    session_abandon_memory_result_t abandoned={0};
    change_after_abandon=1;
    rc=session_selection_abandon_run_with_memory(selection,extra_run,resume_memory,&existing_reservation,&abandoned);
    CHECK(rc.code==CC_ERR_STORAGE && !existing_reservation && !abandoned.abandoned && abandoned.reservation && abandoned.assistant_text &&
          strcmp(abandoned.assistant_text,"resumed")==0 &&
          session_selection_phase(selection)==SESSION_SELECTION_UNAVAILABLE); cc_result_free(&rc);
    change_after_abandon=0; mount_generation--; /* Restore the synthetic mount for explicit verification. */
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&fixture_generation)));
    memset(&still_interrupted,0,sizeof(still_interrupted));
    rc=cc_agent_runtime_load_interrupted_run(cc_runtime_generation_runtime(fixture_generation),extra_run,&still_interrupted);
    CHECK(rc.code==CC_ERR_NOT_FOUND); cc_result_free(&rc); cc_run_checkpoint_cleanup(&still_interrupted);
    char final_message_id[64]; snprintf(final_message_id,sizeof(final_message_id),"%s-final",extra_run);
    cc_message_t saved_final={0}; int final_found=0;
    CHECK(ok(cc_json_segmented_store_find_message(cc_agent_runtime_session_store(cc_runtime_generation_runtime(fixture_generation)),
          "old-session",CC_RUN_HISTORY_MESSAGE_ID,final_message_id,&saved_final,&final_found)) &&
          final_found && saved_final.role==CC_ROLE_ASSISTANT);
    cc_message_cleanup(&saved_final); cc_runtime_generation_release(fixture_generation);
    memset(&inspection,0,sizeof(inspection));
    CHECK(ok(session_selection_inspect_reserved_memory(selection,extra_run,resume_memory,abandoned.reservation,&inspection)) &&
          inspection.state==SESSION_MEMORY_FINAL_ONLY && inspection.assistant_text &&
          strcmp(inspection.assistant_text,"resumed")==0);
    free(inspection.assistant_text);
    CHECK(ok(managed_memory_service_commit(resume_memory,&abandoned.reservation,abandoned.assistant_text)) && !abandoned.reservation);
    free(abandoned.assistant_text); abandoned.assistant_text=NULL;
    provider_response="{\"memories\":[]}"; recovered_memory_seen=0; expect_recovered_memory=1;
    CHECK(ok(managed_memory_service_process_one(resume_memory,43,NULL,&memory_result)) && recovered_memory_seen);
    expect_recovered_memory=0; provider_response="resumed"; free(extra_run);
    session_selection_destroy(selection); CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    controller=NULL; CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)));
    selection=NULL; CHECK(ok(session_selection_create(controller,builder,service,&selection)) &&
          ok(session_selection_choose_existing(selection,"old-session")) &&
          session_selection_phase(selection)==SESSION_SELECTION_INTERRUPTED);
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&fixture_generation)));
    const size_t pad_length=31500U, secret_length=900U;
    char *unknown_args=malloc(pad_length+secret_length+128U); CHECK(unknown_args);
    const char *prefix="{\"payload\":\"", *middle="\",\"api_key\":\"", *suffix="\",\"end\":\"PAGING_DONE\"}";
    size_t used=strlen(prefix); memcpy(unknown_args,prefix,used);
    memset(unknown_args+used,'x',pad_length); used+=pad_length;
    memcpy(unknown_args+used,middle,strlen(middle)); used+=strlen(middle);
    for (size_t i=0;i<secret_length;i++) unknown_args[used++]="LEAKCHECK"[i%9U];
    strcpy(unknown_args+used,suffix);
    cc_tool_call_t unknown={"fixture-unknown","write",unknown_args}; cc_tool_result_t unknown_result={0};
    tool_fail=1;
    CHECK(ok(cc_agent_runtime_execute_manual_tool(cc_runtime_generation_runtime(fixture_generation),
                                                   "old-session",&unknown,&unknown_result)) && !unknown_result.ok);
    tool_fail=0; free(unknown_args); cc_tool_result_cleanup(&unknown_result); cc_runtime_generation_release(fixture_generation);
    cc_pending_operation_t *fixture_pending=NULL; size_t fixture_pending_count=0;
    CHECK(ok(recovery_service_list_operations(service,&fixture_pending,&fixture_pending_count)) && fixture_pending_count==1);
    char *fixture_operation=cc_copy_string(fixture_pending[0].operation_id); CHECK(fixture_operation);
    cc_pending_operation_array_free(fixture_pending,fixture_pending_count);
    memset(&abandoned,0,sizeof(abandoned));
    fail_before_abandon=1;
    rc=session_selection_abandon_run_with_memory(selection,run_id2,resume_memory,NULL,&abandoned);
    CHECK(rc.code==CC_ERR_IO && !abandoned.abandoned && abandoned.reservation && abandoned.assistant_text &&
          !abandoned.assistant_text[0]); cc_result_free(&rc); fail_before_abandon=0;
    cc_memory_pending_reservation_t *retry_reservation=abandoned.reservation;
    free(abandoned.assistant_text); memset(&abandoned,0,sizeof(abandoned));
    session_abandon_memory_result_t alias_abandon={.reservation=retry_reservation};
    rc=session_selection_abandon_run_with_memory(selection,run_id2,resume_memory,
                                                  &alias_abandon.reservation,&alias_abandon);
    CHECK(rc.code==CC_ERR_INVALID_ARGUMENT && alias_abandon.reservation==retry_reservation); cc_result_free(&rc);
    CHECK(ok(session_selection_abandon_run_with_memory(selection,run_id2,resume_memory,&retry_reservation,&abandoned)) &&
          abandoned.abandoned && !abandoned.reservation && !abandoned.assistant_text &&
          !retry_reservation && session_selection_phase(selection)==SESSION_SELECTION_CHATTING);
    cc_message_t *abandoned_history=NULL; size_t abandoned_count=0, abandoned_input_count=0;
    CHECK(ok(cc_runtime_controller_load_messages(controller,"old-session",50,&abandoned_history,&abandoned_count)));
    for (size_t i=0;i<abandoned_count;i++) {
        if (abandoned_history[i].id && strcmp(abandoned_history[i].id,"old-message-2")==0) {
            char *text=NULL; CHECK(ok(cc_message_get_text_summary(&abandoned_history[i],&text)));
            CHECK(abandoned_history[i].role==CC_ROLE_USER && text && strcmp(text,"other input")==0);
            abandoned_input_count++; free(text);
        }
        cc_message_cleanup(&abandoned_history[i]);
    }
    free(abandoned_history); CHECK(abandoned_input_count==1);
    provider_response="{\"memories\":[]}"; expected_memory_input="other input";
    expected_memory_response="\"assistant_response\":\"\"";
    recovered_memory_seen=0; expect_recovered_memory=1;
    CHECK(ok(managed_memory_service_process_one(resume_memory,44,NULL,&memory_result)) && recovered_memory_seen);
    expect_recovered_memory=0; expected_memory_input="old input"; expected_memory_response="resumed";
    provider_response="resumed"; managed_memory_service_destroy(resume_memory);
    session_selection_destroy(selection);
    CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    CHECK(ok(recovery_service_list_runs(service,&runs,&run_count)) && run_count==0);
    recovery_run_info_array_free(runs,run_count); free(run_id); free(run_id2);
    CHECK(ok(recovery_service_list_operations(service,&fixture_pending,&fixture_pending_count)) && fixture_pending_count==1);
    cc_pending_operation_array_free(fixture_pending,fixture_pending_count);

    /* The final menu transition reloads current history, while abandoning a
     * checkpoint leaves an unrelated uncertain tool operation blocked. */
    char *run_id3=NULL; CHECK(ok(cc_id_generate_uuid_v4("run-",&run_id3)));
    CHECK(ok(cc_run_checkpoint_store_open(&fs,runs_dir,&fixture_store)));
    checkpoint.run_id=run_id3; checkpoint.user_message_id="menu-message-3"; checkpoint.user_input="menu abandoned input";
    CHECK(ok(cc_run_checkpoint_save(fixture_store,&checkpoint)));
    cc_run_checkpoint_store_destroy(fixture_store);
    controller=NULL; CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)));
    menu=NULL; CHECK(ok(session_menu_create(controller,builder,service,&menu)) && session_menu_present(menu)==ESP_OK);
    CHECK(session_menu_handle(menu,"/view 1",APP_EVENT_SOURCE_WEB,NULL,NULL));
    CHECK(session_menu_handle(menu,"/resume",APP_EVENT_SOURCE_WEB,NULL,NULL) && strstr(view_text,run_id3));
    CHECK(session_menu_handle(menu,"/sessions",APP_EVENT_SOURCE_SERIAL,NULL,NULL) &&
          strstr(view_text,"/new"));
    CHECK(ok(recovery_service_list_runs(service,&runs,&run_count)) && run_count==1);
    recovery_run_info_array_free(runs,run_count);
    CHECK(session_menu_handle(menu,"/view 1",APP_EVENT_SOURCE_WEB,NULL,NULL) &&
          session_menu_handle(menu,"/resume",APP_EVENT_SOURCE_WEB,NULL,NULL) && strstr(view_text,run_id3));
    CHECK(session_menu_handle(menu,"/new",APP_EVENT_SOURCE_WEB,NULL,NULL) && !session_menu_waiting(menu) &&
          strstr(view_text,"新会话已开启"));
    cc_runtime_controller_get_status(controller,&controller_status);
    CHECK(!controller_status.session_is_persisted);
    CHECK(ok(recovery_service_list_runs(service,&runs,&run_count)) && run_count==1);
    recovery_run_info_array_free(runs,run_count);
    session_menu_destroy(menu); CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    controller=NULL; CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)));
    menu=NULL; CHECK(ok(session_menu_create(controller,builder,service,&menu)) && session_menu_present(menu)==ESP_OK);
    CHECK(session_menu_handle(menu,"/view 1",APP_EVENT_SOURCE_WEB,NULL,NULL) &&
          session_menu_handle(menu,"/resume",APP_EVENT_SOURCE_WEB,NULL,NULL) && strstr(view_text,run_id3));
    CHECK(session_menu_handle(menu,"/abandon 1",APP_EVENT_SOURCE_SERIAL,NULL,NULL) && !session_menu_waiting(menu) &&
          strstr(view_text,"已恢复会话") && strstr(view_text,"resumed") && strstr(view_text,"menu abandoned input"));
    session_menu_destroy(menu); CHECK(ok(cc_runtime_controller_destroy(controller,1000))); free(run_id3);
    CHECK(test_runs_ready_retry(builder, service, &fs, runs_dir, &controller_config) == 0);
    CHECK(ok(recovery_service_list_operations(service,&fixture_pending,&fixture_pending_count)) && fixture_pending_count==1);
    cc_pending_operation_array_free(fixture_pending,fixture_pending_count);
    recovery_console_t *console=NULL; CHECK(ok(recovery_console_create(service,&console)));
    CHECK(recovery_console_handles("/pending") && recovery_console_handles("/resolve id not-executed") &&
          !recovery_console_handles("/pending-other"));
    recovery_console_handle(console,"/pending",APP_EVENT_SOURCE_WEB);
    CHECK(strstr(view_text,fixture_operation) && strstr(view_text,"write"));
    recovery_console_handle(console,"/pending view 1",APP_EVENT_SOURCE_SERIAL);
    CHECK(strstr(view_text,fixture_operation) && strstr(view_text,"/pending more") && !strstr(view_text,"LEAKCHECK"));
    recovery_console_handle(console,"/pending more",APP_EVENT_SOURCE_WEB);
    CHECK(strstr(view_text,"PAGING_DONE") && !strstr(view_text,"LEAKCHECK"));
    char resolve_command[192];
    snprintf(resolve_command,sizeof(resolve_command),"/resolve %s executed",fixture_operation);
    recovery_console_handle(console,resolve_command,APP_EVENT_SOURCE_SERIAL);
    CHECK(ok(recovery_service_list_operations(service,&fixture_pending,&fixture_pending_count)) && fixture_pending_count==1);
    cc_pending_operation_array_free(fixture_pending,fixture_pending_count);
    int calls_before_resolve=tool_calls;
    snprintf(resolve_command,sizeof(resolve_command),"/resolve %s not-executed",fixture_operation);
    recovery_console_handle(console,resolve_command,APP_EVENT_SOURCE_WEB);
    CHECK(tool_calls==calls_before_resolve && strstr(view_text,"共 0 条"));
    CHECK(ok(recovery_service_list_operations(service,&fixture_pending,&fixture_pending_count)) && fixture_pending_count==0);
    cc_pending_operation_array_free(fixture_pending,fixture_pending_count);
    recovery_console_destroy(console);
    free(fixture_operation);
    controller=NULL; CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)));
    menu=NULL; CHECK(ok(session_menu_create(controller,builder,service,&menu)) && session_menu_present(menu)==ESP_OK);
    fail_next_view=1;
    CHECK(session_menu_handle(menu,"/new",APP_EVENT_SOURCE_WEB,NULL,NULL) && session_menu_waiting(menu));
    CHECK(!session_menu_handle(menu,"first message",APP_EVENT_SOURCE_WEB,NULL,NULL) &&
          !session_menu_waiting(menu) && strstr(view_text,"新会话已开启"));
    session_menu_destroy(menu);
    fixture_generation=NULL; CHECK(ok(cc_runtime_builder_acquire_generation(builder,&fixture_generation)));
    fixture_sessions=cc_agent_runtime_session_store(cc_runtime_generation_runtime(fixture_generation));
    listed=NULL; listed_count=0;
    CHECK(ok(fixture_sessions->vtable->list_sessions(fixture_sessions->self,&listed,&listed_count)) && listed_count==1);
    cc_session_free_array(listed,listed_count); cc_runtime_generation_release(fixture_generation);
    cc_runtime_controller_get_status(controller,&controller_status); CHECK(!controller_status.session_is_persisted);
    manual_thread_t manual_thread={.controller=controller}; pthread_t thread;
    CHECK(pthread_create(&thread,NULL,manual_thread_run,&manual_thread)==0);
    struct timespec deadline; CHECK(clock_gettime(CLOCK_REALTIME,&deadline)==0); deadline.tv_sec+=20;
    pthread_mutex_lock(&block_mutex);
    while (!block_started) if (pthread_cond_timedwait(&block_cond,&block_mutex,&deadline)!=0) break;
    int started=block_started; pthread_mutex_unlock(&block_mutex); CHECK(started);
    cc_runtime_controller_get_status(controller,&controller_status);
    CHECK(controller_status.active_run_id && controller_status.session_is_persisted &&
          ok(cc_runtime_controller_cancel(controller,controller_status.active_run_id)));
    pthread_mutex_lock(&block_mutex); block_release=1; pthread_cond_broadcast(&block_cond); pthread_mutex_unlock(&block_mutex);
    CHECK(pthread_join(thread,NULL)==0 && ok(manual_thread.rc) && manual_thread.result.ok &&
          strcmp(manual_thread.result.text,"confirmed")==0);
    cc_tool_result_cleanup(&manual_thread.result);
    CHECK(ok(recovery_service_list_operations(service,&fixture_pending,&fixture_pending_count)) && fixture_pending_count==0);
    cc_pending_operation_array_free(fixture_pending,fixture_pending_count);
    cc_tool_result_t lua_result={0};
    rc=manual_lua_service_execute(controller,service,"/lua print(1)",
                                  MANUAL_LUA_POLICY_PENDING,&lua_result);
    CHECK(rc.code==CC_ERR_INVALID_STATE && lua_calls==0); cc_result_free(&rc); cc_tool_result_cleanup(&lua_result);
    CHECK(ok(manual_lua_service_execute(controller,service,"/lua print(\"你好\")",
                                        MANUAL_LUA_USE_MODEL_REGISTRY,&lua_result)) && !lua_result.ok && lua_calls==0);
    cc_tool_result_cleanup(&lua_result);
    CHECK(ok(manual_lua_service_execute(controller,service,"/lua print(\"你好\")",
                                        MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY,&lua_result)) && lua_result.ok &&
          lua_calls==1 && strcmp(lua_argument,"print(\"你好\")")==0);
    cc_tool_result_cleanup(&lua_result);
    lua_fail=1;
    CHECK(ok(manual_lua_service_execute(controller,service,"/lua work()",
                                        MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY,&lua_result)) && !lua_result.ok && lua_calls==2);
    cc_tool_result_cleanup(&lua_result); lua_fail=0;
    CHECK(ok(manual_lua_service_execute(controller,service,"/lua work()",
                                        MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY,&lua_result)) && !lua_result.ok && lua_calls==2);
    cc_tool_result_cleanup(&lua_result);
    CHECK(ok(manual_lua_service_execute(controller,service,"/luafile lua/work.lua",
                                        MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY,&lua_result)) && lua_result.ok &&
          lua_calls==3 && strcmp(lua_argument,"lua/work.lua")==0);
    cc_tool_result_cleanup(&lua_result);
    CHECK(ok(recovery_service_list_operations(service,&fixture_pending,&fixture_pending_count)) && fixture_pending_count==1 &&
          strcmp(fixture_pending[0].call.name,"lua_run_code")==0);
    char *lua_operation=cc_copy_string(fixture_pending[0].operation_id); CHECK(lua_operation);
    cc_pending_operation_array_free(fixture_pending,fixture_pending_count);
    CHECK(ok(recovery_service_resolve_operation(service,lua_operation,CC_TOOL_RECOVERY_NOT_EXECUTED,NULL)));
    free(lua_operation);
    CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    /* New turns reserve the same id later used by the checkpoint. A full
     * queue must not create an empty session, and a failed provider call can
     * resume using the original live reservation. */
    controller=NULL; CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)));
    selection=NULL; CHECK(ok(session_selection_create(controller,builder,service,&selection)) &&
          ok(session_selection_choose_new(selection)));
    resume_memory=NULL;
    CHECK(ok(managed_memory_service_create(builder,service,memory,&resume_options,&resume_extraction,&resume_memory)));
    CHECK(ok(managed_memory_service_enqueue(resume_memory,"queue-full","old-session","input","answer")));
    cc_runtime_controller_get_status(controller,&controller_status); CHECK(!controller_status.session_is_persisted);
    session_new_turn_memory_result_t new_turn={0}; calls_before_resume=provider_calls;
    rc=session_selection_submit_turn_with_memory(selection,resume_memory,"new input",128,NULL,NULL,&new_turn);
    CHECK(rc.code==CC_ERR_LIMIT_EXCEEDED && !new_turn.run_id && !new_turn.reservation &&
          provider_calls==calls_before_resume); cc_result_free(&rc);
    cc_runtime_controller_get_status(controller,&controller_status); CHECK(!controller_status.session_is_persisted);
    provider_response="{\"memories\":[]}";
    CHECK(ok(managed_memory_service_process_one(resume_memory,43,NULL,&memory_result)));
    provider_response="resumed";
    char *stale_run=NULL; CHECK(ok(cc_id_generate_uuid_v4("run-",&stale_run)));
    char *stale_answer=NULL;
    rc=cc_runtime_controller_submit_text_with_run_id(controller,"different-session",stale_run,"new input",NULL,NULL,&stale_answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !stale_answer); cc_result_free(&rc); free(stale_run);
    cc_runtime_controller_get_status(controller,&controller_status); CHECK(!controller_status.session_is_persisted);
    provider_fail=1; memset(&new_turn,0,sizeof(new_turn));
    rc=session_selection_submit_turn_with_memory(selection,resume_memory,"new input",128,NULL,NULL,&new_turn);
    CHECK(rc.code==CC_ERR_NETWORK && !new_turn.completed && new_turn.run_id && new_turn.reservation &&
          session_selection_phase(selection)==SESSION_SELECTION_INTERRUPTED); cc_result_free(&rc);
    free(new_turn.response); new_turn.response=NULL; provider_fail=0;
    cc_runtime_controller_get_status(controller,&controller_status); CHECK(controller_status.session_is_persisted);
    CHECK(ok(managed_memory_service_verify_reservation(resume_memory,new_turn.reservation,new_turn.run_id,
                                                       controller_status.session_id,"new input")));
    memset(&inspection,0,sizeof(inspection));
    CHECK(ok(session_selection_inspect_reserved_memory(selection,new_turn.run_id,resume_memory,new_turn.reservation,&inspection)) &&
          inspection.state==SESSION_MEMORY_CHECKPOINT_ONLY && !inspection.assistant_text);
    rc=managed_memory_service_verify_reservation(resume_memory,new_turn.reservation,new_turn.run_id,
                                                  controller_status.session_id,"other input");
    CHECK(rc.code==CC_ERR_INVALID_STATE); cc_result_free(&rc);
    session_resume_memory_result_t alias_resume={.reservation=new_turn.reservation};
    rc=session_selection_resume_run_with_reserved_memory(selection,new_turn.run_id,resume_memory,
          &alias_resume.reservation,NULL,NULL,&alias_resume);
    CHECK(rc.code==CC_ERR_INVALID_ARGUMENT && alias_resume.reservation==new_turn.reservation); cc_result_free(&rc);
    calls_before_resume=provider_calls; stale_answer=NULL;
    rc=cc_runtime_controller_submit_text_with_run_id(controller,controller_status.session_id,new_turn.run_id,
                                                      "different input",NULL,NULL,&stale_answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !stale_answer && provider_calls==calls_before_resume); cc_result_free(&rc);
    memset(&resumed,0,sizeof(resumed));
    CHECK(ok(session_selection_resume_run_with_reserved_memory(selection,new_turn.run_id,resume_memory,
          &new_turn.reservation,NULL,NULL,&resumed)) && !new_turn.reservation && resumed.completed &&
          !resumed.reservation && resumed.response && strcmp(resumed.response,"resumed")==0 &&
          session_selection_phase(selection)==SESSION_SELECTION_CHATTING);
    free(resumed.response); calls_before_resume=provider_calls; stale_answer=NULL;
    rc=cc_runtime_controller_submit_text_with_run_id(controller,controller_status.session_id,new_turn.run_id,
                                                      "new input",NULL,NULL,&stale_answer);
    CHECK(rc.code==CC_ERR_INVALID_STATE && !stale_answer && provider_calls==calls_before_resume); cc_result_free(&rc);
    cc_message_t *new_history=NULL; size_t new_count=0, input_count=0;
    CHECK(ok(cc_runtime_controller_load_messages(controller,controller_status.session_id,50,&new_history,&new_count)));
    for (size_t i=0;i<new_count;i++) {
        if (new_history[i].role==CC_ROLE_USER) {
            char *text=NULL; CHECK(ok(cc_message_get_text_summary(&new_history[i],&text)));
            if (text && strcmp(text,"new input")==0) input_count++;
            free(text);
        }
        cc_message_cleanup(&new_history[i]);
    }
    free(new_history); CHECK(input_count==1);
    provider_response="{\"memories\":[]}"; expected_memory_input="new input";
    expect_recovered_memory=1; recovered_memory_seen=0;
    CHECK(ok(managed_memory_service_process_one(resume_memory,44,NULL,&memory_result)) && recovered_memory_seen);
    expect_recovered_memory=0; provider_response="resumed";
    free(new_turn.run_id); memset(&new_turn,0,sizeof(new_turn));
    char *unstarted_run=NULL; CHECK(ok(cc_id_generate_uuid_v4("run-",&unstarted_run)));
    existing_reservation=NULL;
    CHECK(ok(managed_memory_service_reserve(resume_memory,unstarted_run,controller_status.session_id,
                                            "unstarted input",128,&existing_reservation)));
    memset(&inspection,0,sizeof(inspection));
    CHECK(ok(session_selection_inspect_reserved_memory(selection,unstarted_run,resume_memory,existing_reservation,&inspection)) &&
          inspection.state==SESSION_MEMORY_NO_DURABLE_EVIDENCE && !inspection.assistant_text);
    CHECK(ok(managed_memory_service_cancel_reservation(resume_memory,&existing_reservation))); free(unstarted_run);
    provider_fail=1;
    rc=session_selection_submit_turn_with_memory(selection,resume_memory,"abandoned input",128,NULL,NULL,&new_turn);
    CHECK(rc.code==CC_ERR_NETWORK && new_turn.run_id && new_turn.reservation && !new_turn.completed);
    cc_result_free(&rc); free(new_turn.response); provider_fail=0;
    memset(&abandoned,0,sizeof(abandoned));
    CHECK(ok(session_selection_abandon_run_with_memory(selection,new_turn.run_id,resume_memory,
          &new_turn.reservation,&abandoned)) && abandoned.abandoned && !new_turn.reservation &&
          !abandoned.reservation && !abandoned.assistant_text &&
          session_selection_phase(selection)==SESSION_SELECTION_CHATTING);
    free(new_turn.run_id); memset(&new_turn,0,sizeof(new_turn));
    provider_response="{\"memories\":[]}"; expected_memory_input="abandoned input";
    expected_memory_response="\"assistant_response\":\"\""; expect_recovered_memory=1; recovered_memory_seen=0;
    CHECK(ok(managed_memory_service_process_one(resume_memory,45,NULL,&memory_result)) && recovered_memory_seen);
    expect_recovered_memory=0; expected_memory_response="resumed"; provider_response="resumed";
    finish=(finish_action_t){.controller=controller,.change_media=1};
    CHECK(ok(cc_event_bus_subscribe(runtime_bus,CC_OBS_EVENT_RUN_FINISHED,on_run_finished,&finish,NULL,&finish_subscription)));
    rc=session_selection_submit_turn_with_memory(selection,resume_memory,"second input",128,NULL,NULL,&new_turn);
    CHECK(rc.code==CC_ERR_STORAGE && finish.called==1 && finish.cancelled && new_turn.completed &&
          !new_turn.reservation && new_turn.response && strcmp(new_turn.response,"resumed")==0 &&
          session_selection_phase(selection)==SESSION_SELECTION_UNAVAILABLE); cc_result_free(&rc);
    free(new_turn.response); free(new_turn.run_id);
    CHECK(ok(managed_memory_service_status_detail(resume_memory,&memory_ready,&memory_reserved,NULL)) &&
          memory_ready==1 && memory_reserved==0);
    CHECK(ok(cc_event_bus_unsubscribe(runtime_bus,finish_subscription,1000)));
    mount_generation--; /* The fake fixture restores the earlier generation. */
    provider_response="{\"memories\":[]}"; expected_memory_input="second input";
    expect_recovered_memory=1; recovered_memory_seen=0;
    CHECK(ok(managed_memory_service_process_one(resume_memory,46,NULL,&memory_result)) && recovered_memory_seen);
    expect_recovered_memory=0; expected_memory_input="old input"; provider_response="resumed";
    managed_memory_service_destroy(resume_memory);
    session_selection_destroy(selection); CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    CHECK(exercise_owned_memory_turns(builder,service,memory)==0);
    CHECK(exercise_default_memory_owner(builder,service,memory)==0);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);
    builder=NULL; service=NULL; tool_calls=0;
    CHECK(remove_tree(&fs,RECOVERY_TEST_BASE));
    CHECK(ok(recovery_service_prepare(&config,&io,&service)) &&
          recovery_service_state(service)==RECOVERY_SERVICE_READY_TO_ATTACH &&
          ok(startup_recovery_present(service)) && startup_gate_phase()==STARTUP_GATE_LOADING);
    info.size=sizeof(info); CHECK(ok(fs.vtable->stat(fs.self,RECOVERY_TEST_BASE,&info)) && info.type==CC_FS_ENTRY_MISSING);
    startup_recovery_reset(); recovery_service_destroy(service); service=NULL;

    CHECK(ok(fs.vtable->make_dir(fs.self,APP_RECOVERY_ROOT "/operations")) &&
          ok(fs.vtable->make_dir(fs.self,APP_RECOVERY_ROOT "/runs")));
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
          ok(cc_runtime_builder_require_recovery(builder)) && attempt_tool(builder,0) && tool_calls==0);
    cc_runtime_generation_t *manual_generation=NULL;
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&manual_generation)));
    cc_tool_t manual_tool={(void *)"manual-only",&trusted_tool_vt};
    cc_tool_call_t manual_call={"before-attach","manual-only","{}"}; cc_tool_result_t manual_result={0};
    rc=cc_agent_runtime_execute_manual_tool_with_impl(cc_runtime_generation_runtime(manual_generation),
                                                      "manual-session",&manual_call,&manual_tool,&manual_result);
    CHECK(rc.code==CC_ERR_INVALID_STATE && tool_calls==0); cc_result_free(&rc);
    cc_tool_result_cleanup(&manual_result); cc_runtime_generation_release(manual_generation);
    CHECK(ok(recovery_service_open(builder,&config,&io,&service)) &&
          recovery_service_state(service)==RECOVERY_SERVICE_READY && recovery_service_is_current(service));
    cc_pending_operation_t *pending=NULL; size_t pending_count=0;
    mount_generation++; CHECK(!recovery_service_is_current(service)); mount_generation--;
    /* The manually supplied implementation is not in the model registry but
     * shares the same durable journal and same-name blocking. */
    manual_generation=NULL; CHECK(ok(cc_runtime_builder_acquire_generation(builder,&manual_generation)));
    manual_call.name="write";
    rc=cc_agent_runtime_execute_manual_tool_with_impl(cc_runtime_generation_runtime(manual_generation),
                                                      "manual-session",&manual_call,&manual_tool,&manual_result);
    CHECK(rc.code==CC_ERR_INVALID_ARGUMENT && tool_calls==0); cc_result_free(&rc);
    cc_tool_result_cleanup(&manual_result); cc_runtime_generation_release(manual_generation);
    tool_fail=1; CHECK(attempt_trusted_tool(builder,"manual-only","manual-1",0) && tool_calls==1);
    tool_fail=0; CHECK(attempt_trusted_tool(builder,"manual-only","manual-2",0) && tool_calls==1);
    CHECK(ok(recovery_service_list_operations(service,&pending,&pending_count)) && pending_count==1 &&
          strcmp(pending[0].call.name,"manual-only")==0);
    char *manual_operation=cc_copy_string(pending[0].operation_id); CHECK(manual_operation);
    cc_pending_operation_array_free(pending,pending_count);
    CHECK(ok(recovery_service_resolve_operation(service,manual_operation,CC_TOOL_RECOVERY_NOT_EXECUTED,NULL)));
    free(manual_operation);
    CHECK(attempt_trusted_tool(builder,"manual-only","manual-3",1) && tool_calls==2);
    tool_calls=0;
    CHECK(attempt_tool(builder,1) && tool_calls==1);
    /* Retrying the same externally supplied ID must not append another intent
     * or call the tool, including after the earlier call returned normally. */
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&manual_generation)));
    cc_tool_call_t duplicate_call={last_tool_attempt_id,"write","{}"};
    rc=cc_agent_runtime_execute_manual_tool(cc_runtime_generation_runtime(manual_generation),
                                           "session",&duplicate_call,&manual_result);
    CHECK(rc.code==CC_ERR_INVALID_STATE && tool_calls==1); cc_result_free(&rc);
    cc_tool_result_cleanup(&manual_result); cc_runtime_generation_release(manual_generation);
    CHECK(ok(recovery_service_list_operations(service,&pending,&pending_count)) && pending_count==0);
    cc_pending_operation_array_free(pending,pending_count);
    tool_fail=1; CHECK(attempt_tool(builder,0) && tool_calls==2); tool_fail=0;
    CHECK(ok(recovery_service_list_operations(service,&pending,&pending_count)) && pending_count==1 &&
          strcmp(pending[0].session_id,"session")==0 && strcmp(pending[0].call.id,last_tool_attempt_id)==0 &&
          strcmp(pending[0].call.name,"write")==0 && !pending[0].in_progress);
    char *operation_id=cc_copy_string(pending[0].operation_id); CHECK(operation_id);
    cc_pending_operation_array_free(pending,pending_count);
    CHECK(attempt_tool(builder,0) && tool_calls==2);
    CHECK(attempt_trusted_tool(builder,"write","manual-blocked",0) && tool_calls==2);
    /* Simulate two U disks at the same mount path. An unknown operation on A
     * stays on A; B starts from its own empty journal, and returning to A
     * restores the original operation and same-name block. */
    const char *disk_a=RECOVERY_TEST_BASE "-disk-a", *disk_b=RECOVERY_TEST_BASE "-disk-b";
    info.size=sizeof(info); CHECK(ok(fs.vtable->stat(fs.self,disk_a,&info)) && info.type==CC_FS_ENTRY_MISSING);
    info.size=sizeof(info); CHECK(ok(fs.vtable->stat(fs.self,disk_b,&info)) && info.type==CC_FS_ENTRY_MISSING);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);
    builder=NULL; service=NULL;
    CHECK(rename(RECOVERY_TEST_BASE,disk_a)==0); mount_generation++;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
          ok(recovery_service_open(builder,&config,&io,&service)) && recovery_service_is_current(service));
    CHECK(ok(recovery_service_list_operations(service,&pending,&pending_count)) && pending_count==0);
    cc_pending_operation_array_free(pending,pending_count);
    CHECK(attempt_tool(builder,1) && tool_calls==3);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);
    builder=NULL; service=NULL;
    CHECK(rename(RECOVERY_TEST_BASE,disk_b)==0 && rename(disk_a,RECOVERY_TEST_BASE)==0); mount_generation++;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
          ok(recovery_service_open(builder,&config,&io,&service)) && recovery_service_is_current(service));
    CHECK(ok(recovery_service_list_operations(service,&pending,&pending_count)) && pending_count==1 &&
          strcmp(pending[0].operation_id,operation_id)==0 && strcmp(pending[0].call.name,"write")==0);
    cc_pending_operation_array_free(pending,pending_count);
    tool_calls=2;
    CHECK(attempt_tool(builder,0) && tool_calls==2 && remove_tree(&fs,disk_b));
    rc=recovery_service_resolve_operation(service,operation_id,CC_TOOL_RECOVERY_EXECUTED,"");
    CHECK(rc.code==CC_ERR_INVALID_ARGUMENT); cc_result_free(&rc);
    rc=recovery_service_resolve_operation(service,"not-an-operation",CC_TOOL_RECOVERY_NOT_EXECUTED,NULL);
    CHECK(rc.code==CC_ERR_NOT_FOUND); cc_result_free(&rc);
    checks_until_mount_change=3;
    rc=recovery_service_resolve_operation(service,operation_id,CC_TOOL_RECOVERY_EXECUTED,"verified");
    CHECK(rc.code==CC_ERR_STORAGE); cc_result_free(&rc);
    mount_generation--; /* Restore the fake fixture, then verify it stayed pending. */
    CHECK(ok(recovery_service_list_operations(service,&pending,&pending_count)) && pending_count==1);
    cc_pending_operation_array_free(pending,pending_count);
    CHECK(ok(recovery_service_resolve_operation(service,operation_id,CC_TOOL_RECOVERY_EXECUTED,"verified")));
    free(operation_id);
    CHECK(ok(recovery_service_list_operations(service,&pending,&pending_count)) && pending_count==0);
    cc_pending_operation_array_free(pending,pending_count);
    checks_until_mount_change=2;
    rc=recovery_service_list_operations(service,&pending,&pending_count);
    CHECK(rc.code==CC_ERR_STORAGE && !pending && pending_count==0); cc_result_free(&rc);
    mount_generation--; /* Restore the fake fixture; real generations never decrease. */
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);
    builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) && ok(recovery_service_open(builder,&config,&io,&service)) &&
          recovery_service_state(service)==RECOVERY_SERVICE_READY);
    /* Preserve a real session, then simulate a torn history record while the
     * process is stopped. The next boot must offer a separate text-only
     * conversation without editing the damaged bytes. */
    fixture_generation=NULL;
    CHECK(ok(cc_runtime_builder_acquire_generation(builder,&fixture_generation)) &&
          ok(cc_agent_runtime_create_session(cc_runtime_generation_runtime(fixture_generation),"damaged-session",workspace)));
    cc_runtime_generation_release(fixture_generation);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);

    char damaged_segment[512];
    damaged_segment[0]='\0';
    char **session_dirs=NULL; size_t session_dir_count=0;
    CHECK(ok(fs.vtable->list_dir(fs.self,sessions,&session_dirs,&session_dir_count)));
    for (size_t i=0;i<session_dir_count;i++) {
        if (strncmp(session_dirs[i],"damaged-session-",16)==0)
            snprintf(damaged_segment,sizeof(damaged_segment),"%s/%s/events-000001.jsonl",sessions,session_dirs[i]);
        free(session_dirs[i]);
    }
    free(session_dirs); CHECK(damaged_segment[0]);
    char *original_segment=NULL,*damaged_bytes=NULL,*after_boot=NULL;
    CHECK(ok(fs.vtable->read_text(fs.self,damaged_segment,&original_segment)) &&
          ok(fs.vtable->append_bytes(fs.self,damaged_segment,"{\n",2)) &&
          ok(fs.vtable->read_text(fs.self,damaged_segment,&damaged_bytes)));
    builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
          ok(recovery_service_open(builder,&config,&io,&service)) &&
          recovery_service_state(service)==RECOVERY_SERVICE_HISTORY_DAMAGED &&
          recovery_service_history_damaged(service) &&
          strcmp(recovery_service_mode_name(service),"history_damaged")==0);
    CHECK(ok(startup_recovery_present(service)) && ok(startup_recovery_mark_running(service)) &&
          startup_gate_phase()==STARTUP_GATE_RUNNING);
    startup_recovery_reset();
    controller=NULL; menu=NULL;
    CHECK(ok(cc_runtime_controller_create(builder,&controller_config,&controller)) &&
          ok(session_menu_create(controller,builder,service,&menu)) &&
          session_menu_present(menu)==ESP_OK && strstr(view_text,"所有工具已禁用") &&
          session_menu_waiting(menu));
    CHECK(session_menu_handle(menu,"/view 1",APP_EVENT_SOURCE_SERIAL,NULL,NULL) && session_menu_waiting(menu));
    CHECK(session_menu_handle(menu,"/new",APP_EVENT_SOURCE_WEB,NULL,NULL) &&
          !session_menu_waiting(menu) && strstr(view_text,"纯文本对话"));
    CHECK(ok(cc_runtime_builder_reload(builder,&config)) && recovery_service_history_damaged(service));
    provider_response="resumed"; provider_saw_tools=1;
    char *degraded_answer=NULL;
    CHECK(ok(cc_runtime_controller_submit_text(controller,"text in degraded mode",NULL,NULL,&degraded_answer)) &&
          degraded_answer && strcmp(degraded_answer,"resumed")==0 && !provider_saw_tools);
    free(degraded_answer);
    cc_runtime_controller_status_t degraded_status={0};
    cc_runtime_controller_get_status(controller,&degraded_status);
    CHECK(degraded_status.session_is_persisted);
    history=NULL; history_count=0;
    CHECK(ok(cc_runtime_controller_load_messages(controller,degraded_status.session_id,100,&history,&history_count)) &&
          history_count==2 && history[0].role==CC_ROLE_USER && history[1].role==CC_ROLE_ASSISTANT);
    for (size_t i=0;i<history_count;i++) cc_message_cleanup(&history[i]);
    free(history);
    /* Exercise the actual default managed-memory path in damaged-history mode,
     * including a failed provider request whose input still needs evaluation. */
    session_menu_destroy(menu); menu=NULL;
    CHECK(ok(cc_runtime_controller_start_new_session(controller)));
    app_config_t damaged_memory_config={0};
    damaged_memory_config.runtime.memory_path=memory; damaged_memory_config.runtime.model="fake";
    damaged_memory_config.runtime.max_input_bytes=1024; damaged_memory_config.runtime.max_output_bytes=128;
    damaged_memory_config.summary_cache_per_session=8; damaged_memory_config.summary_cache_max_entries=128;
    damaged_memory_config.summary_cache_max_bytes=1048576;
    damaged_memory_config.managed_memory_capacity=100; damaged_memory_config.managed_memory_pending_items=2;
    damaged_memory_config.managed_memory_pending_bytes=4096;
    startup_managed_memory_t *damaged_owner=NULL;
    CHECK(ok(startup_managed_memory_create(&damaged_memory_config,builder,service,&damaged_owner)));
    session_memory_turns_t *damaged_turns=startup_managed_memory_turns(damaged_owner);
    CHECK(ok(session_menu_create_with_memory(controller,builder,service,damaged_turns,&menu)) &&
          session_menu_present(menu)==ESP_OK &&
          session_menu_handle(menu,"/new",APP_EVENT_SOURCE_WEB,NULL,NULL));
    session_memory_turn_result_t damaged_turn={0}; provider_saw_tools=1; provider_response="resumed";
    CHECK(ok(session_menu_submit_turn(menu,"degraded managed input",NULL,NULL,&damaged_turn)) &&
          damaged_turn.completed && !provider_saw_tools);
    session_memory_pump_result_t damaged_pump={0}; provider_response="{\"memories\":[]}";
    expected_memory_input="degraded managed input"; expect_recovered_memory=1; recovered_memory_seen=0;
    CHECK(ok(session_memory_turns_pump(damaged_turns,51,NULL,&damaged_pump)) && recovered_memory_seen);
    provider_fail=1;
    rc=session_menu_submit_turn(menu,"degraded failed input",NULL,NULL,&damaged_turn);
    CHECK(rc.code==CC_ERR_NETWORK && !damaged_turn.completed && !damaged_turn.awaiting_resolution &&
          !session_menu_waiting(menu)); cc_result_free(&rc);
    provider_fail=0; expected_memory_input="degraded failed input";
    expected_memory_response="\"assistant_response\":\"\""; recovered_memory_seen=0;
    CHECK(ok(session_memory_turns_pump(damaged_turns,52,NULL,&damaged_pump)) && recovered_memory_seen);
    expect_recovered_memory=0; expected_memory_input="old input"; expected_memory_response="resumed";
    provider_response="resumed";
    int calls_before_degraded_tool=tool_calls;
    CHECK(attempt_tool(builder,0) && tool_calls==calls_before_degraded_tool);
    CHECK(ok(fs.vtable->read_text(fs.self,damaged_segment,&after_boot)) &&
          strcmp(damaged_bytes,after_boot)==0);
    free(after_boot); free(damaged_bytes);
    mount_generation++;
    CHECK(!recovery_service_is_current(service) && recovery_service_state(service)==RECOVERY_SERVICE_FAILED &&
          strcmp(recovery_service_mode_name(service),"unavailable")==0);
    mount_generation--;
    session_menu_destroy(menu); CHECK(ok(cc_runtime_controller_destroy(controller,1000)));
    CHECK(ok(startup_managed_memory_destroy(&damaged_owner)) && !damaged_owner);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);
    CHECK(ok(fs.vtable->write_text(fs.self,damaged_segment,original_segment)));
    free(original_segment);
    builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
          ok(recovery_service_open(builder,&config,&io,&service)) &&
          recovery_service_state(service)==RECOVERY_SERVICE_READY && !recovery_service_history_damaged(service) &&
          strcmp(recovery_service_mode_name(service),"ready")==0);
    CHECK(attempt_tool(builder,1) && tool_calls==calls_before_degraded_tool+1);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);

    /* Missing journal directories on a disk with existing history cannot be
     * mistaken for a fresh disk. Keep the old data and allow only text. */
    CHECK(rename(APP_RECOVERY_ROOT "/operations",APP_RECOVERY_ROOT "/operations-held")==0);
    builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)) &&
          ok(recovery_service_open(builder,&config,&io,&service)) &&
          recovery_service_history_damaged(service));
    int calls_before_missing_journal=tool_calls;
    CHECK(attempt_tool(builder,0) && tool_calls==calls_before_missing_journal);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);
    info.size=sizeof(info);
    CHECK(ok(fs.vtable->stat(fs.self,APP_RECOVERY_ROOT "/operations",&info)) && info.type==CC_FS_ENTRY_MISSING &&
          rename(APP_RECOVERY_ROOT "/operations-held",APP_RECOVERY_ROOT "/operations")==0);

    /* An I/O error is not proof of damaged history and must never open the
     * degraded service, even if the medium is still reported mounted. */
    wrap_stage_filesystem=1; session_read_fail_path=damaged_segment;
    builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    rc=recovery_service_open(builder,&config,&io,&service);
    CHECK(rc.code==CC_ERR_IO && !service && attempt_tool(builder,0));
    cc_result_free(&rc);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000)));
    session_read_fail_path=NULL; wrap_stage_filesystem=0;

    /* A disconnect during directory preparation leaves tools closed. */
    builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    checks_until_mount_change=5;
    rc=recovery_service_open(builder,&config,&io,&service);
    CHECK(rc.code==CC_ERR_STORAGE && !service && attempt_tool(builder,0)); cc_result_free(&rc);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000)));
    /* If the mount changes after bundle attachment, retain its owner until the
     * builder is destroyed and never advertise the stale mount as usable. */
    builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    checks_until_mount_change=14;
    CHECK(ok(recovery_service_open(builder,&config,&io,&service)) && service &&
          recovery_service_state(service)==RECOVERY_SERVICE_FAILED && !recovery_service_is_current(service));
    CHECK(ok(cc_runtime_builder_destroy(builder,1000))); recovery_service_destroy(service);
    /* A workspace overlapping recovery records is rejected before opening the
     * tools, including when the configured names use different ASCII case. */
    config.workspace_path=APP_RECOVERY_ROOT; builder=NULL; service=NULL;
    CHECK(ok(cc_runtime_builder_create(&config,&features,&builder)));
    rc=recovery_service_open(builder,&config,&io,&service);
    CHECK(rc.code==CC_ERR_PERMISSION_DENIED && !service && attempt_tool(builder,0)); cc_result_free(&rc);
    CHECK(ok(cc_runtime_builder_destroy(builder,1000)));
    CHECK(remove_tree(&fs,RECOVERY_TEST_BASE)); fs.vtable->destroy(fs.self); return 0;
}
