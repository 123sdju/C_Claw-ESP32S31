"""Host execution of production pixel-copy and Lua hook code; no board I/O."""
import re
import unittest

from test_review_regressions import COMMON, function, source
from test_event_ring_ownership import run_c_test


class CameraLuaRegressions(unittest.TestCase):
    def test_rgb565_bytes_stride_and_short_frames(self):
        header = source("main/board/board_camera_rgb565.h")
        copy = source("main/board/board_camera_rgb565.c")
        code = re.sub(r'^#(?:include|pragma).*$', '', header + copy, flags=re.M)
        run_c_test(COMMON + '\n#define ESP_ERR_INVALID_SIZE 99\n' + code + r'''
int main(void) {
    /* Deliberately unaligned source and padded odd stride. */
    uint8_t src[] = {0xee, 0xf8,0x00,0x07,0xe0,0xaa, 0x00,0x1f,0xff,0xff,0xbb};
    uint8_t dst[10]; memset(dst,0xcc,sizeof(dst));
    board_camera_rgb565_copy_config_t cfg = {2,2,5,true,false};
    assert(board_camera_rgb565_copy(dst+1,src+1,10,10,&cfg)==ESP_OK);
    const uint8_t expected[] = {0x00,0xf8,0xe0,0x07,0x1f,0x00,0xff,0xff};
    assert(memcmp(dst+1,expected,8)==0 && dst[0]==0xcc && dst[9]==0xcc);
    for (size_t n=0;n<10;n++) {
        memset(dst,0xcc,sizeof(dst));
        assert(board_camera_rgb565_copy(dst,src+1,10,n,&cfg)==ESP_ERR_INVALID_SIZE);
        assert(dst[0]==0xcc);
    }
    assert(board_camera_rgb565_copy(dst,src+1,10,11,&cfg)==ESP_ERR_INVALID_SIZE);
    cfg.src_stride_bytes=SIZE_MAX;
    assert(board_camera_rgb565_copy(dst,src+1,10,10,&cfg)==ESP_ERR_INVALID_SIZE);
    cfg.src_stride_bytes=5; cfg.swap_rb=true;
    assert(board_camera_rgb565_copy(dst,src+1,10,10,&cfg)==ESP_OK);
    assert(dst[0]==0x1f && dst[1]==0x00); /* red -> blue */
    cfg.width=1;cfg.height=1;cfg.src_stride_bytes=2;cfg.swap_bytes=false;
    const uint8_t red_le[]={0x00,0xf8};
    assert(board_camera_rgb565_copy(dst,red_le,2,2,&cfg)==ESP_OK);
    assert(dst[0]==0x1f && dst[1]==0x00);
    puts("RGB565 endian, stride and bounds: PASS");return 0;
}
''')

    def test_lua_periodically_blocks_without_extending_deadline(self):
        text = source("main/services/lua/lua_runtime.c")
        state = re.search(r'typedef struct \{\s*TickType_t start_ticks;.*?\} lua_timeout_state_t;', text, re.S).group()
        prefix = COMMON + r'''
#include <setjmp.h>
typedef uint32_t TickType_t;
typedef int lua_State;
typedef int lua_Debug;
typedef struct cc_cancel_token cc_cancel_token_t;
struct cc_cancel_token { int cancelled; };
#define LUA_REGISTRYINDEX 1
#define pdMS_TO_TICKS(ms) (ms)
static TickType_t now;
static int delays;
static jmp_buf failure;
static const char *last_error;
static char s_lua_timeout_registry_key;
static void *registry;
static int cc_cancel_token_is_cancelled(cc_cancel_token_t *token) {
    return token && token->cancelled;
}
static TickType_t xTaskGetTickCount(void) {return now;}
static void vTaskDelay(int ticks) {assert(ticks==1);now+=ticks;delays++;}
static void lua_pushlightuserdata(lua_State *L,void *p) {(void)L;(void)p;}
static void lua_rawget(lua_State *L,int n) {(void)L;(void)n;}
static void *lua_touserdata(lua_State *L,int n) {(void)L;(void)n;return registry;}
static void lua_pop(lua_State *L,int n) {(void)L;(void)n;}
static int luaL_error(lua_State *L,const char *msg,...) {(void)L;last_error=msg;longjmp(failure,1);return 0;}
'''
        hook = function("main/services/lua/lua_runtime.c", "static void lua_timeout_hook(")
        run_c_test(prefix + state + hook + r'''
int main(void) {
    lua_timeout_state_t state={0};state.max_ticks=100;state.timeout_ms=100;registry=&state;
    now=9;lua_timeout_hook(NULL,NULL);assert(delays==0);
    now=10;lua_timeout_hook(NULL,NULL);assert(delays==1 && now==11 && state.start_ticks==0);
    now=12;lua_timeout_hook(NULL,NULL);assert(delays==1);
    state.max_ticks=20;state.last_yield_ticks=0;now=19;
    if(setjmp(failure)==0) {lua_timeout_hook(NULL,NULL);assert(!"deadline must include blocked time");}
    assert(now==20 && state.start_ticks==0);
    state.start_ticks=UINT32_MAX-5;state.last_yield_ticks=state.start_ticks;state.max_ticks=100;now=4;
    lua_timeout_hook(NULL,NULL);assert(state.last_yield_ticks==5);
    cc_cancel_token_t token={0};state.cancel_token=&token;
    state.start_ticks=0;state.last_yield_ticks=now;now=6;
    int previous_delays=delays;
    lua_timeout_hook(NULL,NULL);assert(delays==previous_delays);
    token.cancelled=1;
    if(setjmp(failure)==0) {lua_timeout_hook(NULL,NULL);assert(!"cancelled token must stop hook");}
    assert(strcmp(last_error,"Lua execution cancelled")==0 && delays==previous_delays && now==6);
    puts("Lua bounded scheduling and wall-clock timeout: PASS");return 0;
}
''')


if __name__ == "__main__":
    unittest.main()
