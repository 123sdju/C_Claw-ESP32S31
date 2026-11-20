#include "board_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "board_camera_jpeg.h"
#include "board_camera_rgb565.h"
#include "dev_camera.h"
#include "esp_board_manager.h"
#include "esp_board_manager_defs.h"
#include "esp_cache.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <linux/videodev2.h>
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S31
#include "soc/hp_sys_clkrst_struct.h"
#endif

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

#define BOARD_CAMERA_PREVIEW_EXITED_BIT BIT0
#define BOARD_CAMERA_FIRST_FRAME_BIT BIT1
#define BOARD_CAMERA_FIRST_FRAME_TIMEOUT_MS 6000U
#define BOARD_CAMERA_FRAME_STALL_MS 5000U
#define BOARD_CAMERA_STREAM_RECOVERY_COUNT 2U
#define BOARD_CAMERA_STOP_TIMEOUT_MS 1000U
#ifdef CONFIG_CACHE_L1_DCACHE_LINE_SIZE
#define BOARD_CAMERA_CACHE_ALIGN CONFIG_CACHE_L1_DCACHE_LINE_SIZE
#else
#define BOARD_CAMERA_CACHE_ALIGN 64U
#endif

/* V4L2 mmap 指针借自驱动，释放责任由视频设备管理。应用独占的最新帧、预览双缓冲
 * 和 JPEG 原始副本按缓存行对齐，默认只从 PSRAM 分配。关闭严格模式后才启用通用
 * 8BIT 回退。严格模式使用优先 PSRAM 分支，两项宏需共同表达实际放置策略。 */
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


/* 默认依据驱动返回的 RGB565/RGB565X 格式决定字节交换。 */
#ifndef BOARD_CAMERA_SWAP_RGB565_BYTES
#define BOARD_CAMERA_SWAP_RGB565_BYTES -1
#endif
/* -1 自动判定，零保持字节序，正值强制交换。 */

/* 非零时交换 RGB565 的红蓝通道。 */
#ifndef BOARD_CAMERA_SWAP_RB
#define BOARD_CAMERA_SWAP_RB 0
#endif

/* 预览目标帧率必须为正，默认二十四帧每秒，实际间隔受 tick 分辨率影响。 */
#ifndef BOARD_CAMERA_PREVIEW_FPS
#define BOARD_CAMERA_PREVIEW_FPS 24
#endif

/* 应用双缓冲在同步预览回调期间保持稳定，接收方异步使用前复制数据。 */
#define BOARD_CAMERA_PREVIEW_BUFFER_COUNT 2

typedef struct {
    uint8_t *data;
    size_t length;
} board_camera_v4l2_buffer_t;

static const char *TAG = "board_camera";

/**
 * @brief 记录当前目标 LCDCAM 时钟寄存器供启动与停流诊断。
 * @note 仅在 ESP32-S31 编译寄存器读取，调用不修改摄像头与 LCD 共用时钟。
 * @post 按当前日志级别写出诊断，模块状态保持原值。
 */
static void camera_log_clock_state(void)
{
#if CONFIG_IDF_TARGET_ESP32S31
    /* 摄像头与 LCD 共用核心时钟，此处只读取快照。 */
    ESP_LOGW(TAG, "camera clocks core(src=%u div=%u en=%u) xclk(src=%u div=%u en=%u)",
        (unsigned)HP_SYS_CLKRST.lcdcam_lcdcam_ctrl0.reg_lcdcam_clk_src_sel,
        (unsigned)HP_SYS_CLKRST.lcdcam_lcdcam_ctrl0.reg_lcdcam_clk_div_num + 1U,
        (unsigned)HP_SYS_CLKRST.lcdcam_lcdcam_ctrl0.reg_lcdcam_clk_en,
        (unsigned)HP_SYS_CLKRST.lcdcam_cam_ctrl0.reg_cam_clk_src_sel,
        (unsigned)HP_SYS_CLKRST.lcdcam_cam_ctrl0.reg_cam_clk_div_num + 1U,
        (unsigned)HP_SYS_CLKRST.lcdcam_cam_ctrl0.reg_cam_clk_en);
#endif
}

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
/* SetBits 完全返回后发布，覆盖通知尾部仍使用事件组的窗口。 */
static bool s_preview_exited = true;

static bool s_camera_ready;
static bool s_streaming;
static bool s_preview_paused;
static bool s_has_latest_frame;
/* 预览 worker 独占流恢复，生命周期 owner 等待最终退出后接管驱动字段。 */
static bool s_buffers_queued;
static bool s_driver_streaming;
static unsigned s_next_queue_index;
/* 启动首次成功预览前允许重建资源，捕获和公开统计通过 startup_ready 门控。 */
static bool s_startup_ready;
static bool s_board_device_initialized;
static board_camera_stats_t s_stats;
static board_camera_callbacks_t s_callbacks;

/** @brief 读取预览循环继续标志。 @return acquire 观察的请求状态。 */
static bool camera_streaming_load(void)
{
    return __atomic_load_n(&s_streaming, __ATOMIC_ACQUIRE);
}

/** @brief 发布预览循环继续请求。 @param[in] value true 继续、false 请求退出。 @post release 发布新请求，不代表任务已完成退出。 */
static void camera_streaming_store(bool value)
{
    __atomic_store_n(&s_streaming, value, __ATOMIC_RELEASE);
}

/** @brief 读取仅暂停帧处理的标志。 @return acquire 观察的暂停状态。 */
static bool camera_preview_paused_load(void)
{
    return __atomic_load_n(&s_preview_paused, __ATOMIC_ACQUIRE);
}

/** @brief 发布预览暂停状态。 @param[in] value true 跳过处理、false 恢复。 @post release 发布标志，驱动流保持由 worker 管理。 */
static void camera_preview_paused_store(bool value)
{
    __atomic_store_n(&s_preview_paused, value, __ATOMIC_RELEASE);
}

/** @brief 读取资源初始化发布状态。 @return acquire 观察的就绪标志。 */
static bool camera_ready_load(void)
{
    return __atomic_load_n(&s_camera_ready, __ATOMIC_ACQUIRE);
}

/** @brief 发布资源初始化或清理状态。 @param[in] value owner 准备完资源后置真，清理后置假。 @post release 交付前序资源字段，销毁仍须满足外层生命周期屏障。 */
static void camera_ready_store(bool value)
{
    __atomic_store_n(&s_camera_ready, value, __ATOMIC_RELEASE);
}

/** @brief 读取预览任务的借用身份。 @return 当前发布句柄或 NULL，查询不增加内核任务引用。 */
static TaskHandle_t camera_preview_task_load(void)
{
    return __atomic_load_n(&s_preview_task, __ATOMIC_ACQUIRE);
}

