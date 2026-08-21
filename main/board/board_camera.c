#include "board_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "board_camera_jpeg.h"
#include "board_camera_rgb565.h"
#include "board_ui.h"
#include "dev_camera.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <linux/videodev2.h>
#include "sdkconfig.h"

#define BOARD_CAMERA_BUFFER_COUNT 4
#ifndef BOARD_CAMERA_PREVIEW_STACK
#define BOARD_CAMERA_PREVIEW_STACK 4096
#endif

#ifndef BOARD_CAMERA_PREVIEW_TASK_PRIO
#define BOARD_CAMERA_PREVIEW_TASK_PRIO 3
#endif

#ifndef BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS
#define BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS 1
#endif

#ifndef BOARD_CAMERA_DEVICE_READY_RETRY_COUNT
#define BOARD_CAMERA_DEVICE_READY_RETRY_COUNT 100
#endif

#ifndef BOARD_CAMERA_DEVICE_READY_RETRY_MS
#define BOARD_CAMERA_DEVICE_READY_RETRY_MS 50
#endif

#define BOARD_CAMERA_DEFAULT_DEV_PATH "/dev/video2"
#define BOARD_CAMERA_PREVIEW_EXITED_BIT BIT0
#define BOARD_CAMERA_STOP_TIMEOUT_MS 1000U
#ifdef CONFIG_CACHE_L1_DCACHE_LINE_SIZE
#define BOARD_CAMERA_CACHE_ALIGN CONFIG_CACHE_L1_DCACHE_LINE_SIZE
#else
#define BOARD_CAMERA_CACHE_ALIGN 64U
#endif

/*
 * Frame buffer placement policy.
 *
 * - The V4L2 mmap buffers are owned by the camera driver; this file can only
 *   inspect/use the returned pointer. On your current logs they are already in
 *   PSRAM (0x501...).
 * - The application-owned RGB565 buffers below are allocated here:
 *     s_latest_frame
 *     s_preview_frames[]
 *     temporary jpeg_raw_rgb565
 *   Keep them in PSRAM to avoid consuming scarce internal RAM.
 *
 * Set BOARD_CAMERA_FRAME_BUFFER_STRICT_PSRAM=0 if you prefer booting even when
 * PSRAM allocation fails. The default here is strict because this project is
 * tuned for ESP32S31 with PSRAM.
 */
#ifndef BOARD_CAMERA_FRAME_BUFFER_PREFER_PSRAM
#define BOARD_CAMERA_FRAME_BUFFER_PREFER_PSRAM 1
#endif

#ifndef BOARD_CAMERA_FRAME_BUFFER_STRICT_PSRAM
#define BOARD_CAMERA_FRAME_BUFFER_STRICT_PSRAM 1
#endif

#if CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM && configSUPPORT_STATIC_ALLOCATION
#define BOARD_CAMERA_STACK_IN_PSRAM 1
#else
#define BOARD_CAMERA_STACK_IN_PSRAM 0
#endif


/*
 * 当前板级配置报告摄像头输出为 240x240 RGBP�? * RGBP 是大�?RGB565，LCD 显示路径通常按小�?RGB565 处理�? * 所以默认开启字节交换�? */
#ifndef BOARD_CAMERA_SWAP_RGB565_BYTES
#define BOARD_CAMERA_SWAP_RGB565_BYTES -1
#endif
/*
 * -1: decide from the returned V4L2 fourcc.
 *  0: force no RGB565 byte swap.
 *  1: force RGB565 byte swap.
 */

/*
 * 如果画面结构正确，但红蓝通道反了，可以把这个值改�?1�? */
#ifndef BOARD_CAMERA_SWAP_RB
#define BOARD_CAMERA_SWAP_RB 0
#endif

/*
 * 如需降低预览帧率，可以改�?15 �?20�? * 240x240 RGB565 对当前目标压力不大，所以默认保�?24 FPS�? */
#ifndef BOARD_CAMERA_PREVIEW_FPS
#define BOARD_CAMERA_PREVIEW_FPS 24
#endif

/*
 * 预览稳定缓冲数量�? * 不直接把 V4L2 mmap buffer 交给 LCD，避�?QBUF 后摄像头复用同一块内存�? */
#define BOARD_CAMERA_PREVIEW_BUFFER_COUNT 2

typedef struct {
    uint8_t *data;
    size_t length;
} board_camera_v4l2_buffer_t;

static const char *TAG = "board_camera";

static int s_video_fd = -1;
static board_camera_v4l2_buffer_t s_buffers[BOARD_CAMERA_BUFFER_COUNT];

static uint8_t *s_latest_frame;
static uint8_t *s_preview_frames[BOARD_CAMERA_PREVIEW_BUFFER_COUNT];

static size_t s_frame_bytes;
static size_t s_src_stride_bytes;
static size_t s_src_image_bytes;

static int s_width;
static int s_height;
static bool s_src_needs_byte_swap;

static SemaphoreHandle_t s_frame_mutex;
static SemaphoreHandle_t s_state_mutex;
static TaskHandle_t s_preview_task;
static EventGroupHandle_t s_preview_events;

static bool s_camera_ready;
static bool s_streaming;
static bool s_preview_paused;
static bool s_has_latest_frame;
static board_camera_stats_t s_stats;
static board_camera_callbacks_t s_callbacks;

static bool camera_streaming_load(void)
{
    return __atomic_load_n(&s_streaming, __ATOMIC_ACQUIRE);
}

static void camera_streaming_store(bool value)
{
    __atomic_store_n(&s_streaming, value, __ATOMIC_RELEASE);
}

static bool camera_preview_paused_load(void)
{
    return __atomic_load_n(&s_preview_paused, __ATOMIC_ACQUIRE);
}

static void camera_preview_paused_store(bool value)
{
    __atomic_store_n(&s_preview_paused, value, __ATOMIC_RELEASE);
}

