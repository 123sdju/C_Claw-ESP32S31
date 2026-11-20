#include "manual_lua_service.h"
#include "lua_tool.h"
#include "cc/core/cc_id.h"
#include "cc/ports/cc_filesystem.h"
#include "cc/util/cc_json.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief 编码手动 Lua 命令，创建唯一调用 ID 后经控制器执行并核对挂载。
 *
 * 独立策略在本次调用中拥有默认文件系统和临时 Lua 工具，工具借用该文件系统。
 * 执行结束先销毁临时工具，再销毁文件系统，最后释放调用 ID 与 JSON。
 * 注册表策略直接借用当前 generation 中的同名工具，资源由运行时代际持有。
 * @pre 外层 owner 须协调 recovery 装配切换及销毁，所有依赖和 text 在调用期间有效。
 * 控制器负责运行接纳、generation 引用和取消源，本函数没有独立服务实例或内部锁。
 * @param[in,out] controller 当前控制器，须与 recovery 使用同一 builder，同步执行期间借用。
 * @param[in] recovery 当前恢复服务，调用前及分派前核对挂载，返回成功后再次核对。
 * @param[in] text 非空 C 字符串，须为 /lua 加空格和非空代码，或 /luafile 加空格和非空路径。
 * 尾部原文复制进 JSON，脚本、输出、堆和时间预算以及文件路径由实际 Lua 工具核对。
 * @param[in] policy 显式选择的注册表策略或独立策略，PENDING 优先返回状态错误。
 * @param[out] out_result 必填独立输出，入口清零，须先清理旧内容。
 * 控制器转交的拥有字段由调用者使用 cc_tool_result_cleanup 清理，错误返回时也须清理。
 * @return 返回参数、编码、ID 生成、工具创建或控制器执行结果。
 * CC_OK 时仍须读取 out_result->ok。成功返回后发现换盘会清理输出并改报 CC_ERR_STORAGE，
 * 底层错误与账本未知状态也可能发生在工具产生副作用之后。
 */
cc_result_t manual_lua_service_execute(cc_runtime_controller_t *controller,
    recovery_service_t *recovery, const char *text, manual_lua_policy_t policy,
    cc_tool_result_t *out_result)
{
    if (!out_result) return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid manual Lua result");
    memset(out_result,0,sizeof(*out_result));
    if (policy==MANUAL_LUA_POLICY_PENDING)
        return cc_result_error(CC_ERR_INVALID_STATE,"手动 Lua 策略尚待用户选择，暂不执行。");
    if (!controller || !recovery || !text ||
        (policy!=MANUAL_LUA_USE_MODEL_REGISTRY && policy!=MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Invalid manual Lua request");
    if (!recovery_service_uses_controller(recovery,controller))
        return cc_result_error(CC_ERR_STORAGE,"Controller is not attached to current recovery storage");
    bool file=strncmp(text,"/luafile ",9)==0;
    bool code=strncmp(text,"/lua ",5)==0;
    if ((!file && !code) || !(file ? text[9] : text[5]))
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,"Expected /lua code or /luafile path");
    const char *value=text+(file ? 9 : 5), *key=file ? "path" : "code";
    const char *name=file ? "lua_run_file" : "lua_run_code";
    cc_json_value_t *object=cc_json_create_object(), *string=cc_json_create_string(value);
    if (!object || !string) {
        cc_json_destroy(object); cc_json_destroy(string);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot encode manual Lua request");
    }
    cc_json_object_set(object,key,string);
    cc_json_value_t *attached=cc_json_object_get(object,key);
    /* set 没有返回状态，通过对象中的指针确认字符串所有权确已转交。 */
    if (attached!=string) {
        cc_json_destroy(string); cc_json_destroy(object);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot attach manual Lua argument");
    }
    const char *encoded_value=cc_json_string_value(attached);
    /* 核对编码后的字符串仍与整段命令尾部一致，失败时由 object 释放已接管字段。 */
    if (!encoded_value || strcmp(encoded_value,value)!=0) {
        cc_json_destroy(object);
        return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot encode manual Lua argument");
    }
    char *json=cc_json_stringify_unformatted(object); cc_json_destroy(object);
    if (!json) return cc_result_error(CC_ERR_OUT_OF_MEMORY,"Cannot encode manual Lua request");
    char *id=NULL;
    cc_result_t rc=cc_id_generate_uuid_v4("manual-",&id);
    cc_filesystem_t filesystem={0}; cc_tool_t tool={0};
    /* 独立工具只在本次同步调用中存活，工厂借用文件系统，销毁顺序必须覆盖该借用。 */
    if (rc.code==CC_OK && policy==MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY) {
        rc=cc_filesystem_get_default(&filesystem);
        if (rc.code==CC_OK) {
            cc_runtime_tool_factory_ctx_t ctx={0}; ctx.filesystem=filesystem;
            rc=file ? lua_run_file_tool_create(&ctx,&tool) : lua_run_code_tool_create(&ctx,&tool);
        }
    }
    if (rc.code==CC_OK && !recovery_service_is_current(recovery))
        rc=cc_result_error(CC_ERR_STORAGE,"Recovery storage changed before manual Lua execution");
    if (rc.code==CC_OK) {
        cc_tool_call_t call={id,(char *)name,json};
        /* trusted_tool 仅替代实现查找，运行时继续执行策略、账本和同名工具阻断检查。 */
        rc=cc_runtime_controller_execute_manual_tool(controller,&call,
             policy==MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY ? &tool : NULL,out_result);
    }
    if (tool.vtable && tool.vtable->destroy) tool.vtable->destroy(tool.self);
    if (filesystem.vtable && filesystem.vtable->destroy) filesystem.vtable->destroy(filesystem.self);
    free(id); free(json);
    if (rc.code==CC_OK && !recovery_service_is_current(recovery)) {
        /* 执行结果来自已变化的挂载，先释放输出，交由调用者核对设备副作用与持久账本。 */
        cc_tool_result_cleanup(out_result); cc_result_free(&rc);
        return cc_result_error(CC_ERR_STORAGE,"USB changed during manual Lua execution; verify the result");
    }
    return rc;
}
