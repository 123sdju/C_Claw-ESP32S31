#include "camera_tool.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_profile.h"
#include "board_camera.h"
#include "cc/core/cc_media.h"
#include "cc/ports/cc_tool.h"
#include "cc/util/cc_json.h"

typedef struct {
    /* 借用 runtime builder 持有的 filesystem，工具销毁时不能释放该 port。 */
    cc_filesystem_t filesystem;
} camera_tool_t;

#define CAMERA_TOOL_DEFAULT_SAVE_PATH "camera.jpg"
#define CAMERA_TOOL_SAVE_PATH_MAX 128

typedef struct {
    int quality;
    bool save_jpeg;
    bool return_image;
    char save_path[CAMERA_TOOL_SAVE_PATH_MAX];
} camera_tool_args_t;

/*
 * 返回工具名称标识符。
 * 参数: self - 工具实例指针（未使用）
 * 返回: 固定字符串 "camera_capture"
 */
static const char *camera_tool_name(void *self)
{
    (void)self;
    return "camera_capture";
}

/*
 * 返回工具功能描述字符串，提供给 Agent 理解工具用途。
 * 参数: self - 工具实例指针（未使用）
 * 返回: 英文描述字符串
 */
static const char *camera_tool_description(void *self)
{
    (void)self;
    return "Capture one current camera frame as JPEG, optionally save it, and optionally return it as an image";
}

/*
 * 返回 JSON Schema 字符串，定义工具接受参数的类型和约束：
 * quality（JPEG 质量 1-100，默认70）、save_path（保存路径）、return_image（是否返回图像）。
 * 参数: self - 工具实例指针（未使用）
 * 返回: JSON Schema 字符串
 */
static const char *camera_tool_schema_json(void *self)
{
    (void)self;
    return "{"
        "\"type\":\"object\","
        "\"properties\":{"
            "\"quality\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100,\"description\":\"JPEG quality, default 70\"},"
            "\"save_path\":{\"type\":\"string\",\"description\":\"Workspace-relative JPEG path, default camera.jpg; empty disables saving\"},"
            "\"return_image\":{\"type\":\"boolean\",\"description\":\"Return JPEG image artifact, default true\"}"
        "}"
    "}";
}

/*
 * 从 JSON 值中解析布尔类型，支持布尔、数字和字符串 ("true"/"1"/"yes"/"on") 多种格式。
 * 参数: value - JSON 值，可为 NULL
 *       default_value - 解析失败或无值时的默认返回值
 * 返回: 解析出的布尔值
 */
static bool json_bool_or_default(cc_json_value_t *value, bool default_value)
{
    if (!value) {
        return default_value;
    }
    if (cc_json_is_bool(value)) {
        return cc_json_bool_value(value) != 0;
    }
    if (cc_json_is_number(value)) {
        return cc_json_int_value(value) != 0;
    }
    const char *text = cc_json_string_value(value);
    if (!text) {
        return default_value;
    }
    return strcmp(text, "true") == 0 ||
           strcmp(text, "1") == 0 ||
           strcmp(text, "yes") == 0 ||
           strcmp(text, "on") == 0;
}

/*
 * 安全复制字符串到固定大小缓冲区，超长则返回失败并置空。
 * 参数: dst - 目标缓冲区
 *       dst_size - 缓冲区容量（含终止符）
 *       value - 源字符串，可为 NULL（此时置空 dst）
 * 返回: true 成功，false 字符串过长或参数无效
 */
static bool copy_arg_string(char *dst, size_t dst_size, const char *value)
{
    if (!dst || dst_size == 0) {
        return false;
    }
    if (!value) {
        dst[0] = '\0';
        return true;
    }

    size_t len = strlen(value);
    if (len >= dst_size) {
        dst[0] = '\0';
        return false;
    }
    memcpy(dst, value, len + 1);
    return true;
}

/*
 * 将格式化错误消息写入工具调用结果，自动管理 out_result->error 内存。
 * 参数: out_result - 工具调用结果
 *       fmt - printf 风格格式串
 *       ... - 可变参数
 * 无返回值，内部处理内存分配失败时回退到固定错误文本。
 */
static void camera_tool_set_errorf(cc_tool_result_t *out_result, const char *fmt, ...)
{
    if (!out_result || !fmt) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        out_result->error = strdup("Failed to format camera tool error");
        return;
    }

    char *text = malloc((size_t)needed + 1);
    if (!text) {
        va_end(args);
        out_result->error = strdup("Out of memory formatting camera tool error");
        return;
    }

    vsnprintf(text, (size_t)needed + 1, fmt, args);
    va_end(args);
    out_result->error = text;
}

