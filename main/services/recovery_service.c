#include "recovery_service.h"
#include "usb_storage_service.h"
#include "cc/adapters/cc_json_segmented_history.h"
#include "cc/adapters/cc_json_memory_store.h"
#include "cc/app/cc_runtime_controller.h"
#include <stdlib.h>
#include <string.h>

#define RECOVERY_OPERATION_DIR APP_RECOVERY_ROOT "/operations"
#define RECOVERY_CHECKPOINT_DIR APP_RECOVERY_ROOT "/runs"
#ifndef APP_RECOVERY_MEDIA_ROOT
#define APP_RECOVERY_MEDIA_ROOT USB_STORAGE_MOUNT_PATH
#endif

struct recovery_service {
    cc_runtime_builder_t *builder; /* 借用，附加过的 builder 须先于服务销毁。 */
    cc_filesystem_t filesystem;    /* 服务取得并释放的介质文件系统句柄。 */
    cc_recovery_bootstrap_t *bootstrap; /* 服务拥有，完整附加后 builder 持续借用。 */
    recovery_service_state_t state; /* owner 串行维护的装配阶段。 */
    bool journal_damaged;         /* 受限模式由操作账本损坏触发，供诊断区分来源。 */
    size_t max_pending;           /* 从配置复制的普通操作准入条数阈值。 */
    uint64_t mount_generation;    /* 创建时捕获的非零挂载代数。 */
    app_config_stage_t *startup_stage; /* 启动期间借用，成功 release 或销毁时结束。 */
    bool staged_config_repaired;  /* 所选配置修复已完成。 */
    bool staged_workspace_prepared; /* 工作区及 lua 目录准备已完成。 */
    char *workspace_path;         /* 拥有的实际运行时路径副本。 */
    char *session_path;           /* 拥有的分段会话根路径副本。 */
    char *memory_path;            /* 拥有的记忆文件路径副本。 */
};

/**
 * @brief 为已校验路径分配包含 NUL 的独立副本。
 * @param[in] path 必填的同步借用字符串。
 * @return 拥有的路径副本，调用者 free，分配失败返回 NULL。
 */
static char *copy_path(const char *path)
{
    size_t length=strlen(path)+1U;
    char *copy=malloc(length);
    if (copy) memcpy(copy,path,length);
    return copy;
}

/**
 * @brief 查询当前挂载代数并与创建时快照比较。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 服务存在、介质已挂载且代数相同为真，其余为假。
 */
static bool same_mount(const recovery_service_t *service)
{
    uint64_t current=0;
    return service && usb_storage_service_current_generation(&current) && current==service->mount_generation;
}

/**
 * @brief 校验分段后端、介质根绑定及工作区/会话/记忆/日志的 FAT 路径隔离。
 * @param[in] config 必填的借用运行配置，调用方已检查指针。
 * @param[in] io 必填的借用 I/O 配置，提供日志目录。
 * @return 成功为 CC_OK，后端不支持为 CC_ERR_UNSUPPORTED，越出介质根为 CC_ERR_PERMISSION_DENIED，路径校验错误原样返回。
 */
static cc_result_t validate_paths(const cc_config_t *config, const app_runtime_io_config_t *io)
{
    if (!config->storage_type || strcmp(config->storage_type, "json_segmented") != 0)
        return cc_result_error(CC_ERR_UNSUPPORTED, "Recovery requires the json_segmented session backend");
    const char *bound_paths[]={APP_RECOVERY_ROOT,config->data_dir,config->storage_path,config->memory_path,io->usb_log.dir};
    const size_t root_len=strlen(APP_RECOVERY_MEDIA_ROOT);
    for (size_t i=0;i<sizeof(bound_paths)/sizeof(bound_paths[0]);i++) {
        const char *path=bound_paths[i];
        if (!path || strncmp(path,APP_RECOVERY_MEDIA_ROOT,root_len)!=0 ||
            (path[root_len]!='/' && path[root_len]!='\0'))
            return cc_result_error(CC_ERR_PERMISSION_DENIED,"Recovery state must be on the bound USB mount");
        cc_result_t rc=cc_storage_binding_validate_fat_path(path);
        if (rc.code!=CC_OK) return rc;
    }
    const char *paths[] = {config->workspace_path, config->storage_path, config->memory_path, io->usb_log.dir};
    for (size_t i=0; i<sizeof(paths)/sizeof(paths[0]); i++) {
        if (!paths[i] || !paths[i][0])
            return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Recovery storage paths must be explicit");
        cc_result_t rc=cc_storage_binding_validate_fat_paths(paths[i], APP_RECOVERY_ROOT);
        if (rc.code != CC_OK) return rc;
    }
    /* 工作区、会话、记忆及日志各有独立所有者，路径重叠会破坏扫描及工具写入边界。 */
    for (size_t i=0; i<sizeof(paths)/sizeof(paths[0]); i++) {
        for (size_t j=i+1U; j<sizeof(paths)/sizeof(paths[0]); j++) {
            cc_result_t rc=cc_storage_binding_validate_fat_paths(paths[i],paths[j]);
            if (rc.code != CC_OK) return rc;
        }
    }
    return cc_result_ok();
}

/**
 * @brief 在配置应用前检查指针、正的未决阈值及全部存储路径。
 * @param[in] config 调用期间借用的运行配置。
 * @param[in] io 调用期间借用的产品 I/O 配置，max_pending_operations 须大于零。
 * @return 参数非法为 CC_ERR_INVALID_ARGUMENT，其余返回后端及路径校验结果。
 */
cc_result_t recovery_service_validate_configuration(const cc_config_t *config,
                                                     const app_runtime_io_config_t *io)
{
    if (!config || !io || io->max_pending_operations<=0)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid recovery service configuration");
    return validate_paths(config,io);
}

