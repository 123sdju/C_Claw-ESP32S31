"""编译实际 ESP32 存储函数，验证 NVS 读取竞争与文件时间戳边界。"""
from pathlib import Path
import re
import unittest

from test_event_ring_ownership import run_c_test
from test_profile_task_lifetime import function

ROOT = Path(__file__).resolve().parents[1]

RESULT_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
enum { CC_OK, CC_ERR_INVALID_ARGUMENT, CC_ERR_NOT_FOUND, CC_ERR_STORAGE,
       CC_ERR_OUT_OF_MEMORY, CC_ERR_IO };
typedef struct { int code; } cc_result_t;
/** @brief 构造无拥有诊断的替身成功结果。 @return 仅含 CC_OK 的值。 */
static cc_result_t cc_result_ok(void) { return (cc_result_t){CC_OK}; }
/** @brief 构造替身错误。 @param[in] code 错误码。 @param[in] text 借用消息。 @return 错误值。 */
static cc_result_t cc_result_error(int code, const char *text) {
    (void)text; return (cc_result_t){code};
}
/** @brief 构造替身格式错误。 @param[in] code 错误码。 @param[in] text 借用格式。 @return 错误值。 */
static cc_result_t cc_result_errf(int code, const char *text, ...) {
    (void)text; return (cc_result_t){code};
}
'''


class Esp32StorageContracts(unittest.TestCase):
    """以可控原生调用复现竞争和失败，运行地点为主机。"""

    def test_nvs_measured_capacity_and_failures(self):
        """值在两次读取间增长时只清除真实容量，同时检查句柄与初始化错误路径。"""
        source = (ROOT / "third_party/cclaw-sdk/cclaw/platforms/esp32/src/cc_esp32_secret_store.c").read_text(encoding="utf-8")
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        prefix = r'''
typedef int esp_err_t;
typedef unsigned nvs_handle_t;
enum { ESP_OK, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_NVS_INVALID_LENGTH, ESP_FAIL };
enum { NVS_READONLY, NVS_READWRITE };
enum { CC_SECRET_CAP_READ=1, CC_SECRET_CAP_WRITE=2, CC_SECRET_CAP_DELETE=4,
       CC_SECRET_CAP_SECURE_AT_REST=8, CC_SECRET_STORE_VTABLE_VERSION=1 };
