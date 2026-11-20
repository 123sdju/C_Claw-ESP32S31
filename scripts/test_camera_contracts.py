"""执行相机实际转换与生命周期代码的有界故障模型，硬件原语由本地替身提供。"""
import os
from pathlib import Path
import re
import unittest

from test_event_ring_ownership import run_c_test
from test_review_regressions import source


BASE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_NO_MEM 5
#define ESP_ERR_TIMEOUT 6
#define ESP_ERR_NOT_FOUND 7
#define ESP_ERR_NOT_SUPPORTED 8
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
'''


def camera_text():
    """读取当前生产源，负对照可显式指定保存的相机原文。"""
    override = os.environ.get("CC_TEST_CAMERA_SOURCE")
    return Path(override).read_text(encoding="utf-8") if override else source("main/board/board_camera.c")


def selected(signature, optional=False):
    """原样提取当前相机函数，保留完整函数体和条件编译。"""
    text = camera_text()
    if optional and signature not in text:
        return ""
    start = text.index(signature)
    return text[start:text.index("\n}\n", start) + 3]


class CameraContracts(unittest.TestCase):
    def test_jpeg_and_base64_outputs(self):
        """校验矩形容量、原生宽度、驱动失败及精确 Base64 输出分配。"""
        header = re.sub(r'^#(?:include|pragma).*$', '', source("main/board/board_camera.h"), flags=re.M)
        body = re.sub(r'^#include.*$', '', source("main/board/board_camera_jpeg.c"), flags=re.M)
        model = r'''
typedef void *jpeg_encoder_handle_t;
typedef struct {int intr_priority, timeout_ms;} jpeg_encode_engine_cfg_t;
typedef struct {int buffer_direction;} jpeg_encode_memory_alloc_cfg_t;
typedef struct {int src_type, sub_sample;uint32_t image_quality,width,height;} jpeg_encode_cfg_t;
#define JPEG_ENC_ALLOC_OUTPUT_BUFFER 1
#define JPEG_ENCODE_IN_FORMAT_RGB565 2
#define JPEG_DOWN_SAMPLING_YUV422 3
#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -42
static int mode, fail_allocation, live, engines, process_calls;
static size_t requested_bytes, encoded_bytes;
static void *model_malloc(size_t size) {
    if (fail_allocation) return NULL;
    void *p=malloc(size); if(p) live++; return p;
}
static void model_free(void *p) {if(p) {assert(live>0);live--;}free(p);}
static int jpeg_new_encoder_engine(const jpeg_encode_engine_cfg_t *cfg,void **out) {
    assert(cfg->timeout_ms==200);
    if(mode==1) return ESP_ERR_NO_MEM;
    *out=(void*)1;engines++;return ESP_OK;
}
static void *jpeg_alloc_encoder_mem(size_t size,const jpeg_encode_memory_alloc_cfg_t *cfg,size_t *allocated) {
    assert(cfg->buffer_direction==JPEG_ENC_ALLOC_OUTPUT_BUFFER); requested_bytes=size;
    *allocated=mode==2 ? size-1 : size;
    return model_malloc(size);
}
static int jpeg_del_encoder_engine(void *engine) {
    assert(engine==(void*)1 && engines==1);engines--;return mode==4?ESP_FAIL:ESP_OK;
}
static int jpeg_encoder_process(void *engine,const jpeg_encode_cfg_t *cfg,const uint8_t *raw,
    uint32_t raw_size,uint8_t *out,uint32_t capacity,uint32_t *written) {
    (void)raw; assert(engine==(void*)1 && cfg->width==16 && cfg->height==8);
    assert(raw_size==256 && capacity>=256); encoded_bytes=raw_size;process_calls++;
    if(mode==3) return ESP_ERR_TIMEOUT;
    *written=mode==5?capacity+1:mode==6?0:3;
    out[0]=0xff;out[1]=0xd8;out[2]=0xd9;return ESP_OK;
}
static int mbedtls_base64_encode(unsigned char *out,size_t capacity,size_t *written,
    const uint8_t *input,size_t length) {
    (void)input;(void)length;
    if(!out) {*written=mode==7?SIZE_MAX:5;return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;}
    assert(capacity==5);
    if(mode==8) return ESP_FAIL;
    memcpy(out,"AA==",5);*written=mode==9?capacity:4;return 0;
}
#define malloc model_malloc
#define free model_free
'''
        tests = r'''
