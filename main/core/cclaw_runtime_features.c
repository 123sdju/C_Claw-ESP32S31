#include "cclaw_runtime_features.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

#include "app_tool_registry.h"
#include "cc/app/cc_app_features.h"

static cc_tool_descriptor_t *s_tools;
static size_t s_tool_count;
static cc_runtime_feature_set_t s_runtime_features;
/* 原子标志，确保 init_tools_once() 在多线程环境只执行一次。 */
static atomic_bool s_tools_ready = ATOMIC_VAR_INIT(false);
static atomic_flag s_tools_init_lock = ATOMIC_FLAG_INIT;

static const cc_runtime_feature_set_t s_empty_features = {0};

/*
 * 线程安全的工具描述符懒初始化（双重检查锁定模式）。
 *
 * 首次调用时：
 * 1. 通过 cc_app_default_features() 获取 SDK 默认 feature set（内置的
 *    file_read/file_write/http.request/memory 等工具）
 * 2. 通过 app_tool_registry_descriptors() 获取应用层工具描述符（camera、
 *    lua、screen 等）
 * 3. 将两者合并到 s_runtime_features 中，生成完整的运行时 feature set
 *
 * 使用 acquire/release 内存序确保工具数组在其他线程可见时已完全初始化。
 * 如果内存分配失败，仍然设置 ready 标志以避免重复尝试，但 tools 为空。
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

    if (app_tool_count == 0) {
        s_tools = NULL;
        s_tool_count = 0;
        atomic_store_explicit(&s_tools_ready, true, memory_order_release);
        atomic_flag_clear_explicit(&s_tools_init_lock, memory_order_release);
        return;
    }

    cc_tool_descriptor_t *tools = calloc(tool_count, sizeof(*tools));
    if (!tools) {
        atomic_store_explicit(&s_tools_ready, true, memory_order_release);
        atomic_flag_clear_explicit(&s_tools_init_lock, memory_order_release);
        return;
    }

    size_t pos = 0;
    for (size_t i = 0; base->tools && i < base_tool_count; i++) {
        tools[pos++] = base->tools[i];
    }
    for (size_t i = 0; app_tools && i < app_tool_count; i++) {
        tools[pos++] = app_tools[i];
    }

    s_tools = tools;
    s_tool_count = pos;
    s_runtime_features.tools = s_tools;
    s_runtime_features.tool_count = s_tool_count;
    atomic_store_explicit(&s_tools_ready, true, memory_order_release);
    atomic_flag_clear_explicit(&s_tools_init_lock, memory_order_release);
}

/*
 * 获取 CClaw 应用的完整运行时 feature set。
 *
 * 返回: 指向合并了 SDK 内置工具和应用层工具后的运行时 feature set 的指针。
 *       指针在应用生命周期内有效（静态分配），不可由调用方释放。
 *
 * 首次调用会触发线程安全的懒初始化，合并 SDK 默认 features
 * （如 file_read/write、http.request、memory 等）与 app_tool_registry
 * 中的应用层工具（camera、lua、screen）。
 * 该指针由 runtime_service 在启动 Agent 运行时时传入 cc_runtime_builder。
 */
const cc_runtime_feature_set_t *cclaw_runtime_features(void)
{
    init_tools_once();
    return &s_runtime_features;
}
