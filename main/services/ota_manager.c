#include "ota_manager.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"

typedef struct {
    ota_manager_state_t state;
    esp_partition_t target_copy;
    const esp_partition_t *target;
    esp_ota_handle_t update_handle;
    bool update_handle_open;
    size_t expected_size;
    size_t bytes_written;
    size_t next_report_bytes;
    bool image_header_checked;
    bool write_diag_logged;
    char upload_name[96];
    char error[128];
    char running_label[24];
    char running_app_name[33];
    char running_version[33];
    char running_sha256[65];
    char running_uploaded_at[40];
    char running_ota_state[24];
    char target_label[24];
    char target_app_name[33];
    char target_version[33];
    char target_sha256[65];
    char target_uploaded_at[40];
    char boot_label[24];
    char boot_app_name[33];
    char boot_version[33];
    char boot_sha256[65];
    char boot_uploaded_at[40];
    char upload_browser_time[40];
    bool rollback_pending;
} ota_manager_ctx_t;

static const char *TAG = "ota_manager";
static const char *OTA_META_NAMESPACE = "ota_meta";
static ota_manager_ctx_t s_ota;

/*
 * 格式化错误信息并设置状态为 ERROR。支持 printf 风格格式化。
 */
static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_ota.error, sizeof(s_ota.error), fmt, ap);
    va_end(ap);
    s_ota.state = OTA_MANAGER_ERROR;
    ESP_LOGE(TAG, "%s", s_ota.error);
}

/*
 * 将 esp_ota_img_states_t 枚举转换为可读字符串。
 * 返回: "new"/"pending_verify"/"valid"/"invalid"/"aborted"/"undefined"/"unknown"
 */
static const char *ota_image_state_string(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined";
    default: return "unknown";
    }
}

/*
 * 安全复制分区标签到输出缓冲区。
 */
static void copy_label(char *dst, size_t dst_size, const esp_partition_t *partition)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", partition ? partition->label : "");
}

/*
 * 安全复制 app_desc 字段（自动截断到 src_size，确保 \0 结尾）。
 */
static void copy_desc_field(char *dst, size_t dst_size, const char *src, size_t src_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    size_t n = 0;
    while (src && n < src_size && src[n] && n + 1 < dst_size) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
}

/*
 * 复制并净化上传文件名（替换控制字符和路径分隔符为 _）。
 */
static void copy_upload_name(const char *name)
{
    s_ota.upload_name[0] = '\0';
    if (!name || !name[0]) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; name[i] && out + 1 < sizeof(s_ota.upload_name); i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '/' || c == '\\') {
            s_ota.upload_name[out++] = '_';
        } else {
            s_ota.upload_name[out++] = (char)c;
        }
    }
    s_ota.upload_name[out] = '\0';
}

/*
 * 过滤浏览器上传时间字符串，只保留 ISO8601 安全字符。
 */