/** @brief 发布 worker 自身身份或退出清空。 @param[in] task 仅作身份比较的句柄或 NULL。 @post release 更新借用身份，完成等待另由 exited 标志保证。 */
static void camera_preview_task_store(TaskHandle_t task)
{
    __atomic_store_n(&s_preview_task, task, __ATOMIC_RELEASE);
}

/**
 * @brief 查询预览任务是否已结束全部板级资源访问和退出通知。
 * @return acquire 观察的完成位，为真后生命周期 owner 可处理驱动或回收启动资源。
 */
static bool camera_preview_exited_load(void)
{
    return __atomic_load_n(&s_preview_exited, __ATOMIC_ACQUIRE);
}

/**
 * @brief 发布创建前未完成状态或通知返回后的最终完成状态。
 * @param[in] exited 创建前由 owner 写 false，worker 最后一次资源访问后写 true。
 * @post release 发布前序驱动、任务身份和事件组访问，标志本身为进程期存储。
 */
static void camera_preview_exited_store(bool exited)
{
    __atomic_store_n(&s_preview_exited, exited, __ATOMIC_RELEASE);
}

/**
 * @brief 将相对毫秒向上换算为有限 tick，正等待至少阻塞一个 tick。
 * @param[in] milliseconds 毫秒数，零保留为零。
 * @return 使用六十四位中间值，封顶 portMAX_DELAY-1，避免意外永久等待。
 */
static TickType_t camera_timeout_ticks(uint32_t milliseconds)
{
    uint64_t ticks = ((uint64_t)milliseconds * configTICK_RATE_HZ + 999U) / 1000U;
    if (ticks >= (uint64_t)portMAX_DELAY) ticks = (uint64_t)portMAX_DELAY - 1U;
    return (TickType_t)ticks;
}

/**
 * @brief 在短状态锁内复制一致统计，锁未取得时交付零快照。
 * @param[out] out_stats 可选调用者槽，先清零，锁最多等待二十毫秒。
 * @post 非 NULL 输出为一致统计副本或零快照，调用者无动态释放责任。
 * @note 初始化与最终清理由串行 owner 协调，快照不保留内部指针。
 */
static void camera_stats_snapshot(board_camera_stats_t *out_stats)
{
    if (!out_stats) return;
    memset(out_stats, 0, sizeof(*out_stats));
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, camera_timeout_ticks(20)) == pdTRUE) {
        *out_stats = s_stats;
        xSemaphoreGive(s_state_mutex);
    }
}

/** @brief 在短状态锁内饱和累计丢帧数。 @post 最多等待二十毫秒，锁失败跳过本次统计。 */
static void camera_stats_record_drop(void)
{
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, camera_timeout_ticks(20)) == pdTRUE) {
        if (s_stats.preview_drops < UINT32_MAX) s_stats.preview_drops++;
        xSemaphoreGive(s_state_mutex);
    }
}

/**
 * @brief 发布本次 worker 的累计帧数和可选帧率。
 * @param[in] frame_count 本次预览任务内的饱和帧数。
 * @param[in] has_fps 非零时同时替换帧率，停止时可发布零。
 * @param[in] fps 本次窗口每秒帧数。
 * @post 最多等待二十毫秒状态锁，失败跳过统计更新。
 */
static void camera_stats_record_preview(uint32_t frame_count, bool has_fps, uint32_t fps)
{
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, camera_timeout_ticks(20)) == pdTRUE) {
        s_stats.preview_frames = frame_count;
        if (has_fps) s_stats.preview_fps = fps;
        xSemaphoreGive(s_state_mutex);
    }
}

/** @brief 更新最近 JPEG 字节统计。 @param[in] jpeg_bytes 成功编码的字节数。 @post 取得二十毫秒短锁后替换，超时保持旧值。 */
static void camera_stats_set_latest_jpeg(size_t jpeg_bytes)
{
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, camera_timeout_ticks(20)) == pdTRUE) {
        s_stats.latest_jpeg_bytes = jpeg_bytes;
        xSemaphoreGive(s_state_mutex);
    }
}

/**
 * @brief 成对替换预览、统计回调及借用上下文。
 * @param[in] callbacks 调用期稳定结构借用，NULL 清空注册，内部只按值复制。
 * @post 已初始化时最多等待一百毫秒状态锁，超时记录日志并保留原注册。
 * @note 启动资源建立前由 boot owner 串行注册，成功启动后在状态锁内替换。
 *       函数不等待已取快照的回调，旧上下文拥有者排空预览及 capture 调用后再释放。
 */
void board_camera_set_callbacks(const board_camera_callbacks_t *callbacks)
{
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, camera_timeout_ticks(100)) != pdTRUE) {
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

/**
 * @brief 复制同一锁期的回调与统计后，在锁外同步通知接收者。
 * @post 状态锁最多等待二十毫秒，失败跳过通知，回调期间借出栈上快照。
 * @note 可由预览任务或抓图调用线程进入，接收者自行同步共享状态并避免递归抓图。
 */
static void camera_notify_stats(void)
{
    board_camera_callbacks_t callbacks = {0};
    board_camera_stats_t stats = {0};
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, camera_timeout_ticks(20)) == pdTRUE) {
        callbacks = s_callbacks;
        stats = s_stats;
        xSemaphoreGive(s_state_mutex);
    }
    if (callbacks.stats_update) callbacks.stats_update(&stats, callbacks.user_data);
}

/**
 * @brief 在状态锁外同步借出当前 CPU 写入的紧密 RGB565 帧。
 * @param[in] rgb565 调用期间稳定的应用预览缓冲，回调异步保存前须复制。
 * @param[in] width 正像素宽度。
 * @param[in] height 正像素高度。
 * @post 最多等待二十毫秒复制回调，未取得锁时跳过通知。
 * @note 由唯一预览任务调用，回调返回后该缓冲进入后续循环复用。
 */
static void camera_notify_preview(const void *rgb565, int width, int height)
{
    board_camera_callbacks_t callbacks = {0};
    if (s_state_mutex && xSemaphoreTake(s_state_mutex, camera_timeout_ticks(20)) == pdTRUE) {
        callbacks = s_callbacks;
        xSemaphoreGive(s_state_mutex);
    }
    if (callbacks.preview_update) callbacks.preview_update(rgb565, width, height, callbacks.user_data);
}

