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
#include "cc/ports/cc_thread.h"

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
static cc_mutex_t s_ota_manager_mutex;

/** @brief 并发安装进程期锁并串行访问管理器上下文和一次原生 OTA 操作。
 * @return 成功持锁为 true，创建失败为 false，拥有诊断在本层释放。
 * @note 锁持续到进程结束，持锁期间不调用 service、HTTP 或应用回调。
 */
static bool ota_manager_lock(void)
{
    cc_result_t rc = cc_mutex_init_once(&s_ota_manager_mutex);
    bool ready = rc.code == CC_OK;
    cc_result_free(&rc);
    if (ready) cc_mutex_lock(s_ota_manager_mutex);
    return ready;
}

/** @brief 在管理器锁内保存截断诊断并转为 ERROR。
 * @param[in] fmt 必填 printf 格式借用，覆盖格式化调用。
 * @param[in] ... 对应参数，字符串保持到本次调用返回。
 * @post 最多保留 127 字节正文，原生更新句柄仍归本事务持有供 cancel。
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

/** @brief 将原生镜像状态映射为只读诊断文本。
 * @param[in] state 原生枚举，未知数值也可传入。
 * @return 进程期常量借用，未知值为 unknown，无释放责任。
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

/** @brief 按目标容量复制原生分区标签。
 * @param[out] dst 可空槽，非空且容量非零时保持 NUL 终止。
 * @param[in] dst_size 输出字节容量，含 NUL，零时无写入。
 * @param[in] partition 可空进程期分区借用，NULL 交付空串。
 * @post 超长标签截断，不转移分区拥有权。
 */
static void copy_label(char *dst, size_t dst_size, const esp_partition_t *partition)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", partition ? partition->label : "");
}

/** @brief 从定长原生描述字段复制终止文本，避免越界扫描。
 * @param[out] dst 可空输出，容量非零时有效槽写 NUL。
 * @param[in] dst_size 目标字节容量，含 NUL，零时跳过。
 * @param[in] src 可空 src_size 字节稳定借用，NULL 按空串。
 * @param[in] src_size 源最大可读字节数，源无 NUL 时按此界限截断。
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

/** @brief 在管理器锁内复制上传显示名并净化控制字节和路径分隔符。
 * @param[in] name 可空 NUL 文本借用，最多保存 95 字节，NULL 清空。
 * @post 内部名称独立保存，不打开文件，不借用原字符串。
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

/** @brief 复制允许字符组成的浏览器时间显示文本。
 * @param[out] dst 可空输出，非空容量非零时先清空。
 * @param[in] dst_size 输出字节容量，含 NUL，零时跳过。
 * @param[in] time_text 可空同步 NUL 文本借用，NULL 按空串。
 * @post 只保留数字、字母及允许标点并按容量截断，此过滤不验证日历语义。
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

/** @brief 将三十二字节 SHA-256 值格式化为小写十六进制。
 * @param[out] dst 至少 65 字节输出槽，可 NULL，此时跳过。
 * @param[in] dst_size 输出容量，低于 65 时保持输出。
 * @param[in] sha 可空三十二字节稳定借用，NULL 时跳过。
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

/** @brief 打开本次 NVS 句柄读取分区上传时间并立即关闭。
 * @param[in] partition 必填进程期分区借用，标签不能为空。
 * @param[out] dst 必填可写缓冲，参数有效时先清空，读取失败恢复空串。
 * @param[in] dst_size 字节容量，含 NUL，零为 INVALID_ARG。
 * @return 原生 open/get_str 结果或 INVALID_ARG，所有已打开句柄均关闭。
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

/** @brief 使用分区标签写入并提交 uploaded_at 元数据。
 * @param[in] partition 必填分区借用，标签非空。
 * @param[in] uploaded_at 必填非空 NUL 文本借用，覆盖 set_str 返回。
 * @return 原生 open/set/commit 首错或 INVALID_ARG，已打开句柄总会关闭。
 * @note NVS commit 错误按原生持久化状态解释，镜像和 boot 选择由上层另行核对。
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

/** @brief 尽力读取一份分区描述和 NVS 显示元数据。
 * @param[in] partition 可空进程期借用，NULL 只交付空字段。
 * @param[out] label 可空标签槽，按 label_size 截断终止。
 * @param[in] label_size 标签槽字节容量，零时跳过。
 * @param[out] app_name 可空应用名槽，描述读取失败为空。
 * @param[in] app_name_size 应用名槽字节容量，含 NUL。
 * @param[out] version 可空版本槽，描述读取失败为空。
 * @param[in] version_size 版本槽字节容量，含 NUL。
 * @param[out] sha256 可空十六进制槽，容量至少 65 时格式化 app_elf_sha256。
 * @param[in] sha256_size 摘要槽字节容量，含 NUL。
 * @param[out] uploaded_at 可空上传时间槽，NVS 失败为空。
 * @param[in] uploaded_at_size 时间槽字节容量，含 NUL。
 * @note 此摘要来自 app_desc 的 ELF 字段，上传整份 BIN 的摘要由 service 单独验证。
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

/** @brief 在管理器锁内刷新运行、启动和目标分区元数据。
 * @post 原生回滚状态未知时交付 unknown 和 rollback_pending=false，读取失败字段保持空。
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

/** @brief 在已有管理器锁持有内转调分区元数据刷新。
 * @pre 外层公开包装持有进程期 mutex，本 helper 不再次取得该锁。
 */