static void copy_browser_time(char *dst, size_t dst_size, const char *time_text)
{
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (!time_text || !time_text[0]) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; time_text[i] && out + 1 < dst_size; i++) {
        unsigned char c = (unsigned char)time_text[i];
        if ((c >= '0' && c <= '9') ||
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            c == '-' || c == ':' || c == '.' || c == 'Z' || c == '+' || c == '_') {
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
}

/*
 * 将 SHA256 字节数组转换为十六进制字符串。
 */
static void sha256_to_hex(char *dst, size_t dst_size, const uint8_t *sha)
{
    static const char hex[] = "0123456789abcdef";
    if (!dst || dst_size < 65 || !sha) {
        return;
    }
    for (size_t i = 0; i < 32; i++) {
        dst[i * 2] = hex[(sha[i] >> 4) & 0x0f];
        dst[i * 2 + 1] = hex[sha[i] & 0x0f];
    }
    dst[64] = '\0';
}

/*
 * 从 NVS 读取指定分区的 uploaded_at 时间戳。
 */
static esp_err_t read_partition_uploaded_at(const esp_partition_t *partition, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    dst[0] = '\0';
    if (!partition || !partition->label[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(OTA_META_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = dst_size;
    err = nvs_get_str(handle, partition->label, dst, &len);
    nvs_close(handle);
    if (err != ESP_OK) {
        dst[0] = '\0';
    }
    return err;
}

/*
 * 将 uploaded_at 时间戳写入 NVS（以分区标签名作为 key）。
 */
static esp_err_t write_partition_uploaded_at(const esp_partition_t *partition, const char *uploaded_at)
{
    if (!partition || !partition->label[0] || !uploaded_at || !uploaded_at[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(OTA_META_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, partition->label, uploaded_at);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/*
 * 读取分区 app_desc 信息（项目名/版本/SHA256/上传时间）并填充输出字段。
 */
static void read_partition_info(const esp_partition_t *partition,
                                char *label,
                                size_t label_size,
                                char *app_name,
                                size_t app_name_size,
                                char *version,
                                size_t version_size,
                                char *sha256,
                                size_t sha256_size,
                                char *uploaded_at,
                                size_t uploaded_at_size)
{
    copy_label(label, label_size, partition);
    if (app_name && app_name_size > 0) app_name[0] = '\0';
    if (version && version_size > 0) version[0] = '\0';
    if (sha256 && sha256_size > 0) sha256[0] = '\0';
    if (uploaded_at && uploaded_at_size > 0) uploaded_at[0] = '\0';
    if (!partition) {
        return;
    }

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(partition, &desc) == ESP_OK) {
        copy_desc_field(app_name, app_name_size, desc.project_name, sizeof(desc.project_name));
        copy_desc_field(version, version_size, desc.version, sizeof(desc.version));
        sha256_to_hex(sha256, sha256_size, desc.app_elf_sha256);
    }
    (void)read_partition_uploaded_at(partition, uploaded_at, uploaded_at_size);
}

/*
 * 刷新 running/boot/target 分区元数据（标签、版本、SHA256 等）。
 */
static void refresh_partition_status(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    s_ota.running_ota_state[0] = '\0';
    read_partition_info(running,
                        s_ota.running_label,
                        sizeof(s_ota.running_label),
                        s_ota.running_app_name,
                        sizeof(s_ota.running_app_name),
                        s_ota.running_version,
                        sizeof(s_ota.running_version),
                        s_ota.running_sha256,
                        sizeof(s_ota.running_sha256),
                        s_ota.running_uploaded_at,
                        sizeof(s_ota.running_uploaded_at));
    read_partition_info(boot,
                        s_ota.boot_label,
                        sizeof(s_ota.boot_label),
                        s_ota.boot_app_name,
                        sizeof(s_ota.boot_app_name),
                        s_ota.boot_version,
                        sizeof(s_ota.boot_version),
                        s_ota.boot_sha256,
                        sizeof(s_ota.boot_sha256),
                        s_ota.boot_uploaded_at,
                        sizeof(s_ota.boot_uploaded_at));
    read_partition_info(s_ota.target,
                        s_ota.target_label,
                        sizeof(s_ota.target_label),
                        s_ota.target_app_name,
                        sizeof(s_ota.target_app_name),
                        s_ota.target_version,
                        sizeof(s_ota.target_version),
                        s_ota.target_sha256,
                        sizeof(s_ota.target_sha256),
                        s_ota.target_uploaded_at,
                        sizeof(s_ota.target_uploaded_at));

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        snprintf(s_ota.running_ota_state,
                 sizeof(s_ota.running_ota_state),
                 "%s",
                 ota_image_state_string(state));
        s_ota.rollback_pending = state == ESP_OTA_IMG_PENDING_VERIFY;
    } else {
        snprintf(s_ota.running_ota_state,
                 sizeof(s_ota.running_ota_state),
                 "%s",
                 "unknown");
        s_ota.rollback_pending = false;
    }
}

/*
 * 更新分区状态信息：读取 running/boot/target 分区标签等。
 */
void ota_manager_refresh_status(void)
{
    refresh_partition_status();
}

/*
 * 将 ota_image_state 枚举转换为可读字符串。
 */
const char *ota_manager_state_string(ota_manager_state_t state)
{
    switch (state) {
    case OTA_MANAGER_IDLE: return "idle";
    case OTA_MANAGER_UPLOADING: return "uploading";
    case OTA_MANAGER_PENDING_REBOOT: return "pending_reboot";
    case OTA_MANAGER_ERROR: return "error";
    default: return "unknown";
    }
}

/*
 * 获取上次错误描述，无错误时返回 "none"。
 */
const char *ota_manager_last_error(void)
{
    return s_ota.error[0] ? s_ota.error : "none";
}

/*
 * 更新正在运行的分区信息：调用 esp_ota_get_running_partition。
 */
void ota_manager_mark_app_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK) {
        ESP_LOGI(TAG, "running partition=%s ota_state=%d", running->label, (int)state);
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "app marked valid, rollback cancelled");
    } else if (err != ESP_ERR_OTA_ROLLBACK_FAILED && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mark app valid returned: %s", esp_err_to_name(err));
    }
    refresh_partition_status();
}

/*
 * 准备 OTA 升级：重置内部状态，校验升级分区与固件大小，复制上传元数据（文件名、浏览器时间），
 * 进入 UPLOADING 状态并刷新分区状态。
 * 参数: expected_size - 预期固件总字节数
 *       upload_name  - 上传固件的文件名字符串
 *       browser_time - 浏览器端时间戳字符串（可为 NULL）
 * 返回: ESP_OK 成功 / ESP_ERR_INVALID_STATE 已有升级进行中 /
 *       ESP_ERR_NOT_FOUND 无可用升级分区 / ESP_ERR_INVALID_SIZE 固件大小超出目标分区
 */
esp_err_t ota_manager_begin(size_t expected_size, const char *upload_name, const char *browser_time)
{
    if (s_ota.state == OTA_MANAGER_UPLOADING && s_ota.target) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.state = OTA_MANAGER_IDLE;
    copy_upload_name(upload_name);
    copy_browser_time(s_ota.upload_browser_time, sizeof(s_ota.upload_browser_time), browser_time);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        set_error("no OTA update partition");
        return ESP_ERR_NOT_FOUND;
    }
    if (running && target->address == running->address) {
        set_error("target partition equals running partition");
        return ESP_ERR_INVALID_STATE;
    }
    if (expected_size == 0 || expected_size > target->size) {
        set_error("OTA image size %u exceeds target size %u",
                  (unsigned)expected_size,
                  (unsigned)target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    s_ota.target_copy = *target;
    s_ota.target = &s_ota.target_copy;
    s_ota.expected_size = expected_size;
    s_ota.bytes_written = 0;
    s_ota.next_report_bytes = 256 * 1024;
    s_ota.image_header_checked = false;
    s_ota.write_diag_logged = false;
    s_ota.state = OTA_MANAGER_UPLOADING;
    refresh_partition_status();
    ESP_LOGI(TAG,
             "OTA offline flash prepared target=%s expected=%u partition=%p internal=%d external=%d erase=%u",
             s_ota.target->label,
             (unsigned)expected_size,
             s_ota.target,
             esp_ptr_internal(s_ota.target) ? 1 : 0,
             esp_ptr_external_ram(s_ota.target) ? 1 : 0,
             (unsigned)s_ota.target->erase_size);
    return ESP_OK;
}

/*
 * 写入 OTA 固件数据块：首包校验镜像头魔数，首次写入时打开 OTA 句柄（esp_ota_begin），
 * 逐块写入目标分区并跟踪进度，每 256KB 报告一次进度百分比。
 * 参数: data - 固件数据缓冲区指针
 *       len  - 本次写入字节数
 * 返回: ESP_OK 成功 / ESP_ERR_INVALID_STATE 状态不正确或 data 为空 /
 *       ESP_ERR_INVALID_SIZE 写入量超出声明大小 / ESP_ERR_OTA_VALIDATE_FAILED 镜像头魔数无效 /
 *       其他 ESP-IDF OTA 写入错误码
 */
esp_err_t ota_manager_write(const void *data, size_t len)
{
    if (s_ota.state != OTA_MANAGER_UPLOADING || !s_ota.target || !data) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.bytes_written + len > s_ota.expected_size) {
        set_error("OTA upload exceeds declared size");
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_ota.image_header_checked && len > 0) {
        s_ota.image_header_checked = true;
        if (((const uint8_t *)data)[0] != ESP_IMAGE_HEADER_MAGIC) {
            set_error("OTA image has invalid magic byte: 0x%02x", ((const uint8_t *)data)[0]);
            return ESP_ERR_OTA_VALIDATE_FAILED;
        }
    }

    if (!s_ota.write_diag_logged) {
        s_ota.write_diag_logged = true;
        ESP_LOGI(TAG,
                 "OTA first write buffer=%p len=%u internal=%d external=%d",
                 data,
                 (unsigned)len,
                 esp_ptr_internal(data) ? 1 : 0,
                 esp_ptr_external_ram(data) ? 1 : 0);
    }

    if (!s_ota.update_handle_open) {
        esp_err_t begin_err = esp_ota_begin(s_ota.target, s_ota.expected_size, &s_ota.update_handle);
        if (begin_err != ESP_OK) {
            set_error("esp_ota_begin(%s) failed: %s",
                      s_ota.target->label,
                      esp_err_to_name(begin_err));
            return begin_err;
        }
        s_ota.update_handle_open = true;
        ESP_LOGI(TAG,
                 "esp_ota_begin ok target=%s expected=%u",
                 s_ota.target->label,
                 (unsigned)s_ota.expected_size);
    }

    esp_err_t err = esp_ota_write(s_ota.update_handle, data, len);
    if (err != ESP_OK) {
        set_error("esp_ota_write failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ota.bytes_written += len;
    if (s_ota.bytes_written >= s_ota.next_report_bytes || s_ota.bytes_written == s_ota.expected_size) {
        unsigned pct = s_ota.expected_size ?
            (unsigned)((s_ota.bytes_written * 100U) / s_ota.expected_size) : 0;
        ESP_LOGI(TAG,
                 "OTA upload progress %u/%u bytes (%u%%)",
                 (unsigned)s_ota.bytes_written,
                 (unsigned)s_ota.expected_size,
                 pct);
        while (s_ota.next_report_bytes <= s_ota.bytes_written) {
            s_ota.next_report_bytes += 256 * 1024;
        }
    }
    return ESP_OK;
}

/*
 * 完成 OTA 升级：校验写入字节数与预期一致，关闭 OTA 写入句柄（esp_ota_end），
 * 将目标分区设为下次启动分区（esp_ota_set_boot_partition），写入上传时间元数据（如有），
 * 进入 PENDING_REBOOT 状态等待用户重启。
 * 参数: 无
 * 返回: ESP_OK 成功 / ESP_ERR_INVALID_STATE 状态不正确 /
 *       ESP_ERR_INVALID_SIZE 写入量与预期不符（固件截断） /
 *       其他 ESP-IDF OTA 完成或分区设置错误码
 */
esp_err_t ota_manager_finish(void)
{
    if (s_ota.state != OTA_MANAGER_UPLOADING || !s_ota.target || !s_ota.update_handle_open) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.bytes_written != s_ota.expected_size) {
        set_error("OTA upload truncated: got %u expected %u",
                  (unsigned)s_ota.bytes_written,
                  (unsigned)s_ota.expected_size);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * esp_ota_end performs the ESP image validation. With Secure Boot enabled,
     * ESP-IDF also verifies the appended image signature here and returns
     * ESP_ERR_OTA_VALIDATE_FAILED before the boot partition can be selected.
     */
    esp_err_t err = esp_ota_end(s_ota.update_handle);
    s_ota.update_handle = 0;
    s_ota.update_handle_open = false;
    if (err != ESP_OK) {
        set_error("esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(s_ota.target);
    if (err != ESP_OK) {
        set_error("esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_ota.upload_browser_time[0]) {
        esp_err_t meta_err = write_partition_uploaded_at(s_ota.target, s_ota.upload_browser_time);
        if (meta_err != ESP_OK) {
            ESP_LOGW(TAG,
                     "write OTA uploaded_at for %s failed: %s",
                     s_ota.target->label,
                     esp_err_to_name(meta_err));
        }
    }

    s_ota.state = OTA_MANAGER_PENDING_REBOOT;
    refresh_partition_status();
    ESP_LOGI(TAG, "OTA upload complete, next boot partition=%s", s_ota.target->label);
    return ESP_OK;
}

/*
 * 取消当前 OTA：abort 句柄，重置状态。
 */
void ota_manager_cancel(void)
{
    if (s_ota.update_handle_open) {
        (void)esp_ota_abort(s_ota.update_handle);
    }
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.state = OTA_MANAGER_IDLE;
    refresh_partition_status();
}

/*
 * 以指定标签查找 APP 类型分区。
 */
static const esp_partition_t *find_app_partition_by_label(const char *label)
{
    if (!label || !label[0]) {
        return NULL;
    }
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                     ESP_PARTITION_SUBTYPE_ANY,
                                                     NULL);
    const esp_partition_t *found = NULL;
    while (it) {
        const esp_partition_t *partition = esp_partition_get(it);
        if (partition && strcmp(partition->label, label) == 0) {
            found = partition;
            break;
        }
        it = esp_partition_next(it);
    }
    if (it) {
        esp_partition_iterator_release(it);
    }
    return found;
}

/*
 * 设置启动分区为 label 指定的分区。
 */
esp_err_t ota_manager_set_boot_partition(const char *label)
{
    if (s_ota.state == OTA_MANAGER_UPLOADING) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *partition = find_app_partition_by_label(label);
    if (!partition) {
        set_error("partition not found: %s", label ? label : "");
        return ESP_ERR_NOT_FOUND;
    }

    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (boot && boot->address == partition->address) {
        s_ota.target = partition;
        s_ota.error[0] = '\0';
        s_ota.state = (running && running->address == partition->address) ?
            OTA_MANAGER_IDLE : OTA_MANAGER_PENDING_REBOOT;
        refresh_partition_status();
        ESP_LOGI(TAG, "boot partition already selected: %s", partition->label);
        return ESP_OK;
    }

    esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        set_error("esp_ota_set_boot_partition(%s) failed: %s",
                  partition->label,
                  esp_err_to_name(err));
        return err;
    }

    s_ota.target = partition;
    s_ota.error[0] = '\0';
    s_ota.state = (running && running->address == partition->address) ?
        OTA_MANAGER_IDLE : OTA_MANAGER_PENDING_REBOOT;
    refresh_partition_status();
    ESP_LOGI(TAG, "boot partition selected: %s", partition->label);
    return ESP_OK;
}

/*
 * 获取下一个 OTA 更新分区的信息（大小和标签）。
 */
esp_err_t ota_manager_get_next_update_partition_info(size_t *out_size, char *out_label, size_t label_size)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        if (out_size) {
            *out_size = 0;
        }
        if (out_label && label_size > 0) {
            out_label[0] = '\0';
        }
        return ESP_ERR_NOT_FOUND;
    }
    if (out_size) {
        *out_size = target->size;
    }
    if (out_label && label_size > 0) {
        snprintf(out_label, label_size, "%s", target->label);
    }
    return ESP_OK;
}

/*
 * 枚举所有 APP 分区并填充信息数组。
 */
size_t ota_manager_get_partitions(ota_manager_partition_info_t *out_partitions, size_t max_partitions)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP,
                                                     ESP_PARTITION_SUBTYPE_ANY,
                                                     NULL);
    size_t count = 0;
    while (it) {
        const esp_partition_t *partition = esp_partition_get(it);
        if (partition) {
            if (out_partitions && count < max_partitions) {
                ota_manager_partition_info_t *info = &out_partitions[count];
                memset(info, 0, sizeof(*info));
                read_partition_info(partition,
                                    info->label,
                                    sizeof(info->label),
                                    info->app_name,
                                    sizeof(info->app_name),
                                    info->version,
                                    sizeof(info->version),
                                    info->sha256,
                                    sizeof(info->sha256),
                                    info->uploaded_at,
                                    sizeof(info->uploaded_at));
                info->size = partition->size;
                info->running = running && running->address == partition->address;
                info->boot = boot && boot->address == partition->address;
                info->selected = info->boot;
            }
            count++;
        }
        it = esp_partition_next(it);
    }
    if (it) {
        esp_partition_iterator_release(it);
    }
    return count;
}

/*
 * 通过 esp_restart() 重启设备。
 */
void ota_manager_reboot(void)
{
    esp_restart();
}

/*
 * 填充 status 结构体包含 OTA 状态和各分区元数据。
 */
void ota_manager_get_status(ota_manager_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->state = s_ota.state;
    out_status->bytes_written = s_ota.bytes_written;
    out_status->expected_size = s_ota.expected_size;
    out_status->target_size = s_ota.target ? s_ota.target->size : 0;
    snprintf(out_status->upload_name, sizeof(out_status->upload_name), "%s", s_ota.upload_name);
    snprintf(out_status->target_label, sizeof(out_status->target_label), "%s", s_ota.target_label);
    snprintf(out_status->target_app_name, sizeof(out_status->target_app_name), "%s", s_ota.target_app_name);
    snprintf(out_status->target_version, sizeof(out_status->target_version), "%s", s_ota.target_version);
    snprintf(out_status->target_sha256, sizeof(out_status->target_sha256), "%s", s_ota.target_sha256);
    snprintf(out_status->target_uploaded_at, sizeof(out_status->target_uploaded_at), "%s", s_ota.target_uploaded_at);
    snprintf(out_status->error, sizeof(out_status->error), "%s", s_ota.error);
    snprintf(out_status->running_label, sizeof(out_status->running_label), "%s", s_ota.running_label);
    snprintf(out_status->running_app_name, sizeof(out_status->running_app_name), "%s", s_ota.running_app_name);
    snprintf(out_status->running_version, sizeof(out_status->running_version), "%s", s_ota.running_version);
    snprintf(out_status->running_sha256, sizeof(out_status->running_sha256), "%s", s_ota.running_sha256);
    snprintf(out_status->running_uploaded_at, sizeof(out_status->running_uploaded_at), "%s", s_ota.running_uploaded_at);
    snprintf(out_status->running_ota_state, sizeof(out_status->running_ota_state), "%s", s_ota.running_ota_state);
    snprintf(out_status->boot_label, sizeof(out_status->boot_label), "%s", s_ota.boot_label);
    snprintf(out_status->boot_app_name, sizeof(out_status->boot_app_name), "%s", s_ota.boot_app_name);
    snprintf(out_status->boot_version, sizeof(out_status->boot_version), "%s", s_ota.boot_version);
    snprintf(out_status->boot_sha256, sizeof(out_status->boot_sha256), "%s", s_ota.boot_sha256);
    snprintf(out_status->boot_uploaded_at, sizeof(out_status->boot_uploaded_at), "%s", s_ota.boot_uploaded_at);
    out_status->rollback_pending = s_ota.rollback_pending;
}