/**
 * @brief 按当前配置优先创建 PSRAM 栈任务，失败时尝试内部栈。
 * @param[in] task 必需的任务入口，内核可在本函数返回前开始执行。
 * @param[in] name 有效任务名称借用，创建期间有效。
 * @param[in] stack_bytes 正栈字节数，使用 ESP-IDF 任务创建语义。
 * @param[in,out] arg 借给任务的上下文，覆盖整个任务期，允许 NULL。
 * @param[in] priority 原生优先级。
 * @param[out] out_handle 必填私有输出槽，借出内核句柄，由任务自身删除。
 * @return pdPASS 成功，其余为原生创建失败，调用者保留共享资源供回滚。
 * @note WithCaps 创建的任务通过配对 WithCaps 删除入口归还栈及控制块。
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

/**
 * @brief 通过与创建方式配对的 FreeRTOS 入口删除当前任务。
 * @pre 当前任务已结束全部板级资源访问并发布完成位。
 * @post 后续内核回收控制块和栈，当前任务不再执行应用代码。
 */
static void camera_task_delete_self(void)
{
#if BOARD_CAMERA_STACK_IN_PSRAM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

/**
 * @brief 将原生失败时已经捕获的 errno 映射为摄像头错误。
 * @param[in] error_number 原生调用失败后、日志及其他操作前捕获的值。
 * @return ENOMEM/EINVAL/EAGAIN/ENODEV 分别映射内存、参数、超时、未找到，其余 ESP_FAIL。
 * @note 调用者在原生失败后立即捕获，后续日志或其他原生操作可能改变线程 errno。
 */
static esp_err_t errno_to_esp_err(int error_number)
{
    switch (error_number) {
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

/**
 * @brief 按低字节在前提取格式标识并追加终止符。
 * @param[in] fourcc V4L2 四字符编码按值输入。
 * @param[out] out 必需的至少五字节缓冲，写出四字节及 NUL。
 */
static void log_fourcc(uint32_t fourcc, char out[5])
{
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = '\0';
}

/**
 * @brief 按缓存对齐和当前 PSRAM 策略分配并清零应用帧缓冲。
 * @param[in] size 正字节容量，调用者已完成图像尺寸算术检查。
 * @param[in] name 日志名称借用，调用期间有效。
 * @return 成功交付独占 heap_caps 分配，调用者通过 free_frame_buffer 消费，失败 NULL。
 * @note 非严格配置允许 8BIT 回退，分配与日志可能阻塞，调用者持有相应资源预算。
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

/**
 * @brief 消费应用独占帧分配并清空拥有槽。
 * @param[in,out] buf 可选拥有槽，*buf 为 heap_caps 分配或 NULL。
 * @post 非空分配已归还，槽置 NULL，调用前须结束所有像素借用。
 */
static void free_frame_buffer(uint8_t **buf)
{
    if (buf && *buf) {
        heap_caps_free(*buf);
        *buf = NULL;
    }
}

/**
 * @brief 通过 Board Manager 初始化摄像头并借出视频节点路径。
 * @param[out] path 必填输出槽，先置 NULL，成功借用至设备反初始化。
 * @return 成功或原生初始化/句柄错误，不转移 Board Manager 配置与设备拥有权。
 * @note 由启动 owner 串行调用，设备初始化成功后保留标志供失败恢复配对反初始化。
 */
static esp_err_t get_board_camera_path(const char **path)
{
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    *path = NULL;

    const int64_t probe_started_us = esp_timer_get_time();
    ESP_LOGW(TAG, "camera diag-v1: device init begin reset_reason=%d uptime_ms=%" PRId64,
             (int)esp_reset_reason(), probe_started_us / 1000);
    dev_camera_config_t *config = NULL;
    if (esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_CAMERA, (void **)&config) == ESP_OK &&
        config && config->sub_type && strcmp(config->sub_type, "dvp") == 0) {
        ESP_LOGW(TAG, "camera configured pins (verify against board): reset=%d pwdn=%d xclk=%d hz=%" PRIu32 " sccb_hz=%" PRIu32,
                 (int)config->sub_cfg.dvp.reset_io, (int)config->sub_cfg.dvp.pwdn_io,
                 (int)config->sub_cfg.dvp.dvp_io.xclk_io,
                 config->sub_cfg.dvp.xclk_freq, config->sub_cfg.dvp.i2c_freq);
    }
    camera_log_clock_state();
    esp_err_t ret = esp_board_manager_init_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
    ESP_LOGW(TAG, "camera diag-v1: device init end result=%s elapsed_ms=%" PRId64,
             esp_err_to_name(ret), (esp_timer_get_time() - probe_started_us) / 1000);
    camera_log_clock_state();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "camera device init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_board_device_initialized = true;

    dev_camera_handle_t *camera = NULL;
    ret = esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_CAMERA, (void **)&camera);
    if (ret != ESP_OK || !camera || !camera->dev_path) {
        ESP_LOGE(TAG, "camera handle not available from Board Manager");
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    *path = camera->dev_path;
    return ESP_OK;
}

/**
 * @brief 读取编译期覆盖或驱动格式导出的字节交换策略。
 * @return true 将源大端像素转换为小端，false 保持字节。
 * @note 自动策略在就绪发布前写入，资源重建由 owner 串行并排空 worker。
 */
static inline bool should_swap_rgb565_bytes(void)
{
#if BOARD_CAMERA_SWAP_RGB565_BYTES < 0
    return s_src_needs_byte_swap;
#else
    return BOARD_CAMERA_SWAP_RGB565_BYTES != 0;
#endif
}

/**
 * @brief 判断原生缓存同步结果是否允许继续复制帧。
 * @param[in] ret esp_cache_msync 的按值结果。
 * @return ESP_OK 或 ESP_ERR_NOT_SUPPORTED 为真，其余为假并由调用者丢帧。
 */
static inline bool cache_sync_ok_or_unsupported(esp_err_t ret)
{
    return ret == ESP_OK || ret == ESP_ERR_NOT_SUPPORTED;
}

/**
 * @brief 在一次帧处理后按配置阻塞让步，显式零配置仅让出调度。
 * @post 正毫秒向上取整，当前 100 Hz tick 下默认一毫秒会阻塞一个 tick。
 */
static inline void preview_task_relax(void)
{
#if BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS > 0
    vTaskDelay(camera_timeout_ticks(BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS));
#else
    taskYIELD();
#endif
}

/**
 * @brief 使用就绪配置把驱动帧复制为紧密的小端应用像素。
 * @param[out] dst 独占且容量至少 s_frame_bytes 的应用缓冲。
 * @param[in] src 稳定的已出队驱动缓冲借用，调用前完成缓存同步。
 * @param[in] src_buffer_len 驱动缓冲实际容量。
 * @param[in] bytesused 有效帧字节数，须覆盖全部填充行。
 * @return 参数错误或直接透传逐行复制结果，失败目标保持原值。
 * @note 预览 worker 独占读取当前格式，QBUF 在本次复制和回调结束后执行。
 */
static esp_err_t copy_rgb565_camera_frame(uint8_t *dst,
                                          const uint8_t *src,
                                          size_t src_buffer_len,
                                          size_t bytesused)
{
    if (!dst || !src) {
        return ESP_ERR_INVALID_ARG;
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

/**
 * @brief 在启动 owner 中打开视频节点并校验驱动实际返回的 RGB565 格式与尺寸。
 * @return 成功或 Board Manager、节点打开、ioctl、格式和尺寸错误，失败由外层清理 fd。
 * @note 就绪发布前写入宽高、紧密容量和源步长，乘法在提交字段前检查。
 *       DQBUF 等待显式限制为一百毫秒，节点重试默认一百次、间隔五十毫秒。
 */
static esp_err_t open_and_configure_video(void)
{
    const char *path = NULL;
    esp_err_t ret = get_board_camera_path(&path);
    if (ret != ESP_OK) {
        return ret;
    }
    camera_log_clock_state();

    /* 节点建立可晚于传感器初始化，只有未找到设备的错误进入有界重试。 */
    s_video_fd = -1;
    int open_error = 0;
    for (int retry = 0; retry < BOARD_CAMERA_DEVICE_READY_RETRY_COUNT; retry++) {
        s_video_fd = open(path, O_RDWR | O_NONBLOCK);
        if (s_video_fd >= 0) {
            break;
        }
        open_error = errno;
        if (open_error != ENOENT && open_error != ENODEV) {
            break;
        }
        ESP_LOGW(TAG, "camera node %s not ready yet, retry=%d errno=%d", path, retry, open_error);
        vTaskDelay(camera_timeout_ticks(BOARD_CAMERA_DEVICE_READY_RETRY_MS));
    }

    if (s_video_fd < 0) {
        ESP_LOGE(TAG, "open %s failed: errno=%d", path, open_error);
        return errno_to_esp_err(open_error);
    }

    /* 当前 esp_video 的 DQBUF 默认永久等待，须独立配置超时供停止和停帧检测推进。 */
    struct timeval dequeue_timeout = { .tv_sec = 0, .tv_usec = 100000 };
    if (ioctl(s_video_fd, VIDIOC_S_DQBUF_TIMEOUT, &dequeue_timeout) != 0) {
        int error_number = errno;
        ESP_LOGE(TAG, "setting camera dequeue timeout failed: errno=%d", error_number);
        return errno_to_esp_err(error_number);
    }

    struct v4l2_capability cap = {0};
    if (ioctl(s_video_fd, VIDIOC_QUERYCAP, &cap) != 0) {
        int error_number = errno;
        ESP_LOGE(TAG, "VIDIOC_QUERYCAP failed: errno=%d", error_number);
        return errno_to_esp_err(error_number);
    }

    ESP_LOGI(TAG, "camera dev=%s driver=%s card=%s", path, cap.driver, cap.card);

    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };

    /* 请求板级目标尺寸，后续以驱动实际回写格式为准。 */
    fmt.fmt.pix.width = BOARD_CAMERA_WIDTH;
    fmt.fmt.pix.height = BOARD_CAMERA_HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565X;

    if (ioctl(s_video_fd, VIDIOC_S_FMT, &fmt) != 0) {
        int error_number = errno;
        ESP_LOGE(TAG,
                 "VIDIOC_S_FMT RGB565X %dx%d failed: errno=%d",
                 BOARD_CAMERA_WIDTH,
                 BOARD_CAMERA_HEIGHT,
                 error_number);
        return errno_to_esp_err(error_number);
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

    if (fmt.fmt.pix.width == 0 || fmt.fmt.pix.height == 0 ||
        fmt.fmt.pix.width > (uint32_t)INT_MAX || fmt.fmt.pix.height > (uint32_t)INT_MAX ||
        (size_t)fmt.fmt.pix.width > SIZE_MAX / 2U) {
        ESP_LOGE(TAG, "invalid camera dimensions");
        return ESP_ERR_INVALID_ARG;
    }
    size_t tight_line_bytes = (size_t)fmt.fmt.pix.width * 2U;
    size_t stride_bytes = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : tight_line_bytes;
    if (stride_bytes < tight_line_bytes ||
        (size_t)fmt.fmt.pix.height > SIZE_MAX / stride_bytes) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_width = (int)fmt.fmt.pix.width;
    s_height = (int)fmt.fmt.pix.height;
    s_frame_bytes = tight_line_bytes * (size_t)s_height;

    /* 零 bytesperline 使用紧密步长，零 sizeimage 使用校验后的全部含填充行容量。 */
    s_src_stride_bytes = stride_bytes;

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

/**
 * @brief 请求四个驱动帧缓冲，校验容量、映射并逐个入队。
 * @return 成功或原生请求/映射/入队错误，驱动数量不足返回内存错误，短帧容量拒绝。
 * @note 启动 owner 串行执行，部分成功映射记录在 s_buffers 供外层统一清理。
 *       mmap 指针继续由驱动拥有，应用只保存借用及长度。
 */
static esp_err_t setup_stream_buffers(void)
{
    struct v4l2_requestbuffers req = {
        .count = BOARD_CAMERA_BUFFER_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    if (ioctl(s_video_fd, VIDIOC_REQBUFS, &req) != 0) {
        int error_number = errno;
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed: errno=%d", error_number);
        return errno_to_esp_err(error_number);
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
            int error_number = errno;
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF[%d] failed: errno=%d", i, error_number);
            return errno_to_esp_err(error_number);
        }
        if (buf.length < s_src_stride_bytes * (size_t)s_height) {
            ESP_LOGE(TAG, "camera buffer[%d] is too small", i);
            return ESP_ERR_INVALID_SIZE;
        }

        s_buffers[i].data = mmap(NULL,
                                 buf.length,
                                 PROT_READ | PROT_WRITE,
                                 MAP_SHARED,
                                 s_video_fd,
                                 buf.m.offset);

        if (s_buffers[i].data == MAP_FAILED || s_buffers[i].data == NULL) {
            int error_number = errno;
            s_buffers[i].data = NULL;
            ESP_LOGE(TAG, "mmap[%d] failed: errno=%d", i, error_number);
            return errno_to_esp_err(error_number);
        }

        s_buffers[i].length = buf.length;

        ESP_LOGI(TAG,
                 "mmap buffer[%d]: ptr=%p length=%u offset=%u",
                 i,
                 s_buffers[i].data,
                 (unsigned)buf.length,
                 (unsigned)buf.m.offset);

        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            int error_number = errno;
            ESP_LOGE(TAG, "VIDIOC_QBUF[%d] failed: errno=%d", i, error_number);
            return errno_to_esp_err(error_number);
        }
    }
    s_buffers_queued = true;
    s_next_queue_index = BOARD_CAMERA_BUFFER_COUNT;
    return ESP_OK;
}

/**
 * @brief 在停流之后从未成功入队的索引继续恢复全部队列。
 * @return 全部排队成功或首个 ioctl 错误，保留已完成索引供重试。
 * @note 由串行 lifecycle owner 或独占流恢复的 worker 调用，二者之间由退出屏障交接。
 */
static esp_err_t camera_queue_stream_buffers(void)
{
    if (s_buffers_queued) return ESP_OK;
    for (unsigned i = s_next_queue_index; i < BOARD_CAMERA_BUFFER_COUNT; ++i) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            int error_number = errno;
            ESP_LOGE(TAG, "camera requeue failed index=%u errno=%d", i, error_number);
            return errno_to_esp_err(error_number);
        }
        s_next_queue_index = i + 1;
    }
    s_buffers_queued = true;
    return ESP_OK;
}

/**
 * @brief 在 worker 内执行有限次数的停流、重新入队和启流恢复。
 * @param[in,out] recovery_count 有效尝试次数槽，进入本次恢复时增加，最大两次。
 * @param[out] last_valid_tick 成功恢复后写当前 tick，重新开始停帧窗口。
 * @return 恢复成功为真，取消、次数耗尽、参数错误或原生失败为假。
 * @note 调用期 worker 独占驱动，失败保留实际 streaming/排队状态给退出清理与 owner。
 */
static bool camera_restart_stream_after_failure(
    unsigned *recovery_count,
    TickType_t *last_valid_tick)
{
    if (!recovery_count || !last_valid_tick || !camera_streaming_load() ||
        *recovery_count >= BOARD_CAMERA_STREAM_RECOVERY_COUNT) {
        return false;
    }
    (*recovery_count)++;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMOFF, &type) != 0) {
        ESP_LOGE(TAG, "camera recovery STREAMOFF failed errno=%d", errno);
        return false;
    }
    s_driver_streaming = false;
    s_buffers_queued = false;
    s_next_queue_index = 0;
    if (!camera_streaming_load()) return false;
    if (camera_queue_stream_buffers() != ESP_OK) return false;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "camera recovery STREAMON failed errno=%d", errno);
        s_buffers_queued = false;
        s_next_queue_index = 0;
        return false;
    }
    s_driver_streaming = true;
    *last_valid_tick = xTaskGetTickCount();
    return true;
}