/**
 * @brief 打开恢复 bundle，仅明确历史或账本损坏时转为全部工具禁用的纯对话模式。
 * @pre owner 排空输入和手动工具入口，service 已借用新 builder 且原挂载有效。
 * @param[in,out] service 持有文件系统及 builder 的服务，成功附加后拥有 bootstrap。
 * @return 完整附加或当前介质上的明确损坏降级为 CC_OK，普通错误原样返回并置 FAILED，附加后挂载变化可返回成功但状态为 FAILED。
 */
static cc_result_t attach_recovery(recovery_service_t *service)
{
    size_t imported=0;
    cc_recovery_bootstrap_failure_t failure=CC_RECOVERY_BOOTSTRAP_FAILURE_OTHER;
    cc_result_t rc=cc_recovery_bootstrap_open_detailed(service->builder, &service->filesystem,
        RECOVERY_OPERATION_DIR, RECOVERY_CHECKPOINT_DIR, service->max_pending,
        &service->bootstrap, &imported, &failure);
    /* 已附加的 bundle 持续由 builder 借用，换盘后仍保留所有者至 builder 销毁。 */
    if (rc.code == CC_OK)
        service->state=same_mount(service) ? RECOVERY_SERVICE_READY : RECOVERY_SERVICE_FAILED;
    else if ((failure==CC_RECOVERY_BOOTSTRAP_FAILURE_HISTORY_INTEGRITY ||
              failure==CC_RECOVERY_BOOTSTRAP_FAILURE_JOURNAL_INTEGRITY) && same_mount(service)) {
        /* 保留损坏原文件，所需 bundle 保持未附加，runtime 的模型和手动工具入口均关闭。 */
        service->state=RECOVERY_SERVICE_HISTORY_DAMAGED;
        service->journal_damaged=failure==CC_RECOVERY_BOOTSTRAP_FAILURE_JOURNAL_INTEGRITY;
        cc_result_free(&rc);
        rc=cc_result_ok();
    }
    else service->state=RECOVERY_SERVICE_FAILED;
    return rc;
}

/**
 * @brief 检查恢复目录及既有会话，已有会话缺少恢复目录时保留现场供受限装配。
 * @pre owner 独占恢复目录创建并排除卸载，服务持有原挂载及可选配置暂存。
 * @param[in,out] service 借用的准备中服务，新盘路径逐步创建并复查挂载/暂存。
 * @param[out] out_history_damaged 必填，先置假，已有会话且缺少目录时置真，此时跳过创建。
 * @return 成功为 CC_OK，端口不足为 CC_ERR_UNSUPPORTED，目录类型或挂载变化为 CC_ERR_STORAGE，文件系统/暂存校验错误向上传递，失败可能已有部分目录创建。
 */
static cc_result_t ensure_recovery_directories(recovery_service_t *service, bool *out_history_damaged)
{
    *out_history_damaged=false;
    const char *paths[]={APP_RECOVERY_ROOT,RECOVERY_OPERATION_DIR,RECOVERY_CHECKPOINT_DIR};
    if (service->filesystem.vtable->size<offsetof(cc_filesystem_vtable_t,stat)+sizeof(service->filesystem.vtable->stat) ||
        !service->filesystem.vtable->stat || !service->filesystem.vtable->list_dir)
        return cc_result_error(CC_ERR_UNSUPPORTED,"Recovery requires directory inspection");
    bool missing=false;
    for (size_t i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        cc_filesystem_stat_t info={.size=sizeof(info)};
        cc_result_t rc=service->filesystem.vtable->stat(service->filesystem.self,paths[i],&info);
        if (rc.code!=CC_OK) return rc;
        cc_result_free(&rc);
        if (!same_mount(service)) return cc_result_error(CC_ERR_STORAGE,"USB changed during recovery directory inspection");
        if (info.type==CC_FS_ENTRY_MISSING) missing=true;
        else if (info.type!=CC_FS_ENTRY_DIRECTORY)
            return cc_result_error(CC_ERR_STORAGE,"Recovery directory path is not a directory");
    }
    if (missing) {
        char **names=NULL; size_t count=0;
        cc_result_t rc=service->filesystem.vtable->list_dir(service->filesystem.self,service->session_path,&names,&count);
        if (rc.code!=CC_OK) return rc;
        for (size_t i=0;i<count;i++) free(names[i]);
        free(names);
        if (!same_mount(service)) return cc_result_error(CC_ERR_STORAGE,"USB changed while inspecting session history");
        /* 既有会话却缺少恢复目录可能丢失未决操作，保留证据并交由外层关闭全部工具。 */
        if (count) { *out_history_damaged=true; return cc_result_ok(); }
    }
    for (size_t i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        if (!same_mount(service)) {
            service->state=RECOVERY_SERVICE_FAILED;
            return cc_result_error(CC_ERR_STORAGE,"USB changed before recovery directory setup");
        }
        cc_result_t rc=service->filesystem.vtable->make_dir(service->filesystem.self,paths[i]);
        if (service->startup_stage) {
            cc_result_t checked=recovery_service_validate_staged(service);
            if (checked.code!=CC_OK) { cc_result_free(&rc); return checked; }
            cc_result_free(&checked);
        } else if (!same_mount(service)) {
            cc_result_free(&rc);
            service->state=RECOVERY_SERVICE_FAILED;
            return cc_result_error(CC_ERR_STORAGE,"USB changed during recovery directory setup");
        }
        if (rc.code!=CC_OK) return rc;
        cc_result_free(&rc);
    }
    return cc_result_ok();
}

/**
 * @brief 校验配置和挂载，复制路径并取得根范围且无符号链接的文件系统句柄。
 * @param[in] config 同步借用的运行配置，复制工作区、会话及记忆路径。
 * @param[in] io 同步借用的 I/O 配置，复制普通操作阈值。
 * @param[out] out_failure 可选，先置 OTHER，准备成功时置 NONE。
 * @param[out] out_service 必填，先置 NULL，成功取得拥有的 READY_TO_ATTACH 服务。
 * @return 成功为 CC_OK，输出非法为 CC_ERR_INVALID_ARGUMENT，未挂载/换盘为 CC_ERR_STORAGE，分配为 CC_ERR_OUT_OF_MEMORY，其余校验及端口错误向上传递，失败释放本次资源。
 */
