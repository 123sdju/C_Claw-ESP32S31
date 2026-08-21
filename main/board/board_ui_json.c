#include "board_ui_json.h"

#include <stdio.h>
#include <string.h>

/*
 * 从 JSON 对象中获取指定 key 的子对象，若不存在则创建空对象并回填。
 * 参数: obj  - 父 JSON 对象
 *       key  - 子对象键名
 * 返回: 找到或新创建的子 JSON 对象指针
 */
static cc_json_value_t *json_get_obj(cc_json_value_t *obj, const char *key)
{
    cc_json_value_t *child = cc_json_object_get(obj, key);
    if (child && cc_json_is_object(child)) {
        return child;
    }
    child = cc_json_create_object();
    cc_json_object_set(obj, key, child);
    return child;
}

/*
 * 按点号分隔路径查找目标 key 的父对象，并将末级 key 写入 out_key。
 * 参数: root         - JSON 根对象
 *       path         - 点号分隔的路径，如 "network.sta.ssid"
 *       out_key      - 输出缓冲区，接收末级 key
 *       out_key_size - out_key 缓冲区大小
 * 返回: 末级 key 所在父对象；若路径无效则返回 NULL
 */
static cc_json_value_t *json_path_parent(cc_json_value_t *root, const char *path, char *out_key, size_t out_key_size)
{
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *save = NULL;
    char *part = strtok_r(tmp, ".", &save);
    cc_json_value_t *obj = root;

    while (part) {
        char *next = strtok_r(NULL, ".", &save);
        if (!next) {
            snprintf(out_key, out_key_size, "%s", part);
            return obj;
        }
        obj = json_get_obj(obj, part);
        part = next;
    }
    return NULL;
}

/*
 * 按点号分隔路径在 JSON 对象树中查找值，不自动创建缺失对象。
 * 参数: root - JSON 根对象
 *       path - 点号分隔的路径
 * 返回: 路径指向的 JSON 值；若路径中任何节点不存在则返回 NULL
 */
cc_json_value_t *board_ui_json_get_path(cc_json_value_t *root, const char *path)
{
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *save = NULL;
    char *part = strtok_r(tmp, ".", &save);
    cc_json_value_t *value = root;

    while (part && value) {
        value = cc_json_object_get(value, part);
        part = strtok_r(NULL, ".", &save);
    }
    return value;
}

/*
 * 按点号分隔路径获取 JSON 字符串值。
 * 参数: root - JSON 根对象
 *       path - 点号分隔的路径
 * 返回: 字符串值；若路径不存在或值非字符串则返回 NULL
 */
const char *board_ui_json_get_path_string(cc_json_value_t *root, const char *path)
{
    return cc_json_string_value(board_ui_json_get_path(root, path));
}

/*
 * 按点号分隔路径获取 JSON 整数值，路径不存在时返回默认值。
 * 参数: root     - JSON 根对象
 *       path     - 点号分隔的路径
 *       fallback - 路径不存在时的默认值
 * 返回: 整数值
 */
int board_ui_json_get_path_int(cc_json_value_t *root, const char *path, int fallback)
{
    cc_json_value_t *value = board_ui_json_get_path(root, path);
    return value ? cc_json_int_value(value) : fallback;
}

/*
 * 按点号分隔路径获取 JSON 布尔值，非零整数也视为 true。
 * 参数: root     - JSON 根对象
 *       path     - 点号分隔的路径
 *       fallback - 路径不存在时的默认值
 * 返回: 布尔值
 */
bool board_ui_json_get_path_bool(cc_json_value_t *root, const char *path, bool fallback)
{
    cc_json_value_t *value = board_ui_json_get_path(root, path);
    return value ? (cc_json_bool_value(value) || cc_json_int_value(value) != 0) : fallback;
}

/*
 * 按点号分隔路径设置 JSON 字符串值，自动创建路径上缺失的对象节点。
 * 参数: root  - JSON 根对象
 *       path  - 点号分隔的路径
 *       value - 要设置的字符串值
 */
void board_ui_json_set_path_string(cc_json_value_t *root, const char *path, const char *value)
{
    char key[32] = {0};
    cc_json_value_t *parent = json_path_parent(root, path, key, sizeof(key));
    if (parent && key[0]) {
        cc_json_object_set(parent, key, cc_json_create_string(value ? value : ""));
    }
}

/*
 * 按点号分隔路径设置 JSON 数值，自动创建路径上缺失的对象节点。
 * 参数: root  - JSON 根对象
 *       path  - 点号分隔的路径
 *       value - 要设置的数值
 */
void board_ui_json_set_path_number(cc_json_value_t *root, const char *path, double value)
{
    char key[32] = {0};
    cc_json_value_t *parent = json_path_parent(root, path, key, sizeof(key));
    if (parent && key[0]) {
        cc_json_object_set(parent, key, cc_json_create_number(value));
    }
}

/*
 * 按点号分隔路径设置 JSON 布尔值，自动创建路径上缺失的对象节点。
 * 参数: root  - JSON 根对象
 *       path  - 点号分隔的路径
 *       value - 要设置的布尔值
 */
void board_ui_json_set_path_bool(cc_json_value_t *root, const char *path, bool value)
{
    char key[32] = {0};
    cc_json_value_t *parent = json_path_parent(root, path, key, sizeof(key));
    if (parent && key[0]) {
        cc_json_object_set(parent, key, cc_json_create_bool(value ? 1 : 0));
    }
}
