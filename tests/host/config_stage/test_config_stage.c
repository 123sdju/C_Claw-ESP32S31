#include "app_config.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/ports/cc_secret_store.h"
#include "cc/util/cc_redaction.h"
#include "../../../cclaw/core/vendor/cjson/cJSON.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at line %d\n",__LINE__); return 1; } } while (0)

static int mutation_attempts;
static int filesystem_acquisitions;
static cc_filesystem_vtable_t guarded_vtable;
static const cc_filesystem_vtable_t *base_vtable;
static const char *fail_read_path;
static const char *fail_stat_path;
static int allow_repairs,fail_partial_write,fail_sync,fail_replace_after_install,fail_dir_sync;
static int simulate_fat_replace,fat_failure_phase;
static const char *mutate_on_sync_path;
static int allow_hot_writes;
static size_t json_allocation_count;
static size_t json_fail_at = SIZE_MAX;
static int use_fake_secrets, secret_gets, secret_destroys;

/**
 * @brief 为串行引用解析回归提供三个固定凭据。
 * @param[in] self 未使用的 fixture 接收者。
 * @param[in] reference 有效借用引用名。
 * @param[out] out_secret 有效槽，先置 NULL，成功接收须擦除并释放的字符串。
 * @return 已知引用返回 CC_OK，未知返回 NOT_FOUND，复制失败返回 OOM。
 */
static cc_result_t fixture_secret_get(void *self, const char *reference, char **out_secret)
{
    (void)self;
    *out_secret = NULL;
    secret_gets++;
    const char *value = NULL;
    if (strcmp(reference, "ref:model") == 0) value = "model-fixture";
    else if (strcmp(reference, "ref:sta") == 0) value = "sta-fixture";
    else if (strcmp(reference, "ref:ap") == 0) value = "ap-fixture";
    if (!value) return cc_result_error(CC_ERR_NOT_FOUND, "unknown fixture reference");
    *out_secret = strdup(value);
    return *out_secret ? cc_result_ok() : cc_result_error(CC_ERR_OUT_OF_MEMORY, "fixture secret");
}

/**
 * @brief 记录凭据端口被释放，fixture 无动态接收者。
 * @param[in] self 未使用的借用接收者。
 * @post secret_destroys 增加一次。
 */
static void fixture_secret_destroy(void *self)
{
    (void)self;
    secret_destroys++;
}

extern cc_result_t __real_cc_secret_store_get_default(cc_secret_store_t *out_store);

/**
 * @brief 为指定串行用例提供只读凭据端口，其他路径调用真实默认端口。
 * @param[out] out_store 有效输出槽，测试分支填充完整端口结构。
 * @return fixture 返回 CC_OK，其他分支原样返回真实获取结果。
 */
cc_result_t __wrap_cc_secret_store_get_default(cc_secret_store_t *out_store)
{
    if (!use_fake_secrets) return __real_cc_secret_store_get_default(out_store);
    static const cc_secret_store_vtable_t vtable = {
        .size = sizeof(vtable),
        .version = CC_SECRET_STORE_VTABLE_VERSION,
        .capabilities = CC_SECRET_CAP_READ,
        .get = fixture_secret_get,
        .destroy = fixture_secret_destroy
    };
    memset(out_store, 0, sizeof(*out_store));
    out_store->size = sizeof(*out_store);
    out_store->version = CC_SECRET_STORE_VTABLE_VERSION;
    out_store->capabilities = CC_SECRET_CAP_READ;
    out_store->vtable = &vtable;
    return cc_result_ok();
}

/**
 * @brief 仅在串行 fixture 中使一次指定的 cJSON 分配失败。
 * @param[in] size 本次申请字节数。
 * @return 选中的分配返回 NULL，其他返回 malloc 所有权，供 cJSON 的 free 钩子释放。
 */
static void *json_test_allocate(size_t size)
{
    size_t index = json_allocation_count++;
    return index == json_fail_at ? NULL : malloc(size);
}

static cc_result_t guarded_read_text(void *self, const char *path, char **text)
{
    if (fail_read_path && strcmp(path,fail_read_path)==0) {
        *text=NULL;
        return cc_result_error(CC_ERR_IO,"injected text read failure");
    }
    return base_vtable->read_text(self,path,text);
}

static cc_result_t guarded_read_bytes(void *self, const char *path, void **data, size_t *length)
{
    if (fail_read_path && strcmp(path,fail_read_path)==0) {
        *data=NULL; *length=0; return cc_result_error(CC_ERR_IO,"injected read failure");
    }
    return base_vtable->read_bytes(self,path,data,length);
}
static cc_result_t guarded_stat(void *self, const char *path, cc_filesystem_stat_t *info)
{
    if (fail_stat_path && strcmp(path,fail_stat_path)==0) return cc_result_error(CC_ERR_IO,"injected stat failure");
    return base_vtable->stat(self,path,info);
}