#undef malloc
#undef free
int main(void) {
    uint8_t raw[512]={0},*jpeg=(void*)1;size_t length=77;
    assert(board_camera_encode_rgb565_jpeg(raw,255,16,8,70,&jpeg,&length)==ESP_ERR_INVALID_SIZE);
    assert(!jpeg && length==0 && engines==0 && process_calls==0);
    assert(board_camera_encode_rgb565_jpeg(raw,sizeof(raw),16384,1,70,&jpeg,&length)==ESP_ERR_INVALID_SIZE);
    assert(!jpeg && !length && !engines);
    assert(board_camera_encode_rgb565_jpeg(NULL,0,16,8,70,&jpeg,&length)==ESP_ERR_INVALID_ARG);
    assert(!jpeg && !length);
    for(mode=1;mode<=6;mode++) {
        jpeg=(void*)1;length=77;
        assert(board_camera_encode_rgb565_jpeg(raw,sizeof(raw),16,8,70,&jpeg,&length)!=ESP_OK);
        assert(!jpeg && !length && !engines && !live);
    }
    mode=0;fail_allocation=1;
    assert(board_camera_encode_rgb565_jpeg(raw,sizeof(raw),16,8,70,&jpeg,&length)==ESP_ERR_NO_MEM);
    assert(!engines && !live && !jpeg && !length);
    fail_allocation=0;
    assert(board_camera_encode_rgb565_jpeg(raw,sizeof(raw),16,8,0,&jpeg,&length)==ESP_OK);
    assert(requested_bytes==256 && encoded_bytes==256 && length==3 && live==1);
    board_camera_jpeg_t image={jpeg,length,16,8};board_camera_jpeg_cleanup(&image);
    assert(!image.jpeg_data && !image.jpeg_bytes && !image.width && !image.height && !live);
    board_camera_jpeg_cleanup(&image);board_camera_jpeg_cleanup(NULL);
    char *text=(void*)1;
    assert(board_camera_jpeg_to_base64(NULL,0,&text)==ESP_ERR_INVALID_ARG && !text);
    for(mode=7;mode<=9;mode++) {
        assert(board_camera_jpeg_to_base64(raw,1,&text)!=ESP_OK && !text && !live);
    }
    mode=0;fail_allocation=1;
    assert(board_camera_jpeg_to_base64(raw,1,&text)==ESP_ERR_NO_MEM && !text && !live);
    fail_allocation=0;
    assert(board_camera_jpeg_to_base64(raw,1,&text)==ESP_OK && strcmp(text,"AA==")==0 && live==1);
    model_free(text);assert(!live);
    puts("Camera JPEG and Base64 ownership/bounds: PASS");return 0;
}
'''
        run_c_test(BASE + header + model + body + tests)

    def test_preview_notification_tail(self):
        """在 SetBits 已唤醒拥有者而尚未返回的交错点执行真实 stop。"""
        model = r'''
