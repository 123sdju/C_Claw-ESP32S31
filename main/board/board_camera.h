#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* init/reset/start/stop 由启动或 OTA 生命周期 owner 串行调用。
 * 首次成功预览后保留帧和同步资源，运行期抓图、统计与暂停可以并发。
 * 回调及 user_data 由注册者拥有，替换注册只修改快照，释放旧上下文前排空其借用。 */

#define BOARD_CAMERA_WIDTH 240
#define BOARD_CAMERA_HEIGHT 240

typedef struct {
    uint8_t *jpeg_data;    /**< 独占的 JPEG 分配，最终由 jpeg_cleanup 归还。 */
    size_t jpeg_bytes;     /**< JPEG 数据字节数 */
    int width;             /**< 图像宽度（像素） */
    int height;            /**< 图像高度（像素） */
} board_camera_jpeg_t;

typedef struct {
    uint32_t preview_frames;   /**< 本次预览任务的饱和帧数，新任务从零开始。 */
    uint32_t preview_drops;    /**< 本次初始化以来饱和累计丢帧数。 */
    uint32_t preview_fps;      /**< 当前预览帧率（每秒更新） */
    size_t latest_jpeg_bytes;  /**< 最近一次 JPEG 编码字节数 */
    int width;                 /**< 摄像头输出宽度（像素） */
    int height;                /**< 摄像头输出高度（像素） */
} board_camera_stats_t;

typedef struct {
    /**
     * @brief 在预览任务中同步借出一份小端紧密 RGB565 帧。
     * @param[in] rgb565 仅回调期间稳定的像素借用，异步处理前深复制。
     * @param[in] width 正像素宽度。
     * @param[in] height 正像素高度。
     * @param[in,out] user_data 注册者保持存活的上下文借用。
     * @post 返回后应用缓冲进入后续轮转，回调避免重入 start/stop/reset。
     */
    void (*preview_update)(const void *rgb565, int width, int height, void *user_data);
    /**
     * @brief 同步消费按值统计快照，可由预览或抓图线程调用。
     * @param[in] stats 仅回调期间有效的只读栈快照。
     * @param[in,out] user_data 注册者拥有的上下文借用。
     * @post 调用返回前消费或复制快照，接收者自行同步共享状态，避免递归抓图。
     */
    void (*stats_update)(const board_camera_stats_t *stats, void *user_data);
    void *user_data;                                                                    /**< 用户数据，透传给回调函数 */
} board_camera_callbacks_t;

/**
 * @brief 成对替换两个回调及借用上下文，NULL 清空注册。
 * @param[in] callbacks 调用期稳定结构，函数按值复制，NULL 表示取消后续快照。
 * @post 已初始化时最多等待一百毫秒状态锁，超时日志提示并保留旧注册。
 * @note 启动资源建立前串行注册，替换返回后已有回调快照仍可能执行。
 */
void board_camera_set_callbacks(const board_camera_callbacks_t *callbacks);
/**
 * @brief 打开并配置视频节点，建立驱动借用和应用帧资源。
 * @return 已就绪或构造成功为 ESP_OK，失败返回原生或资源错误并清理本次可回收资源。
 * @note 启动 owner 串行调用，抓图和公开统计还需首次成功预览。
 */
esp_err_t board_camera_init(void);
/**
 * @brief 在首次成功预览前，排空旧 worker 并反初始化摄像头供启动重试。
 * @return 成功、停止/驱动错误或成功启动后禁止重建的 INVALID_STATE。
 * @note boot owner 串行调用，停止失败保留资源供重试，底层设备重建沿 Board Manager。
 */
esp_err_t board_camera_reset_startup(void);
/**
 * @brief 启流并在六秒首帧窗口内等待有效最新帧。
 * @return 成功、未就绪/尚在启动、资源、驱动或等待错误，首帧失败后停止错误优先。
 * @note lifecycle owner 串行调用，旧 worker 的最终退出通知完成后才复用驱动。
 */
esp_err_t board_camera_start_preview(void);
/**
 * @brief 请求停止并等待 worker 结束全部板级资源访问及通知尾部。
 * @return 成功、单次一秒总 tick 预算超时、原生停流错误或从 worker 自停的 INVALID_STATE。
 * @note 与其他生命周期操作串行，超时保留资源和停止请求可重试。成功后仍有内核栈回收。
 */
esp_err_t board_camera_stop_preview(void);
/**
 * @brief 原子设置运行中预览的帧处理暂停状态。
 * @param[in] pause true 暂停、false 恢复，非运行状态时跳过。
 * @post worker 后续循环观察新值，暂停期间保留最近完整帧，驱动流由 worker 管理。
 */
void board_camera_pause_preview(bool pause);
/**
 * @brief 查询首次成功启动后资源、请求和 worker 身份的瞬时状态。
 * @return 已成功启动、就绪、循环继续且 worker 身份有效时为真，查询不建立资源租约。
 */
bool board_camera_preview_running(void);
/**
 * @brief 复制当前统计，首次成功预览前或短状态锁超时交付零快照。
 * @param[out] out_stats 可选的调用者记录槽，按值交付且无动态释放责任。
 * @post 非 NULL 输出为一致统计副本或零快照。
 */
void board_camera_get_stats(board_camera_stats_t *out_stats);
/**
 * @brief 在运行中的预览上复制最近完整帧并同步编码为 JPEG。
 * @param[in] quality 一至一百，低于一取七十，高于一百取一百。
 * @param[out] out_image 必填新/已清理槽，先清零，成功由调用者 jpeg_cleanup。
 * @return 成功或状态、内存、帧锁、尺寸和原生编码错误，失败输出为空。
 * @note 预览像素通过帧锁复制后独占编码，原生共享硬件锁和同步统计回调纳入调用寿命。
 */
esp_err_t board_camera_capture_jpeg(int quality, board_camera_jpeg_t *out_image);
/**
 * @brief 按需把 JPEG 二进制转换为独立 Base64 字符串。
 * @param[in] jpeg_data 调用期间稳定的二进制借用。
 * @param[in] jpeg_bytes 正字节数。
 * @param[out] out_base64 必填新/已清理槽，先置 NULL，成功由调用者 free。
 * @return 成功或参数、容量、分配及原生转换错误，失败不发布部分输出。
 */
esp_err_t board_camera_encode_base64(
    const uint8_t *jpeg_data,
    size_t jpeg_bytes,
    char **out_base64);
/**
 * @brief 归还记录拥有的 JPEG 数据并清零字段。
 * @param[in,out] image 可选且已初始化的独占记录，数据兼容 free。
 * @post 数据归还且字段清零，结构体保留，可重复清理空记录。
 */
void board_camera_jpeg_cleanup(board_camera_jpeg_t *image);

#ifdef __cplusplus
}
#endif