static cc_result_t reject_write(void *self, const char *path, const char *text)
{
    if (allow_hot_writes) return base_vtable->write_text(self,path,text);
    mutation_attempts++; return cc_result_error(CC_ERR_IO,"unexpected write");
}
static cc_result_t reject_bytes(void *self, const char *path, const void *data, size_t length)
{
    if (!allow_repairs) { mutation_attempts++; return cc_result_error(CC_ERR_IO,"unexpected write"); }
    if (fail_partial_write) {
        cc_result_t rc=base_vtable->write_bytes(self,path,data,length>3U ? 3U : length);
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_IO,"injected partial write");
    }
    return base_vtable->write_bytes(self,path,data,length);
}
static cc_result_t reject_path(void *self, const char *path)
{ (void)self; (void)path; mutation_attempts++; return cc_result_error(CC_ERR_IO,"unexpected mutation"); }
static cc_result_t reject_replace(void *self, const char *from, const char *to)
{
    if (!allow_repairs) { mutation_attempts++; return cc_result_error(CC_ERR_IO,"unexpected replace"); }
    if (simulate_fat_replace) {
        size_t n=strlen(to); char *backup=malloc(n+5U);
        if (!backup) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"fake FAT backup path");
        memcpy(backup,to,n); memcpy(backup+n,".bak",5U);
        int destination_exists=access(to,F_OK)==0, backup_exists=access(backup,F_OK)==0;
        if (backup_exists) {
            int result=destination_exists ? unlink(backup) : rename(backup,to);
            if (result!=0) { free(backup); return cc_result_error(CC_ERR_IO,"fake FAT backup handling"); }
            destination_exists=1;
        }
        if (fat_failure_phase==1) { free(backup); return cc_result_error(CC_ERR_IO,"fake FAT interruption"); }
        if (destination_exists && rename(to,backup)!=0) {
            free(backup); return cc_result_error(CC_ERR_IO,"fake FAT backup rename");
        }
        if (fat_failure_phase==2) { free(backup); return cc_result_error(CC_ERR_IO,"fake FAT interruption"); }
        if (rename(from,to)!=0) { free(backup); return cc_result_error(CC_ERR_IO,"fake FAT install"); }
        if (fat_failure_phase==3) { free(backup); return cc_result_error(CC_ERR_IO,"fake FAT lost acknowledgement"); }
        if (destination_exists && unlink(backup)!=0) { free(backup); return cc_result_error(CC_ERR_IO,"fake FAT cleanup"); }
        free(backup); return cc_result_ok();
    }
    cc_result_t rc=base_vtable->atomic_replace(self,from,to);
    if (rc.code==CC_OK && fail_replace_after_install) {
        cc_result_free(&rc); return cc_result_error(CC_ERR_IO,"injected lost acknowledgement");
    }
    return rc;
}
static cc_result_t guarded_sync_file(void *self,const char *path)
{
    if (fail_sync) return cc_result_error(CC_ERR_IO,"injected sync failure");
    cc_result_t rc=base_vtable->sync_file(self,path);
    if (rc.code==CC_OK && mutate_on_sync_path) {
        FILE *file=fopen(mutate_on_sync_path,"ab");
        if (file) { fputc(' ',file); fclose(file); }
    }
    return rc;
}
static cc_result_t guarded_sync_dir(void *self,const char *path)
{
    if (fail_dir_sync) return cc_result_error(CC_ERR_IO,"injected directory sync failure");
    return base_vtable->sync_dir(self,path);
}

extern cc_result_t __real_cc_filesystem_get_default(cc_filesystem_t *out);
/**
 * @brief 记录配置入口获取文件系统的次数，并安装受控故障端口。
 * @param[out] out 有效端口输出槽，结果所有权沿真实默认端口契约交给调用者。
 * @return 原样返回真实端口获取结果，成功时替换为 fixture 操作表。
 * @note filesystem_acquisitions 在每次调用增加，供提交容量检查的无 IO 断言使用。
 */
cc_result_t __wrap_cc_filesystem_get_default(cc_filesystem_t *out)
{
    filesystem_acquisitions++;
    cc_result_t rc=__real_cc_filesystem_get_default(out);
    if (rc.code==CC_OK && out->vtable) {
        base_vtable=out->vtable;
        guarded_vtable=*out->vtable;
        guarded_vtable.read_bytes=guarded_read_bytes;
        guarded_vtable.read_text=guarded_read_text;
        guarded_vtable.stat=guarded_stat;
        guarded_vtable.write_text=reject_write;
        guarded_vtable.write_bytes=reject_bytes;
        guarded_vtable.append_bytes=reject_bytes;
        guarded_vtable.make_dir=reject_path;
        guarded_vtable.remove=reject_path;
        guarded_vtable.atomic_replace=reject_replace;
        guarded_vtable.sync_file=guarded_sync_file;
        guarded_vtable.sync_dir=guarded_sync_dir;
        out->vtable=&guarded_vtable;
    }
    return rc;
}

static int put_bytes(const char *path, const void *data, size_t length)
{
    FILE *file=fopen(path,"wb");
    if (!file) return 0;
    int ok=fwrite(data,1,length,file)==length;
    return fclose(file)==0 && ok;
}
static int put_text(const char *path, const char *text)
{ return put_bytes(path,text,strlen(text)); }
static int file_equals(const char *path,const char *expected)
{
    FILE *file=fopen(path,"rb"); if (!file) return 0;
    size_t length=strlen(expected); char *data=malloc(length+1U);
    if (!data) { fclose(file); return 0; }
    size_t read=fread(data,1,length+1U,file); int good=read==length && memcmp(data,expected,length)==0;
    free(data); return fclose(file)==0 && good;
}