cc_result_t recovery_service_prepare_detailed(const cc_config_t *config, const app_runtime_io_config_t *io,
    recovery_preflight_failure_t *out_failure, recovery_service_t **out_service)
{
    if (out_failure) *out_failure=RECOVERY_PREFLIGHT_OTHER;
    if (!out_service)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recovery service output");
    *out_service=NULL;
    cc_result_t rc=recovery_service_validate_configuration(config,io);
    if (rc.code != CC_OK) return rc;
    uint64_t mount_generation=0;
    if (!usb_storage_service_current_generation(&mount_generation))
        return cc_result_error(CC_ERR_STORAGE, "USB storage is not mounted");
    recovery_service_t *service=calloc(1,sizeof(*service));
    if (!service) return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate recovery service");
    service->workspace_path=copy_path(config->workspace_path);
    service->session_path=copy_path(config->storage_path);
    service->memory_path=copy_path(config->memory_path);
    if (!service->workspace_path || !service->session_path || !service->memory_path) {
        recovery_service_destroy(service);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot retain recovery storage paths");
    }
    service->max_pending=(size_t)io->max_pending_operations;
    service->mount_generation=mount_generation;
    rc=cc_filesystem_get_default(&service->filesystem);
    if (rc.code == CC_OK && (!service->filesystem.vtable || !service->filesystem.vtable->make_dir ||
        (service->filesystem.capabilities & (CC_FS_CAP_NO_SYMLINKS|CC_FS_CAP_ROOT_SCOPED)) !=
            (CC_FS_CAP_NO_SYMLINKS|CC_FS_CAP_ROOT_SCOPED)))
        rc=cc_result_error(CC_ERR_UNSUPPORTED, "Recovery requires root-scoped storage without symlinks");
    if (rc.code == CC_OK && !same_mount(service))
        rc=cc_result_error(CC_ERR_STORAGE, "USB storage changed during recovery setup");
    if (rc.code == CC_OK) service->state=RECOVERY_SERVICE_READY_TO_ATTACH;
    if (rc.code != CC_OK) { recovery_service_destroy(service); return rc; }
    if (out_failure) *out_failure=RECOVERY_PREFLIGHT_NONE;
    *out_service=service;
    return cc_result_ok();
}

/**
 * @brief 使用详细准备入口构造服务并省略失败分类。
 * @param[in] config 同步借用的运行配置。
 * @param[in] io 同步借用的 I/O 配置。
 * @param[out] out_service 必填，先置 NULL，成功取得拥有的准备阶段服务。
 * @return 原样返回详细准备结果，失败清理本次分配。
 */
cc_result_t recovery_service_prepare(const cc_config_t *config, const app_runtime_io_config_t *io,
                                     recovery_service_t **out_service)
{
    return recovery_service_prepare_detailed(config,io,NULL,out_service);
}

/**
 * @brief 在配置暂存内容校验前后核对预期挂载代数。
 * @pre owner 排除卸载及其他配置写入者。
 * @param[in] stage 同步借用的只读配置暂存，须非 NULL。
 * @param[in] expected_generation owner 捕获的非零挂载代数。
 * @return 全部核对通过为 CC_OK，参数缺失、挂载或配置变化统一为 CC_ERR_STORAGE。
 */
static cc_result_t validate_config_stage(app_config_stage_t *stage, uint64_t expected_generation)
{
    uint64_t before=0, after=0;
    if (!stage || !expected_generation || !usb_storage_service_current_generation(&before) ||
        before!=expected_generation)
        return cc_result_error(CC_ERR_STORAGE,"USB changed before staged config verification");
    esp_err_t err=app_config_stage_validate(stage);
    if (err!=ESP_OK || !usb_storage_service_current_generation(&after) || after!=expected_generation)
        return cc_result_error(CC_ERR_STORAGE,"Staged config or USB changed during startup");
    return cc_result_ok();
}

/**
 * @brief 从有效的配置暂存准备服务，并核对创建前后的挂载及暂存。
 * @param[in] stage 借用至 release_staged 成功或服务销毁的暂存，拥有者保证持续有效。
 * @param[in] mount_generation owner 捕获的非零挂载代数。
 * @param[out] out_failure 可选，先置 OTHER，成功为 NONE，末次核对失败恢复为 OTHER。
 * @param[out] out_service 必填，参数有效后置 NULL，成功接收拥有服务，失败清理。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，暂存不可用为 CC_ERR_INVALID_STATE，准备及暂存/挂载错误向上传递。
 */
cc_result_t recovery_service_prepare_staged(app_config_stage_t *stage, uint64_t mount_generation,
    recovery_preflight_failure_t *out_failure, recovery_service_t **out_service)
{
    if (out_failure) *out_failure=RECOVERY_PREFLIGHT_OTHER;
    if (!stage || !out_service)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid staged recovery request");
    *out_service=NULL;
    const app_config_t *config=app_config_stage_config(stage);
    if (!config) return cc_result_error(CC_ERR_INVALID_STATE,"Staged config is unavailable");
    cc_result_t rc=validate_config_stage(stage,mount_generation);
    if (rc.code!=CC_OK) return rc;
    rc=recovery_service_prepare_detailed(app_config_runtime(config),app_config_io(config),out_failure,out_service);
    if (rc.code!=CC_OK) return rc;
    if ((*out_service)->mount_generation!=mount_generation)
        rc=cc_result_error(CC_ERR_STORAGE,"USB changed during staged recovery setup");
    else rc=validate_config_stage(stage,mount_generation);
    if (rc.code!=CC_OK) {
        if (out_failure) *out_failure=RECOVERY_PREFLIGHT_OTHER;
        recovery_service_destroy(*out_service);
        *out_service=NULL;
        return rc;
    }
    (*out_service)->startup_stage=stage;
    return cc_result_ok();
}

