#include "screen_tool.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_conversation.h"
#include "board_ui.h"
#include "cc/ports/cc_tool.h"
#include "cc/util/cc_json.h"

typedef struct {
    int unused;
} screen_tool_t;

/**
 * @brief 查询屏幕工具名称。
 * @param[in] self 未使用的实例借用，可 NULL。
 * @return 常驻只读名称借用，可并发，无分配。
 */
static const char *screen_tool_name(void *self)
{
    (void)self;
    return "screen_draw";
}

/**
 * @brief 查询屏幕工具描述。
 * @param[in] self 未使用的实例借用，可 NULL。
 * @return 常驻只读描述借用，可并发，无分配。
 */
static const char *screen_tool_description(void *self)
{
    (void)self;
    return "Draw or write content only inside the dedicated LCD Tool Area, isolated from the main conversation log";
}

/**
 * @brief 查询五种绘制操作的参数 Schema。
 * @param[in] self 未使用的实例借用，可 NULL。
 * @return 常驻只读 JSON 借用，可并发，无分配。
 */
static const char *screen_tool_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"op\":{\"type\":\"string\",\"enum\":[\"clear\",\"append\",\"text\",\"rect\",\"flush\"],"
                "\"description\":\"clear clears only the Tool Area; append writes a Tool Area log line; text/rect draw at coordinates inside the Tool Area\"},"
            "\"role\":{\"type\":\"string\",\"enum\":[\"tool\",\"status\",\"error\",\"thinking\"]},"
            "\"text\":{\"type\":\"string\"},"
            "\"x\":{\"type\":\"integer\",\"description\":\"x coordinate inside Tool Area; required for op=text/rect\"},"
            "\"y\":{\"type\":\"integer\",\"description\":\"y coordinate inside Tool Area; required for op=text/rect\"},"
            "\"width\":{\"type\":\"integer\"},"
            "\"height\":{\"type\":\"integer\"},"
            "\"color\":{\"type\":\"string\",\"description\":\"RGB565 hex like F800 or #F800 for rect\"}"
        "},"
        "\"required\":[\"op\"]"
    "}";
}

/**
 * @brief 将可选角色字符串映射到 UI 枚举。
 * @param[in] role 调用期 NUL 借用，可 NULL。
 * @return 对应枚举，NULL/未知默认 tool，无分配，无共享可变状态。
 */
static board_conversation_role_t parse_role(const char *role)
{
    if (!role) {
        return BOARD_CONV_TOOL;
    }
    if (strcmp(role, "status") == 0) {
        return BOARD_CONV_STATUS;
    }
    if (strcmp(role, "thinking") == 0) {
        return BOARD_CONV_THINKING;
    }
    if (strcmp(role, "error") == 0) {
        return BOARD_CONV_ERROR;
    }
    return BOARD_CONV_TOOL;
}

/**
 * @brief 解析可选井号及恰好四位 RGB565 颜色。
 * @param[in] text 调用期 NUL 借用，可 NULL。
 * @param[in] fallback 缺省或格式无效时采用的颜色。
 * @return 成功转换值或 fallback，无分配，无共享状态。
 */
