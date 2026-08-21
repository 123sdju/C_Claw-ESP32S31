#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cc/app/cc_runtime_builder.h"
#include "cc/util/cc_config.h"

/*
 * 使用 cc_runtime_builder 创建 Agent 运行时并发送自检消息，校验 SDK 消息链路是否正常。
 * 参数: config - 运行时配置（provider/model 等）
 */
void runtime_service_run_self_test(const cc_config_t *config);
/*
 * 使用 cc_config_t 初始化运行时：构建 cc_runtime_builder、提取 cc_agent_runtime_t，
 * 并应用 thinking_mode 配置。
 * 参数: config - 运行时配置
 * 返回: true 表示运行时启动成功，false 表示失败（并记录错误日志）
 */
bool runtime_service_start(const cc_config_t *config);
/* 在有限时间内等待 generation 释放并销毁 runtime builder。 */
cc_result_t runtime_service_stop(uint32_t timeout_ms);
/*
 * 获取全局 cc_agent_runtime_t 实例指针。
 * 返回: 启动后返回有效指针，未启动返回 NULL
 */
cc_runtime_builder_t *runtime_service_builder(void);

/* Reset the HTTP pools owned by this runtime instance. */
cc_result_t runtime_service_reset_http_connections(void);
/*
 * 动态切换运行时 thinking_mode 开关，在运行时已启动的前提下调 set_thinking_mode。
 * 参数: thinking_mode - true 开启思考模式，false 关闭
 */
void runtime_service_set_thinking(bool thinking_mode);