/**
 * @brief 查询服务是否仍借用配置暂存。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 非空服务仍持有 stage 为真，否则为假。
 */
bool recovery_service_has_staged_config(const recovery_service_t *service)
{ return service && service->startup_stage; }

/**
 * @brief 比较所借用配置暂存的对象身份。
 * @param[in] service 借用的服务，允许 NULL。
 * @param[in] stage 待比较的暂存指针，允许 NULL。
 * @return 两个参数存在且指针相同为真，实时内容及挂载另行验证。
 */
bool recovery_service_uses_staged_config(const recovery_service_t *service, const app_config_stage_t *stage)
{ return service && stage && service->startup_stage==stage; }

/**
 * @brief 校验借用暂存及原挂载，发现变化后将服务锁到 FAILED。
 * @param[in,out] service 须仍持有 stage 的借用服务，owner 串行调用。
 * @return 无暂存或已失败为 CC_ERR_INVALID_STATE，其余返回暂存/挂载校验结果，失败后须重新装配。
 */
cc_result_t recovery_service_validate_staged(recovery_service_t *service)
{
    if (!service || !service->startup_stage)
        return cc_result_error(CC_ERR_INVALID_STATE,"Recovery has no staged config");
    if (service->state==RECOVERY_SERVICE_FAILED)
        return cc_result_error(CC_ERR_INVALID_STATE,"Staged recovery has already failed");
    cc_result_t rc=validate_config_stage(service->startup_stage,service->mount_generation);
    if (rc.code!=CC_OK) service->state=RECOVERY_SERVICE_FAILED;
    return rc;
}

/**
 * @brief 外层配置交接失败时锁定仍借用 stage 的服务。
 * @param[in,out] service owner 串行操作的服务，允许 NULL。
 * @post 有暂存借用的服务被置为 FAILED，其余保持原状态。
 */
void recovery_service_invalidate_staged(recovery_service_t *service)
{
    if (service && service->startup_stage) service->state=RECOVERY_SERVICE_FAILED;
}

/**
 * @brief 读取暂存配置修复的完成标志。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 服务、暂存均存在且已修复为真，实际配置和挂载有效性另行核对。
 */
bool recovery_service_staged_config_ready(const recovery_service_t *service)
{ return service && service->startup_stage && service->staged_config_repaired; }

/**
 * @brief 清除准备标志，核对暂存并修复所选配置候选，末次核对通过后记录修复完成。
 * @param[in,out] service 借用的 READY_TO_ATTACH 服务，须持暂存且尚无 bootstrap，owner 串行维护。
 * @return 阶段非法为 CC_ERR_INVALID_STATE，修复分配失败为 CC_ERR_OUT_OF_MEMORY，能力不足为 CC_ERR_UNSUPPORTED，其余修复错误为 CC_ERR_STORAGE，失败锁定装配，成功为 CC_OK。
 */
cc_result_t recovery_service_repair_staged_config(recovery_service_t *service)
{
    if (!service || !service->startup_stage || service->state!=RECOVERY_SERVICE_READY_TO_ATTACH ||
        service->bootstrap)
        return cc_result_error(CC_ERR_INVALID_STATE,"Staged configuration is not ready to repair");
    service->staged_config_repaired=false;
    service->staged_workspace_prepared=false;
    cc_result_t rc=recovery_service_validate_staged(service);
    if (rc.code!=CC_OK) return rc;
    cc_result_free(&rc);
    esp_err_t err=app_config_stage_repair(service->startup_stage);
    if (err!=ESP_OK) {
        service->state=RECOVERY_SERVICE_FAILED;
        return cc_result_error(err==ESP_ERR_NO_MEM ? CC_ERR_OUT_OF_MEMORY :
            err==ESP_ERR_NOT_SUPPORTED ? CC_ERR_UNSUPPORTED : CC_ERR_STORAGE,
            "Staged configuration repair failed; reopen and inspect recovery files");
    }
    rc=recovery_service_validate_staged(service);
    if (rc.code==CC_OK) service->staged_config_repaired=true;
    return rc;
}

/**
 * @brief 创建工作区和 lua 目录，每次创建及 stat 后重新核对暂存。
 * @param[in,out] service 借用的 READY_TO_ATTACH 服务，配置已修复且仍持有暂存，owner 排除其他写入者。
 * @return 全部目录有效为 CC_OK，状态非法为 CC_ERR_INVALID_STATE，路径溢出为 CC_ERR_LIMIT_EXCEEDED，分配失败为 CC_ERR_OUT_OF_MEMORY，端口及暂存错误向上传递，失败可能已创建部分目录。
 * @note 普通文件系统错误可在原挂载上重试，暂存或挂载变化锁定失败，临时路径在本次释放。
 */