typedef uint32_t TickType_t;
typedef unsigned EventBits_t;
typedef void *TaskHandle_t;
#define configTICK_RATE_HZ 100U
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms)*configTICK_RATE_HZ/1000U)
#define pdTICKS_TO_MS(ticks) ((ticks)*10U)
#define pdTRUE 1
#define pdFALSE 0
#define BOARD_CAMERA_STOP_TIMEOUT_MS 1000U
#define BOARD_CAMERA_FRAME_STALL_MS 5000U
#define BOARD_CAMERA_STREAM_RECOVERY_COUNT 2U
#define BOARD_CAMERA_PREVIEW_FPS 24U
#define BOARD_CAMERA_BUFFER_COUNT 4
#define BOARD_CAMERA_PREVIEW_BUFFER_COUNT 2
#define BOARD_CAMERA_PREVIEW_EXITED_BIT 1U
#define BOARD_CAMERA_FIRST_FRAME_BIT 2U
#define BOARD_CAMERA_STACK_IN_PSRAM 0
#define BOARD_CAMERA_PREVIEW_IDLE_DELAY_MS 1U
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_MEMORY_MMAP 2
#define V4L2_BUF_FLAG_DONE 4
#define V4L2_BUF_FLAG_ERROR 8
#define VIDIOC_STREAMOFF 10
#define VIDIOC_DQBUF 11
#define VIDIOC_QBUF 12
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C 1
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE 2
#ifdef _MSC_VER
#define __ATOMIC_ACQUIRE 0
#define __ATOMIC_RELEASE 0
#define __atomic_load_n(p,o) (*(p))
#define __atomic_store_n(p,v,o) (*(p)=(v))
#endif
struct v4l2_buffer {unsigned type,memory,index,flags,bytesused;};
static bool s_camera_ready=true,s_streaming,s_preview_paused,s_preview_exited;
static bool s_startup_ready=true,s_has_latest_frame,s_driver_streaming;
static bool s_buffers_queued;
static unsigned s_next_queue_index;
static void *s_preview_task,*s_preview_events=(void*)3,*s_frame_mutex=(void*)4;
static int s_video_fd=7,s_width=2,s_height=2;
static size_t s_frame_bytes=8;
static struct {uint8_t *data;size_t length;} s_buffers[4];
static uint8_t *s_preview_frames[2],*s_latest_frame;
static TickType_t now;
static TickType_t advance_after_clock_read,wait_ticks;
static EventBits_t bits=BOARD_CAMERA_FIRST_FRAME_BIT;
static void *current_task=(void*)1;
static int tail_stop_result,streamoff_calls,deleted,raw_live,stop_during_lock;
static TickType_t xTaskGetTickCount(void) {
    TickType_t result=now;
    now+=advance_after_clock_read;advance_after_clock_read=0;
    return result;
}
static void *xTaskGetCurrentTaskHandle(void) {return current_task;}
static void vTaskDelay(TickType_t ticks) {assert(ticks>0);now+=ticks;}
static int xSemaphoreTake(void *mutex,TickType_t ticks) {
    (void)mutex;(void)ticks;
    if(stop_during_lock) s_streaming=false;
    return pdTRUE;
}
static void xSemaphoreGive(void *mutex) {(void)mutex;}
static EventBits_t xEventGroupGetBits(void *group) {(void)group;return bits;}
static void xEventGroupClearBits(void *group,EventBits_t clear) {(void)group;bits&=~clear;}
static EventBits_t xEventGroupWaitBits(void *group,EventBits_t wait,int clear,int all,TickType_t ticks) {
    (void)group;(void)clear;(void)all;wait_ticks=ticks;
    if(!(bits&wait)) now+=ticks;return bits;
}
static esp_err_t board_camera_stop_preview(void);
static void xEventGroupSetBits(void *group,EventBits_t set) {
    (void)group;bits|=set;
    if(set&BOARD_CAMERA_PREVIEW_EXITED_BIT) {
        current_task=(void*)2;
        tail_stop_result=board_camera_stop_preview();
        assert(tail_stop_result==ESP_ERR_TIMEOUT && s_preview_events==(void*)3);
        current_task=(void*)1;
    }
}
static int ioctl(int fd,unsigned request,void *arg) {
    (void)fd;(void)arg;assert(request==VIDIOC_STREAMOFF);streamoff_calls++;return 0;
}
static void camera_stats_record_preview(uint32_t count,bool has,uint32_t fps) {(void)count;(void)has;(void)fps;}
static void camera_stats_record_drop(void) {}
static void camera_notify_stats(void) {}
static void camera_log_clock_state(void) {}
static void camera_notify_preview(const void *p,int w,int h) {(void)p;(void)w;(void)h;}
static bool camera_restart_stream_after_failure(unsigned *n,TickType_t *t) {(void)n;(void)t;return false;}
static int copy_rgb565_camera_frame(uint8_t *d,const uint8_t *s,size_t n,size_t b) {(void)d;(void)s;(void)n;(void)b;return ESP_OK;}
static int esp_cache_msync(void *p,size_t n,int flags) {(void)p;(void)n;(void)flags;return ESP_OK;}
static void vTaskDelete(void *task) {assert(!task);deleted++;}
static uint8_t *alloc_frame_buffer(size_t size,const char *name) {
    (void)name;uint8_t *p=malloc(size);if(p)raw_live++;return p;
}
static void heap_caps_free(void *p) {if(p)raw_live--;free(p);}
'''
        signatures = [
            "static bool camera_streaming_load(", "static void camera_streaming_store(",
            "static bool camera_preview_paused_load(", "static void camera_preview_paused_store(",
            "static bool camera_ready_load(", "static TaskHandle_t camera_preview_task_load(",
            "static void camera_preview_task_store(", "static bool camera_preview_exited_load(",
            "static void camera_preview_exited_store(", "static TickType_t camera_timeout_ticks(",
            "static esp_err_t errno_to_esp_err(", "static inline bool cache_sync_ok_or_unsupported(",
            "static inline void preview_task_relax(", "static void camera_task_delete_self(",
            "static void free_frame_buffer(", "esp_err_t board_camera_stop_preview(",
            "static esp_err_t copy_latest_frame_for_jpeg(", "static void preview_task(",
        ]
        functions = "\n".join(selected(signature, optional="exited_" in signature or "timeout_ticks" in signature)
                               for signature in signatures)
        # 公共 stop 声明沿生产链接属性，提前声明供原生通知替身模拟 owner 交错。
        model = model.replace("static esp_err_t board_camera_stop_preview(void);", "esp_err_t board_camera_stop_preview(void);")
        tests = r'''
