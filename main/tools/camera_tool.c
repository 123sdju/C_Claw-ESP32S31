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

/**
 * @brief 查询摄像头工具名称。
 * @param[in] self 未使用实例借用，可 NULL。
 * @return 常驻只读名称借用，无分配，可并发。
 */
static const char *camera_tool_name(void *self)
{
    (void)self;
    return "camera_capture";
}

/**
 * @brief 查询采集功能描述。
 * @param[in] self 未使用实例借用，可 NULL。
 * @return 常驻只读描述借用，无分配，可并发。
 */
static const char *camera_tool_description(void *self)
{
    (void)self;
    return "Capture one current camera frame as JPEG, optionally save it, and optionally return it as an image";
}

/**
 * @brief 查询 JPEG 质量、保存路径及图像返回参数 Schema。
 * @param[in] self 未使用实例借用，可 NULL。
 * @return 常驻只读 JSON 借用，无分配，可并发。
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

/**
 * @brief 兼容解析布尔、非零数值及 true/1/yes/on 字符串。
 * @param[in] value 调用期 JSON 借用，可 NULL，其他字符串解析为 false。
 * @param[in] default_value 缺失或非标量类型时的默认值。
 * @return 按值布尔，无分配，无共享状态。
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

/**
 * @brief 复制完整参数字符串到固定数组。
 * @param[out] dst 独占输出数组借用，与源不重叠。
 * @param[in] dst_size 包含 NUL 的容量。
 * @param[in] value 调用期 NUL 借用，NULL 按空串。
 * @return 复制成功 true，参数无效或超长 false，超长时有效输出清空，无分配。
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

/**
 * @brief 分配格式化摄像头诊断并交付空结果。
 * @param[out] out_result 独占空错误槽借用，NULL 跳过，诊断由 cc_tool_result_cleanup 回收。
 * @param[in] fmt 调用期 printf 格式借用，NULL 跳过。
 * @param[in] ... 与格式匹配并覆盖两次格式化的参数借用。
 * @post 格式化/分配失败尝试复制回退文本，OOM 时 error 可 NULL，调用者设置 ok。
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

/**
 * @brief 解析采集参数并应用质量 70、保存与返回图像默认值。
 * @param[in] args_json 调用期 NUL JSON 借用，NULL/空采用默认值。
 * @param[out] out_args 必填独占结构借用，仅成功写入，质量小于 1 用 70，大于 100 用 100。
 * @param[out] error 可选独占诊断数组借用，失败时容量允许则写入。
 * @param[in] error_size 诊断数组容量。
 * @return 成功 true，JSON/类型/路径过长失败 false，临时树始终释放，无拥有权转交。
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

/**
 * @brief 同步采集 JPEG、按需保存并交付 Base64 图像 artifact。
 * @param[in] self 工具实例借用，覆盖调用期，保存时必须持有有效文件端口。
 * @param[in] args_json 调用期 NUL JSON 借用，NULL/空用默认值。
 * @param[in] ctx 调用期上下文借用，保存要求 workspace_dir。
 * @param[out] out_result 必填独占空结果，拥有字段由 cc_tool_result_cleanup 回收。
 * @return 输出 NULL 为参数错误，其余 CC_OK，失败见 ok/error，诊断 OOM 可 NULL。
 * @note JPEG 与临时 artifact 在各路径清理，已写文件保留，底层摄像头及文件端口负责并发。
 */
static cc_result_t camera_tool_call(
    void *self,
    const char *args_json,
    const cc_tool_context_t *ctx,
    cc_tool_result_t *out_result)
{
    camera_tool_t *tool = (camera_tool_t *)self;

    if (!out_result) {
        return cc_result_error(CC_ERR_INVALID_ARGUMENT, "Null camera tool result");
    }

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

        if (!artifact.id || !artifact.mime || (args.save_jpeg && !artifact.path)) {
            cc_media_artifact_cleanup(&artifact);
            board_camera_jpeg_cleanup(&image);
            return cc_result_error(CC_ERR_OUT_OF_MEMORY, "Failed to allocate camera artifact metadata");
        }

        cc_result_t rc = cc_tool_result_add_artifact(out_result, &artifact);
        cc_media_artifact_cleanup(&artifact);
        if (rc.code != CC_OK) {
            out_result->ok = 0;
            out_result->error = strdup(rc.message ? rc.message : cc_error_string(rc.code));
            cc_result_free(&rc);
            board_camera_jpeg_cleanup(&image);
            return cc_result_ok();
        }
        cc_result_free(&rc);
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

/**
 * @brief 回收摄像头工具实例并结束文件端口借用。
 * @param[in] self 转移拥有权的实例，可 NULL，调用者先等待进行中的调用完成。
 * @post 实例失效，文件端口仍由创建方拥有。
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

/**
 * @brief 创建持有文件端口借用视图的采集工具。
 * @param[in] ctx 工厂借用，要求 root-scoped 二进制写入，端口存活覆盖工具及其调用。
 * @param[out] out_tool 必填独占空槽，校验通过后清空，成功用 vtable destroy 回收。
 * @return CC_OK，参数/能力无效为参数错误，分配失败 OOM，诊断由 cc_result_free 回收。
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