cc_result_t recovery_service_prepare_staged_workspace(recovery_service_t *service)
{
    if (!service || !service->startup_stage || !service->staged_config_repaired ||
        service->state!=RECOVERY_SERVICE_READY_TO_ATTACH ||
        service->bootstrap || !service->filesystem.vtable || !service->filesystem.vtable->make_dir ||
        service->filesystem.vtable->size < offsetof(cc_filesystem_vtable_t,stat) + sizeof(service->filesystem.vtable->stat) ||
        !service->filesystem.vtable->stat)
        return cc_result_error(CC_ERR_INVALID_STATE,"Staged workspace is not ready to prepare");
    service->staged_workspace_prepared=false;
    cc_result_t rc=recovery_service_validate_staged(service);
    if (rc.code!=CC_OK) return rc;
    size_t length=strlen(service->workspace_path);
    size_t separator=length && service->workspace_path[length-1]!='/' ? 1U : 0U;
    if (length>SIZE_MAX-separator-4U)
        return cc_result_error(CC_ERR_LIMIT_EXCEEDED,"Workspace path is too long");
    char *lua=malloc(length+separator+4U);
    if (!lua) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot construct workspace path");
    memcpy(lua,service->workspace_path,length);
    if (separator) lua[length++]='/';
    memcpy(lua+length,"lua",4U);
    const char *paths[]={service->workspace_path,lua};
    for (size_t i=0;i<sizeof(paths)/sizeof(paths[0]);i++) {
        rc=service->filesystem.vtable->make_dir(service->filesystem.self,paths[i]);
        /* mkdir 返回错误也可能遇到换盘，先核对暂存及挂载再决定返回结果。 */
        cc_result_t check=recovery_service_validate_staged(service);
        if (check.code!=CC_OK) { cc_result_free(&rc); rc=check; break; }
        cc_result_free(&check);
        if (rc.code!=CC_OK) break;
        cc_result_free(&rc);
        cc_filesystem_stat_t stat={.size=sizeof(stat)};
        rc=service->filesystem.vtable->stat(service->filesystem.self,paths[i],&stat);
        check=recovery_service_validate_staged(service);
        if (check.code!=CC_OK) { cc_result_free(&rc); rc=check; break; }
        cc_result_free(&check);
        if (rc.code!=CC_OK) break;
        if (stat.type!=CC_FS_ENTRY_DIRECTORY) {
            cc_result_free(&rc);
            rc=cc_result_error(CC_ERR_STORAGE,"Workspace path is not a directory");
            break;
        }
        cc_result_free(&rc);
        rc=cc_result_ok();
    }
    free(lua);
    if (rc.code==CC_OK) service->staged_workspace_prepared=true;
    return rc;
}

/**
 * @brief 在完整或受限装配结束时最后核对暂存，再解除其借用关系。
 * @param[in,out] service 借用的 READY 或 HISTORY_DAMAGED 服务，须仍持有暂存。
 * @return 成功为 CC_OK 并清空 stage，状态非法为 CC_ERR_INVALID_STATE，校验失败保留借用且锁定失败，暂存仍归外层所有。
 */
cc_result_t recovery_service_release_staged(recovery_service_t *service)
{
    if (!service || !service->startup_stage ||
        (service->state!=RECOVERY_SERVICE_READY && service->state!=RECOVERY_SERVICE_HISTORY_DAMAGED))
        return cc_result_error(CC_ERR_INVALID_STATE,"Staged config is still required by recovery");
    cc_result_t rc=recovery_service_validate_staged(service);
    if (rc.code==CC_OK) service->startup_stage=NULL;
    return rc;
}

/**
 * @brief 核对实际运行时路径和介质，要求恢复并装配完整 bundle 或明确损坏的受限模式。
 * @pre owner 排空所有输入及手动工具入口，暂存服务已修复配置并准备工作区。
 * @param[in,out] service 借用的 READY_TO_ATTACH 服务，成功后持有恢复资源或损坏原因。
 * @param[in,out] builder 服务将持续借用的 builder，其实际工作区、会话及记忆路径须与准备配置一致。
 * @return 成功或明确损坏降级为 CC_OK，状态非法为 CC_ERR_INVALID_STATE，挂载变化为 CC_ERR_STORAGE，路径/存储等错误向上传递，成功后仍须核对 is_current，已附加资源保留至 builder 销毁。
 */
cc_result_t recovery_service_attach(recovery_service_t *service, cc_runtime_builder_t *builder)
{
    if (!service || !builder || service->state != RECOVERY_SERVICE_READY_TO_ATTACH || service->bootstrap ||
        (service->builder && service->builder != builder))
        return cc_result_error(CC_ERR_INVALID_STATE, "Recovery is not ready to attach");
    if (service->startup_stage && (!service->staged_config_repaired || !service->staged_workspace_prepared))
        return cc_result_error(CC_ERR_INVALID_STATE, "Staged configuration or workspace has not been prepared");
    if (service->startup_stage) {
        cc_result_t checked=recovery_service_validate_staged(service);
        if (checked.code!=CC_OK) return checked;
    }
    if (!same_mount(service)) {
        service->state=RECOVERY_SERVICE_FAILED;
        return cc_result_error(CC_ERR_STORAGE, "USB storage changed before recovery attachment");
    }
    cc_runtime_generation_t *generation=NULL;
    cc_result_t rc=cc_runtime_builder_acquire_generation(builder,&generation);
    if (rc.code==CC_OK) {
        cc_agent_runtime_t *runtime=cc_runtime_generation_runtime(generation);
        cc_session_store_t *sessions=runtime ? cc_agent_runtime_session_store(runtime) : NULL;
        cc_memory_store_t *memory=cc_runtime_builder_borrow_memory_store(builder);
        if (!runtime || !cc_agent_runtime_workspace_matches(runtime,service->workspace_path) ||
            !cc_json_segmented_store_matches_path(sessions,service->session_path) ||
            (memory && !cc_json_memory_store_matches_path(memory,service->memory_path)))
            rc=cc_result_error(CC_ERR_INVALID_STATE, "Runtime storage paths differ from recovery preflight");
    }
    if (generation) cc_runtime_generation_release(generation);
    if (rc.code==CC_OK && !same_mount(service))
        rc=cc_result_error(CC_ERR_STORAGE, "USB storage changed while checking runtime paths");
    if (rc.code!=CC_OK) return rc;
    rc=cc_runtime_builder_require_recovery(builder);
    if (rc.code != CC_OK) return rc;
    service->builder=builder;
    bool history_damaged=false;
    rc=ensure_recovery_directories(service,&history_damaged);
    if (rc.code!=CC_OK) { service->state=RECOVERY_SERVICE_FAILED; return rc; }
    if (history_damaged) service->state=RECOVERY_SERVICE_HISTORY_DAMAGED;
    else rc=attach_recovery(service);
    if (rc.code==CC_OK && service->startup_stage) {
        cc_result_free(&rc);
        rc=recovery_service_validate_staged(service);
    }
    return rc;
}