int main(void) {
    s_driver_streaming=true;s_streaming=false;s_preview_exited=false;
    preview_task(NULL);
    assert(deleted==1 && streamoff_calls==1 && s_preview_task==NULL && s_preview_exited);
    current_task=(void*)2;
    assert(board_camera_stop_preview()==ESP_OK && streamoff_calls==1);
    s_preview_task=current_task;s_preview_exited=false;s_streaming=true;bits=0;
    assert(board_camera_stop_preview()==ESP_ERR_INVALID_STATE && s_streaming);
    s_preview_task=(void*)1;
    assert(board_camera_stop_preview()==ESP_ERR_TIMEOUT && !s_streaming && s_preview_events==(void*)3);
    now=0;advance_after_clock_read=40;bits=0;
    assert(board_camera_stop_preview()==ESP_ERR_TIMEOUT && wait_ticks==60 && now==100);
    now=0;advance_after_clock_read=101;
    assert(board_camera_stop_preview()==ESP_ERR_TIMEOUT && wait_ticks==0 && now==101);
    s_preview_exited=true;bits=BOARD_CAMERA_PREVIEW_EXITED_BIT;
    assert(board_camera_stop_preview()==ESP_OK);
    uint8_t pixels[8]={0};s_latest_frame=pixels;s_has_latest_frame=true;
    uint8_t *raw=(void*)1;
    assert(copy_latest_frame_for_jpeg(&raw)==ESP_ERR_INVALID_STATE && !raw && !raw_live);
    s_streaming=true;stop_during_lock=1;
    assert(copy_latest_frame_for_jpeg(&raw)==ESP_ERR_INVALID_STATE && !raw && !raw_live);
    puts("Camera notification tail, retry, self-stop and capture gate: PASS");return 0;
}
'''
        run_c_test(BASE + model + functions + tests)

    def test_format_bounds_errno_and_ticks(self):
        """执行真实格式配置函数，模拟 32 位容量并让日志修改 errno。"""
        model = r'''
