"""Run real-Lua runtime contracts with a native C compiler on Linux.

The managed dependency must exist at managed_components/espressif__lua/lua
(materialized by ESP-IDF dependency resolution). The fixture compiles those
sources and the actual runtime/driver C files. ESP heap, FreeRTOS time and
cancellation queries use deterministic host stubs. UI hardware is excluded.

SANITIZE=1 (default) enables ASan/UBSan. SANITIZE=0 runs without sanitizers.
CCLAW_TEST_SANITIZERS is also accepted when SANITIZE is unset.
CC selects the compiler. LUA_TEST_REPO_ROOT selects a copied source tree.
LUA_TEST_BUILD_DIR selects a persistent independent output directory. Otherwise
the fixture creates a temporary directory and removes it after the process ends.
"""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

STUBS = {
    'sdkconfig.h': '#define CONFIG_SPIRAM 1\n',
    'esp_heap_caps.h': '''#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
void *heap_caps_malloc(size_t,int);
void *heap_caps_realloc(void*,size_t,int);
void heap_caps_free(void*);
size_t heap_caps_get_free_size(int);
size_t heap_caps_get_largest_free_block(int);
''',
    'esp_system.h': '''#include <stddef.h>
size_t esp_get_free_heap_size(void);
size_t esp_get_minimum_free_heap_size(void);
''',
    'freertos/FreeRTOS.h': '''#include <stdint.h>
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 1000
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(x) (x)
''',
    'freertos/task.h': '''#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t);
''',
}
HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua_runtime.h"
#include "lua_driver.h"
#include "freertos/task.h"
static size_t calls, fail_after, blocks;
static TickType_t ticks;
struct cc_cancel_token { int cancelled; TickType_t cancel_at; };
void *heap_caps_malloc(size_t n,int caps) {
    (void)caps;
    if (fail_after && ++calls >= fail_after) return NULL;
    void *p = malloc(n); if (p) blocks++; return p;
}
void *heap_caps_realloc(void *p,size_t n,int caps) {
    (void)caps;
    if (fail_after && ++calls >= fail_after) return NULL;
    void *q = realloc(p,n); if (q && !p) blocks++; return q;
}
void heap_caps_free(void *p) { if(p) { assert(blocks); blocks--; free(p); } }
size_t heap_caps_get_free_size(int c) { (void)c; return 1024; }
size_t heap_caps_get_largest_free_block(int c) { (void)c; return 512; }
size_t esp_get_free_heap_size(void) { return 1024; }
size_t esp_get_minimum_free_heap_size(void) { return 512; }
TickType_t xTaskGetTickCount(void) { return ticks++; }
void vTaskDelay(TickType_t n) { ticks += n; }
int cc_cancel_token_is_cancelled(cc_cancel_token_t *t) {
    return t && (t->cancelled || (t->cancel_at && ticks >= t->cancel_at));
}
cc_result_t cc_result_ok(void) { return (cc_result_t){.size=sizeof(cc_result_t)}; }
void cc_result_free(cc_result_t *r) { heap_caps_free(r->message); r->message=NULL; }
const lua_driver_descriptor_t g_board_lua_screen_driver = {0};
static int read_calls, null_file;
static cc_result_t read_relative(void *self,const char *base,const char *path,char **out) {
    (void)self; read_calls++;
    assert(strcmp(base,"/workspace")==0); assert(strcmp(path,"lua/example.lua")==0);
    *out=null_file ? NULL : strdup("return 7"); return cc_result_ok();
}
static void check_file(void) {
    cc_filesystem_vtable_t vt={.read_relative_text=read_relative};
    cc_filesystem_t fs={.vtable=&vt,.capabilities=CC_FS_CAP_ROOT_SCOPED};
    char *out=NULL;
    cc_result_t r=lua_runtime_run_file(&fs,"/workspace","example.lua",NULL,&out);
    assert(r.code==CC_OK && strstr(out,"7")); heap_caps_free(out); cc_result_free(&r);
    const char *bad[]={"../example.lua","/example.lua","a\\example.lua","C:example.lua","example.txt"};
    int before=read_calls;
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        out=(char*)1; r=lua_runtime_run_file(&fs,"/workspace",bad[i],NULL,&out);
        assert(r.code!=CC_OK && !out); cc_result_free(&r);
    }
    assert(read_calls==before);
    null_file=1;
    r=lua_runtime_run_file(&fs,"/workspace","lua/example.lua",NULL,&out);
    assert(r.code==CC_ERR_IO && !out); cc_result_free(&r); null_file=0;
    lua_runtime_limits_t limits=lua_runtime_default_limits(); limits.max_script_bytes=1;
    r=lua_runtime_run_file(&fs,"/workspace","example.lua",&limits,&out);
    assert(r.code==CC_ERR_LIMIT_EXCEEDED && !out); cc_result_free(&r);
    assert(blocks==0);
}
static void check(const char *code, cc_error_code_t expected, const char *contains,
                  lua_runtime_limits_t *limits) {
    char *out=(char*)1;
    cc_result_t r=lua_runtime_run_code(code,limits,&out);
    if (r.code!=expected) fprintf(stderr,"script=%s code=%d expected=%d error=%s\n",code,r.code,expected,r.message?r.message:"");
    assert(r.code==expected);
    if (expected==CC_OK) { assert(out); if(contains) assert(strstr(out,contains)); }
    else assert(!out);
    heap_caps_free(out); cc_result_free(&r); assert(blocks==0);
}
int main(void) {
    check_file();
    lua_runtime_limits_t limits=lua_runtime_default_limits();
    check("print('hello'); return 42",CC_OK,"return: 42",&limits);
    check("return cclaw.heap().free",CC_OK,"1024",&limits);
    check("return pcall(function() error('caught') end)",CC_OK,"false",&limits);
    check("return xpcall(function() error('caught') end, function(e) return 'handled' end)",CC_OK,"handled",&limits);
    check("return load('return 3')()",CC_OK,"3",&limits);
    check("local f,e=load(string.char(27)..'Lua',nil,'b'); return f==nil",CC_OK,"true",&limits);
    check("local =",CC_ERR_INVALID_ARGUMENT,NULL,&limits);
    check("error({})",CC_ERR_TOOL,NULL,&limits);
    check("return setmetatable({}, {__tostring=function() error('bad tostring') end})",CC_ERR_TOOL,NULL,&limits);
    limits.timeout_ms=20;
    check("while true do end",CC_ERR_TIMEOUT,NULL,&limits);
    check("return setmetatable({}, {__tostring=function() while true do end end})",CC_ERR_TIMEOUT,NULL,&limits);
    check("local ok=pcall(function() while true do end end); return ok",CC_OK,"false",&limits);
    struct cc_cancel_token token={.cancelled=1};
    limits.cancel_token=&token;
    check("return 1",CC_ERR_CANCELLED,NULL,&limits);
    token.cancelled=0; token.cancel_at=ticks+5;
    limits.timeout_ms=1000;
    check("while true do end",CC_ERR_CANCELLED,NULL,&limits);
    limits=lua_runtime_default_limits(); limits.max_output_bytes=1;
    check("print('abc')",CC_OK,"a",&limits);
    limits.max_output_bytes=SIZE_MAX;
    check("return 1",CC_ERR_INVALID_ARGUMENT,NULL,&limits);
    limits=lua_runtime_default_limits();
    for(size_t cap=1;cap<50000;cap+=113) {
        limits.max_heap_bytes=cap;
        char *out=NULL; cc_result_t r=lua_runtime_run_code("print('hello'); return 9",&limits,&out);
        assert(r.code==CC_OK || r.code==CC_ERR_OUT_OF_MEMORY);
        heap_caps_free(out); cc_result_free(&r); assert(blocks==0);
    }
    limits=lua_runtime_default_limits();
    for(size_t i=1;i<=600;i++) {
        fail_after=i; calls=0;
        char *out=NULL; cc_result_t r=lua_runtime_run_code("print('hello'); return setmetatable({}, {__tostring=function() return 'converted' end})",&limits,&out);
        fail_after=0;
        assert(r.code==CC_OK || r.code==CC_ERR_OUT_OF_MEMORY);
        heap_caps_free(out); cc_result_free(&r); assert(blocks==0);
    }
    puts("PASS root-scoped file paths/read validation, runtime scripts, pcall/xpcall, tostring timeout, cancellation, 443 heap caps, 600 persistent allocation failures, no live blocks");
}
'''


class LuaRuntimeContracts(unittest.TestCase):
    def test_real_lua_runtime_contracts(self):
        repo = Path(os.environ.get('LUA_TEST_REPO_ROOT', Path(__file__).resolve().parents[1])).resolve()
        lua_sources = repo / 'managed_components/espressif__lua/lua'
        self.assertTrue((lua_sources / 'lua.h').is_file(),
                        f'Missing managed Lua source dependency: {lua_sources}')
        configured_build = os.environ.get('LUA_TEST_BUILD_DIR')
        if configured_build:
            root = Path(configured_build).resolve()
        else:
            temporary = tempfile.TemporaryDirectory(prefix='cclaw-lua-contracts-')
            self.addCleanup(temporary.cleanup)
            root = Path(temporary.name)
        root.mkdir(parents=True, exist_ok=True)
        for name, data in STUBS.items():
            path = root / 'include' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(data)
        (root / 'harness.c').write_text(HARNESS)
        sources = [str(p) for p in sorted(lua_sources.glob('*.c'))
                   if p.name not in ('lua.c', 'luac.c', 'ltests.c', 'onelua.c')]
        self.assertTrue(sources, f'Missing managed Lua C sources: {lua_sources}')
        runtime_dir = repo / 'main/services/lua'
        sanitizers = os.environ.get('SANITIZE', os.environ.get('CCLAW_TEST_SANITIZERS', '1'))
        sanitizer_flags = []
        if sanitizers.lower() not in ('0', 'false', 'off', ''):
            sanitizer_flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        cmd = [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', '-g', '-O1',
               '-Wall', '-Wextra', '-Iinclude', f'-I{runtime_dir}', f'-I{lua_sources}',
               f'-I{repo / "third_party/cclaw-sdk/cclaw/core/include"}', f'-I{repo / "third_party/cclaw-sdk/cclaw/ports/include"}',
               *sanitizer_flags, 'harness.c', str(runtime_dir / 'lua_runtime.c'),
               str(runtime_dir / 'lua_driver.c'), str(runtime_dir / 'lua_driver_core.c'),
               *sources, '-lm', '-ldl', '-o', 'runtime_test']
        subprocess.run(cmd, cwd=root, check=True, timeout=120)
        subprocess.run([str(root / 'runtime_test')], cwd=root, check=True, timeout=30)


if __name__ == '__main__':
    unittest.main()