/**
 * @brief 在启动失败恢复时归还映射借用、应用帧、fd 和本次同步原语。
 * @pre lifecycle owner 已排空 worker 和启动期外部访问，首次成功预览前调用。
 * @post 应用拥有槽与就绪状态清空，可重复清理；原生停流、munmap、close 错误记录日志。
 * @note 当前 IDF close 无论返回值都会注销 fd，驱动自身失败状态由随后 Board Manager
 *       反初始化处理，s_board_device_initialized 保留以完成这一步。
 */
static void cleanup_camera_resources(void)
{
    if (s_driver_streaming && s_video_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s_video_fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF during cleanup failed: errno=%d", errno);
        }
        camera_streaming_store(false);
    }

    for (int i = 0; i < BOARD_CAMERA_BUFFER_COUNT; i++) {
        if (s_buffers[i].data && s_buffers[i].length > 0) {
            if (munmap(s_buffers[i].data, s_buffers[i].length) != 0)
                ESP_LOGW(TAG, "camera munmap failed: errno=%d", errno);
            s_buffers[i].data = NULL;
            s_buffers[i].length = 0;
        }
    }

    for (int i = 0; i < BOARD_CAMERA_PREVIEW_BUFFER_COUNT; i++) {
        free_frame_buffer(&s_preview_frames[i]);
    }

    free_frame_buffer(&s_latest_frame);

    if (s_video_fd >= 0) {
        if (close(s_video_fd) != 0) ESP_LOGW(TAG, "camera close failed: errno=%d", errno);
        s_video_fd = -1;
    }

    if (s_frame_mutex) {
        vSemaphoreDelete(s_frame_mutex);
        s_frame_mutex = NULL;
    }

    camera_preview_task_store(NULL);
    camera_preview_exited_store(true);
    camera_preview_paused_store(false);
    camera_ready_store(false);
    s_buffers_queued = false;
    s_driver_streaming = false;
    s_next_queue_index = 0;
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