#undef SIZE_MAX
#define SIZE_MAX UINT32_MAX
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 100U
#define portMAX_DELAY UINT32_MAX
#define BOARD_CAMERA_WIDTH 240
#define BOARD_CAMERA_HEIGHT 240
#define BOARD_CAMERA_DEVICE_READY_RETRY_COUNT 2
#define BOARD_CAMERA_DEVICE_READY_RETRY_MS 50
#define O_RDWR 1
#define O_NONBLOCK 2
#define V4L2_BUF_TYPE_VIDEO_CAPTURE 1
#define V4L2_PIX_FMT_RGB565 2
#define V4L2_PIX_FMT_RGB565X 3
#define VIDIOC_S_DQBUF_TIMEOUT 4
#define VIDIOC_QUERYCAP 5
#define VIDIOC_S_FMT 6
#undef ESP_LOGE
#undef ESP_LOGW
#define ESP_LOGE(...) ((void)(errno=EIO))
#define ESP_LOGW(...) ((void)(errno=EIO))
struct timeval {long tv_sec,tv_usec;};
struct v4l2_capability {char driver[16],card[32];};
struct v4l2_format {int type;struct {struct {uint32_t width,height,pixelformat,sizeimage,bytesperline;} pix;} fmt;};
static int s_video_fd,s_width,s_height;
static size_t s_frame_bytes,s_src_stride_bytes,s_src_image_bytes;
static bool s_src_needs_byte_swap;
static int mode,open_calls;
static uint32_t width=240,height=240,stride;
static void camera_log_clock_state(void) {}
static int get_board_camera_path(const char **out) {*out="/video";return ESP_OK;}
static void log_fourcc(uint32_t value,char out[5]) {(void)value;memcpy(out,"RGBP",5);}
static void vTaskDelay(TickType_t ticks) {assert(ticks>0);}
static int open(const char *path,int flags) {
    (void)path;(void)flags;open_calls++;
    if(mode==1) {errno=EAGAIN;return -1;}
    if(mode==2) {errno=ENODEV;return -1;}
    return 7;
}
static int ioctl(int fd,unsigned command,void *arg) {
    assert(fd==7);
    if(mode==3 && command==VIDIOC_QUERYCAP) {errno=ENOMEM;return -1;}
    if(command==VIDIOC_S_FMT) {
        struct v4l2_format *format=arg;
        format->fmt.pix.width=width;format->fmt.pix.height=height;
        format->fmt.pix.bytesperline=stride;format->fmt.pix.pixelformat=V4L2_PIX_FMT_RGB565X;
    }
    return 0;
}
'''
        functions = "\n".join(selected(signature) for signature in [
            "static TickType_t camera_timeout_ticks(", "static esp_err_t errno_to_esp_err(",
            "static esp_err_t open_and_configure_video(",
        ])
        tests = r'''
int main(void) {
    assert(camera_timeout_ticks(0)==0 && camera_timeout_ticks(1)==1 && camera_timeout_ticks(2)==1);
    assert(camera_timeout_ticks(1000)==100 && camera_timeout_ticks(UINT32_MAX)<portMAX_DELAY);
    mode=1;assert(open_and_configure_video()==ESP_ERR_TIMEOUT);
    mode=2;open_calls=0;assert(open_and_configure_video()==ESP_ERR_NOT_FOUND && open_calls==2);
    mode=3;assert(open_and_configure_video()==ESP_ERR_NO_MEM);
    mode=0;width=0;assert(open_and_configure_video()==ESP_ERR_INVALID_ARG);
    width=UINT32_MAX;assert(open_and_configure_video()==ESP_ERR_INVALID_ARG);
    width=INT_MAX;height=2;assert(open_and_configure_video()==ESP_ERR_INVALID_SIZE);
    width=240;height=240;stride=479;assert(open_and_configure_video()==ESP_ERR_INVALID_SIZE);
    stride=0;assert(open_and_configure_video()==ESP_OK);
    assert(s_width==240 && s_height==240 && s_frame_bytes==115200 && s_src_stride_bytes==480 && s_src_image_bytes==115200);
    puts("Camera format bounds, saved errno and positive finite ticks: PASS");return 0;
}
'''
        run_c_test(BASE + model + functions + tests)


if __name__ == "__main__":
    unittest.main()
