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

/*
 * 返回工具名称标识符。
 * 参数: self - 工具实例指针（未使用）
 * 返回: 固定字符串 "screen_draw"
 */
static const char *screen_tool_name(void *self)
{
    (void)self;
    return "screen_draw";
}

/*
 * 返回工具功能描述字符串。
 * 参数: self - 工具实例指针（未使用）
 * 返回: 英文描述字符串
 */
static const char *screen_tool_description(void *self)
{
    (void)self;
    return "Draw or write content only inside the dedicated LCD Tool Area, isolated from the main conversation log";
}

/*
 * 返回 JSON Schema 字符串，定义 5 种操作（clear/append/text/rect/flush）及
 * 对应参数（role/text/x/y/width/height/color），op 为必填字段。
 * 参数: self - 工具实例指针（未使用）
 * 返回: JSON Schema 字符串
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

/*
 * 将角色字符串映射为 board_conversation_role_t 枚举值。
 * 参数: role - 角色名（"tool"/"status"/"thinking"/"error"），NULL 或未知时默认 BOARD_CONV_TOOL
 * 返回: 对应的枚举值
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

/*
 * 解析 RGB565 十六进制颜色字符串（支持可选 "#" 前缀，如 "F800" 或 "#F800"）。
 * 参数: text - 颜色字符串，可为 NULL
 *       fallback - 解析失败时的默认颜色值
 * 返回: 16 位 RGB565 颜色值（仅当恰好 4 位十六进制字符时成功）
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

    return digits == 4 ? (uint16_t)value : fallback;
}

/*
 * 向工具调用结果写入错误状态，复制错误消息到 out_result->error。
 * 参数: out_result - 工具调用结果
 *       message - 错误消息，NULL 时使用默认 "screen_draw failed"
 */
static void set_tool_error(cc_tool_result_t *out_result, const char *message)
{
    out_result->ok = 0;
    out_result->error = strdup(message ? message : "screen_draw failed");
}

/*
 * 判断 JSON 对象是否包含指定键。
 * 参数: root - JSON 对象
 *       key - 键名
 * 返回: true 存在，false 不存在或参数无效
 */
static bool json_has_key(cc_json_value_t *root, const char *key)
{
    return root && key && cc_json_object_get(root, key) != NULL;
}

/*
 * 执行 screen_draw 工具调用：解析 JSON 参数中的 op 字段，分发到 5 种操作：
 *   clear  - 清空工具区域
 *   append - 在工具区域追加一行日志
 *   text   - 在指定坐标绘制文字（缺 x/y 时退化为 append）
 *   rect   - 在指定坐标绘制矩形（需 x/y/width/height）
 *   flush  - 请求刷新显示
 * 参数: self - 工具实例（未使用）
 *       args_json - JSON 参数字符串
 *       ctx - 工具上下文（未使用）
 *       out_result - [出参] 工具调用结果
 * 返回: 始终返回 CC_OK（错误信息写入 out_result）
 */
static cc_result_t screen_tool_call(void *self,
                                    const char *args_json,
                                    const cc_tool_context_t *ctx,
                                    cc_tool_result_t *out_result)
{
    (void)self;
    (void)ctx;

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
        /*
         * If x/y are omitted, do not draw at 0,0. Treat it as an append inside
         * Tool Area so it cannot leave stale text in the main chat region.
         */
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

/*
 * 释放 screen_tool_t 工具实例的内存。
 * 参数: self - 工具实例指针
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

/*
 * 创建 screen_draw 工具实例，分配 screen_tool_t 结构体并挂载虚函数表。
 * 参数: ctx - 工具工厂上下文（当前未使用）
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 成功，CC_ERR_INVALID_ARGUMENT 空输出指针，CC_ERR_OUT_OF_MEMORY 内存分配失败
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