static bool camera_ready_load(void)
{
    return __atomic_load_n(&s_camera_ready, __ATOMIC_ACQUIRE);
}

static void camera_ready_store(bool value)
{
    __atomic_store_n(&s_camera_ready, value, __ATOMIC_RELEASE);
}

static TaskHandle_t camera_preview_task_load(void)
{
    return __atomic_load_n(&s_preview_task, __ATOMIC_ACQUIRE);
}

static void camera_preview_task_store(TaskHandle_t task)
{
    __atomic_store_n(&s_preview_task, task, __ATOMIC_RELEASE);
}

static void camera_stats_snapshot(board_camera_stats_t *out_stats)
{
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        *out_stats = s_stats;
        xSemaphoreGive(s_state_mutex);
    }
}

static void camera_stats_record_drop(void)
{
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_stats.preview_drops++;
        xSemaphoreGive(s_state_mutex);
    }
}

static void camera_stats_record_preview(uint32_t frame_count, bool has_fps, uint32_t fps)
{
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_stats.preview_frames = frame_count;
        if (has_fps) s_stats.preview_fps = fps;
        xSemaphoreGive(s_state_mutex);
    }
}

static void camera_stats_set_latest_jpeg(size_t jpeg_bytes)
{
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_stats.latest_jpeg_bytes = jpeg_bytes;
        xSemaphoreGive(s_state_mutex);
    }
}

/*
 * 设置摄像头回调函数。
 *
 * 传入非空指针时注册预览帧更新和统计更新两个回调；传入 NULL 则清空所有回调。
 * 此函数仅拷贝回调结构体内容，调用者仍需保证回调函数指针在其生命周期内有效。
 *
 * 参数:
 *   callbacks - 回调结构体指针，包含 preview_update、stats_update 及 user_data。
 *               NULL 表示取消所有回调。
 */
void board_camera_set_callbacks(const board_camera_callbacks_t *callbacks)
{
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "timed out updating camera callbacks");
        return;
    }
    if (callbacks) {
        s_callbacks = *callbacks;
    } else {
        memset(&s_callbacks, 0, sizeof(s_callbacks));
    }
    if (s_state_mutex) xSemaphoreGive(s_state_mutex);
}

/*
 * 通知上层模块摄像头统计信息已更新。
 *
 * 如果已通过 board_camera_set_callbacks 注册 stats_update 回调，
 * 则将当前全局统计结构 s_stats 通过回调传递给调用者。
 */
static void camera_notify_stats(void)
{
    board_camera_callbacks_t callbacks = {0};
    board_camera_stats_t stats = {0};
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        callbacks = s_callbacks;
        stats = s_stats;
        xSemaphoreGive(s_state_mutex);
    }
    if (callbacks.stats_update) callbacks.stats_update(&stats, callbacks.user_data);
}

/*
 * 通知上层模块有新的预览帧可用。
 *
 * 如果已注册 preview_update 回调，则将 RGB565 预览帧数据及尺寸传递给调用者
 * （典型用途是刷新 LCD 显示）。
 *
 * 参数:
 *   rgb565 - 指向 RGB565 格式帧缓冲的指针
 *   width  - 帧宽度（像素）
 *   height - 帧高度（像素）
 */
static void camera_notify_preview(const void *rgb565, int width, int height)
{
    board_camera_callbacks_t callbacks = {0};
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        callbacks = s_callbacks;
        xSemaphoreGive(s_state_mutex);
    }
    if (callbacks.preview_update) callbacks.preview_update(rgb565, width, height, callbacks.user_data);
}

/*
 * 创建 FreeRTOS 任务，优先在 PSRAM 中分配任务栈。
 *
 * 当 BOARD_CAMERA_STACK_IN_PSRAM 使能时，使用 xTaskCreateWithCaps 尝试在
 * PSRAM 中创建任务；若失败则回退到内部 RAM。否则使用标准 xTaskCreate。
 *
 * 参数:
 *   task       - 任务函数入口
 *   name       - 任务名称（调试用）
 *   stack_bytes- 任务栈大小（字节）
 *   arg        - 传递给任务函数的参数
 *   priority   - 任务优先级
 *   out_handle - 输出任务句柄
 * 返回: pdPASS 表示成功，否则失败。
 */