/**
 * @brief 先要求 builder 恢复，再组合非暂存 prepare 和 attach。
 * @pre owner 排空输入、模型及手动工具入口，并独占本次装配。
 * @param[in,out] builder 借用至服务销毁前的 builder，须先于服务销毁。
 * @param[in] config 同步借用的运行配置。
 * @param[in] io 同步借用的 I/O 配置。
 * @param[out] out_service 必填，builder/输出有效后置 NULL，成功取得拥有服务。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，其他准备/附加错误向上传递，失败清理本次服务并保留 builder 恢复要求，成功后还须核对状态及挂载。
 */
cc_result_t recovery_service_open(cc_runtime_builder_t *builder, const cc_config_t *config,
                                  const app_runtime_io_config_t *io, recovery_service_t **out_service)
{
    if (!builder || !out_service)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid recovery service request");
    *out_service=NULL;
    cc_result_t rc=cc_runtime_builder_require_recovery(builder);
    if (rc.code != CC_OK) return rc;
    recovery_service_t *service=NULL;
    rc=recovery_service_prepare(config,io,&service);
    if (rc.code != CC_OK) return rc;
    service->builder=builder;
    if (service->state == RECOVERY_SERVICE_READY_TO_ATTACH)
        rc=recovery_service_attach(service,builder);
    if (rc.code != CC_OK) { recovery_service_destroy(service); return rc; }
    *out_service=service;
    return cc_result_ok();
}

/**
 * @brief 返回当前可观察阶段，完整或受限装配换盘时报告 FAILED。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 空对象或就绪阶段挂载失效为 FAILED，其余返回存储的阶段，查询本身保持原字段。
 */
recovery_service_state_t recovery_service_state(const recovery_service_t *service)
{
    if (!service || ((service->state == RECOVERY_SERVICE_READY ||
                      service->state == RECOVERY_SERVICE_HISTORY_DAMAGED) && !same_mount(service)))
        return RECOVERY_SERVICE_FAILED;
    return service->state;
}

/**
 * @brief 复制当前 bundle 的操作列表，受限模式返回损坏来源和修复后重载诊断。
 * @param[in] service 借用的服务，须保持原挂载可用。
 * @param[out] out_items 必填且无待释放内容，两个输出有效后置 NULL，成功数组用 cc_pending_operation_array_free 清理。
 * @param[out] out_count 必填，两个输出有效后置零，成功得到条数。
 * @return 成功为 CC_OK，输出非法为 CC_ERR_INVALID_ARGUMENT，受限装配或读取后换盘为 CC_ERR_STORAGE，未就绪为 CC_ERR_INVALID_STATE，账本复制错误向上传递，失败保留空输出。
 */
cc_result_t recovery_service_list_operations(const recovery_service_t *service,
                                             cc_pending_operation_t **out_items, size_t *out_count)
{
    if (!out_items || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid pending operation output");
    *out_items=NULL;
    *out_count=0;
    if (!recovery_service_is_current(service))
        return cc_result_error(CC_ERR_INVALID_STATE, "Recovery storage is not current");
    if (service->state==RECOVERY_SERVICE_HISTORY_DAMAGED)
        return cc_result_error(CC_ERR_STORAGE, service->journal_damaged ?
            "Operation journal is damaged; all tools are disabled; repair the journal and reload" :
            "Recovery history is damaged; all tools are disabled; repair the records and reload");
    if (!service->bootstrap)
        return cc_result_error(CC_ERR_INVALID_STATE, "Recovery storage is not current");
    cc_result_t rc=cc_operation_journal_list(cc_recovery_bootstrap_journal(service->bootstrap),
                                             out_items, out_count);
    if (rc.code == CC_OK && !same_mount(service)) {
        cc_pending_operation_array_free(*out_items, *out_count);
        *out_items=NULL;
        *out_count=0;
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE, "USB storage changed while listing operations");
    }
    return rc;
}

/**
 * @brief 按用户外部核实结论定位原调用，在 generation 租约内持久化核对结果。
 * @pre owner 串行执行核对命令，用户已核实结果。
 * @param[in] service 借用的当前完整装配服务。
 * @param[in] operation_id 同步借用的非空操作 ID。
 * @param[in] disposition 须为 EXECUTED 或 NOT_EXECUTED。
 * @param[in] observation 同步借用的观察正文，EXECUTED 时须非空，NOT_EXECUTED 时允许 NULL。
 * @return 成功为 CC_OK，参数非法为 CC_ERR_INVALID_ARGUMENT，ID 缺失为 CC_ERR_NOT_FOUND，执行/核对中为 CC_ERR_INVALID_STATE，列表、租约、持久化及换盘错误向上传递，末次换盘错误时结果可能已提交。
 */
cc_result_t recovery_service_resolve_operation(const recovery_service_t *service,
                                               const char *operation_id,
                                               cc_tool_recovery_disposition_t disposition,
                                               const char *observation)
{
    if (!service || !operation_id || !operation_id[0] ||
        (disposition != CC_TOOL_RECOVERY_EXECUTED && disposition != CC_TOOL_RECOVERY_NOT_EXECUTED) ||
        (disposition == CC_TOOL_RECOVERY_EXECUTED && (!observation || !observation[0])))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid operation reconciliation");
    cc_pending_operation_t *items=NULL;
    size_t count=0;
    cc_result_t rc=recovery_service_list_operations(service,&items,&count);
    if (rc.code != CC_OK) return rc;
    size_t match=count;
    for (size_t i=0; i<count; i++) {
        if (items[i].operation_id && strcmp(items[i].operation_id,operation_id)==0) { match=i; break; }
    }
    if (match==count) {
        cc_pending_operation_array_free(items,count);
        return cc_result_error(CC_ERR_NOT_FOUND, "Pending operation was not found");
    }
    if (items[match].in_progress || items[match].reconciling) {
        cc_pending_operation_array_free(items,count);
        return cc_result_error(CC_ERR_INVALID_STATE, "Operation is still active or being reconciled");
    }
    cc_runtime_generation_t *generation=NULL;
    rc=cc_runtime_builder_acquire_generation(service->builder,&generation);
    if (rc.code == CC_OK && !same_mount(service))
        rc=cc_result_error(CC_ERR_STORAGE, "USB storage changed before reconciliation");
    if (rc.code == CC_OK) {
        cc_tool_result_t observed={0};
        observed.ok=1;
        observed.text=(char *)observation;
        rc=cc_agent_runtime_resolve_pending_tool(cc_runtime_generation_runtime(generation),
            items[match].session_id,items[match].call.id,disposition,
            disposition == CC_TOOL_RECOVERY_EXECUTED ? &observed : NULL);
    }
    if (generation) cc_runtime_generation_release(generation);
    cc_pending_operation_array_free(items,count);
    if (rc.code == CC_OK && !same_mount(service)) {
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE, "USB storage changed during reconciliation; check it after remount");
    }
    return rc;
}