/**
 * @brief 在首次成功预览前停止旧 worker、归还资源并重置 Board Manager 摄像头设备。
 * @return 成功、停止错误或原生设备反初始化错误，首次成功预览后返回 INVALID_STATE。
 * @note 由 boot owner 串行调用，停止失败保持当前资源供重试，设备失败保留 initialized。
 *       传感器引脚及共享 I2C 的重建由 Board Manager 下次初始化完成。
 */
esp_err_t board_camera_reset_startup(void)
{
    /* 首次成功预览后，抓图和状态查询可借用资源，启动重建入口随之关闭。 */
    if (__atomic_load_n(&s_startup_ready, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = board_camera_stop_preview();
    if (err != ESP_OK) return err;
    cleanup_camera_resources();
    if (s_board_device_initialized) {
        err = esp_board_manager_deinit_device_by_name(ESP_BOARD_DEVICE_NAME_CAMERA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "camera startup device deinit failed: %s", esp_err_to_name(err));
            return err;
        }
        s_board_device_initialized = false;
    }
    /* 下次设备初始化重建 XCLK、传感器探测和格式，当前调用只等待硬件复位间隔。 */
    vTaskDelay(camera_timeout_ticks(50));
    ESP_LOGW(TAG, "camera startup recovery: full device reinitialization required");
    return ESP_OK;
}

/**
 * @brief 由启动 owner 依次建立视频格式、四个驱动缓冲、三份应用帧和同步资源。
 * @return 已就绪或本次构造成功为 ESP_OK，失败归还本次可回收资源并返回实际错误。
 * @note 初始化与 reset/start/stop 串行，成功 release 发布格式和缓冲后通知统计接收者。
 *       就绪表示资源建立，抓图与公开统计还需首次有效预览的 startup_ready 门。
 */
esp_err_t board_camera_init(void)
{
    if (camera_ready_load()) {
        return ESP_OK;
    }
    vTaskDelay(camera_timeout_ticks(50));

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
    xEventGroupSetBits(s_preview_events, BOARD_CAMERA_PREVIEW_EXITED_BIT);

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
    if (xSemaphoreTake(s_state_mutex, camera_timeout_ticks(20)) == pdTRUE) {
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

/**
 * @brief 独占视频流的预览循环，完成缓存同步、逐行复制、帧通知和有界停帧恢复。
 * @param[in] arg 忽略的任务上下文，允许 NULL。
 * @post 退出时停流、发布零帧率、取消最新帧，SetBits 返回后 release 完成位，再内核自删。
 * @note 驱动帧 QBUF 前只同步借给复制流程，应用预览缓冲回调返回后轮转复用。
 *       五秒无有效最新帧触发恢复，首次启动由 boot owner 重试，后续恢复最多两次。
 */
static void preview_task(void *arg)
{
    (void)arg;
    /* 由实际任务发布自身身份，创建 API 的输出使用 owner 栈槽，消除混合写入。 */
    camera_preview_task_store(xTaskGetCurrentTaskHandle());

    int preview_index = 0;
    uint32_t frame_count = 0;
    uint32_t fps_window_count = 0;
    TickType_t last_draw_tick = 0;
    TickType_t last_fps_tick = xTaskGetTickCount();
    const TickType_t draw_interval = camera_timeout_ticks(1000U / BOARD_CAMERA_PREVIEW_FPS);
    TickType_t last_valid_tick = xTaskGetTickCount();
    unsigned recovery_count = 0;
    unsigned dequeue_failures = 0;
    unsigned rejected_frames = 0;

    ESP_LOGI(TAG,
             "camera RGB565 preview task started fps=%d prio=%d stack=%d idle_delay_ms=%d",
             BOARD_CAMERA_PREVIEW_FPS,
             BOARD_CAMERA_PREVIEW_TASK_PRIO,
             BOARD_CAMERA_PREVIEW_STACK,
             BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS);

    while (camera_streaming_load()) {
        if (camera_preview_paused_load()) {
            last_valid_tick = xTaskGetTickCount();
            vTaskDelay(camera_timeout_ticks(50));
            continue;
        }

        if ((xTaskGetTickCount() - last_valid_tick) >= camera_timeout_ticks(BOARD_CAMERA_FRAME_STALL_MS)) {
            ESP_LOGW(TAG, "camera frame timeout frames=%" PRIu32 " dequeue_failures=%u rejected=%u recovery=%u",
                     frame_count, dequeue_failures, rejected_frames, recovery_count);
            camera_log_clock_state();
            if (xSemaphoreTake(s_frame_mutex, camera_timeout_ticks(1000)) == pdTRUE) {
                s_has_latest_frame = false;
                xSemaphoreGive(s_frame_mutex);
            }
            xEventGroupClearBits(s_preview_events, BOARD_CAMERA_FIRST_FRAME_BIT);
            camera_stats_record_preview(frame_count, true, 0);
            camera_notify_stats();
            /* 首帧由启动 owner 重试，已成功运行的流由本 worker 在现有缓冲上恢复。 */
            if (frame_count == 0 || recovery_count >= BOARD_CAMERA_STREAM_RECOVERY_COUNT) break;
            if (!camera_restart_stream_after_failure(&recovery_count, &last_valid_tick)) break;
            continue;
        }

        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(s_video_fd, VIDIOC_DQBUF, &buf) != 0) {
            dequeue_failures++;
            /* 当前驱动空队列也可能映射为 EPERM，持续无有效帧由停帧窗口处理。 */
            if (dequeue_failures == 1 && errno != EPERM && errno != EAGAIN && errno != ETIMEDOUT) {
                ESP_LOGW(TAG, "VIDIOC_DQBUF failed: errno=%d", errno);
                camera_stats_record_drop();
                camera_notify_stats();
            }
            vTaskDelay(camera_timeout_ticks(2));
            continue;
        }

        if (!(buf.flags & V4L2_BUF_FLAG_DONE) || (buf.flags & V4L2_BUF_FLAG_ERROR) || buf.bytesused == 0) {
            rejected_frames++;
            ESP_LOGW(TAG, "drop invalid frame: flags=0x%x bytesused=%u", (unsigned)buf.flags, (unsigned)buf.bytesused);
            camera_stats_record_drop();
            goto requeue;
        }

        if (buf.index >= BOARD_CAMERA_BUFFER_COUNT) {
            ESP_LOGW(TAG, "drop frame with invalid index=%u", (unsigned)buf.index);
            camera_stats_record_drop();
            goto requeue;
        }

        /* 按目标间隔丢弃过密帧，仍归还原生缓冲。 */
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

        /* 从驱动借用复制为应用独占的小端像素，原始 DMA 缓冲稍后归还队列。 */
        esp_err_t copy_ret = copy_rgb565_camera_frame(preview_frame,
                                                      src_frame,
                                                      src_len,
                                                      buf.bytesused);
        if (copy_ret != ESP_OK) {
            rejected_frames++;
            camera_stats_record_drop();
            goto requeue;
        }

        /* 在帧锁内更新抓图副本，抓图线程只在锁内读取该份最新帧。 */
        if (xSemaphoreTake(s_frame_mutex, camera_timeout_ticks(20)) == pdTRUE) {
            memcpy(s_latest_frame, preview_frame, s_frame_bytes);
            s_has_latest_frame = true;
            xSemaphoreGive(s_frame_mutex);
            last_valid_tick = xTaskGetTickCount();
            if ((xEventGroupGetBits(s_preview_events) & BOARD_CAMERA_FIRST_FRAME_BIT) == 0) {
                ESP_LOGI(TAG, "camera first valid frame received bytes=%u", (unsigned)buf.bytesused);
                xEventGroupSetBits(s_preview_events, BOARD_CAMERA_FIRST_FRAME_BIT);
            }
        }

        camera_notify_preview(preview_frame, s_width, s_height);

        if (frame_count < UINT32_MAX) frame_count++;
        fps_window_count++;
        TickType_t fps_now = xTaskGetTickCount();
        bool has_fps = false;
        uint32_t current_fps = 0;
        if ((fps_now - last_fps_tick) >= camera_timeout_ticks(1000)) {
            uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(fps_now - last_fps_tick);
            uint64_t measured_fps = elapsed_ms ?
                ((uint64_t)fps_window_count * 1000U) / elapsed_ms : fps_window_count;
            current_fps = measured_fps > UINT32_MAX ? UINT32_MAX : (uint32_t)measured_fps;
            has_fps = true;
            fps_window_count = 0;
            last_fps_tick = fps_now;
        }
        camera_stats_record_preview(frame_count, has_fps, current_fps);
        camera_notify_stats();

requeue:
        if (ioctl(s_video_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGW(TAG, "VIDIOC_QBUF failed: errno=%d", errno);
            camera_stats_record_drop();
            camera_notify_stats();
            if (!camera_restart_stream_after_failure(&recovery_count, &last_valid_tick)) break;
            continue;
        }

        preview_task_relax();
    }

    ESP_LOGI(TAG, "camera RGB565 preview task stopped");
    camera_streaming_store(false);
    /* 停帧自然退出也由当前 worker 先归还驱动流，随后发布 owner 可接管的完成位。 */
    if (s_driver_streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s_video_fd, VIDIOC_STREAMOFF, &type) == 0) {
            s_driver_streaming = false;
            s_buffers_queued = false;
            s_next_queue_index = 0;
        } else {
            ESP_LOGE(TAG, "camera worker STREAMOFF failed errno=%d", errno);
        }
    }
    camera_stats_record_preview(frame_count, true, 0);
    camera_notify_stats();
    if (xSemaphoreTake(s_frame_mutex, camera_timeout_ticks(1000)) == pdTRUE) {
        s_has_latest_frame = false;
        xSemaphoreGive(s_frame_mutex);
    }
    xEventGroupClearBits(s_preview_events, BOARD_CAMERA_FIRST_FRAME_BIT);
    camera_preview_task_store(NULL);
    if (s_preview_events) xEventGroupSetBits(s_preview_events, BOARD_CAMERA_PREVIEW_EXITED_BIT);
    camera_preview_exited_store(true);
    camera_task_delete_self();
}

/**
 * @brief 在串行生命周期入口排队启流并等待首个有效最新帧。
 * @return 首帧已发布为成功，未就绪/尚在启动为 INVALID_STATE，其他为队列、驱动、
 *         创建或等待错误。首帧超时后先停止，停止错误优先保留。
 * @note 先排空旧 worker 的通知尾部再触碰驱动，创建失败归还流并恢复完成状态。
 *       首次成功通过 startup_ready 永久关闭启动资源重建入口。
 */
esp_err_t board_camera_start_preview(void)
{
    if (!camera_ready_load()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (camera_streaming_load()) {
        return (xEventGroupGetBits(s_preview_events) & BOARD_CAMERA_FIRST_FRAME_BIT)
            ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    /* 完成位覆盖原生退出通知返回尾部，成功 acquire 后再读取驱动拥有状态。 */
    if (!camera_preview_exited_load() || s_driver_streaming) {
        esp_err_t stop_err = board_camera_stop_preview();
        if (stop_err != ESP_OK) return stop_err;
    }

    esp_err_t queue_err = camera_queue_stream_buffers();
    if (queue_err != ESP_OK) return queue_err;

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_video_fd, VIDIOC_STREAMON, &type) != 0) {
        int error_number = errno;
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed: errno=%d", error_number);
        return errno_to_esp_err(error_number);
    }
    s_driver_streaming = true;

    ESP_LOGW(TAG, "camera diag-v1: STREAMON accepted; waiting for first frame");
    camera_log_clock_state();

    camera_preview_paused_store(false);
    camera_streaming_store(true);
    camera_preview_exited_store(false);
    if (s_preview_events) xEventGroupClearBits(s_preview_events,
        BOARD_CAMERA_PREVIEW_EXITED_BIT | BOARD_CAMERA_FIRST_FRAME_BIT);

    TaskHandle_t created_task = NULL;
    BaseType_t ok = camera_create_task(preview_task,
                                       "cam_preview",
                                       BOARD_CAMERA_PREVIEW_STACK,
                                       NULL,
                                       BOARD_CAMERA_PREVIEW_TASK_PRIO,
                                       &created_task);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create preview task");

        camera_streaming_store(false);
        if (ioctl(s_video_fd, VIDIOC_STREAMOFF, &type) != 0) {
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF after task create failure failed: errno=%d", errno);
        } else {
            s_driver_streaming = false;
            s_buffers_queued = false;
            s_next_queue_index = 0;
        }
        xEventGroupSetBits(s_preview_events, BOARD_CAMERA_PREVIEW_EXITED_BIT);
        camera_preview_exited_store(true);

        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "camera preview task created prio=%d stack=%d stack_in_psram=%d frame_buf_strict_psram=%d",
             BOARD_CAMERA_PREVIEW_TASK_PRIO,
             BOARD_CAMERA_PREVIEW_STACK,
             BOARD_CAMERA_STACK_IN_PSRAM ? 1 : 0,
             BOARD_CAMERA_FRAME_BUFFER_STRICT_PSRAM ? 1 : 0);

    EventBits_t bits = xEventGroupWaitBits(s_preview_events,
        BOARD_CAMERA_FIRST_FRAME_BIT | BOARD_CAMERA_PREVIEW_EXITED_BIT,
        pdFALSE, pdFALSE, camera_timeout_ticks(BOARD_CAMERA_FIRST_FRAME_TIMEOUT_MS));
    if ((bits & BOARD_CAMERA_FIRST_FRAME_BIT) && !(bits & BOARD_CAMERA_PREVIEW_EXITED_BIT)) {
        __atomic_store_n(&s_startup_ready, true, __ATOMIC_RELEASE);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "camera startup failed: no valid first frame within %u ms",
             (unsigned)BOARD_CAMERA_FIRST_FRAME_TIMEOUT_MS);
    esp_err_t stop_err = board_camera_stop_preview();
    return stop_err == ESP_OK ? ESP_ERR_TIMEOUT : stop_err;
}