typedef struct {
    size_t size; unsigned version; uint64_t capabilities;
    cc_result_t (*get)(void *, const char *, char **);
    cc_result_t (*set)(void *, const char *, const char *);
    cc_result_t (*remove)(void *, const char *);
    void (*destroy)(void *);
} cc_secret_store_vtable_t;
typedef struct {
    void *self; const cc_secret_store_vtable_t *vtable;
    size_t size; unsigned version; uint64_t capabilities;
} cc_secret_store_t;
static int scenario, closes, opens, commits, init_error, fail_alloc;
static void *tracked;
static size_t tracked_size, wiped;
static size_t grown_length = 16;
/** @brief 分配并记录本次实际容量。 @param[in] size 字节数。 @return 堆地址或 NULL。 */
static void *test_malloc(size_t size) {
    if (fail_alloc) return NULL;
    assert(!tracked); tracked = malloc(size); tracked_size = size;
    return tracked;
}
/** @brief 模拟工厂状态分配。 @param[in] n 数量。 @param[in] size 单项字节。 @return 分配结果。 */
static void *test_calloc(size_t n, size_t size) {
    return fail_alloc ? NULL : calloc(n, size);
}
/** @brief 验证释放地址的清除范围。 @param[in,out] p 拥有地址。 @post 分配释放。 */
static void test_free(void *p) {
    if (p == tracked) { tracked = NULL; tracked_size = 0; }
    free(p);
}
/** @brief 检查清除长度处于真实分配内。 @param[out] p 可写区。 @param[in] n 清除字节。 @post 区域清零。 */
static void cc_secret_zero(void *p, size_t n) {
    assert(p == tracked && n <= tracked_size); wiped = n; memset(p, 0, n);
}
/** @brief 模拟打开 NVS。 @param[in] name 命名空间。 @param[in] mode 访问模式。 @param[out] out 句柄。 @return 原生状态。 */
static int nvs_open(const char *name, int mode, nvs_handle_t *out) {
    assert(strcmp(name, "cclaw_sec") == 0); (void)mode; opens++;
    if (scenario == 1) return ESP_ERR_NVS_NOT_FOUND;
    if (scenario == 2) return ESP_FAIL;
    *out = 7; return ESP_OK;
}
/** @brief 检查句柄关闭。 @param[in] handle 借用句柄。 @post 关闭次数递增。 */
static void nvs_close(nvs_handle_t handle) { assert(handle == 7); closes++; }
/** @brief 模拟测量后值变化。 @param[in] h 句柄。 @param[in] key 键。 @param[out] out 缓冲。 @param[in,out] length 容量与所需长度。 @return 原生状态。 */
static int nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *length) {
    assert(h == 7 && strcmp(key, "token") == 0);
    if (!out) {
        if (scenario == 3) return ESP_ERR_NVS_NOT_FOUND;
        *length = scenario == 4 ? 0 : 8; return ESP_OK;
    }
    assert(*length == 8);
    if (scenario == 5) { *length = grown_length; return ESP_ERR_NVS_INVALID_LENGTH; }
    if (scenario == 6) return ESP_ERR_NVS_NOT_FOUND;
    if (scenario == 7) return ESP_FAIL;
    memcpy(out, "value", 6); *length = 6; return ESP_OK;
}
/** @brief 模拟同步设置。 @param[in] h 句柄。 @param[in] key 键。 @param[in] value 正文。 @return 原生状态。 */
static int nvs_set_str(nvs_handle_t h, const char *key, const char *value) {
    assert(h == 7 && key && value); return scenario == 8 ? ESP_FAIL : ESP_OK;
}
/** @brief 模拟删除。 @param[in] h 句柄。 @param[in] key 键。 @return 原生状态。 */
static int nvs_erase_key(nvs_handle_t h, const char *key) {
    assert(h == 7 && key); return scenario == 9 ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
/** @brief 模拟提交。 @param[in] h 句柄。 @return 原生状态。 */
static int nvs_commit(nvs_handle_t h) {
    assert(h == 7); commits++; return scenario == 10 ? ESP_FAIL : ESP_OK;
}
/** @brief 返回可控初始化状态。 @return 原生状态。 */
static int nvs_flash_init(void) { return init_error; }
/** @brief 借出测试诊断名称。 @param[in] err 状态。 @return 静态字符串。 */
static const char *esp_err_to_name(int err) { (void)err; return "test"; }
#define malloc test_malloc
#define calloc test_calloc
#define free test_free
'''
        tests = r'''
#undef malloc
#undef calloc
#undef free
/** @brief 运行容量竞争与资源退出断言。 @return 全部通过为零。 */
int main(void) {
    const int expected[] = {CC_OK, CC_ERR_NOT_FOUND, CC_ERR_STORAGE, CC_ERR_NOT_FOUND,
        CC_ERR_STORAGE, CC_ERR_STORAGE, CC_ERR_NOT_FOUND, CC_ERR_STORAGE};
    for (scenario = 0; scenario <= 7; scenario++) {
        char *value = (char *)(uintptr_t)1; opens = closes = 0; wiped = 0;
        cc_result_t rc = nvs_secret_get(NULL, "nvs:token", &value);
        assert(rc.code == expected[scenario]);
        assert(opens == 1 && closes == (scenario == 1 || scenario == 2 ? 0 : 1));
        if (scenario == 0) {
            assert(value == tracked && strcmp(value, "value") == 0);
            cc_secret_zero(value, tracked_size); test_free(value);
        } else {
            assert(!value && !tracked);
            if (scenario >= 5) assert(wiped == 8);
        }
    }
    const size_t growth_cases[] = {16, 4000, 4096};
    scenario = 5;
    for (size_t i = 0; i < sizeof(growth_cases)/sizeof(growth_cases[0]); i++) {
        grown_length = growth_cases[i]; wiped = 0;
        char *discard = NULL;
        assert(nvs_secret_get(NULL, "token", &discard).code == CC_ERR_STORAGE);
        assert(!discard && !tracked && wiped == 8);
    }
    scenario = 0; fail_alloc = 1; closes = 0;
    char *value = NULL;
    assert(nvs_secret_get(NULL, "token", &value).code == CC_ERR_OUT_OF_MEMORY);
    assert(!value && closes == 1);
    fail_alloc = 0;
    assert(nvs_secret_get(NULL, "", &value).code == CC_ERR_INVALID_ARGUMENT);
    assert(nvs_secret_get(NULL, "1234567890123456", &value).code == CC_ERR_INVALID_ARGUMENT);
    cc_secret_store_t store = {0};
    init_error = ESP_FAIL;
    assert(cc_secret_store_get_default(&store).code == CC_ERR_STORAGE && !store.self && !store.vtable);
    init_error = ESP_OK; fail_alloc = 1;
    assert(cc_secret_store_get_default(&store).code == CC_ERR_OUT_OF_MEMORY && !store.self);
    fail_alloc = 0;
    assert(cc_secret_store_get_default(&store).code == CC_OK && store.self);
    assert(store.capabilities == (CC_SECRET_CAP_READ | CC_SECRET_CAP_WRITE | CC_SECRET_CAP_DELETE));
    store.vtable->destroy(store.self);
    scenario = 0; commits = 0; closes = 0;
    assert(nvs_secret_set(NULL, "token", "value").code == CC_OK && commits == 1 && closes == 1);
    scenario = 8; commits = 0; closes = 0;
    assert(nvs_secret_set(NULL, "token", "value").code == CC_ERR_STORAGE && !commits && closes == 1);
    scenario = 9; commits = 0; closes = 0;
    assert(nvs_secret_remove(NULL, "token").code == CC_OK && commits == 1 && closes == 1);
    scenario = 10;
    assert(nvs_secret_set(NULL, "token", "value").code == CC_ERR_STORAGE);
    scenario = 1; closes = 0;
    assert(nvs_secret_remove(NULL, "token").code == CC_OK && !closes);
    assert(!tracked);
    return 0;
}
'''
        run_c_test(RESULT_PREFIX + prefix + source + tests)

    def test_stat_unix_milliseconds(self):
        """负时间归零、正秒转毫秒并饱和溢出，缺失项与空参数保持明确输出。"""
        source = (ROOT / "third_party/cclaw-sdk/cclaw/platforms/esp32/src/cc_esp32_filesystem.c").read_text(encoding="utf-8")
        prefix = r'''
enum { CC_FS_ENTRY_MISSING, CC_FS_ENTRY_FILE, CC_FS_ENTRY_DIRECTORY };
typedef struct { size_t size; int type; uint64_t byte_size, modified_ms; } cc_filesystem_stat_t;
struct stat { int st_mode; int64_t st_mtime; size_t st_size; };
#define S_ISREG(mode) ((mode) == 1)
#define S_ISDIR(mode) ((mode) == 2)
static struct stat native;
static int native_error, calls;
/** @brief 输出可控元数据。 @param[in] path 路径。 @param[out] out 元数据。 @return 模拟原生返回值。 */
static int stat(const char *path, struct stat *out) {
    assert(path); calls++; if (native_error) { errno = native_error; return -1; }
    *out = native; return 0;
}
'''
        tests = r'''
/** @brief 核对实际 stat 实现的时间和失败边界。 @return 全部通过为零。 */
int main(void) {
    cc_filesystem_stat_t out = {0};
    assert(esp32_stat_path(NULL, "file", NULL).code == CC_ERR_INVALID_ARGUMENT);
    assert(esp32_stat_path(NULL, NULL, &out).code == CC_ERR_INVALID_ARGUMENT && !calls);
    assert(out.size == sizeof(out) && out.type == CC_FS_ENTRY_MISSING);
    const int64_t times[] = {-1, 0, 42, INT64_MAX};
    const uint64_t expected[] = {0, 0, 42000, UINT64_MAX};
    native.st_mode = 1; native.st_size = 123;
    for (size_t i = 0; i < sizeof(times)/sizeof(times[0]); i++) {
        native.st_mtime = times[i];
        assert(esp32_stat_path(NULL, "file", &out).code == CC_OK);
        assert(out.modified_ms == expected[i] && out.type == CC_FS_ENTRY_FILE && out.byte_size == 123);
    }
    native_error = ENOENT;
    assert(esp32_stat_path(NULL, "missing", &out).code == CC_OK);
    assert(out.type == CC_FS_ENTRY_MISSING && !out.modified_ms && !out.byte_size);
    native_error = EIO;
    assert(esp32_stat_path(NULL, "error", &out).code == CC_ERR_IO);
    assert(out.type == CC_FS_ENTRY_MISSING && !out.modified_ms);
    return 0;
}
'''
        validators = function(source, "static int esp32_ascii_letter(")
        validators += function(source, "static int esp32_host_path(")
        validators += function(source, "static cc_result_t esp32_validate_path(")
        run_c_test(RESULT_PREFIX + prefix + validators +
                   function(source, "static cc_result_t esp32_stat_path(") + tests)


if __name__ == "__main__":
    unittest.main()