/**
 * @brief 释放运行列表的 run/session ID 和数组。
 * @param[in,out] runs 拥有数组，允许 NULL，前 count 条可为有效或零初始化记录。
 * @param[in] count 需清理的槽数，须在数组范围内。
 * @post 所有拥有 ID 及数组已释放，调用者指针失效。
 */
void recovery_run_info_array_free(recovery_run_info_t *runs, size_t count)
{
    if (!runs) return;
    for (size_t i=0; i<count; i++) { free(runs[i].run_id); free(runs[i].session_id); }
    free(runs);
}

/**
 * @brief 列举并加载全部中断检查点，复制阶段、调用进度及最多 95 字节的 UTF-8 边界预览。
 * @param[in] service 借用的当前完整装配服务。
 * @param[out] out_runs 必填且无待释放内容，两个输出有效后置 NULL，成功数组用 recovery_run_info_array_free 清理。
 * @param[out] out_count 必填，两个输出有效后置零，成功得到条数。
 * @return 成功为 CC_OK，输出非法为 CC_ERR_INVALID_ARGUMENT，受限装配或末次换盘为 CC_ERR_STORAGE，未就绪为 CC_ERR_INVALID_STATE，分配及检查点错误向上传递，任一错误清理部分列表。
 */
cc_result_t recovery_service_list_runs(const recovery_service_t *service,
                                      recovery_run_info_t **out_runs, size_t *out_count)
{
    if (!out_runs || !out_count)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid interrupted-run output");
    *out_runs=NULL;
    *out_count=0;
    if (!recovery_service_is_current(service))
        return cc_result_error(CC_ERR_INVALID_STATE, "Recovery storage is not current");
    if (service->state==RECOVERY_SERVICE_HISTORY_DAMAGED)
        return cc_result_error(CC_ERR_STORAGE, service->journal_damaged ?
            "Operation journal is damaged; interrupted runs are unavailable until repair and reload" :
            "Recovery history is damaged; interrupted runs are unavailable until repair and reload");
    if (!service->bootstrap)
        return cc_result_error(CC_ERR_INVALID_STATE, "Recovery storage is not current");
    cc_run_checkpoint_store_t *store=cc_recovery_bootstrap_checkpoints(service->bootstrap);
    char **ids=NULL;
    size_t count=0;
    cc_result_t rc=cc_run_checkpoint_list(store,&ids,&count);
    if (rc.code != CC_OK) return rc;
    recovery_run_info_t *runs=count ? calloc(count,sizeof(*runs)) : NULL;
    if (count && !runs) rc=cc_result_error(CC_ERR_OUT_OF_MEMORY, "Cannot allocate interrupted runs");
    for (size_t i=0; rc.code == CC_OK && i<count; i++) {
        cc_run_checkpoint_t checkpoint={0};
        rc=cc_run_checkpoint_load(store,ids[i],&checkpoint);
        if (rc.code == CC_OK) {
            runs[i].run_id=checkpoint.run_id; checkpoint.run_id=NULL;
            runs[i].session_id=checkpoint.session_id; checkpoint.session_id=NULL;
            runs[i].step=checkpoint.step;
            runs[i].phase=checkpoint.phase;
            runs[i].completed_calls=checkpoint.next_call;
            runs[i].planned_calls=checkpoint.planned_calls.count;
            runs[i].recovered_from_backup=checkpoint.recovered_from_backup != 0;
            runs[i].recovered_from_temporary=checkpoint.recovered_from_temporary != 0;
            size_t preview_length=strlen(checkpoint.user_input);
            if (preview_length>=sizeof(runs[i].input_preview)) {
                preview_length=sizeof(runs[i].input_preview)-1U;
                while (preview_length && ((unsigned char)checkpoint.user_input[preview_length]&0xc0U)==0x80U)
                    preview_length--;
            }
            memcpy(runs[i].input_preview,checkpoint.user_input,preview_length);
            runs[i].input_preview[preview_length]='\0';
        }
        cc_run_checkpoint_cleanup(&checkpoint);
    }
    for (size_t i=0; i<count; i++) free(ids[i]);
    free(ids);
    if (rc.code == CC_OK && !same_mount(service)) {
        cc_result_free(&rc);
        rc=cc_result_error(CC_ERR_STORAGE, "USB storage changed while listing interrupted runs");
    }
    if (rc.code != CC_OK) { recovery_run_info_array_free(runs,count); return rc; }
    *out_runs=runs;
    *out_count=count;
    return rc;
}

/**
 * @brief 确认用户已核对的临时检查点，完成后复查原挂载。
 * @pre owner 在用户确认临时输入后串行调用。
 * @param[in] service 借用的当前完整装配服务。
 * @param[in] run_id 同步借用的非空运行 ID。
 * @return 状态或输入不可用为 CC_ERR_INVALID_STATE，其余返回检查点确认结果，末次换盘为 CC_ERR_STORAGE 且确认可能已完成。
 */