/**
 * @brief 请求 worker 停止，并在同一个一秒 tick 预算内跨过通知和最终完成屏障。
 * @return 无资源或停止成功为 ESP_OK，自任务调用为 INVALID_STATE，等待未完成为
 *         TIMEOUT，驱动停流错误原样映射。超时保留资源及退出请求供重试。
 * @note lifecycle owner 串行 start/stop/reset，回调从 worker 调用本入口会被拒绝。
 *       完成位确认后接管驱动，内核后续回收任务控制块和栈由 FreeRTOS 管理。
 */
esp_err_t board_camera_stop_preview(void)
{
    if (!camera_ready_load()) {
        return ESP_OK;
    }
    if (!camera_preview_exited_load() && camera_preview_task_load() == xTaskGetCurrentTaskHandle()) {
        return ESP_ERR_INVALID_STATE;
    }

    camera_streaming_store(false);
    TickType_t started = xTaskGetTickCount();
    TickType_t budget = camera_timeout_ticks(BOARD_CAMERA_STOP_TIMEOUT_MS);

    /* 先等待原驱动拥有者结束 QBUF/恢复，再由本 owner 执行必要的剩余停流。 */
    if (s_preview_events) {
        TickType_t elapsed = (TickType_t)(xTaskGetTickCount() - started);
        TickType_t remaining = elapsed < budget ? budget - elapsed : 0;
        EventBits_t bits = xEventGroupWaitBits(s_preview_events,
                                                BOARD_CAMERA_PREVIEW_EXITED_BIT,
                                                pdFALSE,
                                                pdTRUE,
                                                remaining);
        if ((bits & BOARD_CAMERA_PREVIEW_EXITED_BIT) == 0) {
            ESP_LOGE(TAG, "camera preview task did not exit within %u ms",
                     (unsigned)BOARD_CAMERA_STOP_TIMEOUT_MS);
            return ESP_ERR_TIMEOUT;
        }
    }
    while (!camera_preview_exited_load()) {
        if ((TickType_t)(xTaskGetTickCount() - started) >= budget) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }

    esp_err_t streamoff_result = ESP_OK;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (s_video_fd >= 0 && s_driver_streaming) {
        if (ioctl(s_video_fd, VIDIOC_STREAMOFF, &type) != 0) {
            int error_number = errno;
            ESP_LOGW(TAG, "VIDIOC_STREAMOFF failed: errno=%d", error_number);
            streamoff_result = errno_to_esp_err(error_number);
        } else {
            s_driver_streaming = false;
            s_buffers_queued = false;
            s_next_queue_index = 0;
        }
    }

    camera_preview_paused_store(false);
    return streamoff_result;
}