static uint16_t parse_rgb565(const char *text, uint16_t fallback)
{
    if (!text || !text[0]) {
        return fallback;
    }
    if (text[0] == '#') {
        text++;
    }

    unsigned value = 0;
    int digits = 0;
    for (int i = 0; text[i] && i < 4; i++) {
        char c = text[i];
        value <<= 4;
        digits++;
        if (c >= '0' && c <= '9') {
            value |= (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= (unsigned)(c - 'A' + 10);
        } else {
            return fallback;
        }
    }

    return digits == 4 && text[4] == '\0' ? (uint16_t)value : fallback;
}

/**
 * @brief 向空结果写入工具失败状态。
 * @param[out] out_result 非 NULL 独占空结果借用，错误副本由 cc_tool_result_cleanup 回收。
 * @param[in] message 调用期 NUL 借用，NULL 用默认诊断。
 * @post ok 为零，OOM 时 error 可 NULL，不覆盖已有拥有字段。
 */
static void set_tool_error(cc_tool_result_t *out_result, const char *message)
{
    out_result->ok = 0;
    out_result->error = strdup(message ? message : "screen_draw failed");
}

/**
 * @brief 检查 JSON 对象是否含指定键。
 * @param[in] root 调用期 JSON 借用，可 NULL。
 * @param[in] key 调用期 NUL 键名借用，可 NULL。
 * @return 存在为 true，参数无效或缺失为 false，无分配。
 */
static bool json_has_key(cc_json_value_t *root, const char *key)
{
    return root && key && cc_json_object_get(root, key) != NULL;
}

/**
 * @brief 解析并分发清空、追加、坐标文字、矩形和刷新操作。
 * @param[in] self 未使用实例借用，可 NULL。
 * @param[in] args_json 调用期 NUL JSON 借用，NULL/空按空对象。
 * @param[in] ctx 未使用上下文借用，可 NULL。
 * @param[out] out_result 必填独占空结果，文本/诊断由 cc_tool_result_cleanup 回收。
 * @return 输出 NULL 返回参数错误，其余 CC_OK，工具失败见 ok/error，OOM 文本可 NULL。
 * @note JSON 临时树始终清理，UI 负责并发同步，缺坐标的 text 按追加处理。
 */
static cc_result_t screen_tool_call(void *self,
                                    const char *args_json,
                                    const cc_tool_context_t *ctx,
                                    cc_tool_result_t *out_result)
{
    (void)self;
    (void)ctx;

    if (!out_result) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null screen tool result");
    }

    memset(out_result, 0, sizeof(*out_result));
    cc_media_artifact_list_init(&out_result->artifacts);

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(args_json && args_json[0] ? args_json : "{}", &root);
    if (rc.code != CC_OK) {
        set_tool_error(out_result, rc.message ? rc.message : cc_error_string(rc.code));
        cc_result_free(&rc);
        return cc_result_ok();
    }
    cc_result_free(&rc);

    if (!cc_json_is_object(root)) {
        cc_json_destroy(root);
        set_tool_error(out_result, "screen_draw args must be an object");
        return cc_result_ok();
    }

    const char *op = cc_json_string_value(cc_json_object_get(root, "op"));
    const char *text = cc_json_string_value(cc_json_object_get(root, "text"));
    const char *role_text = cc_json_string_value(cc_json_object_get(root, "role"));
    const char *color_text = cc_json_string_value(cc_json_object_get(root, "color"));
    board_conversation_role_t role = parse_role(role_text);

    if (!op) {
        cc_json_destroy(root);
        set_tool_error(out_result, "screen_draw missing op");
        return cc_result_ok();
    }

    if (strcmp(op, "clear") == 0) {
        board_ui_tool_area_clear();

    } else if (strcmp(op, "append") == 0) {
        board_ui_tool_area_append(role, text ? text : "");

    } else if (strcmp(op, "text") == 0) {
        /* 缺少 x/y 时按工具区域日志追加，避免默认坐标绘制留下过期文字。 */
        if (!json_has_key(root, "x") || !json_has_key(root, "y")) {
            board_ui_tool_area_append(role, text ? text : "");
        } else {
            int x = cc_json_int_value(cc_json_object_get(root, "x"));
            int y = cc_json_int_value(cc_json_object_get(root, "y"));
            board_ui_tool_area_draw_text(x, y, role, text ? text : "");
        }

    } else if (strcmp(op, "rect") == 0) {
        if (!json_has_key(root, "x") || !json_has_key(root, "y") ||
            !json_has_key(root, "width") || !json_has_key(root, "height")) {
            board_ui_tool_area_append(BOARD_CONV_ERROR, "screen_draw rect missing x/y/width/height");
            cc_json_destroy(root);
            set_tool_error(out_result, "screen_draw rect missing x/y/width/height");
            return cc_result_ok();
        } else {
            int x = cc_json_int_value(cc_json_object_get(root, "x"));
            int y = cc_json_int_value(cc_json_object_get(root, "y"));
            int width = cc_json_int_value(cc_json_object_get(root, "width"));
            int height = cc_json_int_value(cc_json_object_get(root, "height"));
            uint16_t color = parse_rgb565(color_text, 0xffff);
            board_ui_tool_area_draw_rect(x, y, width, height, color);
        }

    } else if (strcmp(op, "flush") == 0) {
        board_ui_tool_area_request_flush();

    } else {
        cc_json_destroy(root);
        set_tool_error(out_result, "unknown screen_draw op");
        return cc_result_ok();
    }

    cc_json_destroy(root);
    out_result->ok = 1;
    out_result->text = strdup("screen_draw ok");
    return cc_result_ok();
}

/**
 * @brief 回收屏幕工具实例。
 * @param[in] self 转移拥有权的实例，可 NULL，调用者先等待进行中的调用完成。
 * @post 实例失效，无其他拥有资源。
 */
static void screen_tool_destroy(void *self)
{
    free(self);
}

static const cc_tool_vtable_t s_screen_tool_vtable = {
    screen_tool_name,
    screen_tool_description,
    screen_tool_schema_json,
    screen_tool_call,
    screen_tool_destroy,
};

/**
 * @brief 创建 screen_draw 工具并交付实例拥有权。
 * @param[in] ctx 未使用工厂借用，可 NULL。
 * @param[out] out_tool 必填独占空槽，有效槽入口清空，成功后用 vtable destroy 回收。
 * @return CC_OK，输出 NULL 为参数错误，分配失败为 OOM，诊断由 cc_result_free 回收。
 */
cc_result_t screen_tool_create(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    (void)ctx;
    if (!out_tool) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null screen tool output");
    }

    memset(out_tool, 0, sizeof(*out_tool));
    screen_tool_t *tool = calloc(1, sizeof(*tool));
    if (!tool) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate screen tool");
    }

    out_tool->self = tool;
    out_tool->vtable = &s_screen_tool_vtable;
    return cc_result_ok();
}