static BaseType_t camera_create_task(TaskFunction_t task,
                                     const char *name,
                                     uint32_t stack_bytes,
                                     void *arg,
                                     UBaseType_t priority,
                                     TaskHandle_t *out_handle)
{
#if BOARD_CAMERA_STACK_IN_PSRAM
    BaseType_t ok = xTaskCreateWithCaps(task,
                                        name,
                                        stack_bytes,
                                        arg,
                                        priority,
                                        out_handle,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok == pdPASS) return ok;

    ESP_LOGW(TAG, "PSRAM stack allocation failed for %s, retrying internal", name);
    return xTaskCreateWithCaps(task,
                               name,
                               stack_bytes,
                               arg,
                               priority,
                               out_handle,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(task, name, stack_bytes, arg, priority, out_handle);
#endif
}

/*
 * 删除当前正在执行的任务自身。
 *
 * 根据 BOARD_CAMERA_STACK_IN_PSRAM 选择 vTaskDeleteWithCaps 或 vTaskDelete，
 * 确保 PSRAM 栈任务能正确释放 PSRAM 资源。
 */
static void camera_task_delete_self(void)
{
#if BOARD_CAMERA_STACK_IN_PSRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

/*
 * 将 POSIX errno 转换为 ESP-IDF 错误码。
 *
 * 映射关系：ENOMEM→ESP_ERR_NO_MEM, EINVAL→ESP_ERR_INVALID_ARG,
 * EAGAIN→ESP_ERR_TIMEOUT, ENODEV→ESP_ERR_NOT_FOUND, 其他→ESP_FAIL。
 *
 * 返回: 对应的 esp_err_t 错误码。
 */
static esp_err_t errno_to_esp_err(void)
{
    switch (errno) {
    case ENOMEM:
        return ESP_ERR_NO_MEM;
    case EINVAL:
        return ESP_ERR_INVALID_ARG;
    case EAGAIN:
        return ESP_ERR_TIMEOUT;
    case ENODEV:
        return ESP_ERR_NOT_FOUND;
    default:
        return ESP_FAIL;
    }
}

/*
 * 将 V4L2 fourcc 四字符编码转换为可读字符串。
 *
 * 参数:
 *   fourcc - V4L2 pixelformat 值（如 V4L2_PIX_FMT_RGB565）
 *   out    - 输出缓冲区，至少 5 字节，函数写入以 '\0' 结尾的 4 字符标识
 */
static void log_fourcc(uint32_t fourcc, char out[5])
{
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = '\0';
}

/*
 * 分配帧缓冲区，优先使用 PSRAM 以减少内部 RAM 占用。
 *
 * 分配策略：
 *  - PSRAM 可用时：使用 heap_caps_aligned_alloc 在 PSRAM 中分配缓存对齐内存。
 *  - 非严格模式下回退到 8BIT 通用内存。
 *  - 分配成功后 memset 清零。
 *
 * 参数:
 *   size - 缓冲区大小（字节）
 *   name - 缓冲区名称（仅用于日志输出）
 * 返回: 成功返回缓冲区指针，失败返回 NULL。
 */
static uint8_t *alloc_frame_buffer(size_t size, const char *name)
{
    uint8_t *buf = NULL;
    const char *where = "none";

#if BOARD_CAMERA_FRAME_BUFFER_PREFER_PSRAM
    buf = heap_caps_aligned_alloc(BOARD_CAMERA_CACHE_ALIGN,
                                  size,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED);
    if (buf) {
        where = "PSRAM";
    }
#endif

#if !BOARD_CAMERA_FRAME_BUFFER_STRICT_PSRAM
    if (!buf) {
        buf = heap_caps_aligned_alloc(BOARD_CAMERA_CACHE_ALIGN,
                                      size,
                                      MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED);
        if (buf) {
            where = "8BIT fallback";
        }
    }
#endif

    if (!buf) {
#if BOARD_CAMERA_FRAME_BUFFER_STRICT_PSRAM
        ESP_LOGE(TAG,
                 "failed to allocate %s in PSRAM, size=%u align=%u",
                 name,
                 (unsigned)size,
                 (unsigned)BOARD_CAMERA_CACHE_ALIGN);
#else
        ESP_LOGE(TAG,
                 "failed to allocate %s, size=%u align=%u",
                 name,
                 (unsigned)size,
                 (unsigned)BOARD_CAMERA_CACHE_ALIGN);
#endif
        return NULL;
    }

    memset(buf, 0, size);
    ESP_LOGI(TAG,
             "allocated %s: %p, size=%u align=%u place=%s strict_psram=%d",
             name,
             buf,
             (unsigned)size,
             (unsigned)BOARD_CAMERA_CACHE_ALIGN,
             where,
             BOARD_CAMERA_FRAME_BUFFER_STRICT_PSRAM ? 1 : 0);
    return buf;
}

/*
 * 释放帧缓冲区，使用 heap_caps_free 并置空指针。
 *
 * 参数:
 *   buf - 指向缓冲区指针的指针，释放后 *buf 置为 NULL。
 */
static void free_frame_buffer(uint8_t **buf)
{
    if (buf && *buf) {
        heap_caps_free(*buf);
        *buf = NULL;
    }
}

/*
 * 获取摄像头 V4L2 设备路径。
 *
 * 优先通过 Board Manager (esp_board_manager) 查询板级配置的摄像头设备路径；
 * 若 Board Manager 未配置或查询失败，则回退到默认路径 /dev/video2。
 *
 * 参数:
 *   path - 输出设备路径字符串指针（不可释放，指向静态或 Board Manager 持有内存）
 * 返回: ESP_OK 表示成功获取路径。
 */
static esp_err_t get_board_camera_path(const char **path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "camera device init failed: %s, using %s",
                 esp_err_to_name(ret),
                 BOARD_CAMERA_DEFAULT_DEV_PATH);
        *path = BOARD_CAMERA_DEFAULT_DEV_PATH;
        return ESP_OK;
    }

    dev_camera_handle_t *camera = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_CAMERA, (void **)&camera);
    if (ret != ESP_OK || !camera || !camera->dev_path) {
        ESP_LOGW(TAG,
                 "camera handle not available from Board Manager, using %s",
                 BOARD_CAMERA_DEFAULT_DEV_PATH);
        *path = BOARD_CAMERA_DEFAULT_DEV_PATH;
        return ESP_OK;
    }

    *path = camera->dev_path;
    return ESP_OK;
}

/*
 * 判断是否需要交换 RGB565 的字节序（大端↔小端）。
 *
 * 若 BOARD_CAMERA_SWAP_RGB565_BYTES < 0：根据 V4L2 返回的格式自动判断
 *   （V4L2_PIX_FMT_RGB565X 表示需要交换）。
 * 否则：按宏定义的 0/1 值强制决定。
 *
 * 返回: true 表示需要字节交换。
 */
static inline bool should_swap_rgb565_bytes(void)
{
#if BOARD_CAMERA_SWAP_RGB565_BYTES < 0
    return s_src_needs_byte_swap;
#else
    return BOARD_CAMERA_SWAP_RGB565_BYTES != 0;
#endif
}

/*
 * 检查缓存同步操作结果是否为"成功"或"不支持"。
 *
 * ESP_OK 表示同步成功；ESP_ERR_NOT_SUPPORTED 表示平台无硬件缓存同步需求，
 * 两种均为可接受的结果。
 *
 * 参数:
 *   ret - esp_cache_msync 的返回值
 * 返回: true 表示缓存同步已就绪或无需求。
 */
