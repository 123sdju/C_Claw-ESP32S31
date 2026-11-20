"""用当前 cJSON 原文验证容器深度、SDK 包装输出及失败清理。

32、64 与未覆写的默认 1000 分别独立编译。数组、对象和混合容器均将根
计为第一层，标量为零层。此模型验证宿主 C 调用与分配，不测 MCU 栈水位。
"""
from pathlib import Path
import unittest

from test_event_ring_ownership import run_c_test

ROOT = Path(__file__).resolve().parents[1]


def production_function(relative_path, signature):
    """原样取得一个生产函数定义，保留函数体和条件分支。"""
    text = (ROOT / relative_path).read_text(encoding="utf-8")
    start = text.index(signature)
    return text[start:text.index("\n}\n", start) + 3]


MODEL = r'''
typedef void cc_json_value_t;
typedef struct {
    size_t calls;
    size_t fail_at;
    size_t live_blocks;
    size_t live_bytes;
} allocation_state_t;
typedef union {
    long double alignment;
    void *pointer_alignment;
    struct {
        allocation_state_t *owner;
        size_t bytes;
    } record;
} allocation_header_t;
static allocation_state_t vendor_allocations, sdk_allocations;

/**
 * @brief 为当前夹具分配带归属记录的对齐块，并支持指定调用点失败。
 * @param[in] bytes 用户申请字节数。
 * @param[in,out] state 必需独占统计，调用序号在每次尝试时增加。
 * @return 对应 state 拥有的块，指定失败点、容量溢出或原生 OOM 为 NULL。
 */
static void *tracked_allocate(size_t bytes, allocation_state_t *state)
{
    state->calls++;
    if (state->calls == state->fail_at || bytes > SIZE_MAX - sizeof(allocation_header_t)) return NULL;
    allocation_header_t *header = malloc(sizeof(*header) + bytes);
    if (!header) return NULL;
    header->record.owner = state;
    header->record.bytes = bytes;
    assert(bytes <= SIZE_MAX - state->live_bytes);
    state->live_blocks++;
    state->live_bytes += bytes;
    return header + 1;
}

/**
 * @brief 归还指定分配层的拥有块并核对归属。
 * @param[in] pointer 可空拥有指针。
 * @param[in,out] state 与分配时相同的独占统计。
 * @post 计数及字节归还，重复或跨层释放通过断言或 sanitizer 暴露。
 */
static void tracked_release(void *pointer, allocation_state_t *state)
{
    if (!pointer) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    assert(header->record.owner == state && state->live_blocks);
    assert(header->record.bytes <= state->live_bytes);
    state->live_blocks--;
    state->live_bytes -= header->record.bytes;
    header->record.owner = NULL;
    free(header);
}

/** @brief cJSON 的实际分配钩子。 @param[in] bytes 字节数。 @return vendor 统计拥有的块或 NULL。 */
static void *CJSON_CDECL vendor_allocate(size_t bytes)
{ return tracked_allocate(bytes, &vendor_allocations); }

/** @brief cJSON 的实际释放钩子。 @param[in] pointer 可空拥有块。 @post vendor 统计归还。 */
static void CJSON_CDECL vendor_release(void *pointer)
{ tracked_release(pointer, &vendor_allocations); }

/** @brief SDK 文本复制所用的私有分配替身。 @param[in] bytes 字节数。 @return SDK 统计拥有的块或 NULL。 */
static void *sdk_allocate(size_t bytes)
{ return tracked_allocate(bytes, &sdk_allocations); }

/** @brief 归还 SDK 结果诊断或 stringify 副本。 @param[in] pointer 可空拥有块。 @post SDK 统计归还。 */
static void sdk_release(void *pointer)
{ tracked_release(pointer, &sdk_allocations); }

/**
 * @brief 选择纯数组、纯对象或交替容器的指定层。
 * @param[in] shape 零为数组，一为对象，二为交替。
 * @param[in] level 从零开始的容器层下标。
 * @return 本层使用对象为 true，使用数组为 false。
 */
static bool object_at_level(unsigned shape, size_t level)
{ assert(shape <= 2); return shape == 1 || (shape == 2 && level % 2 != 0); }

/**
 * @brief 构造具有精确容器层数的 JSON 文本，输入缓冲独立于 cJSON 钩子。
 * @param[in] depth 容器层数，零生成标量零。
 * @param[in] shape 传给 object_at_level 的形状。
 * @param[in] empty_leaf true 时最深容器为空，false 时包含标量零。
 * @return 测试调用者 free 的正文，预留额外尾部空间供错误用例，原生 OOM 触发断言。
 */
static char *make_json_text(size_t depth, unsigned shape, bool empty_leaf)
{
    assert(depth <= (SIZE_MAX - 32U) / 7U);
    char *text = malloc(depth * 7U + 32U);
    assert(text);
    size_t used = 0;
    for (size_t level = 0; level < depth; level++) {
        bool object = object_at_level(shape, level);
        text[used++] = object ? '{' : '[';
        if (object && !(empty_leaf && level + 1U == depth)) {
            memcpy(text + used, "\"k\":", 4);
            used += 4;
        }
    }
    if (!depth || !empty_leaf) text[used++] = '0';
    for (size_t level = depth; level > 0; level--)
        text[used++] = object_at_level(shape, level - 1U) ? '}' : ']';
    text[used] = '\0';
    return text;
}

/**
 * @brief 通过真实构造 API 创建可超过解析上限的拥有树。
 * @param[in] depth 精确容器层数，零创建标量。
 * @param[in] shape 纯数组、对象或交替形状。
 * @param[in] empty_leaf 是否使用空的最深容器。
 * @return 由调用者 cJSON_Delete 的独立根，每次成功挂接转交子节点拥有权。
 */
static cJSON *make_owned_tree(size_t depth, unsigned shape, bool empty_leaf)
{
    size_t remaining = depth;
    cJSON *current = NULL;
    if (empty_leaf && depth) {
        remaining--;
        current = object_at_level(shape, remaining) ? cJSON_CreateObject() : cJSON_CreateArray();
    } else {
        current = cJSON_CreateNumber(0);
    }
    assert(current);
    while (remaining) {
        remaining--;
        bool object = object_at_level(shape, remaining);
        cJSON *parent = object ? cJSON_CreateObject() : cJSON_CreateArray();
        assert(parent);
        cJSON_bool added = object ? cJSON_AddItemToObject(parent, "k", current) :
                                  cJSON_AddItemToArray(parent, current);
        assert(added);
        current = parent;
    }
    return current;
}
'''