/**
 * @brief 在运行期原子发布帧处理暂停或恢复。
 * @param[in] pause true 暂停处理，false 恢复。
 * @post 资源未就绪或已停止时跳过，否则 worker 在后续循环观察，暂停时每五十毫秒让步。
 * @note 驱动流继续由 worker 保持，暂停期间保留最近完整帧供抓图。
 */
void board_camera_pause_preview(bool pause)
{
    if (!camera_ready_load() || !camera_streaming_load()) {
        return;
    }
    camera_preview_paused_store(pause);
    ESP_LOGI(TAG, "camera preview %s", pause ? "paused" : "resumed");
}

/**
 * @brief 按首次启动、资源、循环请求和任务身份的原子快照查询预览运行状态。
 * @return 四项条件均成立为真，瞬时查询不提供任务租约或关闭屏障。
 */
bool board_camera_preview_running(void)
{
    return __atomic_load_n(&s_startup_ready, __ATOMIC_ACQUIRE) &&
        camera_ready_load() && camera_streaming_load() && camera_preview_task_load() != NULL;
}

/**
 * @brief 获取初始化后受短状态锁保护的统计副本。
 * @param[out] out_stats 可选调用者槽，首次成功预览前或锁超时时交付全零。
 * @post 非 NULL 输出持有当前统计副本或零快照。
 * @note 副本无拥有指针，资源重建由 startup_ready 门隔离。
 */
