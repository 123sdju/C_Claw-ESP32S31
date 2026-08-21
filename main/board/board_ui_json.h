#pragma once

#include <stdbool.h>

#include "cc/util/cc_json.h"

/*
 * 按点号分隔路径在 JSON 对象树中查找值，不自动创建缺失对象。
 * 参数: root - JSON 根对象
 *       path - 点号分隔的路径
 * 返回: 路径指向的 JSON 值；若路径中任何节点不存在则返回 NULL
 */
cc_json_value_t *board_ui_json_get_path(cc_json_value_t *root, const char *path);
/*
 * 按点号分隔路径获取 JSON 字符串值。
 * 参数: root - JSON 根对象
 *       path - 点号分隔的路径
 * 返回: 字符串值；若路径不存在或值非字符串则返回 NULL
 */
const char *board_ui_json_get_path_string(cc_json_value_t *root, const char *path);
/*
 * 按点号分隔路径获取 JSON 整数值，路径不存在时返回默认值。
 * 参数: root     - JSON 根对象
 *       path     - 点号分隔的路径
 *       fallback - 路径不存在时的默认值
 * 返回: 整数值
 */
int board_ui_json_get_path_int(cc_json_value_t *root, const char *path, int fallback);
/*
 * 按点号分隔路径获取 JSON 布尔值，非零整数也视为 true。
 * 参数: root     - JSON 根对象
 *       path     - 点号分隔的路径
 *       fallback - 路径不存在时的默认值
 * 返回: 布尔值
 */
bool board_ui_json_get_path_bool(cc_json_value_t *root, const char *path, bool fallback);

/*
 * 按点号分隔路径设置 JSON 字符串，自动创建路径上缺失的对象节点。
 * 参数: root  - JSON 根对象
 *       path  - 点号分隔的路径
 *       value - 要设置的字符串值
 */
void board_ui_json_set_path_string(cc_json_value_t *root, const char *path, const char *value);
/*
 * 按点号分隔路径设置 JSON 数值，自动创建路径上缺失的对象节点。
 * 参数: root  - JSON 根对象
 *       path  - 点号分隔的路径
 *       value - 要设置的数值
 */
void board_ui_json_set_path_number(cc_json_value_t *root, const char *path, double value);
/*
 * 按点号分隔路径设置 JSON 布尔值，自动创建路径上缺失的对象节点。
 * 参数: root  - JSON 根对象
 *       path  - 点号分隔的路径
 *       value - 要设置的布尔值
 */
void board_ui_json_set_path_bool(cc_json_value_t *root, const char *path, bool value);