static int test_managed_memory_capacity_config(void)
{
    app_config_t defaults={0}, configured={0};
    CHECK(app_config_prepare(&defaults,"{}")==ESP_OK && defaults.managed_memory_capacity==100U &&
          defaults.managed_memory_pending_items==4U && defaults.managed_memory_pending_bytes==256U*1024U &&
          defaults.summary_cache_per_session==8U && defaults.summary_cache_max_entries==128U &&
          defaults.summary_cache_max_bytes==512U*1024U &&
          !defaults.runtime.active_memory_enabled && !defaults.runtime.active_memory_write_summary);
    CHECK(app_config_prepare(&configured,
          "{\"storage\":{\"managed_memory\":{\"capacity\":200,\"pending_items\":8,\"pending_bytes\":524288}}}")==ESP_OK &&
          configured.managed_memory_capacity==200U && configured.managed_memory_pending_items==8U &&
          configured.managed_memory_pending_bytes==512U*1024U &&
          app_config_requires_restart(&defaults,&configured));
    app_config_destroy(&configured);
    CHECK(app_config_prepare(&configured,
          "{\"storage\":{\"summary_cache\":{\"per_session\":2,\"max_entries\":12,\"max_bytes\":4096}}}")==ESP_OK &&
          configured.summary_cache_per_session==2U && configured.summary_cache_max_entries==12U &&
          configured.summary_cache_max_bytes==4096U && app_config_requires_restart(&defaults,&configured));
    app_config_destroy(&configured);
    CHECK(app_config_prepare(&configured,
          "{\"storage\":{\"active_memory\":{\"enabled\":true,\"write_summary\":true}}}")==ESP_OK &&
          !configured.runtime.active_memory_enabled && !configured.runtime.active_memory_write_summary);
    app_config_destroy(&configured);
    const char *invalid[]={
        "{\"storage\":{\"managed_memory\":false}}",
        "{\"storage\":{\"managed_memory\":{\"capacity\":null}}}",
        "{\"storage\":{\"managed_memory\":{\"capacity\":0}}}",
        "{\"storage\":{\"managed_memory\":{\"capacity\":1.5}}}",
        "{\"storage\":{\"managed_memory\":{\"capacity\":1025}}}",
        "{\"storage\":{\"managed_memory\":{\"capacity\":\"100\"}}}",
        "{\"storage\":{\"managed_memory\":{\"pending_items\":65}}}",
        "{\"storage\":{\"managed_memory\":{\"pending_bytes\":0}}}",
        "{\"storage\":{\"managed_memory\":{\"pending_bytes\":8388609}}}",
        "{\"storage\":{\"summary_cache\":false}}",
        "{\"storage\":{\"summary_cache\":{\"per_session\":0}}}",
        "{\"storage\":{\"summary_cache\":{\"max_entries\":4097}}}",
        "{\"storage\":{\"summary_cache\":{\"max_bytes\":16777217}}}",
        "{\"storage\":{\"summary_cache\":{\"max_bytes\":1.5}}}"
    };
    for (size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++) {
        CHECK(app_config_prepare(&configured,invalid[i])==ESP_ERR_INVALID_ARG);
        app_config_destroy(&configured);
    }
    app_config_destroy(&defaults);
    return 0;
}

/**
 * @brief 从正常 prepare 入口验证完整文本、NUL、长路径、数值预算和目标别名边界。
 * @return 全部断言通过返回零，失败返回一，成功路径释放全部配置。
 */
static int test_prepare_boundaries(void)
{
    app_config_t config = {0};
    CHECK(app_config_prepare(&config, "{\"model\":{\"name\":\"retained\"}}") == ESP_OK);
    const char *invalid[] = {
        "{} trailing",
        "{\"model\":{\"name\":\"cut\\u0000off\"}}",
        "{\"model\":{\"name\":\"bad\\u12x4\"}}",
        "{\"multimodal\":{\"limits\":{\"max_artifacts\":1e100}}}",
        "{\"multimodal\":{\"limits\":{\"max_artifact_bytes\":-1}}}",
        "{\"multimodal\":{\"limits\":{\"max_base64_bytes\":\"12\"}}}",
        "{\"storage\":{\"session_segment_bytes\":1e100}}",
        "{\"events\":{\"usb_log\":{\"max_bytes\":1e100}}}"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        CHECK(app_config_prepare(&config, invalid[i]) == ESP_ERR_INVALID_ARG);
        CHECK(config.loaded && strcmp(config.runtime.model, "retained") == 0);
        CHECK(app_config_prepare_update(&config, &config, invalid[i]) == ESP_ERR_INVALID_ARG);
    }
    CHECK(app_config_prepare(&config, app_config_json(&config)) == ESP_OK);
    CHECK(config.loaded && strcmp(config.runtime.model, "retained") == 0);
    CHECK(app_config_commit_prepared(&config, &config, "/unused-config") == ESP_ERR_INVALID_ARG);
    CHECK(config.loaded && strcmp(config.runtime.model, "retained") == 0);
    CHECK(app_config_prepare(&config, "{\"model\":{\"name\":\"literal\\\\u0000\"}}") == ESP_OK);
    CHECK(strcmp(config.runtime.model, "literal\\u0000") == 0);
    char root[321], text[700], expected[340];
    root[0] = '/';
    memset(root + 1, 'r', sizeof(root) - 2);
    root[sizeof(root) - 1] = '\0';
    snprintf(text, sizeof(text), "{\"storage\":{\"root\":\"%s\"}}", root);
    CHECK(app_config_prepare(&config, text) == ESP_OK);
    snprintf(expected, sizeof(expected), "%s/sessions", root);
    CHECK(strcmp(config.runtime.storage_path, expected) == 0);
    snprintf(expected, sizeof(expected), "%s/memory.json", root);
    CHECK(strcmp(config.runtime.memory_path, expected) == 0);
    snprintf(expected, sizeof(expected), "%s/logs", root);
    CHECK(strcmp(config.io.usb_log.dir, expected) == 0);
    app_config_destroy(&config);
    return 0;
}

/**
 * @brief 逐次拒绝 cJSON 分配，验证脱敏合并失败保留目标且任何成功都保留旧凭据。
 * @return 断言通过返回零，失败返回一；每次调用后恢复默认全局钩子再检查结果。
 * @note 本测试无工作线程，钩子仅包围单次 prepare_update，不跨用例保留。
 */
static int test_redacted_merge_allocation_failures(void)
{
    app_config_t current = {0}, prepared = {0};
    current.raw_json = strdup("{\"model\":{\"name\":\"old\",\"api_key\":\"fixture-secret\"}}");
    CHECK(current.raw_json);
    current.loaded = true;
    const char *submitted = "{\"model\":{\"name\":\"new\"}}";
    cJSON_Hooks hooks = {json_test_allocate, free};
    json_allocation_count = 0;
    json_fail_at = SIZE_MAX;
    cJSON_InitHooks(&hooks);
    esp_err_t err = app_config_prepare_update(&prepared, &current, submitted);
    size_t allocation_count = json_allocation_count;
    cJSON_InitHooks(NULL);
    CHECK(err == ESP_OK && prepared.runtime.api_key &&
          strcmp(prepared.runtime.api_key, "fixture-secret") == 0);
    app_config_destroy(&prepared);
    for (size_t i = 0; i < allocation_count; i++) {
        CHECK(app_config_prepare(&prepared, "{\"model\":{\"name\":\"retained\"}}") == ESP_OK);
        json_allocation_count = 0;
        json_fail_at = i;
        cJSON_InitHooks(&hooks);
        err = app_config_prepare_update(&prepared, &current, submitted);
        cJSON_InitHooks(NULL);
        if (err == ESP_OK) {
            CHECK(prepared.runtime.api_key && strcmp(prepared.runtime.api_key, "fixture-secret") == 0);
        } else {
            CHECK(prepared.loaded && strcmp(prepared.runtime.model, "retained") == 0);
        }
        CHECK(strcmp(current.raw_json,
              "{\"model\":{\"name\":\"old\",\"api_key\":\"fixture-secret\"}}") == 0);
        app_config_destroy(&prepared);
    }
    json_fail_at = SIZE_MAX;
    app_config_destroy(&current);
    return 0;
}