cc_result_t recovery_service_confirm_temporary_run(const recovery_service_t *service, const char *run_id)
{
    if (!run_id || !run_id[0] || !recovery_service_is_current(service) || !service->bootstrap)
        return cc_result_error(CC_ERR_INVALID_STATE,"Recovery storage is not current");
    cc_result_t rc=cc_run_checkpoint_confirm_temporary(
        cc_recovery_bootstrap_checkpoints(service->bootstrap),run_id);
    if (rc.code==CC_OK && !same_mount(service)) {
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE,"USB storage changed while confirming the temporary run");
    }
    return rc;
}

/**
 * @brief 在 generation 租约内放弃指定运行检查点，操作前后复查原挂载。
 * @param[in] service 借用的当前服务及 builder，owner 负责运行生命周期同步。
 * @param[in] run_id 同步借用的非空运行 ID。
 * @return 参数非法为 CC_ERR_INVALID_ARGUMENT，服务不可用为 CC_ERR_INVALID_STATE，租约、检查点及换盘错误向上传递，末次错误时删除可能已完成。
 * @note 原输入保留及记忆排队由 selection/turns 的组合入口协调，工具结果单独核对。
 */
cc_result_t recovery_service_abandon_run(const recovery_service_t *service, const char *run_id)
{
    if (!service || !run_id || !run_id[0])
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Invalid interrupted-run request");
    if (!recovery_service_is_current(service) || !service->builder)
        return cc_result_error(CC_ERR_INVALID_STATE, "Recovery storage is not current");
    cc_runtime_generation_t *generation=NULL;
    cc_result_t rc=cc_runtime_builder_acquire_generation(service->builder,&generation);
    if (rc.code == CC_OK && !same_mount(service))
        rc=cc_result_error(CC_ERR_STORAGE, "USB storage changed before abandoning the run");
    if (rc.code == CC_OK)
        rc=cc_agent_runtime_abandon_run(cc_runtime_generation_runtime(generation),run_id);
    if (generation) cc_runtime_generation_release(generation);
    if (rc.code == CC_OK && !same_mount(service)) {
        cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE, "USB storage changed while abandoning the run");
    }
    return rc;
}

/**
 * @brief 判断完整或受限装配是否仍对应创建时挂载。
 * @param[in] service 借用的服务，允许 NULL。
 * @return READY/HISTORY_DAMAGED 且原挂载仍当前为真，其余为假，受限模式能力由调用方另行检查。
 */
bool recovery_service_is_current(const recovery_service_t *service)
{
    return service && (service->state == RECOVERY_SERVICE_READY ||
                       service->state == RECOVERY_SERVICE_HISTORY_DAMAGED) && same_mount(service);
}

/**
 * @brief 判断当前挂载上的服务是否为明确损坏后的受限装配。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 历史或账本损坏且挂载仍当前为真，陈旧/完整装配为假。
 */
bool recovery_service_history_damaged(const recovery_service_t *service)
{ return recovery_service_is_current(service) && service->state==RECOVERY_SERVICE_HISTORY_DAMAGED; }

/**
 * @brief 提供区分完整装配、历史损坏、账本损坏和不可用的静态模式名。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 静态借用字符串 ready、history_damaged、journal_damaged 或 unavailable，ready 内部仍可有同名工具阻断。
 */
const char *recovery_service_mode_name(const recovery_service_t *service)
{
    recovery_service_state_t state=recovery_service_state(service);
    if (state==RECOVERY_SERVICE_READY) return "ready";
    if (state==RECOVERY_SERVICE_HISTORY_DAMAGED)
        return service->journal_damaged ? "journal_damaged" : "history_damaged";
    return "unavailable";
}

/**
 * @brief 核对当前服务的 builder 对象身份。
 * @param[in] service 借用的服务，允许 NULL。
 * @param[in] builder 待比较的 builder，允许 NULL。
 * @return 服务当前可用且指针一致为真，其余为假。
 */
bool recovery_service_uses_builder(const recovery_service_t *service, const cc_runtime_builder_t *builder)
{ return recovery_service_is_current(service) && builder && service->builder==builder; }

/**
 * @brief 核对 controller 与当前服务是否使用同一 builder。
 * @param[in] service 借用的服务，允许 NULL。
 * @param[in] controller 同步借用的 controller，允许 NULL。
 * @return 服务当前可用且 controller 依赖匹配为真，其余为假。
 */
bool recovery_service_uses_controller(const recovery_service_t *service, const cc_runtime_controller_t *controller)
{ return recovery_service_is_current(service) && cc_runtime_controller_uses_builder(controller,service->builder); }

/**
 * @brief 当前服务可用时借出 builder 管理的记忆存储。
 * @param[in] service 借用的服务，允许 NULL。
 * @return 记忆存储借用指针，寿命受 builder 和原挂载限制，服务不可用或存储缺失返回 NULL。
 */
cc_memory_store_t *recovery_service_borrow_memory_store(const recovery_service_t *service)
{ return recovery_service_is_current(service) ? cc_runtime_builder_borrow_memory_store(service->builder) : NULL; }

/**
 * @brief 释放 bootstrap、文件系统句柄、路径副本及服务外壳。
 * @pre owner 已关闭入口、排空调用并销毁已附加的 builder，仍借用的 stage 及文件系统依赖有效。
 * @param[in,out] service 待释放的完整或部分装配，允许 NULL，stage 本身由外层释放。
 * @post 服务寿命结束，所有借出的服务子对象指针失效，磁盘恢复文件保留。
 */
void recovery_service_destroy(recovery_service_t *service)
{
    if (!service) return;
    cc_recovery_bootstrap_destroy(service->bootstrap);
    if (service->filesystem.vtable && service->filesystem.vtable->destroy)
        service->filesystem.vtable->destroy(service->filesystem.self);
    free(service->workspace_path); free(service->session_path); free(service->memory_path);
    free(service);
}
