#pragma once

#include "cc/app/cc_runtime_controller.h"
#include "recovery_service.h"

typedef enum manual_lua_policy {
    MANUAL_LUA_POLICY_PENDING = 0, /**< 用户尚未选择策略，手动命令返回状态错误。 */
    MANUAL_LUA_USE_MODEL_REGISTRY = 1, /**< 从当前运行时代际的模型工具注册表查找 Lua 工具。 */
    MANUAL_LUA_INDEPENDENT_OF_MODEL_REGISTRY /**< 临时创建 Lua 工具，手动入口独立于模型工具注册开关。 */
} manual_lua_policy_t;

/**
 * @brief 将 /lua 或 /luafile 命令同步交给当前控制器及恢复账本执行。
 *
 * /lua 后的原文作为 code，/luafile 后的原文作为 path，命令尾部原样编码为 JSON。
 * 通过命令与装配校验后生成新的 manual- 前缀工具调用 ID，控制器在当前会话及所持运行时代际中执行。
 * 独立策略使用默认文件系统创建临时 Lua 工具，仍经过工具策略、操作账本与同名工具禁用检查。
 * 账本和介质必须属于当前 recovery 装配，未知工具结果按账本保留供人工核对。
 *
 * 当前 Lua 实现使用默认脚本 4096 字节、输出 2048 字节、堆 128 KiB 和超时 1000 毫秒，
 * 超时还受工具上下文限制。文件路径由 Lua 运行时约束在当前工作区 lua/ 下，
 * 须为相对 .lua 路径，补齐 lua/ 前缀后的路径连同 NUL 须适配 192 字节缓冲区。
 * 控制器负责传递取消请求，执行期间的响应时机由工具实现及其运行限制决定。
 * @pre 交互 owner 协调本调用及 recovery 的装配切换与销毁，依赖须存活至调用结束。
 * 控制器负责串行接纳运行和持有 generation，繁忙或关闭状态可能拒绝本次调用。
 * @param[in,out] controller 非空当前控制器，借用至同步执行结束，须与 recovery 使用同一 builder。
 * @param[in] recovery 非空当前恢复服务，调用期间借用，用于核对控制器装配与 U 盘挂载代数。
 * @param[in] text 非空 NUL 结尾命令，须以 /lua 加空格或 /luafile 加空格开头且尾部非空。
 * 调用期间借用，前缀区分大小写，尾部原文的字节预算与路径检查由 Lua 工具执行。
 * @param[in] policy 调用者显式提供的策略，PENDING 在其他依赖校验前返回 CC_ERR_INVALID_STATE。
 * @param[out] out_result 必填独立输出，入口即清零，调用者须先清理旧内容。
 * 返回后的 text、error 及其他拥有字段归调用者，成功或失败后均使用 cc_tool_result_cleanup 清理。
 * @return CC_OK 表示调用路径返回结果，仍须检查 out_result->ok，工具失败或未知结果可由 error 表达。
 * 参数、分配、文件系统、工具创建、控制器、取消或账本错误按实际结果返回。
 * 执行返回成功后若发现换盘，清理输出并返回 CC_ERR_STORAGE，要求核对执行结果。
 * 返回错误也可能已有工具副作用或部分持久化，重试前须按当前账本核对。
 */
cc_result_t manual_lua_service_execute(cc_runtime_controller_t *controller,
    recovery_service_t *recovery, const char *text, manual_lua_policy_t policy,
    cc_tool_result_t *out_result);