/**
 * @brief 验证单个大写凭据键可保留旧值、清空及读取占位符后的引用，并检查脱敏遍历所有成员。
 * @return 全部断言通过返回零，失败返回一；凭据 fixture 仅在同步 prepare 调用内启用。
 */
static int test_secret_case_and_reference_boundaries(void)
{
    app_config_t current = {0}, prepared = {0};
    current.raw_json = strdup("{\"model\":{\"api_key\":\"model-old\"},\"network\":{\"sta\":{\"password\":\"sta-old\"},\"ap\":{\"password\":\"ap-old\"}}}");
    CHECK(current.raw_json);
    current.loaded = true;
    const char *submitted = "{\"model\":{\"API_KEY\":\"[REDACTED]\"},\"network\":{\"STA\":{\"PASSWORD\":\"***\"},\"AP\":{\"PASSWORD\":\"[REDACTED]\"}}}";
    CHECK(app_config_prepare_update(&prepared, &current, submitted) == ESP_OK);
    CHECK(prepared.runtime.api_key && strcmp(prepared.runtime.api_key, "model-old") == 0 &&
          strcmp(prepared.network.password, "sta-old") == 0 &&
          strcmp(prepared.network.softap_password, "ap-old") == 0);
    app_config_destroy(&prepared);
    CHECK(app_config_prepare_update(&prepared, &current,
          "{\"model\":{\"API_KEY\":\"\"},\"network\":{\"STA\":{\"PASSWORD\":\"\"},\"AP\":{\"PASSWORD\":\"\"}}}") == ESP_OK);
    CHECK(!prepared.runtime.api_key && !prepared.network.password[0] && !prepared.network.softap_password[0]);
    app_config_destroy(&prepared);
    secret_gets = secret_destroys = 0;
    use_fake_secrets = 1;
    esp_err_t err = app_config_prepare(&prepared,
        "{\"model\":{\"API_KEY\":\"***\",\"API_KEY_REF\":\"ref:model\"},\"network\":{\"STA\":{\"PASSWORD\":\"***\",\"PASSWORD_REF\":\"ref:sta\"},\"AP\":{\"PASSWORD\":\"***\",\"PASSWORD_REF\":\"ref:ap\"}}}");
    use_fake_secrets = 0;
    CHECK(err == ESP_OK && secret_gets == 3 && secret_destroys == 1);
    CHECK(prepared.runtime.api_key && strcmp(prepared.runtime.api_key, "model-fixture") == 0 &&
          strcmp(prepared.network.password, "sta-fixture") == 0 &&
          strcmp(prepared.network.softap_password, "ap-fixture") == 0);
    app_config_destroy(&prepared);
    char *redacted = cc_redact_secrets(
        "{\"api_key\":\"first-secret\",\"API_KEY\":\"second-secret\",\"api_key\":\"third-secret\",\"PASSWORD\":\"fourth-secret\"}");
    CHECK(redacted && !strstr(redacted, "first-secret") && !strstr(redacted, "second-secret") &&
          !strstr(redacted, "third-secret") && !strstr(redacted, "fourth-secret"));
    free(redacted);
    app_config_destroy(&current);
    return 0;
}

/**
 * @brief 验证启动与提交共同拒绝凭据路径歧义，同时保留大写键与业务 JSON 的合法用法。
 * @param[in] primary 借用正式文件路径，用例覆盖写入其内容。
 * @param[in] temporary 借用 .tmp 路径，写入有效候选以验证歧义主文件终止选择。
 * @param[in] backup 借用 .bak 路径，写入有效候选以验证不回退旧配置。
 * @return 全部断言通过返回零，失败返回一；成功释放配置和暂存对象，文件由 main 后续覆盖及删除。
 * @note 正常 load 的目录创建仍由现有 fixture 拒绝，此用例在只读阶段断言结束后运行。
 */