static inline bool cache_sync_ok_or_unsupported(esp_err_t ret)
{
    return ret == ESP_OK || ret == ESP_ERR_NOT_SUPPORTED;
}

/*
 * 预览任务让步，释放 CPU 给其他任务。
 *
 * 根据 BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS 配置：
 *   > 0：阻塞延迟指定毫秒数；
 *   = 0：立即 taskYIELD 让出 CPU。
 */
static inline void preview_task_relax(void)
{
#if BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS > 0
    vTaskDelay(pdMS_TO_TICKS(BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS));
#else
    taskYIELD();
#endif
}

/*
 * 将摄像头 V4L2 mmap 缓冲区中的 RGB565 帧复制到应用层目标缓冲区。
 *
 * 功能：
 *  - 支持 stride（bytesperline）处理，逐行复制而非整块 memcpy。
 *  - 支持 RGB565_BE → RGB565_LE 字节交换（大端→小端）。
 *  - 可选支持红蓝通道交换（BOARD_CAMERA_SWAP_RB）。
 *  - 对不完整帧（bytesused/缓冲区长度不足）进行丢弃并预警。
 *
 * 参数:
 *   dst          - 目标缓冲区指针
 *   src          - V4L2 mmap 源缓冲区指针
 *   src_buffer_len - V4L2 缓冲区总长度（mmap length）
 *   bytesused    - V4L2 buf.bytesused，为 0 时使用 src_buffer_len 判断
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_SIZE 帧不完整。
 */
