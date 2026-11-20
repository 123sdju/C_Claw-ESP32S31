"""提取本机 IDF 的真实 FatFs 关闭代码，区分 share 遗留和动态缓冲清理。"""
import os
from pathlib import Path
import unittest

from test_event_ring_ownership import run_c_test
from test_profile_task_lifetime import function

FATFS_SOURCE = next((path for path in (
    Path(os.environ.get("CC_TEST_FATFS_SOURCE", "")),
    Path("C:/esp/master/esp-idf/components/fatfs/src/ff.c"),
    Path("/mnt/c/esp/master/esp-idf/components/fatfs/src/ff.c"),
) if path.is_file()), None)


def check_close(lock_count, dynamic_buffer):
    """编译实际 f_close 和 share 辅助函数，断言失败抛出测试异常。"""
    source = FATFS_SOURCE.read_text(encoding="utf-8")
    prefix = r'''
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#define FF_FS_LOCK LOCK_COUNT
#define FF_FS_READONLY 0
#define FF_FS_REENTRANT 0
#define FF_FS_TINY 0
#define FF_USE_DYN_BUFFER DYNAMIC_BUFFER
typedef unsigned UINT;
typedef enum { FR_OK, FR_DISK_ERR, FR_INT_ERR, FR_INVALID_OBJECT } FRESULT;
typedef struct { int marker; } FATFS;
typedef struct { FATFS *fs; UINT lockid; } FFOBJID;
typedef struct { FFOBJID obj; void *buf; } FIL;
typedef struct { FATFS *fs; UINT ctr; } FILESEM;
static FILESEM Files[FF_FS_LOCK ? FF_FS_LOCK : 1];
static FRESULT sync_result, validate_result;
static unsigned freed;
/** @brief 同步阶段故障替身。 @param[in] fp 借用文件。 @return 当前指定结果。 */
static FRESULT f_sync(FIL *fp) { assert(fp); return sync_result; }
/** @brief 卷校验替身。 @param[in] obj 借用对象。 @param[out] fs 借用卷输出。 @return 当前指定结果。 */
static FRESULT validate(FFOBJID *obj, FATFS **fs) { *fs = obj->fs; return validate_result; }
/** @brief 记录真实关闭代码的动态缓冲释放。 @param[in] ptr 交接堆缓冲。 @post 释放一次并累计计数。 */
static void ff_memfree(void *ptr) { assert(ptr); freed++; free(ptr); }
'''.replace("LOCK_COUNT", str(lock_count)).replace("DYNAMIC_BUFFER", str(dynamic_buffer))
    native = ""
    if lock_count:
        native += function(source, "static FRESULT dec_share (")
        native += "\n" + function(source, "static void clear_share (")
    native += "\n" + function(source, "FRESULT f_close (")
    checks = r'''
/** @brief 检查真实关闭函数的失败退场和恢复边界。 @return 全部断言通过返回零。 */
int main(void) {
    FATFS volume = {1};
    FIL file = {{&volume, 1}, NULL};
    Files[0].fs = &volume;
    Files[0].ctr = 0x100;
#if FF_USE_DYN_BUFFER
    file.buf = malloc(8);
    assert(file.buf);
#endif
    sync_result = FR_DISK_ERR;
    assert(f_close(&file) == FR_DISK_ERR);
    assert(file.obj.fs == &volume);
#if FF_FS_LOCK
    assert(Files[0].fs == &volume && Files[0].ctr == 0x100);
#endif
#if FF_USE_DYN_BUFFER
    assert(!file.buf && freed == 1);
#endif
    sync_result = FR_OK;
    validate_result = FR_INVALID_OBJECT;
    assert(f_close(&file) == FR_INVALID_OBJECT);
    assert(file.obj.fs == &volume);
    validate_result = FR_OK;
    assert(f_close(&file) == FR_OK);
    assert(!file.obj.fs);
#if FF_FS_LOCK
    assert(!Files[0].fs && !Files[0].ctr);
    Files[0].fs = &volume;
    Files[0].ctr = 0x100;
    clear_share(&volume);
    assert(!Files[0].fs);
#endif
    return 0;
}
'''
    run_c_test(prefix + native + checks)


@unittest.skipUnless(FATFS_SOURCE, "本机未提供实际 IDF FatFs 源码")
class FatFsCloseTests(unittest.TestCase):
    """只验证真实关闭控制流，磁盘同步、内核互斥和 BSP 恢复仍使用替身。"""

    def test_without_share_table(self):
        """FF_FS_LOCK=0 时失败保持 FIL 有效，成功使对象失效。"""
        check_close(0, 0)

    def test_with_share_table(self):
        """FF_FS_LOCK=2 时失败保留 share 记录，成功或卷清理释放记录。"""
        check_close(2, 0)

    def test_dynamic_buffer_on_failure(self):
        """IDF 动态缓冲分支即使关闭失败也释放缓冲，重试不能假定原缓冲仍在。"""
        check_close(2, 1)


if __name__ == "__main__":
    unittest.main()