static int test_ambiguous_credential_configuration(const char *primary,
                                                   const char *temporary,
                                                   const char *backup)
{
    const char *ambiguous[] = {
        "{\"model\":{\"api_key\":\"[REDACTED]\",\"api_key\":\"new-plaintext\"}}",
        "{\"model\":{\"API_KEY\":\"[REDACTED]\",\"api_key\":\"new-plaintext\"}}",
        "{\"model\":{\"api_key\":\"[REDACTED]\",\"api\\u005fkey\":\"new-plaintext\"}}",
        "{\"model\":{\"api_key_ref\":\"one\",\"api_key_ref\":\"two\"}}",
        "{\"model\":{\"api_key_ref\":\"one\",\"API_KEY_REF\":\"two\"}}",
        "{\"network\":{\"sta\":{\"password\":\"***\",\"password\":\"new-plaintext\"}}}",
        "{\"network\":{\"sta\":{\"password\":\"***\",\"PASSWORD\":\"new-plaintext\"}}}",
        "{\"network\":{\"sta\":{\"password_ref\":\"one\",\"PASSWORD_REF\":\"two\"}}}",
        "{\"network\":{\"ap\":{\"password\":\"***\",\"password\":\"new-plaintext\"}}}",
        "{\"network\":{\"ap\":{\"PASSWORD\":\"***\",\"password\":\"new-plaintext\"}}}",
        "{\"network\":{\"ap\":{\"password_ref\":\"one\",\"password_ref\":\"two\"}}}",
        "{\"network\":{\"ap\":{\"password_ref\":\"one\",\"PASSWORD_REF\":\"two\"}}}",
        "{\"model\":{\"api_key\":\"***\"},\"model\":{\"api_key\":\"new-plaintext\"}}",
        "{\"model\":{\"api_key\":\"***\"},\"MODEL\":{\"api_key\":\"new-plaintext\"}}",
        "{\"network\":{},\"network\":{\"sta\":{\"password\":\"new-plaintext\"}}}",
        "{\"network\":{},\"NETWORK\":{\"ap\":{\"password\":\"new-plaintext\"}}}",
        "{\"network\":{\"sta\":{},\"sta\":{\"password\":\"new-plaintext\"}}}",
        "{\"network\":{\"sta\":{},\"STA\":{\"password\":\"new-plaintext\"}}}",
        "{\"network\":{\"ap\":{},\"ap\":{\"password\":\"new-plaintext\"}}}",
        "{\"network\":{\"ap\":{},\"AP\":{\"password\":\"new-plaintext\"}}}"
    };
    app_config_t current = {0}, prepared = {0};
    CHECK(app_config_prepare(&current, "{\"model\":{\"name\":\"current\"}}") == ESP_OK);
    CHECK(app_config_prepare(&prepared, "{\"model\":{\"name\":\"retained\"}}") == ESP_OK);
    char *current_json = current.raw_json;
    char *prepared_json = prepared.raw_json;
    const char *fallback = "{\"model\":{\"name\":\"fallback\"}}";
    for (size_t i = 0; i < sizeof(ambiguous) / sizeof(ambiguous[0]); i++) {
        CHECK(app_config_prepare(&prepared, ambiguous[i]) == ESP_ERR_INVALID_ARG);
        CHECK(app_config_prepare_update(&prepared, &current, ambiguous[i]) == ESP_ERR_INVALID_ARG);
        CHECK(current.raw_json == current_json && prepared.raw_json == prepared_json &&
              strcmp(current.runtime.model, "current") == 0 &&
              strcmp(prepared.runtime.model, "retained") == 0);
        app_config_t invalid_current = {0};
        invalid_current.raw_json = strdup(ambiguous[i]);
        CHECK(invalid_current.raw_json);
        invalid_current.loaded = true;
        CHECK(app_config_prepare_update(&prepared, &invalid_current, "{}") == ESP_ERR_INVALID_STATE);
        CHECK(prepared.raw_json == prepared_json);
        app_config_destroy(&invalid_current);
        CHECK(put_text(primary, ambiguous[i]) && put_text(temporary, fallback) && put_text(backup, fallback));
        app_config_stage_t *stage = NULL;
        CHECK(app_config_stage_open(primary, &stage) == ESP_ERR_INVALID_ARG && !stage);
        CHECK(app_config_load(&current, primary) == ESP_ERR_INVALID_ARG);
        CHECK(current.raw_json == current_json && file_equals(primary, ambiguous[i]) &&
              file_equals(temporary, fallback) && file_equals(backup, fallback));
    }
    const char *uppercase = "{\"MODEL\":{\"name\":\"uppercase\",\"API_KEY\":\"boot-fixture\"},\"NETWORK\":{\"STA\":{\"PASSWORD\":\"sta-fixture\"},\"AP\":{\"PASSWORD\":\"ap-fixture\"}}}";
    CHECK(put_text(primary, uppercase));
    app_config_stage_t *stage = NULL;
    CHECK(app_config_stage_open(primary, &stage) == ESP_OK && app_config_stage_source(stage) == 0);
    CHECK(strcmp(app_config_stage_config(stage)->runtime.api_key, "boot-fixture") == 0);
    app_config_stage_destroy(stage);
    CHECK(app_config_load(&current, primary) == ESP_OK &&
          strcmp(current.runtime.model, "uppercase") == 0 &&
          strcmp(current.runtime.api_key, "boot-fixture") == 0 &&
          strcmp(current.network.password, "sta-fixture") == 0 &&
          strcmp(current.network.softap_password, "ap-fixture") == 0);
    app_config_destroy(&current);
    CHECK(app_config_prepare(&current, "{}") == ESP_OK);
    const char *business = "{\"model\":{\"extra_body\":{\"api_key\":\"first\",\"API_KEY\":\"second\",\"api_key\":\"third\",\"network\":{\"sta\":{},\"STA\":{}}}},\"business\":{\"model\":{},\"MODEL\":{},\"password\":\"one\",\"PASSWORD\":\"two\"}}";
    CHECK(app_config_prepare(&prepared, business) == ESP_OK && prepared.runtime.model_extra_body_json);
    CHECK(app_config_prepare_update(&prepared, &current, business) == ESP_OK);
    CHECK(put_text(primary, business));
    stage = NULL;
    CHECK(app_config_stage_open(primary, &stage) == ESP_OK && app_config_stage_source(stage) == 0);
    app_config_stage_destroy(stage);
    CHECK(app_config_load(&current, primary) == ESP_OK && current.runtime.model_extra_body_json);
    app_config_destroy(&current);
    app_config_destroy(&prepared);
    return 0;
}

/**
 * @brief 从真实 prepare、save 和 commit 入口验证规范化写入文本的 1 MiB 边界。
 * @param[in] primary 借用正式文件路径，成功的等边界提交覆盖该文件。
 * @param[in] temporary 借用临时文件路径，等边界安装前删除其他用例遗留的 .tmp。
 * @return 全部断言通过返回零，失败返回一；成功清理配置与输入文本，磁盘文件由 main 删除。
 * @note 超限断言同时核对端口获取次数、写入尝试计数以及 current/prepared 的指针与拥有状态。
 */