TESTS = r'''
/**
 * @brief 检查两个真实 SDK 解析包装的结果、输出初始化和诊断清理。
 * @param[in] text 调用期间稳定的非 NULL 正文。
 * @param[in] ordinary_success 普通首值解析的预期成功状态。
 * @param[in] strict_success 完整尾部解析的预期成功状态。
 * @post 每次调用结束后树、诊断和两层分配统计全部归零。
 */
static void check_wrappers(const char *text, bool ordinary_success, bool strict_success)
{
    for (unsigned strict = 0; strict < 2; strict++) {
        cc_json_value_t *output = (void *)1;
        cc_result_t result = strict ? cc_json_parse_strict(text, &output) : cc_json_parse(text, &output);
        bool expected = strict ? strict_success : ordinary_success;
        assert(result.code == (expected ? CC_OK : CC_ERR_JSON));
        assert(expected ? output != NULL : output == NULL);
        cc_json_destroy(output);
        cc_result_free(&result);
        assert(!vendor_allocations.live_blocks && !vendor_allocations.live_bytes);
        assert(!sdk_allocations.live_blocks && !sdk_allocations.live_bytes);
    }
}

/**
 * @brief 验证四类真实打印入口及两个 SDK stringify 包装的深度边界。
 * @param[in] tree 调用者继续拥有的稳定树。
 * @param[in] depth 已知容器深度，用于预分配足够的格式化缓冲。
 * @param[in] expected_text 可空的紧凑预期文本，成功时必需。
 * @param[in] success 预期允许打印为 true，超过上限为 false。
 * @post 返回时仅保留输入树原有分配，失败打印不遗留动态半输出。
 */
static void check_prints(cJSON *tree, size_t depth, const char *expected_text, bool success)
{
    size_t old_blocks = vendor_allocations.live_blocks;
    size_t old_bytes = vendor_allocations.live_bytes;
    char *formatted = cJSON_Print(tree);
    char *compact = cJSON_PrintUnformatted(tree);
    assert((formatted != NULL) == success && (compact != NULL) == success);
    if (success) {
        assert(expected_text && !strcmp(compact, expected_text));
        cJSON *roundtrip = cJSON_ParseWithOpts(formatted, NULL, 1);
        assert(roundtrip);
        cJSON_Delete(roundtrip);
    }
    for (int format = 0; format < 2; format++) {
        char *buffered = cJSON_PrintBuffered(tree, 1, format);
        assert((buffered != NULL) == success);
        if (success) assert(!strcmp(buffered, format ? formatted : expected_text));
        cJSON_free(buffered);
        assert(depth <= 1001U);
        size_t capacity = depth * (depth + 12U) + 256U;
        assert(capacity <= INT_MAX);
        char *preallocated = malloc(capacity);
        assert(preallocated);
        memset(preallocated, 0x5a, capacity);
        cJSON_bool printed = cJSON_PrintPreallocated(tree, preallocated, (int)capacity, format);
        assert((printed != 0) == success);
        if (success) assert(!strcmp(preallocated, format ? formatted : expected_text));
        /* 原生预分配入口失败时可留下前缀，调用者只释放自己的缓冲。 */
        free(preallocated);
    }
    cJSON_free(formatted);
    cJSON_free(compact);
    char *sdk_formatted = cc_json_stringify(tree);
    char *sdk_compact = cc_json_stringify_unformatted(tree);
    assert((sdk_formatted != NULL) == success && (sdk_compact != NULL) == success);
    if (success) assert(!strcmp(sdk_compact, expected_text));
    sdk_release(sdk_formatted);
    sdk_release(sdk_compact);
    assert(vendor_allocations.live_blocks == old_blocks && vendor_allocations.live_bytes == old_bytes);
    assert(!sdk_allocations.live_blocks && !sdk_allocations.live_bytes);
}

/** @brief 检查标量、上限前一层、精确上限和超一层的全部形状。 @post 每轮树、打印缓冲和正文全部清理。 */
static void test_boundaries(void)
{
    const size_t depths[] = {0, CJSON_NESTING_LIMIT - 1U, CJSON_NESTING_LIMIT, CJSON_NESTING_LIMIT + 1U};
    for (unsigned shape = 0; shape < 3; shape++) {
        for (unsigned empty = 0; empty < 2; empty++) {
            for (size_t index = 0; index < sizeof(depths) / sizeof(depths[0]); index++) {
                size_t depth = depths[index];
                bool allowed = depth <= CJSON_NESTING_LIMIT;
                char *text = make_json_text(depth, shape, empty != 0);
                const char *end = NULL;
                cJSON *tree = cJSON_ParseWithOpts(text, &end, 1);
                assert((tree != NULL) == allowed);
                if (allowed) assert(end == text + strlen(text));
                if (!allowed) {
                    assert(end && end >= text && end <= text + strlen(text));
                    assert(!vendor_allocations.live_blocks && !vendor_allocations.live_bytes);
                    tree = make_owned_tree(depth, shape, empty != 0);
                }
                check_prints(tree, depth, text, allowed);
                cJSON_Delete(tree);
                assert(!vendor_allocations.live_blocks && !vendor_allocations.live_bytes);
                check_wrappers(text, allowed, allowed);
                free(text);
            }
        }
    }
}

/** @brief 验证深子树完成后的外层语法错误及首值后尾部错误。 @post cJSON 与包装的失败树、结果诊断和正文均清理。 */
static void test_trailing_errors(void)
{
    for (unsigned shape = 0; shape < 3; shape++) {
        char *text = make_json_text(CJSON_NESTING_LIMIT, shape, false);
        size_t length = strlen(text);
        text[length - 1U] = '!';
        assert(!cJSON_ParseWithOpts(text, NULL, 1));
        assert(!vendor_allocations.live_blocks && !vendor_allocations.live_bytes);
        check_wrappers(text, false, false);
        text[length - 1U] = object_at_level(shape, 0) ? '}' : ']';
        memcpy(text + length, " !", 3);
        assert(!cJSON_ParseWithOpts(text, NULL, 1));
        assert(!vendor_allocations.live_blocks && !vendor_allocations.live_bytes);
        check_wrappers(text, true, false);
        free(text);
    }
}

/** @brief 遍历实际 cJSON 解析和格式化打印的分配失败点。 @post 失败只保留调用者原树，所有暂存输出归还，两层分配最终归零。 */
static void test_allocation_failures(void)
{
    char *text = make_json_text(CJSON_NESTING_LIMIT, 2, false);
    vendor_allocations.calls = 0;
    cJSON *tree = cJSON_ParseWithOpts(text, NULL, 1);
    assert(tree);
    size_t parse_calls = vendor_allocations.calls;
    cJSON_Delete(tree);
    for (size_t failure = 1; failure <= parse_calls; failure++) {
        vendor_allocations.calls = 0;
        vendor_allocations.fail_at = failure;
        assert(!cJSON_ParseWithOpts(text, NULL, 1));
        assert(!vendor_allocations.live_blocks && !vendor_allocations.live_bytes);
    }
    vendor_allocations.fail_at = 0;
    tree = cJSON_ParseWithOpts(text, NULL, 1);
    assert(tree);
    size_t old_blocks = vendor_allocations.live_blocks;
    size_t old_bytes = vendor_allocations.live_bytes;
    vendor_allocations.calls = 0;
    char *printed = cJSON_Print(tree);
    assert(printed);
    size_t print_calls = vendor_allocations.calls;
    cJSON_free(printed);
    for (size_t failure = 1; failure <= print_calls; failure++) {
        vendor_allocations.calls = 0;
        vendor_allocations.fail_at = failure;
        assert(!cJSON_Print(tree));
        assert(vendor_allocations.live_blocks == old_blocks && vendor_allocations.live_bytes == old_bytes);
    }
    vendor_allocations.fail_at = 0;
    sdk_allocations.calls = 0;
    sdk_allocations.fail_at = 1;
    assert(!cc_json_stringify(tree));
    assert(!sdk_allocations.live_blocks && vendor_allocations.live_blocks == old_blocks);
    sdk_allocations.fail_at = 0;
    cJSON_Delete(tree);
    vendor_allocations.calls = 0;
    vendor_allocations.fail_at = 1;
    cc_json_value_t *output = (void *)1;
    cc_result_t result = cc_json_parse_strict(text, &output);
    assert(result.code == CC_ERR_JSON && !output);
    cc_result_free(&result);
    vendor_allocations.fail_at = 0;
    assert(!vendor_allocations.live_blocks && !vendor_allocations.live_bytes);
    assert(!sdk_allocations.live_blocks && !sdk_allocations.live_bytes);
    free(text);
}

/** @brief 沿两个实际包装的 NULL 参数契约核对输出及诊断。 @post 无树分配或诊断泄漏。 */
static void test_wrapper_parameters(void)
{
    cc_json_value_t *output = (void *)1;
    cc_result_t result = cc_json_parse(NULL, &output);
    assert(result.code == CC_ERR_INVALID_ARGUMENT && !output);
    cc_result_free(&result);
    output = (void *)1;
    result = cc_json_parse_strict(NULL, &output);
    assert(result.code == CC_ERR_INVALID_ARGUMENT && output == (void *)1);
    cc_result_free(&result);
    result = cc_json_parse_strict("0", NULL);
    assert(result.code == CC_ERR_INVALID_ARGUMENT);
    cc_result_free(&result);
    assert(!vendor_allocations.live_blocks && !sdk_allocations.live_blocks);
}

/** @brief 运行本次编译深度的真实 vendor 与 SDK 包装契约。 @return 全部边界及失败清理断言通过为零。 */
int main(void)
{
    assert(CJSON_NESTING_LIMIT == TEST_EXPECTED_LIMIT);
    cJSON_Hooks hooks = {vendor_allocate, vendor_release};
    cJSON_InitHooks(&hooks);
    test_boundaries();
    test_trailing_errors();
    test_allocation_failures();
    test_wrapper_parameters();
    assert(!vendor_allocations.live_blocks && !vendor_allocations.live_bytes);
    assert(!sdk_allocations.live_blocks && !sdk_allocations.live_bytes);
    cJSON_InitHooks(NULL);
    printf("cJSON parse/print exact container limit %d and cleanup: PASS\n", CJSON_NESTING_LIMIT);
    return 0;
}
'''