static void ota_manager_refresh_status_locked(void)
{
    refresh_partition_status();
}

/** @brief 将管理器 RAM 事务状态映射为只读文本。
 * @param[in] state 任意枚举值。
 * @return 进程期常量借用，未知值为 unknown。
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

/** @brief 在管理器锁内选择内部错误文本或常量 none。
 * @return 内部或进程期文本借用，调用者保留期间排除错误更新和 cancel。
 */
static const char *ota_manager_last_error_locked(void)
{
    return s_ota.error[0] ? s_ota.error : "none";
}

/** @brief 在管理器锁内调用原生 VALID 确认并刷新回滚诊断。
 * @return 原生确认结果，失败保留原生 otadata 状态供外层解释。
 * @pre service 已建立 flash 敏感操作的离线窗口。
 */
static esp_err_t ota_manager_mark_app_valid_locked(void)
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
    return err;
}

/** @brief 在管理器锁内选择目标并建立无原生句柄的上传事务。
 * @param[in] expected_size 非零镜像字节数，须不超过目标容量。
 * @param[in] upload_name 可空稳定 NUL 名称借用，内部净化复制。
 * @param[in] browser_time 可空稳定 NUL 时间借用，内部过滤复制。
 * @return 成功、状态错误、无目标或大小错误，失败可更新 ERROR 诊断。
 * @post UPLOADING 或尚存原生句柄时保留旧事务，其余情况重置 RAM 元数据。
 */