static int test_configuration_write_size_budget(const char *primary, const char *temporary)
{
    const size_t file_limit = 1024U * 1024U;
    const char *prefix = "{\"model\":{\"extra_body\":{\"padding\":\"";
    const char *suffix = "\"}}}";
    size_t prefix_length = strlen(prefix), suffix_length = strlen(suffix);
    size_t input_length = file_limit + 1U;
    char *text = malloc(input_length + 1U);
    CHECK(text);
    memcpy(text, prefix, prefix_length);
    memset(text + prefix_length, 'x', input_length - prefix_length - suffix_length);
    memcpy(text + input_length - suffix_length, suffix, suffix_length + 1U);
    app_config_t current = {0}, oversized = {0};
    CHECK(app_config_prepare(&current, "{\"model\":{\"name\":\"retained\"}}") == ESP_OK);
    CHECK(app_config_prepare(&oversized, text) == ESP_OK && strlen(oversized.raw_json) > file_limit);
    char *current_json = current.raw_json, *oversized_json = oversized.raw_json;
    int acquisitions_before = filesystem_acquisitions, mutations_before = mutation_attempts;
    CHECK(app_config_commit_prepared(&current, &oversized, primary) == ESP_ERR_INVALID_ARG);
    CHECK(app_config_save(&current, primary, text) == ESP_ERR_INVALID_ARG);
    CHECK(current.raw_json == current_json && oversized.raw_json == oversized_json &&
          current.loaded && oversized.loaded && !current.updates_blocked &&
          filesystem_acquisitions == acquisitions_before && mutation_attempts == mutations_before);
    size_t excess = strlen(oversized.raw_json) - file_limit;
    size_t remove_count = excess - 1U;
    memmove(text + input_length - suffix_length - remove_count,
            text + input_length - suffix_length, suffix_length + 1U);
    input_length -= remove_count;
    CHECK(app_config_prepare(&oversized, text) == ESP_OK && strlen(oversized.raw_json) == file_limit + 1U);
    oversized_json = oversized.raw_json;
    CHECK(app_config_commit_prepared(&current, &oversized, primary) == ESP_ERR_INVALID_ARG);
    CHECK(current.raw_json == current_json && oversized.raw_json == oversized_json &&
          oversized.loaded && filesystem_acquisitions == acquisitions_before &&
          mutation_attempts == mutations_before);
    memmove(text + input_length - suffix_length - 1U,
            text + input_length - suffix_length, suffix_length + 1U);
    input_length--;
    CHECK(app_config_prepare(&oversized, text) == ESP_OK && strlen(oversized.raw_json) == file_limit);
    if (access(temporary, F_OK) == 0) CHECK(unlink(temporary) == 0);
    int previous_hot_writes = allow_hot_writes;
    allow_hot_writes = 1;
    esp_err_t committed = app_config_commit_prepared(&current, &oversized, primary);
    allow_hot_writes = previous_hot_writes;
    CHECK(committed == ESP_OK && current.loaded && !oversized.loaded && !oversized.raw_json &&
          strlen(current.raw_json) == file_limit && file_equals(primary, current.raw_json));
    app_config_stage_t *stage = NULL;
    CHECK(app_config_stage_open(primary, &stage) == ESP_OK && app_config_stage_source(stage) == 0);
    app_config_stage_destroy(stage);
    cc_secret_zero(text, input_length);
    free(text);
    app_config_destroy(&current);
    app_config_destroy(&oversized);
    return 0;
}

/**
 * @brief 串行验证配置暂存、修复和提交的文件及内存失败边界。
 * @return 全部断言通过返回零，失败返回一；成功删除临时文件与目录。
 */
