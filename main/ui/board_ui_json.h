#pragma once

#include <stdbool.h>

#include "cc/util/cc_json.h"

/**
 * @brief 按完整点分路径读取节点，不修改树。
 * @param[in] root 调用期稳定根借用，可 NULL，owner 排斥并发修改和销毁。
 * @param[in] path NUL 路径借用，可 NULL，点号空段跳过，空串或仅点号返回 root。
 * @return 节点借用，缺失、NULL 路径或路径副本 OOM 为 NULL，借用止于树变更或销毁。
 * @note 分量按 cc_json_object_get 忽略大小写查询，空段跳过，少于 128 字节的路径使用栈副本，其余完整复制到堆。
 */
cc_json_value_t *board_ui_json_get_path(cc_json_value_t *root, const char *path);
/**
 * @brief 按完整路径读取字符串视图。
 * @param[in] root 稳定根借用，可 NULL。
 * @param[in] path 调用期路径借用，可 NULL。
 * @return 字符串借用，缺失、非字符串或路径复制失败为 NULL，父树须覆盖使用期。
 */
const char *board_ui_json_get_path_string(cc_json_value_t *root, const char *path);
/**
 * @brief 按路径取得数字的 int 缓存视图。
 * @param[in] root 稳定根借用，可 NULL。
 * @param[in] path 路径借用，可 NULL。
 * @param[in] fallback 缺失或路径复制失败的缺省返回值。
 * @return 有节点时沿 cc_json_int_value，非数字返回零，缺失返回 fallback。
 */
int board_ui_json_get_path_int(cc_json_value_t *root, const char *path, int fallback);
/**
 * @brief 按路径读取 JSON true 或非零 int 视图。
 * @param[in] root 稳定根借用，可 NULL。
 * @param[in] path 路径借用，可 NULL。
 * @param[in] fallback 缺失或路径复制失败的缺省返回值。
 * @return JSON true 或整数视图非零为 true，其他存在节点为 false，缺失为 fallback。
 */
bool board_ui_json_get_path_bool(cc_json_value_t *root, const char *path, bool fallback);

/**
 * @brief 复制字符串，完整构造缺失对象后缀后一次安装。
 * @param[in,out] root 调用者独占的有效对象根，失败保持原树。
 * @param[in] path 完整路径借用，NULL、空串或仅点号失败，空点段跳过。
 * @param[in] value 调用期字符串借用，NULL 按空串，可引用旧树字段。
 * @return 完整安装 true，参数、分配或挂接失败 false，所有临时节点清理。
 * @note 中间字段忽略大小写查找，最终安装保留路径分量大小写，调用者应检查返回后再保存候选配置。
 */
bool board_ui_json_set_path_string(cc_json_value_t *root, const char *path, const char *value);
/**
 * @brief 完整构造数字和缺失对象后缀后安装。
 * @param[in,out] root 独占对象根，失败保持原树。
 * @param[in] path 完整路径借用，NULL、空串或仅点号失败。
 * @param[in] value 按值数字，特殊值行为沿 cc_json_create_number。
 * @return 完整安装 true，参数、分配或挂接失败 false，临时拥有资源全部归还。
 */
bool board_ui_json_set_path_number(cc_json_value_t *root, const char *path, double value);
/**
 * @brief 完整构造布尔值和缺失对象后缀后安装。
 * @param[in,out] root 独占对象根，失败保持原树。
 * @param[in] path 完整路径借用，NULL、空串或仅点号失败。
 * @param[in] value 按值目标布尔值。
 * @return 完整安装 true，参数、分配或挂接失败 false，临时拥有资源全部归还。
 */
bool board_ui_json_set_path_bool(cc_json_value_t *root, const char *path, bool value);