void board_camera_get_stats(board_camera_stats_t *out_stats)
{
    if (!out_stats) {
        return;
    }
    if (!__atomic_load_n(&s_startup_ready, __ATOMIC_ACQUIRE)) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    camera_stats_snapshot(out_stats);
}

/**
 * @brief 为本次抓图分配私有帧并在帧锁内复制最近完整图像。
 * @param[out] out_raw 必填新槽，先置 NULL，成功交付 heap_caps 拥有分配。
 * @return 成功或尚未成功启动/已停流/无完整帧、内存、帧锁超时错误，失败清理副本。
 * @note 先分配再等待一秒帧锁，锁内再次检查停止请求，避免停止后复用旧有效标志。
 */
static esp_err_t copy_latest_frame_for_jpeg(uint8_t **out_raw)
{
    if (!out_raw) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_raw = NULL;

    if (!__atomic_load_n(&s_startup_ready, __ATOMIC_ACQUIRE)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!camera_ready_load() || !camera_streaming_load() || !s_latest_frame) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *raw = alloc_frame_buffer(s_frame_bytes, "jpeg_raw_rgb565");
    if (!raw) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_frame_mutex, camera_timeout_ticks(1000)) != pdTRUE) {
        free_frame_buffer(&raw);
        return ESP_ERR_TIMEOUT;
    }

    if (!s_has_latest_frame || !camera_streaming_load()) {
        xSemaphoreGive(s_frame_mutex);
        free_frame_buffer(&raw);
        ESP_LOGW(TAG, "no complete camera frame captured yet");
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(raw, s_latest_frame, s_frame_bytes);
    xSemaphoreGive(s_frame_mutex);

    *out_raw = raw;
    return ESP_OK;
}

/**
 * @brief 按稳定板级尺寸编码私有原始帧并在成功后通知 JPEG 统计。
 * @param[in] raw 整个编码期间稳定的 s_frame_bytes 字节借用。
 * @param[in] quality 硬件辅助函数处理的质量参数。
 * @param[out] out_jpeg 独立的新/已清理槽，成功数据由调用者 free。
 * @param[out] out_jpeg_size 必填独立字节数槽。
 * @return 透传编码结果，成功时尽力更新统计并同步通知回调。
 * @note startup_ready 发布后尺寸保持稳定，抓图线程只持有本次像素副本。
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

/**
 * @brief 把最近完整帧复制到本次拥有缓冲并同步编码为 JPEG。
 * @param[in] quality 一至一百，低于一采用七十，高于一百取一百。
 * @param[out] out_image 必填新/已 cleanup 记录，先清零，成功数据交调用者 cleanup。
 * @return 成功或复制/编码错误，私有原始帧始终归还，失败输出保持空。
 * @note 捕获与 preview 的像素访问通过帧锁串行，编码使用私有副本；原生硬件锁与
 *       统计回调覆盖调用期，外层在释放回调上下文前排空本入口。
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
    free_frame_buffer(&raw);
    if (ret != ESP_OK) {
        return ret;
    }

    out_image->jpeg_data = jpeg;
    out_image->jpeg_bytes = jpeg_size;
    out_image->width = s_width;
    out_image->height = s_height;

    return ESP_OK;
}

/**
 * @brief 委托无共享状态的 JPEG Base64 转换。
 * @param[in] jpeg_data 稳定二进制输入借用。
 * @param[in] jpeg_bytes 正字节数。
 * @param[out] out_base64 必填新/已清理槽，失败为空，成功由调用者 free。
 * @return 透传参数、长度、分配和编码结果。
 */
esp_err_t board_camera_encode_base64(
    const uint8_t *jpeg_data,
    size_t jpeg_bytes,
    char **out_base64)
{
    return board_camera_jpeg_to_base64(jpeg_data, jpeg_bytes, out_base64);
}