static esp_err_t copy_rgb565_camera_frame(uint8_t *dst,
                                          const uint8_t *src,
                                          size_t src_buffer_len,
                                          size_t bytesused)
{
    if (!dst || !src) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t src_line_bytes = s_src_stride_bytes;
    const size_t required_src_bytes = src_line_bytes * (size_t)s_height;

    /*
     * 当前日志显示 sizeimage=0。部�?V4L2 驱动�?bytesused 也可能为 0�?     * 所�?bytesused=0 时使�?mmap buffer length 判断�?     */
    size_t available = bytesused ? bytesused : src_buffer_len;

    if (src_buffer_len < required_src_bytes || available < required_src_bytes) {
        ESP_LOGW(TAG,
                 "drop incomplete RGB565 frame: bytesused=%u buf_len=%u required=%u stride=%u",
                 (unsigned)bytesused,
                 (unsigned)src_buffer_len,
                 (unsigned)required_src_bytes,
                 (unsigned)src_line_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    const board_camera_rgb565_copy_config_t cfg = {
        .width = s_width,
        .height = s_height,
        .src_stride_bytes = s_src_stride_bytes,
        .swap_bytes = should_swap_rgb565_bytes(),
        .swap_rb = BOARD_CAMERA_SWAP_RB != 0,
    };

    return board_camera_rgb565_copy(dst, src, src_buffer_len, bytesused, &cfg);
}

/*
 * 打开摄像头 V4L2 设备并配置视频格式。
 *
 * 流程：
 *  1. 通过 Board Manager 获取设备路径，带重试机制打开设备节点（O_RDWR | O_NONBLOCK）。
 *  2. 查询设备能力 (VIDIOC_QUERYCAP)。
 *  3. 设置格式为 V4L2_PIX_FMT_RGB565X，目标分辨率 BOARD_CAMERA_WIDTH × BOARD_CAMERA_HEIGHT。
 *  4. 验证驱动返回的格式仍为 RGB565/RGB565X。
 *  5. 根据返回的 pixelformat 判断是否需要字节交换，并补全 stride/image_size 等参数。
 *
 * 返回: ESP_OK 成功，否则 ESP 错误码。
 */
static esp_err_t open_and_configure_video(void)
{
    const char *path = NULL;
    esp_err_t ret = get_board_camera_path(&path);
    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * 使用 O_RDWR | O_NONBLOCK 更适合 V4L2 streaming�?     * Board Manager 刚完�?camera probe 时，/dev/video2 可能还没完全可打开�?     * 因此这里等待最多约 1 秒，避免偶发 ENOENT/ENODEV�?     */
    s_video_fd = -1;
    for (int retry = 0; retry < BOARD_CAMERA_DEVICE_READY_RETRY_COUNT; retry++) {
        s_video_fd = open(path, O_RDWR | O_NONBLOCK);
        if (s_video_fd >= 0) {
            break;
        }
        if (errno != ENOENT && errno != ENODEV) {
            break;
        }
        ESP_LOGW(TAG, "camera node %s not ready yet, retry=%d errno=%d", path, retry, errno);
        vTaskDelay(pdMS_TO_TICKS(BOARD_CAMERA_DEVICE_READY_RETRY_MS));
    }

    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "open %s failed: errno=%d", path, errno);
        return errno_to_esp_err();
    }

    struct v4l2_capability cap = {0};
    if (ioctl(s_video_fd, VIDIOC_QUERYCAP, &cap) != 0) {
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed: errno=%d", errno);
        return errno_to_esp_err();
    }

    ESP_LOGI(TAG, "camera dev=%s driver=%s card=%s", path, cap.driver, cap.card);

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };

    /*
     * 这里仍然请求 BOARD_CAMERA_WIDTH/HEIGHT�?     * 如果 sdkconfig 已经选择 240x240 RGB565_BE，驱动最终返回应�?240x240 RGBP�?     */
    fmt.fmt.pix.width = BOARD_CAMERA_WIDTH;
    fmt.fmt.pix.height = BOARD_CAMERA_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565X;

    if (ioctl(s_video_fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG,
                 "VIDIOC_S_FMT RGB565X %dx%d failed: errno=%d",
                 BOARD_CAMERA_WIDTH,
                 BOARD_CAMERA_HEIGHT,
                 errno);
        return errno_to_esp_err();
    }

    char fourcc[5];
    log_fourcc(fmt.fmt.pix.pixelformat, fourcc);

    ESP_LOGI(TAG,
             "camera format %dx%d %s sizeimage=%" PRIu32 " bytesperline=%" PRIu32,
             (int)fmt.fmt.pix.width,
             (int)fmt.fmt.pix.height,
             fourcc,
             fmt.fmt.pix.sizeimage,
             fmt.fmt.pix.bytesperline);

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565 &&
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565X) {
        ESP_LOGE(TAG, "camera did not accept RGB565/RGB565X output, got %s", fourcc);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_src_needs_byte_swap = fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565X;

    s_width = (int)fmt.fmt.pix.width;
    s_height = (int)fmt.fmt.pix.height;

    if (s_width <= 0 || s_height <= 0) {
        ESP_LOGE(TAG, "invalid camera size: %dx%d", s_width, s_height);
        return ESP_ERR_INVALID_ARG;
    }

    const size_t tight_line_bytes = (size_t)s_width * 2;
    s_frame_bytes = tight_line_bytes * (size_t)s_height;

    /*
     * sizeimage �?bytesperline 可能�?0�?     * 当前日志显示 sizeimage=0，所以这里必须手动兜底�?     */
    s_src_stride_bytes = fmt.fmt.pix.bytesperline ?
                         fmt.fmt.pix.bytesperline :
                         tight_line_bytes;

    if (s_src_stride_bytes < tight_line_bytes) {
        ESP_LOGE(TAG,
                 "invalid bytesperline=%u, expected at least=%u",
                 (unsigned)s_src_stride_bytes,
                 (unsigned)tight_line_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    s_src_image_bytes = fmt.fmt.pix.sizeimage ?
                        fmt.fmt.pix.sizeimage :
                        s_src_stride_bytes * (size_t)s_height;

    ESP_LOGI(TAG,
             "RGB565 config: width=%d height=%d frame=%u stride=%u image=%u source=%s swap_bytes=%d swap_rb=%d",
             s_width,
             s_height,
             (unsigned)s_frame_bytes,
             (unsigned)s_src_stride_bytes,
             (unsigned)s_src_image_bytes,
             s_src_needs_byte_swap ? "RGB565X" : "RGB565",
             should_swap_rgb565_bytes(),
             BOARD_CAMERA_SWAP_RB);

    return ESP_OK;
}

/*
 * 申请并映射 V4L2 mmap 缓冲区，全部入队准备接收帧数据。
 *
 * 流程：
 *  1. VIDIOC_REQBUFS 请求 BOARD_CAMERA_BUFFER_COUNT 个缓冲区。
 *  2. 对每个缓冲区执行 VIDIOC_QUERYBUF → mmap → VIDIOC_QBUF。
 *  3. 缓冲区信息记录在 s_buffers 全局数组。
 *
 * 返回: ESP_OK 成功，否则 ESP 错误码。
 */
static esp_err_t setup_stream_buffers(void)
{
    struct v4l2_requestbuffers req = {
        .count = BOARD_CAMERA_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(s_video_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed: errno=%d", errno);
        return errno_to_esp_err();
    }

    if (req.count < BOARD_CAMERA_BUFFER_COUNT) {
        ESP_LOGE(TAG, "camera returned too few buffers: %" PRIu32, req.count);
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < BOARD_CAMERA_BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };

        if (ioctl(s_video_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%d] failed: errno=%d", i, errno);
            return errno_to_esp_err();
        }

        s_buffers[i].data = mmap(NULL,
                                 buf.length,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED,
                                 s_video_fd,
                                 buf.m.offset);

        if (s_buffers[i].data == MAP_FAILED || s_buffers[i].data == NULL) {
            s_buffers[i].data = NULL;
            ESP_LOGE(TAG, "mmap[%d] failed: errno=%d", i, errno);
            return errno_to_esp_err();
        }

        s_buffers[i].length = buf.length;

        ESP_LOGI(TAG,
                 "mmap buffer[%d]: ptr=%p length=%u offset=%u",
                 i,
                 s_buffers[i].data,
                 (unsigned)buf.length,
                 (unsigned)buf.m.offset);

        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed: errno=%d", i, errno);
            return errno_to_esp_err();
        }
    }

    return ESP_OK;
}

/*
 * 清理所有摄像头资源，将模块恢复到未初始化状态。
 *
 * 清理顺序：
 *  1. 若流仍在运行，发送 VIDIOC_STREAMOFF 停止流。
 *  2. munmap 所有 V4L2 mmap 缓冲区。
 *  3. 释放所有预览帧缓冲和最新帧缓冲。
 *  4. 关闭 V4L2 设备文件描述符。
 *  5. 删除帧互斥锁并重置所有全局状态变量。
 *
 * 此函数可安全多次调用，内部有 NULL/有效性检查。
 */
static void cleanup_camera_resources(void)
{
    if (camera_streaming_load() && s_video_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s_video_fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF during cleanup failed: errno=%d", errno);
        }
        camera_streaming_store(false);
    }

    for (int i = 0; i < BOARD_CAMERA_BUFFER_COUNT; i++) {
        if (s_buffers[i].data && s_buffers[i].length > 0) {
            munmap(s_buffers[i].data, s_buffers[i].length);
            s_buffers[i].data = NULL;
            s_buffers[i].length = 0;
        }
    }

    for (int i = 0; i < BOARD_CAMERA_PREVIEW_BUFFER_COUNT; i++) {
        free_frame_buffer(&s_preview_frames[i]);
    }

    free_frame_buffer(&s_latest_frame);

    if (s_video_fd >= 0) {
        close(s_video_fd);
        s_video_fd = -1;
    }

    if (s_frame_mutex) {
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
    }

    camera_preview_task_store(NULL);
    camera_preview_paused_store(false);
    camera_ready_store(false);
    s_has_latest_frame = false;
    s_src_needs_byte_swap = false;
    memset(&s_stats, 0, sizeof(s_stats));

    if (s_preview_events) {
        vEventGroupDelete(s_preview_events);
        s_preview_events = NULL;
    }
    if (s_state_mutex) {
        vSemaphoreDelete(s_state_mutex);
        s_state_mutex = NULL;
    }
}

