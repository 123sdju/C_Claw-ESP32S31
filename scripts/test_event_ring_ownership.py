"""Host regression for the firmware ring's real C ownership code.

Runs with a host C compiler; ESP-IDF/FreeRTOS and hardware are not exercised.
The interleavings deliberately clear/overwrite history before the publisher
has finished dispatching to sinks.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define APP_EVENT_RECENT_MAX 2U
#define APP_EVENT_RECENT_MAX_BYTES 256U
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
typedef struct { unsigned refs; bool released; size_t len; } app_event_envelope_t;
typedef struct { unsigned seq; app_event_envelope_t *envelope; } app_event_record_t;
static struct {
    app_event_record_t *recent;
    app_event_record_t view[2];
    size_t view_count;
    unsigned view_latest_seq;
    bool view_active;
    size_t recent_capacity, recent_count, recent_start, recent_bytes;
    unsigned dropped, sink_dropped, oom_dropped, ring_overwrites;
} g_app_event_bus;
static bool fail_allocation;
static void *heap_caps_calloc(size_t n, size_t size, int caps) {
    (void)caps;
    return fail_allocation ? NULL : calloc(n, size);
}
/* Poison on final release, rather than free, to make a bad interleaving
 * deterministic without relying on the host allocator reusing an address. */
static void app_event_envelope_retain(app_event_envelope_t *e) {
    assert(e && !e->released && e->refs);
    ++e->refs;
}
static void app_event_envelope_release(app_event_envelope_t *e) {
    if (!e) return;
    assert(!e->released && e->refs);
    if (!--e->refs) e->released = true;
}
'''

TESTS = r'''
static void reset(void) {
    app_event_ring_clear_locked();
    free(g_app_event_bus.recent);
    memset(&g_app_event_bus, 0, sizeof(g_app_event_bus));
    fail_allocation = false;
}
int main(void) {
    app_event_envelope_t e = {1, false};
    e.len = 23;
    app_event_record_t rec = {1, &e};
    assert(app_event_ring_append_locked(&rec));
    assert(g_app_event_bus.recent_bytes == sizeof(e) + 24);
    /* A concurrent clear must not invalidate the publisher's pointer. */
    app_event_ring_clear_locked();
    assert(g_app_event_bus.recent_bytes == 0);
    assert(!e.released);
    app_event_envelope_retain(&e); /* queued sink takes its own reference */
    app_event_envelope_release(&e); /* publisher finishes */
    assert(!e.released);
    app_event_envelope_release(&e); /* sink finishes */
    assert(e.released);
    reset();

    app_event_envelope_t a = {1, false}, b = {1, false}, c = {1, false};
    a.len = 10; b.len = 20; c.len = 30;
    app_event_record_t ra = {1, &a}, rb = {2, &b}, rc = {3, &c};
    assert(app_event_ring_append_locked(&ra));
    assert(app_event_ring_append_locked(&rb));
    assert(app_event_ring_append_locked(&rc)); /* overwrites a */
    assert(g_app_event_bus.recent_bytes == 2 * sizeof(b) + 52);
    assert(!a.released);
    app_event_envelope_release(&a);
    assert(a.released);
    app_event_envelope_release(&b);
    app_event_envelope_release(&c);
    assert(!b.released && !c.released);
    assert(g_app_event_bus.ring_overwrites == 1);
    app_event_ring_clear_locked();
    assert(b.released && c.released);
    app_event_ring_clear_locked(); /* idempotent, no double release */
    reset();

    app_event_envelope_t oom = {1, false};
    app_event_record_t ro = {4, &oom};
    fail_allocation = true;
    assert(!app_event_ring_append_locked(&ro));
    assert(!oom.released && oom.refs == 1);
    app_event_envelope_release(&oom);
    assert(oom.released);
    reset();
    app_event_envelope_t budget_a = {1, false}, budget_b = {1, false};
    budget_a.len = 180; budget_b.len = 100;
    app_event_record_t budget_ra = {5, &budget_a}, budget_rb = {6, &budget_b};
    assert(app_event_ring_append_locked(&budget_ra));
    assert(app_event_ring_append_locked(&budget_rb));
    assert(g_app_event_bus.recent_count == 1 && g_app_event_bus.recent_bytes <= APP_EVENT_RECENT_MAX_BYTES);
    app_event_envelope_release(&budget_a);
    app_event_envelope_release(&budget_b);
    reset();
    assert(budget_a.released && budget_b.released);
    puts("Event ring ownership and byte budget: PASS");
    return 0;
}
'''


def run_c_test(source):
    with tempfile.TemporaryDirectory(prefix='cclaw-host-c-') as folder:
        tmp = Path(folder)
        (tmp / 'test.c').write_text(source, encoding='utf-8')
        if os.name == 'nt':
            vswhere = Path(os.environ.get('ProgramFiles(x86)', 'C:/Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
            install = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
                'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
            if not install:
                raise RuntimeError('A host Visual C compiler is required')
            setup = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
            (tmp / 'test.cmd').write_text(
                f'@echo off\ncall "{setup}" >nul\nif errorlevel 1 exit /b 1\n'
                'cl /nologo /utf-8 /std:c11 /W4 /Fe:test.exe test.c\n'
                'if errorlevel 1 exit /b 1\ntest.exe\n', encoding='utf-8')
            subprocess.run(['cmd.exe', '/d', '/c', str(tmp / 'test.cmd')], cwd=tmp, check=True)
        else:
            cc = shutil.which('cc') or shutil.which('gcc') or shutil.which('clang')
            if not cc:
                raise RuntimeError('A host C compiler is required')
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-o', 'test', 'test.c'], cwd=tmp, check=True)
            subprocess.run([str(tmp / 'test')], cwd=tmp, check=True)


def main():
    ring = (ROOT / 'main/services/events/app_event_ring.c').read_text(encoding='utf-8')
    internal = (ROOT / 'main/services/events/app_event_bus_internal.h').read_text(encoding='utf-8')
    counter = internal[internal.index('static inline void app_event_counter_increment('):
                       internal.index('/**', internal.index('static inline void app_event_counter_increment('))]
    code = ring[ring.index('bool app_event_ring_ensure_locked(void)'):
                ring.index('void app_event_json_escape_append(')]
    run_c_test(PREFIX + counter + code + TESTS)


if __name__ == '__main__':
    main()