/*
 * 解析 JSON 参数字符串为 camera_tool_args_t 结构体，应用默认值（quality=70,
 * save_jpeg=true, return_image=true）并对 quality 做 1-100 钳位。
 * 参数: args_json - JSON 参数字符串，可为 NULL 或空（使用默认值）
 *       out_args - [出参] 解析结果
 *       error - 错误消息缓冲区
 *       error_size - 错误消息缓冲区大小
 * 返回: true 解析成功，false 失败（错误信息写入 error）
 */
static bool parse_camera_tool_args(const char *args_json,
                                   camera_tool_args_t *out_args,
                                   char *error,
                                   size_t error_size)
{
    if (!out_args) {
        return false;
    }

    camera_tool_args_t args = {
        .quality = 70,
        .save_jpeg = true,
        .return_image = true,
    };
    snprintf(args.save_path, sizeof(args.save_path), "%s", CAMERA_TOOL_DEFAULT_SAVE_PATH);

    if (!args_json || !args_json[0]) {
        *out_args = args;
        return true;
    }

    cc_json_value_t *root = NULL;
    cc_result_t rc = cc_json_parse(args_json, &root);
    if (rc.code != CC_OK) {
        if (error && error_size > 0) {
            snprintf(error,
                     error_size,
                     "Invalid camera_capture JSON args: %s",
                     rc.message ? rc.message : cc_error_string(rc.code));
        }
        cc_result_free(&rc);
        return false;
    }
    cc_result_free(&rc);

    if (!cc_json_is_object(root)) {
        cc_json_destroy(root);
        if (error && error_size > 0) {
            snprintf(error, error_size, "camera_capture args must be a JSON object");
        }
        return false;
    }

    cc_json_value_t *quality_val = cc_json_object_get(root, "quality");
    if (quality_val) {
        args.quality = cc_json_int_value(quality_val);
    }
    if (args.quality < 1) {
        args.quality = 70;
    } else if (args.quality > 100) {
        args.quality = 100;
    }

    cc_json_value_t *return_image = cc_json_object_get(root, "return_image");
    if (!return_image) {
        return_image = cc_json_object_get(root, "returnImage");
    }
    args.return_image = json_bool_or_default(return_image, true);

    cc_json_value_t *save_path = cc_json_object_get(root, "save_path");
    if (!save_path) {
        save_path = cc_json_object_get(root, "savePath");
    }
    const char *path = cc_json_string_value(save_path);
    if (path) {
        args.save_jpeg = path[0] != '\0';
        if (!copy_arg_string(args.save_path, sizeof(args.save_path), path)) {
            cc_json_destroy(root);
            if (error && error_size > 0) {
                snprintf(error,
                         error_size,
                         "save_path is too long, max %u bytes",
                         (unsigned)(sizeof(args.save_path) - 1));
            }
            return false;
        }
    }

    cc_json_destroy(root);
    *out_args = args;
    return true;
}

/*
 * 执行摄像头采集流程：解析参数 → 捕获 JPEG → 可选保存 → 按需生成 Base64 artifact。
 * 通过 app_profile_span 记录采集和保存的性能耗时。
 * 参数: self - 工具实例，持有借用的 filesystem port 视图
 *       args_json - JSON 参数字符串
 *       ctx - 单次工具调用上下文
 *       out_result - [出参] 工具调用结果（ok/error/text/artifacts）
 * 返回: 始终返回 CC_OK（错误信息写入 out_result）
 */