/*
 * 初始化摄像头模块：打开设备、配置视频格式、分配缓冲并设置状态。
 *
 * 流程：
 *  1. 创建帧数据互斥锁 (s_frame_mutex)。
 *  2. 调用 open_and_configure_video() 打开并配置 V4L2 设备。
 *  3. 调用 setup_stream_buffers() 申请并映射 mmap 缓冲区。
 *  4. 分配 s_latest_frame（JPEG 抓图用）和 s_preview_frames（预览双缓冲）。
 *  5. 标记 s_camera_ready = true 并通知统计更新。
 *
 * 返回: ESP_OK 表示初始化成功；若已初始化则直接返回 ESP_OK。
 */
esp_err_t board_camera_init(void)
{
    if (camera_ready_load()) {
        return ESP_OK;
    }

    s_frame_mutex = xSemaphoreCreateMutex();
    if (!s_frame_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_state_mutex = xSemaphoreCreateMutex();
    s_preview_events = xEventGroupCreate();
    if (!s_state_mutex || !s_preview_events) {
        cleanup_camera_resources();
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = open_and_configure_video();
    if (ret != ESP_OK) {
        cleanup_camera_resources();
        return ret;
    }

    ret = setup_stream_buffers();
    if (ret != ESP_OK) {
        cleanup_camera_resources();
        return ret;
    }

    s_latest_frame = alloc_frame_buffer(s_frame_bytes, "latest_rgb565_frame");
    if (!s_latest_frame) {
        cleanup_camera_resources();
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < BOARD_CAMERA_PREVIEW_BUFFER_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof(name), "preview_rgb565_frame[%d]", i);

        s_preview_frames[i] = alloc_frame_buffer(s_frame_bytes, name);
        if (!s_preview_frames[i]) {
            cleanup_camera_resources();
            return ESP_ERR_NO_MEM;
        }
    }

    camera_ready_store(true);
    if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_stats.width = s_width;
        s_stats.height = s_height;
        xSemaphoreGive(s_state_mutex);
    }
    camera_notify_stats();

    ESP_LOGI(TAG,
             "Camera initialized through Board Manager V4L2 RGB565 path: %dx%d, frame_bytes=%u",
             s_width,
             s_height,
             (unsigned)s_frame_bytes);

    return ESP_OK;
}

/*
 * 摄像头预览任务主循环：从 V4L2 出队帧、限帧率、同步缓存、复制到预览缓冲并通知上层。
 *
 * 工作流程：
 *  1. VIDIOC_DQBUF 出队一个 V4L2 帧缓冲。
 *  2. 检查帧率限制（BOARD_CAMERA_PREVIEW_FPS），超速帧直接丢弃并重新入队。
 *  3. esp_cache_msync 同步 CPU 缓存（M2C + INVALIDATE）。
 *  4. copy_rgb565_camera_frame 将帧复制到预览双缓冲之一（处理字节交换/stride）。
 *  5. 复制预览帧到 s_latest_frame（供 JPEG 抓图使用），通知 preview_update 回调。
 *  6. 更新帧率统计（每秒计算一次 FPS）并通知 stats_update 回调。
 *  7. VIDIOC_QBUF 重新入队缓冲，让步 CPU 后继续循环。
 *  8. 当 s_streaming 变为 false 时退出循环并自删除任务。
 */
