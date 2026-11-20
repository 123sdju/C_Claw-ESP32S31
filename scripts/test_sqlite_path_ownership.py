"""编译实际 SQLite 构造函数，核对路径 OOM 在打开数据库前退出。"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from test_event_ring_ownership import run_c_test as run_plain_c_test
from test_profile_task_lifetime import function

ROOT = Path(__file__).resolve().parents[1]


def run_c_test(source):
    """普通模式沿用既有 runner，SAN 模式独立编译同一组创建期断言。"""
    if os.environ.get('CCLAW_TEST_SANITIZERS') != '1':
        run_plain_c_test(source)
        return
    compiler = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
    if os.name == 'nt' or not compiler:
        raise RuntimeError('SQLite sanitizer 夹具需要 Linux/WSL 的本机 C 编译器')
    with tempfile.TemporaryDirectory(prefix='cclaw-sqlite-path-san-') as folder:
        directory = Path(folder)
        fixture = directory / 'fixture.c'
        binary = directory / 'fixture'
        fixture.write_text(source, encoding='utf-8')
        subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-o', str(binary), str(fixture)], check=True)
        subprocess.run([str(binary)], check=True, timeout=30)


PREFIX = r'''
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
enum { CC_OK, CC_ERR_INVALID_ARGUMENT, CC_ERR_OUT_OF_MEMORY, CC_ERR_STORAGE };
typedef struct { int code; } cc_result_t;
typedef int cc_mutex_t;
typedef struct { int unused; } sqlite3;
typedef struct {
    sqlite3 *db;
    cc_mutex_t mutex;
    int transaction_failed;
} cc_sqlite_session_store_t;
typedef struct { void *self; const int *vtable; } cc_session_store_t;
enum { SQLITE_OK, SQLITE_OPEN_READWRITE=2, SQLITE_OPEN_CREATE=4, SQLITE_NOMEM=7, SQLITE_OPEN_FULLMUTEX=0x10000 };
#define CC_DEFAULT_STORAGE_PATH "default.db"
static const int sqlite_vtable = 0;
static const char *init_sql = "schema";
static sqlite3 database;
static int path_failure, pragma_failure, created, destroyed, opened, closed, allocations;
static unsigned destroyed_mask;
static const char *expected_path;

/** @brief 构造替身成功值。 @return 无拥有诊断的成功结果。 */
static cc_result_t cc_result_ok(void) { return (cc_result_t){CC_OK}; }
/** @brief 构造替身错误。 @param[in] code 错误码。 @param[in] text 借用描述。 @return 错误结果。 */
static cc_result_t cc_result_error(int code, const char *text) {
    (void)text; return (cc_result_t){code};
}
/** @brief 登记状态分配。 @param[in] n 项数。 @param[in] size 项大小。 @return 清零堆地址。 */
static void *tracked_calloc(size_t n, size_t size) {
    void *p = calloc(n, size); assert(p); allocations++; return p;
}
/** @brief 归还状态或路径。 @param[in,out] p 可空拥有指针。 @post 有效分配数递减。 */
static void tracked_free(void *p) {
    if (p) { assert(allocations > 0); allocations--; free(p); }
}
/** @brief 在路径复制点注入 OOM。 @param[in] text 借用路径。 @return 独立副本或 NULL。 */
static char *cc_copy_string(const char *text) {
    if (path_failure) return NULL;
    size_t bytes = strlen(text) + 1;
    char *p = malloc(bytes); assert(p); memcpy(p, text, bytes); allocations++; return p;
}
/** @brief 登记成功 mutex。 @param[out] out 句柄。 @return 成功结果。 */
static cc_result_t cc_mutex_create(cc_mutex_t *out) { *out = ++created; return cc_result_ok(); }
/** @brief 验证锁已创建且逐个归还。 @param[in,out] mutex 拥有句柄。 @post 销毁计数递增。 */
static void cc_mutex_destroy(cc_mutex_t mutex) {
    assert(mutex > 0 && mutex <= created);
    unsigned bit = 1U << mutex;
    assert(!(destroyed_mask & bit));
    destroyed_mask |= bit;
    destroyed++;
}
/** @brief 捕获 SQLite 打开实参。 @param[in] path 路径。 @param[out] out 连接。 @param[in] flags 打开位。 @param[in] vfs 可空后端名。 @return 成功。 */
static int sqlite3_open_v2(const char *path, sqlite3 **out, int flags, const char *vfs) {
    opened++; assert(path && strcmp(path, expected_path) == 0);
    assert(flags == (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX));
    assert(!vfs); *out = &database; return SQLITE_OK;
}
/** @brief 借出错误文本。 @param[in] db 连接。 @return 静态说明。 */
static const char *sqlite3_errmsg(sqlite3 *db) { (void)db; return "database error"; }
/** @brief 登记连接关闭。 @param[in,out] db 连接。 @return 成功。 */
static int sqlite3_close(sqlite3 *db) { assert(db == &database); closed++; return SQLITE_OK; }
/** @brief 模拟表初始化。 @param[in] db 连接。 @param[in] sql SQL。 @param[in] callback 空回调。 @param[in] data 空数据。 @param[out] error 错误槽。 @return 成功。 */
static int sqlite3_exec(sqlite3 *db, const char *sql, void *callback, void *data, char **error) {
    assert(db == &database && sql == init_sql && !callback && !data && error); return SQLITE_OK;
}
/** @brief 模拟 SQLite 错误分配清理。 @param[in,out] p 可空指针。 @post 指针归还。 */
static void sqlite3_free(void *p) { free(p); }
/** @brief 模拟 pragma 成功或失败。 @param[in] db 连接。 @param[in] wal WAL选择。 @return 无拥有诊断的结果，无额外资源。 */
static cc_result_t setup_pragmas(sqlite3 *db, int wal) {
    assert(db == &database && wal == 1);
    return pragma_failure ? cc_result_error(CC_ERR_STORAGE, "pragma failed") : cc_result_ok();
}
#define calloc tracked_calloc
#define free tracked_free
'''

TESTS = r'''
#undef calloc
#undef free
/** @brief 核对路径失败和正常默认/显式路径。 @return 全部通过为零。 */
int main(void) {
    cc_session_store_t out = {&database, &sqlite_vtable};
    path_failure = 1;
    const char *paths[] = {NULL, "configured.db"};
    cc_result_t rc;
    for (size_t i = 0; i < 2; i++) {
        out = (cc_session_store_t){&database, &sqlite_vtable};
        rc = cc_sqlite_session_store_create(paths[i], &out);
        assert(rc.code == CC_ERR_OUT_OF_MEMORY);
        assert(!out.self && !out.vtable && !opened && !closed && !allocations);
    }
    assert(created == 2 && destroyed == 2);
    path_failure = 0;
    pragma_failure = 1;
    expected_path = CC_DEFAULT_STORAGE_PATH;
    rc = cc_sqlite_session_store_create(NULL, &out);
    assert(rc.code == CC_ERR_STORAGE && !out.self && !out.vtable);
    assert(opened == 1 && closed == 1 && created == destroyed && !allocations);
    pragma_failure = 0;
    for (size_t i = 0; i < 2; i++) {
        expected_path = paths[i] ? paths[i] : CC_DEFAULT_STORAGE_PATH;
        rc = cc_sqlite_session_store_create(paths[i], &out);
        assert(rc.code == CC_OK && out.self && out.vtable == &sqlite_vtable);
        assert(allocations == 1);
        cc_sqlite_session_store_t *store = out.self;
        sqlite3_close(store->db);
        cc_mutex_destroy(store->mutex);
        tracked_free(store);
    }
    assert(opened == 3 && closed == 3 && created == destroyed && !allocations);
    return 0;
}
'''


class SqlitePathOwnership(unittest.TestCase):
    """用实际路径 helper 与构造函数核对创建期拥有资源。"""

    def test_path_oom_preserves_failure(self):
        """路径 OOM 返回内存错误且不开库，正常路径仍可交付并完整归还资源。"""
        source = (ROOT / "third_party/cclaw-sdk/cclaw/adapters/src/storage/cc_sqlite_session_store.c").read_text(encoding="utf-8")
        functions = function(source, "static char *build_db_path(")
        functions += "\n" + function(source, "cc_result_t cc_sqlite_session_store_create(")
        run_c_test(PREFIX + functions + TESTS)


if __name__ == "__main__":
    unittest.main()