static cc_result_t camera_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result)
{
    camera_tool_t *tool = (camera_tool_t *)self;

    memset(out_result, 0, sizeof(*out_result));
    cc_media_artifact_list_init(&out_result->artifacts);

    camera_tool_args_t args;
    char parse_error[160];
    if (!parse_camera_tool_args(args_json, &args, parse_error, sizeof(parse_error))) {
        out_result->ok = 0;
        out_result->error = strdup(parse_error);
        return cc_result_ok();
    }

    board_camera_jpeg_t image = {0};
    app_profile_span_t capture_span = app_profile_span_begin("tool.camera_capture", "jpeg_capture", NULL, NULL);
    esp_err_t err = board_camera_capture_jpeg(args.quality, &image);
    app_profile_span_end(&capture_span, err == ESP_OK ? "ok" : "error");
    if (err != ESP_OK) {
        out_result->ok = 0;
        char error[96];
        snprintf(error, sizeof(error), "Camera capture failed: %s", esp_err_to_name(err));
        out_result->error = strdup(error);
        return cc_result_ok();
    }

    size_t saved_bytes = 0;
    if (args.save_jpeg) {
        app_profile_span_t save_span = app_profile_span_begin("tool.camera_capture", "jpeg_save", NULL, NULL);
        if (!tool || !ctx || !ctx->workspace_dir || !tool->filesystem.vtable ||
            !(tool->filesystem.capabilities & CC_FS_CAP_ROOT_SCOPED) ||
            !tool->filesystem.vtable->write_relative_bytes) {
            app_profile_span_end(&save_span, "filesystem_unavailable");
            out_result->ok = 0;
            camera_tool_set_errorf(out_result,
                                   "Filesystem cannot safely save workspace JPEG path %s",
                                   args.save_path);
            board_camera_jpeg_cleanup(&image);
            return cc_result_ok();
        }

        cc_result_t write_rc = tool->filesystem.vtable->write_relative_bytes(
            tool->filesystem.self,
            ctx->workspace_dir,
            args.save_path,
            image.jpeg_data,
            image.jpeg_bytes);
        if (write_rc.code != CC_OK) {
            app_profile_span_end(&save_span, "write_error");
            out_result->ok = 0;
            camera_tool_set_errorf(out_result,
                                   "Failed to write JPEG save path %s: %s",
                                   args.save_path,
                                   write_rc.message ? write_rc.message : cc_error_string(write_rc.code));
            cc_result_free(&write_rc);
            board_camera_jpeg_cleanup(&image);
            return cc_result_ok();
        }
        cc_result_free(&write_rc);
        saved_bytes = image.jpeg_bytes;
        app_profile_span_end(&save_span, "ok");
    }

    if (args.return_image) {
        char *base64 = NULL;
        app_profile_span_t base64_span = app_profile_span_begin(
            "tool.camera_capture", "base64_encode", NULL, NULL);
        err = board_camera_encode_base64(image.jpeg_data, image.jpeg_bytes, &base64);
        app_profile_span_end(&base64_span, err == ESP_OK ? "ok" : "error");
        if (err != ESP_OK) {
            out_result->ok = 0;
            camera_tool_set_errorf(out_result,
                "Camera Base64 encode failed: %s", esp_err_to_name(err));
            board_camera_jpeg_cleanup(&image);
            return cc_result_ok();
        }

        cc_media_artifact_t artifact;
        cc_media_artifact_init(&artifact);
        artifact.id = strdup("camera_capture.latest");
        artifact.kind = CC_MEDIA_IMAGE;
        artifact.mime = strdup("image/jpeg");
        artifact.path = args.save_jpeg ? strdup(args.save_path) : NULL;
        artifact.data_base64 = base64;
        artifact.bytes = image.jpeg_bytes;
        artifact.width = image.width;
        artifact.height = image.height;

        cc_result_t rc = cc_tool_result_add_artifact(out_result, &artifact);
        cc_media_artifact_cleanup(&artifact);
        if (rc.code != CC_OK) {
            out_result->ok = 0;
            out_result->error = strdup(rc.message ? rc.message : cc_error_string(rc.code));
            cc_result_free(&rc);
            board_camera_jpeg_cleanup(&image);
            return cc_result_ok();
        }
    }

    char text[220];
    if (args.save_jpeg) {
        snprintf(text,
                 sizeof(text),
                 "Captured camera frame: %dx%d JPEG, %u bytes, saved to %s.",
                 image.width,
                 image.height,
                 (unsigned)saved_bytes,
                 args.save_path);
    } else {
        snprintf(text,
                 sizeof(text),
                 "Captured camera frame: %dx%d JPEG, %u bytes.",
                 image.width,
                 image.height,
                 (unsigned)image.jpeg_bytes);
    }

    board_camera_jpeg_cleanup(&image);
    out_result->ok = 1;
    out_result->text = strdup(text);
    return cc_result_ok();
}

/*
 * 释放 camera_tool_t 工具实例的内存。
 * 参数: self - 工具实例指针
 */
static void camera_tool_destroy(void *self)
{
    free(self);
}

static const cc_tool_vtable_t s_camera_tool_vtable = {
    camera_tool_name,
    camera_tool_description,
    camera_tool_schema_json,
    camera_tool_call,
    camera_tool_destroy,
};

/*
 * 创建 camera_capture 工具实例，分配 camera_tool_t 结构体并挂载虚函数表。
 * 参数: ctx - 工具工厂上下文，必须提供 workspace root-scoped 二进制写入能力
 *       out_tool - [出参] 接收创建的工具实例
 * 返回: CC_OK 成功，CC_ERR_INVALID_ARGUMENT 空输出指针，CC_ERR_OUT_OF_MEMORY 内存分配失败
 */
cc_result_t camera_tool_create(const cc_runtime_tool_factory_ctx_t *ctx, cc_tool_t *out_tool)
{
    if (!ctx || !out_tool || !ctx->filesystem.vtable ||
        !(ctx->filesystem.capabilities & CC_FS_CAP_ROOT_SCOPED) ||
        !ctx->filesystem.vtable->write_relative_bytes) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT,
                               "Camera tool requires root-scoped binary filesystem writes");
    }
    memset(out_tool, 0, sizeof(*out_tool));
    camera_tool_t *tool = calloc(1, sizeof(*tool));
    if (!tool) {
        return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate camera tool");
    }
    tool->filesystem = ctx->filesystem;
    out_tool->self = tool;
    out_tool->vtable = &s_camera_tool_vtable;
    return cc_result_ok();
}
