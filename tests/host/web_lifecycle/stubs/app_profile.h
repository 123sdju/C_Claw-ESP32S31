#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { bool enabled; uint64_t start_us; } app_profile_span_t;
/**
 * @brief 创建不启用采样的模型 span。
 * @param[in] category 借用分类，模型忽略。
 * @param[in] name 借用操作名，模型忽略。
 * @param[in] session 借用会话，允许 NULL。
 * @param[in] detail 借用细节，允许 NULL。
 * @return 零初始化模型span，无动态资源。
 */
app_profile_span_t app_profile_span_begin(const char *category, const char *name,
    const char *session, const char *detail);
/**
 * @brief 结束模型 span，本模型仅验证接口可调用。
 * @param[in,out] span 借用模型span。
 * @param[in] status 借用结束状态。
 * @post 无动态资源变化。
 */
void app_profile_span_end(app_profile_span_t *span, const char *status);