int main(void)
{
    CHECK(test_managed_memory_capacity_config()==0);
    CHECK(test_prepare_boundaries()==0);
    CHECK(test_redacted_merge_allocation_failures()==0);
    CHECK(test_secret_case_and_reference_boundaries()==0);
    char root[]="/tmp/cclaw-config-stage-XXXXXX";
    CHECK(mkdtemp(root));
    char primary[256], temporary[260], backup[260], workspace[256];
    snprintf(primary,sizeof(primary),"%s/config.json",root);
    snprintf(temporary,sizeof(temporary),"%s.tmp",primary);
    snprintf(backup,sizeof(backup),"%s.bak",primary);
    snprintf(workspace,sizeof(workspace),"%s/workspace",root);
    char original[512], changed[512], fallback[512];
    snprintf(original,sizeof(original),"{\"workspace\":{\"root\":\"%s\"},\"runtime\":{\"context_window_tokens\":8192},\"model\":{\"name\":\"first\",\"api_key\":\"test-key\",\"extra_body\":{\"cache_control\":null}}}",workspace);
    snprintf(changed,sizeof(changed),"{\"workspace\":{\"root\":\"%s\"},\"model\":{\"name\":\"other\",\"api_key\":\"test-key\"}}",workspace);
    snprintf(fallback,sizeof(fallback),"{\"workspace\":{\"root\":\"%s\"},\"model\":{\"name\":\"backup\"}}",workspace);
    CHECK(put_text(primary,original));

    app_config_stage_t *stage=NULL;
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && stage && app_config_stage_source(stage)==0);
    const app_config_t *staged=app_config_stage_config(stage);
    CHECK(staged && staged->loaded && strcmp(staged->runtime.model,"first")==0 &&
          strcmp(staged->runtime.api_key,"test-key")==0 && staged->runtime.model_extra_body_json &&
          strcmp(staged->runtime.model_extra_body_json,"{\"cache_control\":null}")==0);
    struct stat st;
    CHECK(stat(workspace,&st)!=0 && mutation_attempts==0 && app_config_stage_validate(stage)==ESP_OK &&
          app_config_stage_repair(stage)==ESP_OK && mutation_attempts==0);
    fail_stat_path=primary;
    CHECK(app_config_stage_validate(stage)==ESP_FAIL);
    fail_stat_path=NULL;
    CHECK(app_config_stage_validate(stage)==ESP_OK);
    CHECK(put_text(primary,changed) && app_config_stage_validate(stage)==ESP_ERR_INVALID_STATE);
    app_config_t adopted={0};
    CHECK(app_config_stage_take(stage,&adopted)==ESP_ERR_INVALID_STATE && !adopted.loaded);
    CHECK(put_text(primary,original));
    const unsigned char invalid_tmp[]={'{','\0','}'};
    /* A lower-priority candidate cannot change the selection of a valid main file. */
    CHECK(put_bytes(temporary,invalid_tmp,sizeof(invalid_tmp)) && app_config_stage_validate(stage)==ESP_OK);
    CHECK(unlink(temporary)==0 && app_config_stage_take(stage,&adopted)==ESP_OK &&
          adopted.loaded && strcmp(adopted.runtime.model,"first")==0);
    CHECK(!app_config_stage_config(stage) && app_config_stage_source(stage)==-1 &&
          app_config_stage_validate(stage)==ESP_OK);
    CHECK(app_config_stage_take(stage,&adopted)==ESP_ERR_INVALID_STATE && adopted.loaded &&
          strcmp(adopted.runtime.model,"first")==0);
    app_config_stage_destroy(stage); app_config_destroy(&adopted);
    CHECK(stat(workspace,&st)!=0 && mutation_attempts==0);

    /* Lower-priority junk is never read when the primary is usable. */
    CHECK(mkdir(backup,0700)==0);
    stage=NULL;
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==0 &&
          app_config_stage_validate(stage)==ESP_OK);
    app_config_stage_destroy(stage); CHECK(rmdir(backup)==0);

    /* An I/O error on a required candidate is not treated as a missing or
     * malformed file that permits silently using an older backup. */
    CHECK(put_text(backup,fallback));
    fail_read_path=primary; stage=NULL;
    CHECK(app_config_stage_open(primary,&stage)==ESP_FAIL && !stage);
    fail_read_path=NULL;
    /* 合法主文件解析分配失败时终止选择，不能静默切换到旧备份。 */
    cJSON_Hooks hooks = {json_test_allocate, free};
    json_allocation_count = 0;
    json_fail_at = 0;
    cJSON_InitHooks(&hooks);
    esp_err_t allocation_error = app_config_stage_open(primary, &stage);
    cJSON_InitHooks(NULL);
    json_fail_at = SIZE_MAX;
    CHECK(allocation_error == ESP_FAIL && !stage);
    CHECK(file_equals(primary, original) && file_equals(backup, fallback));
    /* 解码 NUL 的主配置被选中后语义拒绝，保留原文件并终止加载。 */
    CHECK(put_text(primary, "{\"model\":{\"name\":\"bad\\u0000name\"}}"));
    CHECK(app_config_stage_open(primary, &stage) == ESP_ERR_INVALID_ARG && !stage);
    CHECK(put_text(primary, original));
    CHECK(unlink(backup)==0);

    /* Invalid binary temporary files do not hide a valid backup or get repaired. */
    CHECK(put_text(primary,"{") && put_bytes(temporary,invalid_tmp,sizeof(invalid_tmp)) && put_text(backup,fallback));
    stage=NULL;
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==2 &&
          strcmp(app_config_stage_config(stage)->runtime.model,"backup")==0);
    const unsigned char different_tmp[]={'{','\0',']'};
    CHECK(put_bytes(temporary,different_tmp,sizeof(different_tmp)) && app_config_stage_validate(stage)==ESP_ERR_INVALID_STATE);
    CHECK(put_bytes(temporary,invalid_tmp,sizeof(invalid_tmp)) && app_config_stage_validate(stage)==ESP_OK);
    CHECK(app_config_stage_take(stage,&adopted)==ESP_OK && strcmp(adopted.runtime.model,"backup")==0);
    app_config_stage_destroy(stage); app_config_destroy(&adopted);
    CHECK(mutation_attempts==0 && stat(primary,&st)==0 && st.st_size==1 && stat(workspace,&st)!=0);

    CHECK(put_text(temporary,original));
    stage=NULL;
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==1);
    CHECK(put_text(primary,changed) && app_config_stage_validate(stage)==ESP_ERR_INVALID_STATE);
    app_config_stage_destroy(stage);

    /* A syntactically valid but semantically invalid primary must not be
     * silently replaced with a backup. */
    CHECK(put_text(primary,"{\"runtime\":{\"max_pending_operations\":0}}"));
    stage=NULL;
    CHECK(app_config_stage_open(primary,&stage)==ESP_ERR_INVALID_ARG && !stage && mutation_attempts==0);
    /* The selected config must be a complete JSON document. */
    CHECK(put_text(primary,"{} trailing") && put_text(temporary,original));
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==1);
    app_config_stage_destroy(stage); stage=NULL;
    FILE *large=fopen(primary,"wb"); CHECK(large && fseek(large,1024L*1024L,SEEK_SET)==0 && fputc('x',large)!=EOF);
    CHECK(fclose(large)==0 && app_config_stage_open(primary,&stage)==ESP_ERR_INVALID_STATE && !stage);
    /* A bound owner may repair an exact selected backup without changing the
     * parsed configuration or creating the workspace. */
    CHECK(put_text(primary,"{") && put_text(temporary,"broken") && put_text(backup,fallback));
    stage=NULL; CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==2);
    allow_repairs=1;
    CHECK(app_config_stage_repair(stage)==ESP_OK && app_config_stage_source(stage)==0 &&
          app_config_stage_validate(stage)==ESP_OK && file_equals(primary,fallback) && stat(workspace,&st)!=0);
    app_config_stage_destroy(stage); stage=NULL;

    CHECK(test_ambiguous_credential_configuration(primary, temporary, backup) == 0);
    CHECK(test_configuration_write_size_budget(primary, temporary) == 0);

    /* 公开 load 必须等恢复安装成功；同步失败保留原内存快照和恢复副本。 */
    app_config_t load_target = {0};
    CHECK(app_config_prepare(&load_target, "{\"model\":{\"name\":\"retained\"}}") == ESP_OK);
    CHECK(put_text(primary, "{") && put_text(temporary, "broken") && put_text(backup, fallback));
    fail_sync = 1;
    esp_err_t load_error = app_config_load(&load_target, primary);
    fail_sync = 0;
    CHECK(load_error == ESP_FAIL && load_target.loaded &&
          strcmp(load_target.runtime.model, "retained") == 0 && file_equals(backup, fallback));
    CHECK(app_config_load(&load_target, primary) == ESP_OK &&
          strcmp(load_target.runtime.model, "backup") == 0 && file_equals(primary, fallback));
    app_config_destroy(&load_target);

    /* Partial write keeps the backup; a failed stage cannot be reused, but a
     * fresh stage can recover after the storage problem is resolved. */
    CHECK(put_text(primary,"{") && put_text(temporary,"broken") && put_text(backup,fallback));
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==2);
    fail_partial_write=1; CHECK(app_config_stage_repair(stage)==ESP_FAIL); fail_partial_write=0;
    CHECK(app_config_stage_validate(stage)==ESP_ERR_INVALID_STATE && !app_config_stage_config(stage) &&
          file_equals(backup,fallback) && file_equals(primary,"{"));
    app_config_stage_destroy(stage); stage=NULL;
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==2 &&
          app_config_stage_repair(stage)==ESP_OK && file_equals(primary,fallback));
    app_config_stage_destroy(stage); stage=NULL;

    /* A sync failure retains a valid temporary and backup; the new stage
     * selects the temporary and promotes it after sync works again. */
    CHECK(put_text(primary,"{") && put_text(temporary,"broken") && put_text(backup,fallback));
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==2);
    fail_sync=1; CHECK(app_config_stage_repair(stage)==ESP_FAIL); fail_sync=0;
    CHECK(app_config_stage_validate(stage)==ESP_ERR_INVALID_STATE && file_equals(backup,fallback) &&
          file_equals(temporary,fallback));
    app_config_stage_destroy(stage); stage=NULL;
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==1 &&
          app_config_stage_repair(stage)==ESP_OK && file_equals(primary,fallback));
    app_config_stage_destroy(stage); stage=NULL;

    /* An external change during sync is detected before replacement. */
    CHECK(put_text(primary,"{") && put_text(temporary,original) && put_text(backup,fallback));
    CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==1);
    mutate_on_sync_path=primary; CHECK(app_config_stage_repair(stage)==ESP_ERR_INVALID_STATE); mutate_on_sync_path=NULL;
    CHECK(app_config_stage_validate(stage)==ESP_ERR_INVALID_STATE && file_equals(temporary,original) &&
          file_equals(backup,fallback));
    app_config_stage_destroy(stage); stage=NULL;

    /* Lost acknowledgements and directory-sync failures may follow installation;
     * neither is reported as success or reusable without reopening. */
    for (int sync_failure=0;sync_failure<=1;sync_failure++) {
        CHECK(put_text(primary,"{") && put_text(temporary,original) && put_text(backup,fallback));
        CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==1);
        if (sync_failure) fail_dir_sync=1; else fail_replace_after_install=1;
        esp_err_t repair=app_config_stage_repair(stage);
        fail_dir_sync=0; fail_replace_after_install=0;
        CHECK(repair==(sync_failure ? ESP_ERR_NOT_FINISHED : ESP_FAIL) &&
              app_config_stage_validate(stage)==ESP_ERR_INVALID_STATE && file_equals(primary,original));
        app_config_stage_destroy(stage); stage=NULL;
        CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==0 &&
              app_config_stage_repair(stage)==ESP_OK);
        app_config_stage_destroy(stage); stage=NULL;
    }

    /* Model FAT's backup removal, destination-to-backup rename and install as
     * separate interruption points. A fresh stage must still find the valid
     * selected config, although this simulation cannot prove real flash order. */
    simulate_fat_replace=1;
    for (int phase=1;phase<=3;phase++) {
        CHECK(put_text(primary,"{") && put_text(temporary,"broken") && put_text(backup,fallback));
        CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==2);
        fat_failure_phase=phase; CHECK(app_config_stage_repair(stage)==ESP_FAIL); fat_failure_phase=0;
        CHECK(app_config_stage_validate(stage)==ESP_ERR_INVALID_STATE);
        app_config_stage_destroy(stage); stage=NULL;
        CHECK(app_config_stage_open(primary,&stage)==ESP_OK && app_config_stage_source(stage)==(phase==3 ? 0 : 1) &&
              strcmp(app_config_stage_config(stage)->runtime.model,"backup")==0);
        app_config_stage_destroy(stage); stage=NULL;
    }
    simulate_fat_replace=0;
    allow_repairs=0;
    CHECK(unlink(primary)==0 && unlink(backup)==0);
    allow_hot_writes=1; allow_repairs=1;
    for (int unreadable=0;unreadable<=1;unreadable++) {
        app_config_t current={0},prepared={0},next={0};
        CHECK(put_text(primary,original));
        CHECK(app_config_load(&current,primary)==ESP_OK && app_config_prepare(&prepared,fallback)==ESP_OK);
        char *visible=cc_redact_secrets(app_config_json(&current));
        CHECK(visible && app_config_prepare_update(&next,&current,visible)==ESP_OK &&
              next.runtime.context_window_tokens==8192 && strcmp(next.runtime.api_key,"test-key")==0);
        free(visible); app_config_destroy(&next);
        char *expected=strdup(app_config_json(&prepared)); CHECK(expected);
        fail_replace_after_install=1;
        if (unreadable) fail_read_path=primary;
        esp_err_t commit=app_config_commit_prepared(&current,&prepared,primary);
        fail_replace_after_install=0; fail_read_path=NULL;
        CHECK(commit==(unreadable ? ESP_FAIL : ESP_ERR_NOT_FINISHED) && current.updates_blocked);
        CHECK(file_equals(primary,expected));
        CHECK(strcmp(current.runtime.model,unreadable ? "first" : "backup")==0);
        CHECK(app_config_prepare(&next,fallback)==ESP_OK &&
              app_config_commit_prepared(&current,&next,primary)==ESP_ERR_INVALID_STATE);
        CHECK(file_equals(primary,expected));
        app_config_destroy(&next); app_config_destroy(&prepared); app_config_destroy(&current);
        CHECK(app_config_load(&current,primary)==ESP_OK && !current.updates_blocked &&
              strcmp(current.runtime.model,"backup")==0);
        app_config_destroy(&current);
        free(expected);
    }
    CHECK(unlink(primary)==0 && rmdir(root)==0);
    return 0;
}