def fixture_source(override, expected):
    """组合原样 vendor include、真实 SDK 函数和手写分配审计模型。"""
    vendor = (ROOT / "third_party/cclaw-sdk/cclaw/core/vendor/cjson/cJSON.c").as_posix()
    result_header = (ROOT / "third_party/cclaw-sdk/cclaw/core/include/cc/core/cc_result.h").as_posix()
    allocation_header = (ROOT / "third_party/cclaw-sdk/cclaw/core/include/cc/internal/cc_alloc.h").as_posix()
    prefix = "#include <assert.h>\n#include <stdbool.h>\n#include <stdint.h>\n#include <limits.h>\n#include <stdarg.h>\n"
    if override is not None:
        prefix += f"#define CJSON_NESTING_LIMIT {override}\n"
    prefix += f'#define TEST_EXPECTED_LIMIT {expected}\n#include "{vendor}"\n#include "{result_header}"\n'
    prefix += MODEL
    prefix += f'#define malloc sdk_allocate\n#include "{allocation_header}"\n#undef malloc\n'
    prefix += "#define malloc sdk_allocate\n#define free sdk_release\n"
    result = "third_party/cclaw-sdk/cclaw/core/src/core/cc_result.c"
    for signature in ("cc_result_t cc_result_ok(", "cc_result_t cc_result_error(",
                      "cc_result_t cc_result_errf(",
                      "void cc_error_detail_cleanup(", "void cc_result_free("):
        prefix += production_function(result, signature)
    prefix += "#undef malloc\n#undef free\n"
    json = "third_party/cclaw-sdk/cclaw/core/src/util/cc_json.c"
    for signature in ("static int json_unicode_escapes_valid(", "cc_result_t cc_json_parse(",
                      "cc_result_t cc_json_parse_strict(", "char *cc_json_stringify(",
                      "char *cc_json_stringify_unformatted(", "void cc_json_destroy("):
        prefix += production_function(json, signature)
    return prefix + TESTS


class JsonNestingContracts(unittest.TestCase):
    def test_firmware_limit_32(self):
        """显式编译 32 层，精确边界允许、33 层拒绝并完整归还资源。"""
        run_c_test(fixture_source(32, 32))

    def test_configured_limit_64(self):
        """显式改成 64 层，核对解析与全部打印入口共用该配置。"""
        run_c_test(fixture_source(64, 64))

    def test_sdk_vendor_default_1000(self):
        """不定义深度宏，核对 vendor 默认 1000 及超限清理。"""
        run_c_test(fixture_source(None, 1000))


if __name__ == "__main__":
    unittest.main()