static esp_err_t ota_manager_begin_locked(size_t expected_size, const char *upload_name, const char *browser_time)
{
    if (s_ota.update_handle_open || s_ota.state == OTA_MANAGER_UPLOADING) {
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

/** @brief 在管理器锁内验证一块容量并同步交给原生 OTA writer。
 * @param[in] data 必填 len 字节借用，覆盖 esp_ota_write 返回。
 * @param[in] len 本块字节数，零时直接成功。
 * @return 状态/大小/魔数或原生错误，错误转 ERROR 并保留尚存句柄供 cancel。
 * @post 成功累计真实写入量，首个非空块先检查魔数再创建原生句柄。
 */
static esp_err_t ota_manager_write_locked(const void *data, size_t len)
{
    if (s_ota.state != OTA_MANAGER_UPLOADING || !s_ota.target || !data) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.bytes_written > s_ota.expected_size ||
        len > s_ota.expected_size - s_ota.bytes_written) {
        set_error("OTA upload exceeds declared size");
        return ESP_ERR_INVALID_SIZE;
    }
    if (len == 0) return ESP_OK;
    if (!s_ota.image_header_checked) {
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
            (unsigned)(((uint64_t)s_ota.bytes_written * 100U) / s_ota.expected_size) : 0;
        ESP_LOGI(TAG,
                 "OTA upload progress %u/%u bytes (%u%%)",
                 (unsigned)s_ota.bytes_written,
                 (unsigned)s_ota.expected_size,
                 pct);
        while (s_ota.next_report_bytes <= s_ota.bytes_written &&
               s_ota.next_report_bytes <= SIZE_MAX - 256U * 1024U) {
            s_ota.next_report_bytes += 256 * 1024;
        }
    }
    return ESP_OK;
}

/** @brief 在管理器锁内消费原生 writer 并提交下次启动分区。
 * @return 字节不齐为 INVALID_SIZE，其他失败保留原生结果，成功为 PENDING_REBOOT。
 * @post end 返回后句柄即失效，boot 提交失败保留已写镜像，时间元数据失败仅日志记录。
 */
static esp_err_t ota_manager_finish_locked(void)
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

    /* 原生 end 校验镜像，启用 Secure Boot 时还校验签名，失败先于 boot 选择。 */
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

/** @brief 在管理器锁内消费尚存 writer 并清空 RAM 事务。
 * @post 原生 abort 的 NOT_FOUND 表示句柄已不存在，已写 flash 和 boot 选择继续保留。
 */
static void ota_manager_cancel_locked(void)
{
    if (s_ota.update_handle_open) {
        (void)esp_ota_abort(s_ota.update_handle);
    }
    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.state = OTA_MANAGER_IDLE;
    refresh_partition_status();
}

/** @brief 枚举 APP 分区并按标签寻找原生描述对象。
 * @param[in] label 可空 NUL 借用，空串直接返回 NULL。
 * @return 原生分区表的进程期借用或 NULL，迭代器本次释放。
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

/** @brief 在管理器锁内写 boot 选择并更新 RAM 状态。
 * @param[in] label 可空 NUL 标签借用，空或未知为 NOT_FOUND。
 * @return 活动事务为 INVALID_STATE，其余原生结果，成功按是否运行分区选择 IDLE/PENDING_REBOOT。
 */
static esp_err_t ota_manager_set_boot_partition_locked(const char *label)
{
    if (s_ota.update_handle_open || s_ota.state == OTA_MANAGER_UPLOADING) {
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

/** @brief 尽力取得原生下一个更新分区的容量和标签。
 * @param[out] out_size 可空字节数槽，无目标写零。
 * @param[out] out_label 可空标签槽，容量非零时终止，必要时截断。
 * @param[in] label_size 标签槽字节容量，含 NUL，零时跳过。
 * @return 成功或 NOT_FOUND，不改变管理器事务，输出为独立值。
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

/** @brief 枚举全部 APP 分区，按容量填充独立值记录。
 * @param[out] out_partitions 可空数组，NULL 时只统计总数。
 * @param[in] max_partitions 可写元素个数，零时不填写。
 * @return 实际分区总数，可能超过容量，已填条目数为二者较小值。
 * @note 标签/描述/NVS 读取失败字段为空，运行及 boot 身份分别取样，不构成跨分区事务快照。
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

/** @brief 调用原生 esp_restart 触发设备复位。
 * @pre lifecycle owner 已执行所需关闭屏障，正常调用不返回。
 */
void ota_manager_reboot(void)
{
    esp_restart();
}

/** @brief 在外层管理器锁内复制 RAM 事务及已缓存分区字段。
 * @param[out] out_status 可空独占输出，非空清零并填充，所有文本按值保存。
 */
static void ota_manager_get_status_locked(ota_manager_status_t *out_status)
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

/** @brief 在进程期锁内刷新 running、boot 和当前 target 的独立元数据。
 * @post 原生读取失败的字段为空或 unknown，锁创建失败时保持上次快照。
 */
void ota_manager_refresh_status(void)
{
    if (!ota_manager_lock()) return;
    ota_manager_refresh_status_locked();
    cc_mutex_unlock(s_ota_manager_mutex);
}

/** @brief 借回最近错误文本或进程期常量 none。
 * @return 借用字符串，锁创建失败返回常量说明，调用者不释放。
 * @pre 保留内部文本期间由外层 owner 排除 begin、cancel 和其他错误更新。
 */
const char *ota_manager_last_error(void)
{
    if (!ota_manager_lock()) return "OTA manager lock unavailable";
    const char *error = ota_manager_last_error_locked();
    cc_mutex_unlock(s_ota_manager_mutex);
    return error;
}

/** @brief 串行确认运行镜像为 VALID，并刷新实际回滚状态。
 * @return 原生确认结果或 NO_MEM，持久提交成功后的恢复由 service 负责。
 * @pre 外层已关闭相机、交互、Web 与网络等 flash 敏感活动。
 */
esp_err_t ota_manager_mark_app_valid(void)
{
    if (!ota_manager_lock()) return ESP_ERR_NO_MEM;
    esp_err_t err = ota_manager_mark_app_valid_locked();
    cc_mutex_unlock(s_ota_manager_mutex);
    return err;
}

/** @brief 复制本次上传元数据并选择与运行分区不同的目标。
 * @param[in] expected_size 非零镜像总字节数，须不超过目标容量。
 * @param[in] upload_name 可空同步文本借用，净化后截断到内部容量。
 * @param[in] browser_time 可空同步时间文本借用，过滤后保存。
 * @return 成功、NO_MEM、INVALID_STATE、NOT_FOUND 或 INVALID_SIZE。
 * @note 打开的句柄包括 ERROR 状态都拒绝覆盖，调用者先 cancel 再开始新事务。
 */
esp_err_t ota_manager_begin(size_t expected_size, const char *upload_name, const char *browser_time)
{
    if (!ota_manager_lock()) return ESP_ERR_NO_MEM;
    esp_err_t err = ota_manager_begin_locked(expected_size, upload_name, browser_time);
    cc_mutex_unlock(s_ota_manager_mutex);
    return err;
}

/** @brief 在单次锁持有内按顺序写入一块镜像，首个非空块触发原生 begin。
 * @param[in] data 必填 len 字节稳定借用，零长度也要求非 NULL。
 * @param[in] len 本块字节数，零长度无原生副作用。
 * @return 原生结果、NO_MEM、状态/容量错误或无效魔数，失败可能已有 flash 前缀。
 * @pre 外层以事务 owner 排除块间 cancel、begin 和重排序，并停止 flash 敏感活动。
 */
esp_err_t ota_manager_write(const void *data, size_t len)
{
    if (!ota_manager_lock()) return ESP_ERR_NO_MEM;
    esp_err_t err = ota_manager_write_locked(data, len);
    cc_mutex_unlock(s_ota_manager_mutex);
    return err;
}

/** @brief 校验总字节、消费原生更新句柄并设置下次启动分区。
 * @return 原生结果或 NO_MEM，成功为 PENDING_REBOOT，NVS 时间写失败只记录诊断。
 * @note end 返回即消费句柄，选 boot 失败时镜像仍已写入，cancel 不回滚 otadata。
 */
esp_err_t ota_manager_finish(void)
{
    if (!ota_manager_lock()) return ESP_ERR_NO_MEM;
    esp_err_t err = ota_manager_finish_locked();
    cc_mutex_unlock(s_ota_manager_mutex);
    return err;
}

/** @brief 串行 abort 尚存原生句柄并清除 RAM 事务，刷新实际 boot 信息。
 * @post 不擦除已写镜像或撤销已选择的启动分区，锁创建失败时保留状态。
 */
void ota_manager_cancel(void)
{
    if (!ota_manager_lock()) return;
    ota_manager_cancel_locked();
    cc_mutex_unlock(s_ota_manager_mutex);
}

/** @brief 串行选择已存在的 APP 分区，选择当前 boot 时复用元数据。
 * @param[in] label 同步 NUL 标签借用，NULL/空或不存在为 NOT_FOUND。
 * @return 成功、NO_MEM、事务活动的 INVALID_STATE 或原生错误。
 * @pre 外层已建立写 otadata 的离线窗口，成功选择不能由 RAM cancel 撤销。
 */
esp_err_t ota_manager_set_boot_partition(const char *label)
{
    if (!ota_manager_lock()) return ESP_ERR_NO_MEM;
    esp_err_t err = ota_manager_set_boot_partition_locked(label);
    cc_mutex_unlock(s_ota_manager_mutex);
    return err;
}

/** @brief 在锁内复制管理器的一致 RAM 快照，字符串均为独立值。
 * @param[out] out_status 可空槽，非空先清零，锁创建失败交付 ERROR 和诊断。
 * @note 查询不建立 OTA 事务租约，分区元数据在 refresh 或状态变更时更新。
 */
void ota_manager_get_status(ota_manager_status_t *out_status)
{
    if (!out_status) return;
    memset(out_status, 0, sizeof(*out_status));
    if (!ota_manager_lock()) {
        out_status->state = OTA_MANAGER_ERROR;
        snprintf(out_status->error, sizeof(out_status->error), "OTA manager lock unavailable");
        return;
    }
    ota_manager_get_status_locked(out_status);
    cc_mutex_unlock(s_ota_manager_mutex);
}
