"""执行真实 OTA 事务函数，核对句柄所有权、整数边界和接收前参数校验。"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from test_event_ring_ownership import run_c_test as run_plain_c_test
from test_ota_boot_confirmation import BUILDER_PREFIX, builder_source

ROOT = Path(__file__).resolve().parents[1]


def run_c_test(source):
    """普通模式沿用既有 runner，SAN 模式在独立目录实际插桩同一组 C 断言。"""
    if os.environ.get('CCLAW_TEST_SANITIZERS') != '1':
        run_plain_c_test(source)
        return
    compiler = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if os.name == 'nt' or not compiler:
        raise RuntimeError('OTA sanitizer 夹具需要 Linux/WSL 的本机 C 编译器')
    with tempfile.TemporaryDirectory(prefix='cclaw-ota-san-') as folder:
        directory = Path(folder)
        fixture = directory / 'fixture.c'
        binary = directory / 'fixture'
        fixture.write_text(source, encoding='utf-8')
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-o', str(binary), str(fixture)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


def fn(path, signature):
    """按完整签名提取真实生产函数，保留所有原始分支和局部声明。"""
    source = (ROOT / path).read_text(encoding='utf-8-sig')
    start = source.index(signature)
    return source[start:source.index('\n}\n', start) + 3]

MANAGER_PREFIX = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_OTA_VALIDATE_FAILED 4
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef struct { char label[24]; size_t size; unsigned address, erase_size; } esp_partition_t;
typedef int esp_ota_handle_t;
enum { OTA_MANAGER_IDLE, OTA_MANAGER_UPLOADING, OTA_MANAGER_PENDING_REBOOT, OTA_MANAGER_ERROR };
static struct {
    int state; esp_partition_t target_copy; const esp_partition_t *target;
    esp_ota_handle_t update_handle; bool update_handle_open;
    size_t expected_size, bytes_written, next_report_bytes;
    bool image_header_checked, write_diag_logged;
    char upload_name[96], error[128], upload_browser_time[40];
} s_ota;
static esp_partition_t running = {"ota_0", SIZE_MAX, 0, 4096};
static esp_partition_t target = {"ota_1", SIZE_MAX, 1, 4096};
static int begin_calls, write_calls, end_calls, abort_calls, boot_calls, fail_write, fail_end;
static const esp_partition_t *esp_ota_get_running_partition(void) { return &running; }
static const esp_partition_t *esp_ota_get_next_update_partition(void *p) { (void)p; return &target; }
static const esp_partition_t *esp_ota_get_boot_partition(void) { return &running; }
static const esp_partition_t *find_app_partition_by_label(const char *s) { return s && !strcmp(s,"ota_1") ? &target : NULL; }
static void set_error(const char *fmt, ...) { (void)fmt; s_ota.state = OTA_MANAGER_ERROR; }
static void copy_upload_name(const char *s) { snprintf(s_ota.upload_name,sizeof(s_ota.upload_name),"%s",s?s:""); }
static void copy_browser_time(char *d,size_t n,const char *s) { snprintf(d,n,"%s",s?s:""); }
static void refresh_partition_status(void) {}
static const char *esp_err_to_name(int e) { (void)e; return "stub"; }
static int esp_ota_begin(const esp_partition_t *p,size_t n,int *h) { (void)p;(void)n;begin_calls++;*h=8;return 0; }
static int esp_ota_write(int h,const void *p,size_t n) { assert(h==8);(void)p;(void)n;write_calls++;return fail_write; }
static int esp_ota_end(int h) { assert(h==8);end_calls++;return fail_end; }
static int esp_ota_abort(int h) { assert(h==8);abort_calls++;return 0; }
static int esp_ota_set_boot_partition(const esp_partition_t *p) { (void)p;boot_calls++;return 0; }
static int write_partition_uploaded_at(const esp_partition_t *p,const char *s) { (void)p;(void)s;return 0; }
'''
MANAGER_TESTS = r'''
int main(void) {
    unsigned char magic=0xe9;
    assert(ota_manager_begin_locked(10,"f","t")==0);
    assert(ota_manager_write_locked(&magic,0)==0 && begin_calls==0);
    assert(ota_manager_write_locked(&magic,1)==0 && begin_calls==1);
    fail_write=77;
    assert(ota_manager_write_locked(&magic,1)==77 && s_ota.update_handle_open);
    assert(ota_manager_begin_locked(10,"f",NULL)==ESP_ERR_INVALID_STATE);
    assert(ota_manager_set_boot_partition_locked("ota_1")==ESP_ERR_INVALID_STATE);
    assert(boot_calls==0 && abort_calls==0 && s_ota.update_handle==8);
    ota_manager_cancel_locked();
    assert(abort_calls==1 && !s_ota.update_handle_open);
    fail_write=0;
    assert(ota_manager_begin_locked(1,"f",NULL)==0);
    assert(ota_manager_write_locked(&magic,1)==0);
    fail_end=78;
    assert(ota_manager_finish_locked()==78 && !s_ota.update_handle_open);
    ota_manager_cancel_locked(); assert(abort_calls==1);
    assert(ota_manager_begin_locked(1,"f",NULL)==0);
    assert(ota_manager_write_locked(&magic,SIZE_MAX)==ESP_ERR_INVALID_SIZE);
    ota_manager_cancel_locked();
    assert(ota_manager_begin_locked(SIZE_MAX,"f",NULL)==0);
    s_ota.bytes_written=SIZE_MAX-1; s_ota.image_header_checked=true;
    assert(ota_manager_write_locked(&magic,2)==ESP_ERR_INVALID_SIZE);
    ota_manager_cancel_locked();
    assert(ota_manager_begin_locked(1024,"f",NULL)==0);
    assert(ota_manager_write_locked(&magic,1)==0);
    s_ota.bytes_written=1023;
    assert(ota_manager_write_locked(&magic,1)==0);
    fail_end=0;
    assert(ota_manager_finish_locked()==0 && boot_calls==1);
    ota_manager_cancel_locked(); assert(abort_calls==1);
    assert(ota_manager_begin_locked(1,"f",NULL)==0);
    magic=0;
    assert(ota_manager_write_locked(&magic,1)==ESP_ERR_OTA_VALIDATE_FAILED && !s_ota.update_handle_open);
    puts("manager real functions: handle retention, abort/end ownership, overflow, zero block, finish PASS");
    return 0;
}
'''
PARSER_PREFIX = '''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
'''
PARSER_TESTS = r'''
int main(void) {
 size_t n=99; char hash[65]; unsigned char image[34]={0xe9,1};
 assert(!ota_parse_size(NULL,&n)&&n==0);
 assert(!ota_parse_size("-1",&n)); assert(!ota_parse_size("+1",&n));
 assert(!ota_parse_size("1x",&n)); assert(!ota_parse_size(" 1",&n));
 assert(!ota_parse_size("0",&n)); assert(!ota_parse_size("18446744073709551616",&n));
 assert(ota_parse_size("00123",&n)&&n==123);
 assert(ota_normalize_sha256("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789",hash));
 assert(!strcmp(hash,"abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"));
 assert(!ota_normalize_sha256("x",hash));
 assert(!ota_validate_image_layout(NULL,0));
 assert(ota_validate_image_layout(image,sizeof image));
 image[28]=0xff; assert(!ota_validate_image_layout(image,sizeof image));
 image[28]=0; image[1]=17; assert(!ota_validate_image_layout(image,sizeof image));
 puts("service real parsers: decimal, overflow, SHA normalization, image layout PASS");
}
'''

# 模拟 watchdog 观察旧恢复标志后被新操作抢先完成恢复和 prepare 的交错。
WATCHDOG_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define OTA_SERVICE_FAILURE_UPLOAD_TIMEOUT 1
#ifndef __GNUC__
#define __ATOMIC_ACQUIRE 2
#define __atomic_load_n(pointer,order) (*(pointer))
#endif
static bool s_ota_recovery_pending, s_ota_prepared;
static uint64_t s_ota_recovery_retry_after_ms, s_ota_prepared_deadline_ms;
static uint32_t s_ota_recovery_retry_delay_ms=1000;
static int s_ota_operation_mutex=1;
static int delays, leaves, failures, lock_depth, replace_observation, fail_recovery;
static jmp_buf end_iteration;
/* 第二次延时结束测试，只运行真实无限循环的一轮，无 task 或栈资源需回收。 */
static void vTaskDelay(int ticks) {
    assert(ticks==1000);
    if (++delays==2) longjmp(end_iteration,1);
}
static uint64_t ota_monotonic_ms(void) { return 1000; }
/* 获取租约时模拟另一操作已经清除恢复标志并建立新的准备截止时间。 */
static int xSemaphoreTakeRecursive(int mutex,int ticks) {
    (void)mutex; (void)ticks;
    assert(!lock_depth);
    lock_depth=1;
    if (replace_observation) {
        s_ota_recovery_pending=false;
        s_ota_prepared=true;
        s_ota_prepared_deadline_ms=2000;
    }
    return pdTRUE;
}
static void xSemaphoreGiveRecursive(int mutex) {
    (void)mutex;
    assert(lock_depth==1);
    lock_depth=0;
}
static bool ota_state_lock(void) { assert(lock_depth); return true; }
static void ota_state_unlock(void) {}
/* 部分恢复失败时保留责任，以便真实退避代码安排下次重试。 */
static void ota_leave_maintenance_mode(void) {
    assert(lock_depth);
    leaves++;
    s_ota_prepared=false;
    s_ota_recovery_pending=fail_recovery!=0;
}
static void ota_record_failure(int code,const char *stage,const char *message) {
    (void)code; (void)stage; (void)message;
    failures++;
}
static void board_ui_ota_end(bool success,const char *message) { (void)success; (void)message; }
'''
WATCHDOG_TESTS = r'''
/* 每个场景重新建立全局状态，迭代退出点确保已释放操作锁。 */
static void reset_watch(void) {
    assert(!lock_depth);
    s_ota_recovery_pending=false;
    s_ota_prepared=true;
    s_ota_prepared_deadline_ms=2000;
    s_ota_recovery_retry_after_ms=0;
    s_ota_recovery_retry_delay_ms=1000;
    delays=leaves=failures=replace_observation=fail_recovery=0;
}
static void one_iteration(void) {
    if (setjmp(end_iteration)==0) ota_prepared_watchdog_task(NULL);
    assert(!lock_depth);
}
int main(void) {
    reset_watch(); s_ota_recovery_pending=true; replace_observation=1;
    one_iteration();
    assert(s_ota_prepared && !leaves && !failures);
    reset_watch(); s_ota_prepared_deadline_ms=500;
    one_iteration();
    assert(!s_ota_prepared && leaves==1 && failures==1);
    reset_watch(); s_ota_recovery_pending=true;
    one_iteration();
    assert(leaves==1 && !s_ota_recovery_pending && !failures);
    reset_watch(); s_ota_recovery_pending=true; fail_recovery=1;
    one_iteration();
    assert(leaves==1 && s_ota_recovery_pending);
    assert(s_ota_recovery_retry_delay_ms==2000 && s_ota_recovery_retry_after_ms==3000);
    puts("service real watchdog: rechecked recovery lease, expiry, retry recovery and backoff PASS");
}
'''

# 用构造器真实实现注入扩容故障，验证 OTA 每次 append 的拥有诊断均已消费。
BUILDER_TESTS = r'''
int main(void) {
    cc_string_builder_t sb;
    fail_builder_init=1;
    assert(!ota_builder_init(&sb));
    cc_string_builder_deinit(&sb);
    assert(messages_created==messages_released);
    fail_builder_init=0;
    assert(ota_builder_init(&sb));
    char large[1024]; memset(large,'x',sizeof large-1); large[sizeof large-1]=0;
    fail_builder_growth=1;
    ota_json_escape_append(&sb,large);
    assert(cc_string_builder_take(&sb)==NULL);
    assert(messages_created>1 && messages_created==messages_released);
    fail_builder_growth=0;
    assert(ota_builder_init(&sb));
    ota_json_escape_append(&sb,"\n\"\\\t");
    char *json=cc_string_builder_take(&sb);
    assert(json && strcmp(json,"\"\\n\\\"\\\\\\t\"")==0); free(json);
    puts("service real builder: init/growth failure diagnostics, sticky cleanup, JSON escaping PASS");
}
'''

# 首次创建递归锁时重入模拟两个 initializer 的受控交错，外层 CAS 必须释放落选候选。
INIT_PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_STATE 3
#define pdPASS 1
#define portMAX_DELAY 0
#define OTA_IO_CHUNK_FALLBACK 4096
#define OTA_PREPARE_WATCH_STACK_BYTES 8192
typedef struct mutex_fixture { int depth; } *SemaphoreHandle_t;
typedef void *TaskHandle_t;
#ifndef __GNUC__
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __atomic_load_n(pointer,order) (*(pointer))
#define __atomic_store_n(pointer,value,order) (*(pointer)=(value))
/* 确定性交错 fixture 的顺序 CAS，用于 MSVC 运行同一组发布分支断言。 */
static bool fixture_cas(SemaphoreHandle_t *p,SemaphoreHandle_t *expected,SemaphoreHandle_t value) {
    if (*p!=*expected) { *expected=*p; return false; } *p=value; return true;
}
#define __atomic_compare_exchange_n(p,e,v,weak,success,failure) fixture_cas(p,e,v)
#endif
'''
INIT_STUBS = r'''
static ota_service_transport_t s_transport, wanted;
static bool s_ota_initialized;
static SemaphoreHandle_t s_ota_operation_mutex, s_ota_mutex;
static TaskHandle_t s_ota_watchdog;
static int created,deleted,watchdogs,inject_reentry,fail_task;
static esp_err_t ota_service_init(const ota_service_transport_t *transport);
/* worker 为惰性桩，初始化测试只核对是否重复创建及其栈容量。 */
static void ota_prepared_watchdog_task(void *arg) { (void)arg; }
static SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
    SemaphoreHandle_t p=calloc(1,sizeof(*p)); assert(p); created++;
    if (inject_reentry) { inject_reentry=0; assert(ota_service_init(&wanted)==ESP_OK); }
    return p;
}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreHandle_t p=calloc(1,sizeof(*p)); assert(p); created++; return p;
}
static void vSemaphoreDelete(SemaphoreHandle_t p) { assert(p && !p->depth); deleted++; free(p); }
static int xSemaphoreTakeRecursive(SemaphoreHandle_t p,int timeout) { (void)timeout; assert(p); p->depth++; return 1; }
static void xSemaphoreGiveRecursive(SemaphoreHandle_t p) { assert(p && p->depth); p->depth--; }
static int xTaskCreate(void (*task)(void *),const char *name,int stack,void *arg,int priority,TaskHandle_t *out) {
    (void)task;(void)name;(void)arg;(void)priority; assert(stack==8192);
    if (fail_task) return 0;
    watchdogs++;
    *out=(void *)1;
    return pdPASS;
}
static esp_err_t text(void *r,const char *t,const char *type) { (void)r;(void)t;(void)type; return 0; }
static esp_err_t error(void *r,int code,const char *m) { (void)r;(void)code;(void)m; return 0; }
static char *query(void *r,const char *key) { (void)r;(void)key; return NULL; }
static int recv_body(void *r,char *b,int n) { (void)r;(void)b;(void)n; return 0; }
static esp_err_t web(void *arg) { (void)arg; return 0; }
'''
INIT_TESTS = r'''
int main(void) {
    wanted.send_text=text; wanted.send_error=error; wanted.query_value=query;
    wanted.recv=recv_body; wanted.stop_web=web; wanted.start_web=web;
    wanted.request_timeout_code=-7;
    assert(ota_service_init(NULL)==ESP_ERR_INVALID_ARG);
    wanted.io_chunk=(size_t)INT_MAX+1U;
    assert(ota_service_init(&wanted)==ESP_ERR_INVALID_ARG);
    wanted.io_chunk=0;
    inject_reentry=1;
    assert(ota_service_init(&wanted)==ESP_OK);
    assert(s_ota_initialized && watchdogs==1 && created-deleted==2);
    assert(ota_service_init(&wanted)==ESP_OK && watchdogs==1);
    ota_service_transport_t changed=wanted; changed.user_data=(void *)2;
    assert(ota_service_init(&changed)==ESP_ERR_INVALID_STATE);
    assert(!s_transport.user_data && s_transport.io_chunk==4096);
    vSemaphoreDelete(s_ota_mutex); vSemaphoreDelete(s_ota_operation_mutex);
    s_ota_mutex=NULL; s_ota_operation_mutex=NULL; s_ota_watchdog=NULL; s_ota_initialized=false;
    fail_task=1;
    assert(ota_service_init(&wanted)==ESP_ERR_NO_MEM && !s_ota_initialized);
    assert(s_ota_operation_mutex->depth==0);
    fail_task=0;
    assert(ota_service_init(&wanted)==ESP_OK && watchdogs==2);
    vSemaphoreDelete(s_ota_mutex); vSemaphoreDelete(s_ota_operation_mutex);
    assert(created==deleted);
    puts("service real init: validation, competing publication, immutable transport, task retry PASS");
}
'''


class OtaTransactionBoundaryTests(unittest.TestCase):
    """每个入口独立提取当前生产函数，再交给既有宿主 C runner 编译执行。"""

    def test_manager_writer_ownership_and_integer_boundaries(self):
        """覆盖 ERROR 句柄保留、abort/end 消费、剩余量溢出及零长度块。"""
        manager = '\n'.join(fn('main/services/ota_manager.c', signature) for signature in (
            'static esp_err_t ota_manager_begin_locked(',
            'static esp_err_t ota_manager_write_locked(',
            'static esp_err_t ota_manager_finish_locked(',
            'static void ota_manager_cancel_locked(',
            'static esp_err_t ota_manager_set_boot_partition_locked(',
        ))
        run_c_test(MANAGER_PREFIX + manager + MANAGER_TESTS)

    def test_service_parsers_and_image_layout(self):
        """覆盖严格十进制大小、SHA-256 规范化及镜像布局边界。"""
        parsers = '\n'.join(fn('main/services/ota_service.c', signature) for signature in (
            'static bool ota_parse_size(',
            'static bool ota_normalize_sha256(',
            'static bool ota_validate_image_layout(',
        ))
        run_c_test(PARSER_PREFIX + parsers + PARSER_TESTS)

    def test_watchdog_rechecks_lease_and_recovery(self):
        """覆盖旧观察值交错、真实准备到期、恢复成功及失败退避。"""
        watchdog = '\n'.join(fn('main/services/ota_service.c', signature) for signature in (
            'static bool ota_expire_prepared(',
            'static void ota_prepared_watchdog_task(void *arg)\n{',
        ))
        run_c_test(WATCHDOG_PREFIX + watchdog + WATCHDOG_TESTS)

    def test_builder_failure_releases_owned_diagnostics(self):
        """以真实 builder 注入初始化和扩容失败，核对诊断释放及 JSON 转义。"""
        helpers = '\n'.join(fn('main/services/ota_service.c', signature) for signature in (
            'static void ota_consume_builder_result(',
            'static bool ota_builder_init(',
            'static void ota_json_escape_append(',
        ))
        run_c_test(BUILDER_PREFIX + builder_source() + helpers + BUILDER_TESTS)

    def test_init_publication_immutable_transport_and_retry(self):
        """覆盖参数拒绝、竞争锁发布、同表幂等、异表拒绝及任务创建失败重试。"""
        header = (ROOT / 'main/services/ota_service.h').read_text(encoding='utf-8')
        start = header.index('typedef struct {')
        end = header.index('} ota_service_transport_t;') + len('} ota_service_transport_t;')
        transport_type = header[start:end]
        init_code = fn('main/services/ota_service.c', 'esp_err_t ota_service_init(')
        init_code = init_code.replace('esp_err_t ota_service_init(',
                                      'static esp_err_t ota_service_init(', 1)
        run_c_test(INIT_PREFIX + transport_type + INIT_STUBS + init_code + INIT_TESTS)


if __name__ == '__main__':
    unittest.main()
