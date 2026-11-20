#include "board_ui_json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief 复制完整路径供本次调用独立分词，避免定长路径和末键截断。
 * @param[in] path 调用期 NUL 路径借用，可为 NULL。
 * @return 调用者 free 的完整副本，NULL 输入、长度溢出或 OOM 返回 NULL。
 */
static char *json_path_copy(const char *path)
{
    if (!path) return NULL;
    size_t length = strlen(path);
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1U);
    if (copy) memcpy(copy, path, length + 1U);
    return copy;
}

/**
 * @brief 离线构造缺失对象后缀，准备完整后一次安装到原树。
 * @param[in,out] root 调用者独占的对象根，NULL 或非对象失败。
 * @param[in] path 调用期路径借用，可来自 root，点号空段沿 strtok_r 规则跳过。
 * @param[in,out] value 消费的独立拥有节点，NULL 为失败，所有退出路径均结束此拥有权。
 * @return 完整安装为 true，空路径、参数、分配或挂接失败为 false，失败保持原树。
 * @note 查找中间字段忽略大小写，安装按路径分量原文大小写，沿现有 JSON helper 规则。
 */
static bool json_set_path_owned(cc_json_value_t *root, const char *path, cc_json_value_t *value)
{
    char *copy = NULL;
    cc_json_value_t *chain = NULL;
    cc_json_value_t *attach_parent = NULL;
    char *attach_key = NULL;
    if (!value || !cc_json_is_object(root)) goto fail;
    copy = json_path_copy(path);
    if (!copy) goto fail;
    char *save = NULL;
    char *part = strtok_r(copy, ".", &save);
    cc_json_value_t *object = root;
    while (part) {
        char *next = strtok_r(NULL, ".", &save);
        if (!next) {
            bool installed = cc_json_object_set_owned(object, part, value) != 0;
            value = NULL;
            if (installed && chain) {
                installed = cc_json_object_set_owned(attach_parent, attach_key, chain) != 0;
                chain = NULL;
            }
            if (!installed) goto fail;
            free(copy);
            return true;
        }
        cc_json_value_t *child = cc_json_object_get(object, part);
        if (!cc_json_is_object(child)) {
            child = cc_json_create_object();
            if (!child) goto fail;
            if (!chain) {
                chain = child;
                attach_parent = object;
                attach_key = part;
            } else if (!cc_json_object_set_owned(object, part, child)) {
                goto fail;
            }
        }
        object = child;
        part = next;
    }
fail:
    cc_json_destroy(value);
    cc_json_destroy(chain);
    free(copy);
    return false;
}

/**
 * @brief 按完整点分路径读取借用节点，保持根树不变。
 * @param[in] root 调用期间稳定的根借用，可为 NULL。
 * @param[in] path NUL 路径借用，NULL 失败，空串或仅点号返回 root。
 * @return 路径节点借用，缺失、路径复制 OOM 或 NULL 参数为 NULL，寿命受原树变更/销毁影响。
 * @note 短路径使用 128 字节调用栈副本，当前表单全部常量路径无需堆分配；较长路径完整复制到堆。
 */
cc_json_value_t *board_ui_json_get_path(cc_json_value_t *root, const char *path)
{
    if (!root || !path) return NULL;
    char local_copy[128];
    size_t length = strlen(path);
    char *copy = length < sizeof(local_copy) ? local_copy : json_path_copy(path);
    if (!copy) return NULL;
    if (copy == local_copy) memcpy(copy, path, length + 1U);
    char *save = NULL;
    char *part = strtok_r(copy, ".", &save);
    cc_json_value_t *value = root;

    while (part && value) {
        value = cc_json_object_get(value, part);
        part = strtok_r(NULL, ".", &save);
    }
    if (copy != local_copy) free(copy);
    return value;
}

/**
 * @brief 按完整路径读取字符串借用。
 * @param[in] root 稳定根借用，可 NULL。
 * @param[in] path 路径借用，可 NULL。
 * @return 字符串正文借用，缺失、非字符串或路径分配失败为 NULL，父树修改或销毁可结束借用。
 */
const char *board_ui_json_get_path_string(cc_json_value_t *root, const char *path)
{
    return cc_json_string_value(board_ui_json_get_path(root, path));
}

/**
 * @brief 按完整路径读取 cJSON 的整数视图。
 * @param[in] root 稳定根借用，可 NULL。
 * @param[in] path 路径借用，可 NULL。
 * @param[in] fallback 无节点或路径复制失败时的返回值。
 * @return 有节点时沿 cc_json_int_value，非数字为零，缺失时为 fallback。
 */
int board_ui_json_get_path_int(cc_json_value_t *root, const char *path, int fallback)
{
    cc_json_value_t *value = board_ui_json_get_path(root, path);
    return value ? cc_json_int_value(value) : fallback;
}

/**
 * @brief 按完整路径读取布尔真值或非零整数视图。
 * @param[in] root 稳定根借用，可 NULL。
 * @param[in] path 路径借用，可 NULL。
 * @param[in] fallback 无节点或路径复制失败时的缺省值。
 * @return JSON true 或整数视图非零为 true，存在的其他类型为 false，缺失为 fallback。
 */
bool board_ui_json_get_path_bool(cc_json_value_t *root, const char *path, bool fallback)
{
    cc_json_value_t *value = board_ui_json_get_path(root, path);
    return value ? (cc_json_bool_value(value) || cc_json_int_value(value) != 0) : fallback;
}

/**
 * @brief 复制字符串并事务式安装完整路径。
 * @param[in,out] root 独占对象根，失败保留原树。
 * @param[in] path 调用期路径借用，NULL、空或仅点号失败。
 * @param[in] value 调用期字符串借用，NULL 按空串，可借用原树字段。
 * @return 完整安装 true，构造、路径或挂接失败 false，临时资源全部归还。
 */
bool board_ui_json_set_path_string(cc_json_value_t *root, const char *path, const char *value)
{
    return json_set_path_owned(root, path, cc_json_create_string(value ? value : ""));
}

/**
 * @brief 构造数字并事务式安装完整路径。
 * @param[in,out] root 独占对象根，失败保留原树。
 * @param[in] path 调用期路径借用，NULL、空或仅点号失败。
 * @param[in] value 数字值，有限性与序列化沿 cc_json_create_number。
 * @return 完整安装 true，构造、路径或挂接失败 false，无未交接节点泄漏。
 */
bool board_ui_json_set_path_number(cc_json_value_t *root, const char *path, double value)
{
    return json_set_path_owned(root, path, cc_json_create_number(value));
}

/**
 * @brief 构造布尔值并事务式安装完整路径。
 * @param[in,out] root 独占对象根，失败保留原树。
 * @param[in] path 调用期路径借用，NULL、空或仅点号失败。
 * @param[in] value 按值目标布尔值。
 * @return 完整安装 true，构造、路径或挂接失败 false，无临时拥有资源残留。
 */
bool board_ui_json_set_path_bool(cc_json_value_t *root, const char *path, bool value)
{
    return json_set_path_owned(root, path, cc_json_create_bool(value ? 1 : 0));
}
