#include "cclaw_runtime_features.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_tool_registry.h"
#include "cc/app/cc_app_features.h"
#include "cc/ports/cc_memory_tool_factory.h"

static cc_tool_descriptor_t *s_tools;
static size_t s_tool_count;
static cc_runtime_feature_set_t s_runtime_features;
/* 原子标志，确保 init_tools_once() 在多线程环境只执行一次。 */
static atomic_bool s_tools_ready = ATOMIC_VAR_INIT(false);
static atomic_flag s_tools_init_lock = ATOMIC_FLAG_INIT;

static const cc_runtime_feature_set_t s_empty_features = {0};

#if CC_HAS_MEMORY
/**
 * @brief 为当前 generation 创建只读的 memory 查询工具。
 * @param[in] ctx 借用的工厂上下文，须提供有效 memory_store 端口。
 * @param[out] out_tool 接收工具句柄，成功后由 registry 或 generation 销毁。
 * @return 成功返回 CC_OK，并将可选能力输出设为 READ_ONLY。上下文或输出非法时
 *         返回 INVALID_ARGUMENT，工具分配失败返回 OUT_OF_MEMORY，错误消息由调用者释放。
 * @note memory_store 在工具存活期间保持有效。上下文校验失败时保留输出原值，
 *       进入底层工具工厂后，合法输出会先被清零。
 */
static cc_result_t create_readonly_memory_tool(
    const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    if (!ctx || !ctx->memory_store || !ctx->memory_store->vtable)
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Memory store is unavailable");
    cc_result_t rc = cc_memory_tool_create_readonly(ctx->memory_store, out_tool);
    if (rc.code == CC_OK && ctx->out_capabilities)
        *ctx->out_capabilities = CC_TOOL_CAPABILITY_READ_ONLY;
    return rc;
}
#endif

/**
 * @brief 将名称为 memory 的描述符约束为应用的只读记忆策略。
 * @param[in,out] tool 待覆盖的描述符，NULL 或名称不匹配时直接返回。
 * @post 启用记忆时替换为只读工厂，裁剪记忆时禁用描述符并清空工厂指针。
 */
static void enforce_memory_tool_policy(cc_tool_descriptor_t *tool)
{
    if (!tool || !tool->name || strcmp(tool->name,"memory") != 0) return;
#if CC_HAS_MEMORY
    tool->create = create_readonly_memory_tool;
#else
    tool->compiled = 0;
    tool->create = NULL;
#endif
}

/**
 * @brief 在进程内首次访问时合并 SDK 与应用工具，并发布只读 feature 集合。
 * @post 首次初始化发布 SDK 的 provider、store 等描述符及合并后的工具数组。
 *       工具数组分配失败时发布空工具集合，保留其余 feature，后续调用复用该结果。
 * @note atomic_flag 串行化初始化，ready 的 release/acquire 使完整数组对后续调用可见。
 *       等待者自旋，初始化期间仅调用静态描述符查询并分配数组。数组及其借用的描述符
 *       来源均保留到进程结束，发布后调用者只读访问。
 */
static void init_tools_once(void)
{
    if (atomic_load_explicit(&s_tools_ready, memory_order_acquire)) {
        return;
    }

    while (atomic_flag_test_and_set_explicit(&s_tools_init_lock, memory_order_acquire)) {
    }
    if (atomic_load_explicit(&s_tools_ready, memory_order_acquire)) {
        atomic_flag_clear_explicit(&s_tools_init_lock, memory_order_release);
        return;
    }

    const cc_runtime_feature_set_t *base = cc_app_default_features();
    if (!base) {
        base = &s_empty_features;
    }
    s_runtime_features = *base;

    size_t app_tool_count = 0;
    const cc_tool_descriptor_t *app_tools = app_tool_registry_descriptors(&app_tool_count);
    const size_t base_tool_count = base->tool_count;
    const size_t tool_count = base_tool_count + app_tool_count;

    if (tool_count == 0) {
        s_tools = NULL;
        s_tool_count = 0;
        atomic_store_explicit(&s_tools_ready, true, memory_order_release);
        atomic_flag_clear_explicit(&s_tools_init_lock, memory_order_release);
        return;
    }

    cc_tool_descriptor_t *tools = calloc(tool_count, sizeof(*tools));
    if (!tools) {
        /* 分配失败时发布空工具集合，以保持应用规定的只读记忆策略。 */
        s_runtime_features.tools = NULL;
        s_runtime_features.tool_count = 0;
        atomic_store_explicit(&s_tools_ready, true, memory_order_release);
        atomic_flag_clear_explicit(&s_tools_init_lock, memory_order_release);
        return;
    }

    size_t pos = 0;
    for (size_t i = 0; base->tools && i < base_tool_count; i++) {
        tools[pos++] = base->tools[i];
        enforce_memory_tool_policy(&tools[pos-1]);
    }
    for (size_t i = 0; app_tools && i < app_tool_count; i++) {
        tools[pos++] = app_tools[i];
        enforce_memory_tool_policy(&tools[pos-1]);
    }

    s_tools = tools;
    s_tool_count = pos;
    s_runtime_features.tools = s_tools;
    s_runtime_features.tool_count = s_tool_count;
    atomic_store_explicit(&s_tools_ready, true, memory_order_release);
    atomic_flag_clear_explicit(&s_tools_init_lock, memory_order_release);
}

/**
 * @brief 获取经过应用记忆策略处理的运行时 feature 集合。
 * @return 返回进程期有效的只读借用指针。工具数组首次分配失败时返回的集合仍然有效，
 *         其 tools 为 NULL、tool_count 为零，后续调用维持该结果。
 * @note 首次调用执行同步初始化，后续通过原子 ready 直接访问发布结果。
 *       runtime_service 将该指针借给 builder，静态集合与工具数组覆盖所有 generation 寿命。
 */
const cc_runtime_feature_set_t *cclaw_runtime_features(void)
{
    init_tools_once();
    return &s_runtime_features;
}