static void preview_task(void *arg)
{
    (void)arg;

    int preview_index = 0;
    uint32_t frame_count = 0;
    uint32_t fps_window_count = 0;
    TickType_t last_draw_tick = 0;
    TickType_t last_fps_tick = xTaskGetTickCount();
    const TickType_t draw_interval = pdMS_TO_TICKS(1000 / BOARD_CAMERA_PREVIEW_FPS);

    ESP_LOGI(TAG,
             "camera RGB565 preview task started fps=%d prio=%d stack=%d idle_delay_ms=%d",
             BOARD_CAMERA_PREVIEW_FPS,
             BOARD_CAMERA_PREVIEW_TASK_PRIO,
             BOARD_CAMERA_PREVIEW_STACK,
             BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS);

    while (camera_streaming_load()) {
        if (camera_preview_paused_load()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(s_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            if (errno != EAGAIN) {
                ESP_LOGW(TAG, "VIDIOC_DQBUF failed: errno=%d", errno);
                camera_stats_record_drop();
                camera_notify_stats();
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (!(buf.flags & V4L2_BUF_FLAG_DONE)) {
            ESP_LOGW(TAG, "drop frame without DONE flag: flags=0x%x", (unsigned)buf.flags);
            camera_stats_record_drop();
            goto requeue;
        }

        if (buf.index >= BOARD_CAMERA_BUFFER_COUNT) {
            ESP_LOGW(TAG, "drop frame with invalid index=%u", (unsigned)buf.index);
            camera_stats_record_drop();
            goto requeue;
        }

        /*
         * 限帧，避免显示任务刷得太快�?         */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_draw_tick) < draw_interval) {
            camera_stats_record_drop();
            goto requeue;
        }
        last_draw_tick = now;

        uint8_t *src_frame = s_buffers[buf.index].data;
        size_t src_len = s_buffers[buf.index].length;

        if (!src_frame || src_len == 0) {
            ESP_LOGW(TAG, "drop empty V4L2 buffer index=%u", (unsigned)buf.index);
            camera_stats_record_drop();
            goto requeue;
        }

        esp_err_t sync_ret = esp_cache_msync(src_frame,
                                             src_len,
                                             ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        if (!cache_sync_ok_or_unsupported(sync_ret)) {
            ESP_LOGW(TAG, "camera buffer cache sync failed: %s", esp_err_to_name(sync_ret));
            camera_stats_record_drop();
            goto requeue;
        }

        uint8_t *preview_frame = s_preview_frames[preview_index];
        preview_index = (preview_index + 1) % BOARD_CAMERA_PREVIEW_BUFFER_COUNT;

        if (!preview_frame) {
            ESP_LOGW(TAG, "preview frame buffer is NULL");
            camera_stats_record_drop();
            goto requeue;
        }

        /*
         * 关键修复�?         * 1. 不直接绘�?V4L2 mmap buffer�?         * 2. RGB565_BE -> RGB565_LE�?         */
        esp_err_t copy_ret = copy_rgb565_camera_frame(preview_frame,
                                                      src_frame,
                                                      src_len,
                                                      buf.bytesused);
        if (copy_ret != ESP_OK) {
            camera_stats_record_drop();
            goto requeue;
        }

        /*
         * �?JPEG capture 使用�?40x240 较小，每帧复制压力不大�?         */
        if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
            memcpy(s_latest_frame, preview_frame, s_frame_bytes);
            s_has_latest_frame = true;
            xSemaphoreGive(s_frame_mutex);
        }

        camera_notify_preview(preview_frame, s_width, s_height);

        frame_count++;
        fps_window_count++;
        TickType_t fps_now = xTaskGetTickCount();
        bool has_fps = false;
        uint32_t current_fps = 0;
        if ((fps_now - last_fps_tick) >= pdMS_TO_TICKS(1000)) {
            uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(fps_now - last_fps_tick);
            current_fps = elapsed_ms ? (fps_window_count * 1000U) / elapsed_ms : fps_window_count;
            has_fps = true;
            fps_window_count = 0;
            last_fps_tick = fps_now;
        }
        camera_stats_record_preview(frame_count, has_fps, current_fps);
        camera_notify_stats();

        if ((frame_count % 120) == 0) {
            // ESP_LOGI(TAG,
            //          "RGB565 preview frames=%" PRIu32
            //          " bytesused=%u buf_len=%u stride=%u frame=%u stack=%u",
            //          frame_count,
            //          (unsigned)buf.bytesused,
            //          (unsigned)src_len,
            //          (unsigned)s_src_stride_bytes,
            //          (unsigned)s_frame_bytes,
            //          (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }

requeue:
        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_QBUF failed: errno=%d", errno);
        }

        preview_task_relax();
    }

    ESP_LOGI(TAG, "camera RGB565 preview task stopped");
    camera_preview_task_store(NULL);
    if (s_preview_events) xEventGroupSetBits(s_preview_events, BOARD_CAMERA_PREVIEW_EXITED_BIT);
    camera_task_delete_self();
}

/*
 * 启动 V4L2 视频流并创建预览任务。
 *
 * 发送 VIDIOC_STREAMON 启动摄像头数据流，然后创建 preview_task 线程。
 * 若任务创建失败则自动回滚（发送 STREAMOFF）。
 *
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_STATE 摄像头未初始化，ESP_ERR_NO_MEM 任务创建失败。
 */
esp_err_t board_camera_start_preview(void)
{
    if (!camera_ready_load()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (camera_streaming_load()) {
        return ESP_OK;
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed: errno=%d", errno);
        return errno_to_esp_err();
    }

    camera_preview_paused_store(false);
    camera_streaming_store(true);
    if (s_preview_events) xEventGroupClearBits(s_preview_events, BOARD_CAMERA_PREVIEW_EXITED_BIT);

    BaseType_t ok = camera_create_task(preview_task,
                                       "cam_preview",
                                       BOARD_CAMERA_PREVIEW_STACK,
                                       NULL,
                                       BOARD_CAMERA_PREVIEW_TASK_PRIO,
                                       &s_preview_task);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create preview task");

        camera_streaming_store(false);
        if (ioctl(s_video_fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF after task create failure failed: errno=%d", errno);
        }

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "camera preview task created prio=%d stack=%d stack_in_psram=%d frame_buf_strict_psram=%d",
             BOARD_CAMERA_PREVIEW_TASK_PRIO,
             BOARD_CAMERA_PREVIEW_STACK,
             BOARD_CAMERA_STACK_IN_PSRAM ? 1 : 0,
             BOARD_CAMERA_FRAME_BUFFER_STRICT_PSRAM ? 1 : 0);

    return ESP_OK;
}

/*
 * 停止 V4L2 视频流并等待预览任务退出。
 *
 * 先将 s_streaming 置为 false 通知 preview_task 退出循环，
 * 然后发送 VIDIOC_STREAMOFF 停止硬件流，最后轮询等待任务句柄清空。
 *
 * 返回: ESP_OK 成功，若流已停止也返回 ESP_OK。
 */
esp_err_t board_camera_stop_preview(void)
{
    if (!camera_ready_load()) {
        return ESP_OK;
    }

    camera_streaming_store(false);

    esp_err_t streamoff_result = ESP_OK;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (s_video_fd >= 0) {
        if (ioctl(s_video_fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed: errno=%d", errno);
            streamoff_result = errno_to_esp_err();
        }
    }

    if (camera_preview_task_load() && s_preview_events) {
        EventBits_t bits = xEventGroupWaitBits(s_preview_events,
                                                BOARD_CAMERA_PREVIEW_EXITED_BIT,
                                                pdFALSE,
                                                pdTRUE,
                                                pdMS_TO_TICKS(BOARD_CAMERA_STOP_TIMEOUT_MS));
        if ((bits & BOARD_CAMERA_PREVIEW_EXITED_BIT) == 0) {
            ESP_LOGE(TAG, "camera preview task did not exit within %u ms",
                     (unsigned)BOARD_CAMERA_STOP_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
    }

    camera_preview_paused_store(false);
    return streamoff_result;
}

/*
 * 暂停或恢复摄像头预览。
 *
 * 仅设置 s_preview_paused 标志位；preview_task 检测到此标志后会跳过帧处理
 * 并短暂休眠（50ms），V4L2 流本身不会停止。
 *
 * 参数:
 *   pause - true 暂停预览，false 恢复预览。
 */
void board_camera_pause_preview(bool pause)
{
    if (!camera_ready_load() || !camera_streaming_load()) {
        return;
    }
    camera_preview_paused_store(pause);
    ESP_LOGI(TAG, "camera preview %s", pause ? "paused" : "resumed");
}

/*
 * 查询摄像头预览是否正在运行。
 *
 * 返回: true 表示摄像头已就绪、流已开启且预览任务句柄有效。
 */
bool board_camera_preview_running(void)
{
    return camera_ready_load() && camera_streaming_load() && camera_preview_task_load() != NULL;
}

/*
 * 获取当前摄像头统计信息的快照。
 *
 * 从全局 s_stats 拷贝预览帧数、丢弃帧数、实时 FPS、最新 JPEG 大小及分辨率到输出结构体。
 *
 * 参数:
 *   out_stats - 输出统计结构体指针（调用者提供内存）。
 */
void board_camera_get_stats(board_camera_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    camera_stats_snapshot(out_stats);
}

/*
 * 分配内存并从 s_latest_frame 复制最新帧，供 JPEG 编码使用。
 *
 * 获取互斥锁后从 s_latest_frame 拷贝完整帧数据到新分配的 PSRAM 缓冲区。
 * 若尚未捕获到完整帧（s_has_latest_frame == false），返回错误。
 *
 * 参数:
 *   out_raw - 输出，指向新分配的 RGB565 帧缓冲（调用者负责 heap_caps_free 释放）。
 * 返回: ESP_OK 成功，ESP_ERR_INVALID_STATE 摄像头未就绪或无最新帧，
 *       ESP_ERR_NO_MEM 分配失败，ESP_ERR_TIMEOUT 获取互斥锁超时。
 */
static esp_err_t copy_latest_frame_for_jpeg(uint8_t **out_raw)
{
    if (!out_raw) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_raw = NULL;

    if (!camera_ready_load() || !s_latest_frame) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *raw = alloc_frame_buffer(s_frame_bytes, "jpeg_raw_rgb565");
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        free(raw);
        return ESP_ERR_TIMEOUT;
    }

    if (!s_has_latest_frame) {
        xSemaphoreGive(s_frame_mutex);
        free(raw);
        ESP_LOGW(TAG, "no complete camera frame captured yet");
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(raw, s_latest_frame, s_frame_bytes);
    xSemaphoreGive(s_frame_mutex);

    *out_raw = raw;
    return ESP_OK;
}

/*
 * 将 RGB565 原始帧编码为 JPEG，并更新全局 JPEG 大小统计。
 *
 * 封装 board_camera_encode_rgb565_jpeg 调用，成功后更新 s_stats.latest_jpeg_bytes
 * 并通知统计回调。
 *
 * 参数:
 *   raw          - RGB565 原始帧数据
 *   quality      - JPEG 质量 (1-100)
 *   out_jpeg     - 输出 JPEG 数据指针（调用者负责 free）
 *   out_jpeg_size- 输出 JPEG 数据大小
 * 返回: 编码结果 esp_err_t。
 */
static esp_err_t encode_rgb565_jpeg(uint8_t *raw,
                                    int quality,
                                    uint8_t **out_jpeg,
                                    size_t *out_jpeg_size)
{
    esp_err_t ret = board_camera_encode_rgb565_jpeg(raw,
                                                    s_frame_bytes,
                                                    s_width,
                                                    s_height,
                                                    quality,
                                                    out_jpeg,
                                                    out_jpeg_size);
    if (ret == ESP_OK) {
        camera_stats_set_latest_jpeg(*out_jpeg_size);
        camera_notify_stats();
    }
    return ret;
}

/*
 * 捕获当前摄像头帧并编码为 JPEG；Base64 由调用方按需生成。
 *
 * 完整流水线：copy_latest_frame_for_jpeg → encode_rgb565_jpeg。
 * 结果填充到 out_image 结构体（包含原始 JPEG 数据和尺寸信息）。
 * 调用者使用完毕后应调用 board_camera_jpeg_cleanup 释放资源。
 *
 * 参数:
 *   quality   - JPEG 压缩质量 (1-100)，越界时自动裁剪。
 *   out_image - 输出结构体，包含 jpeg_data、jpeg_bytes、width、height。
 * 返回: ESP_OK 成功，否则 ESP 错误码。
 */
esp_err_t board_camera_capture_jpeg(int quality, board_camera_jpeg_t *out_image)
{
    if (!out_image) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_image, 0, sizeof(*out_image));

    uint8_t *raw = NULL;
    esp_err_t ret = copy_latest_frame_for_jpeg(&raw);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    ret = encode_rgb565_jpeg(raw, quality, &jpeg, &jpeg_size);
    free(raw);
    if (ret != ESP_OK) {
        return ret;
    }

    out_image->jpeg_data = jpeg;
    out_image->jpeg_bytes = jpeg_size;
    out_image->width = s_width;
    out_image->height = s_height;

    return ESP_OK;
}

esp_err_t board_camera_encode_base64(
    const uint8_t *jpeg_data,
    size_t jpeg_bytes,
    char **out_base64)
{
    if (!jpeg_data || jpeg_bytes == 0 || !out_base64) return ESP_ERR_INVALID_ARG;
    *out_base64 = NULL;
    return board_camera_jpeg_to_base64(jpeg_data, jpeg_bytes, out_base64);
}
