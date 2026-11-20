"""编译实际 ESP32 平台函数，检查 64 位时钟换算及随机缓冲参数。"""
from pathlib import Path
import unittest

from test_event_ring_ownership import run_c_test
from test_profile_task_lifetime import function

ROOT = Path(__file__).resolve().parents[1]


class PlatformClockTests(unittest.TestCase):
    """使用可控 IDF 调用替身检查实际函数，设备计时精度另行验证。"""

    def test_monotonic_beyond_tick_wrap(self):
        """输入跨 32 位毫秒边界的微秒值，断言转换保持单调且按毫秒截断。"""
        source = (ROOT / "third_party/cclaw-sdk/cclaw/platforms/esp32/src/cc_esp32_platform.c").read_text(encoding="utf-8")
        prefix = r'''
#include <assert.h>
#include <stdint.h>
static int64_t timer_us;
static int64_t esp_timer_get_time(void) { return timer_us; }
'''
        tests = r'''
int main(void) {
    timer_us = 0;
    assert(cc_platform_monotonic_ms() == 0);
    timer_us = 1999;
    assert(cc_platform_monotonic_ms() == 1);
    timer_us = (INT64_C(1) << 32) * 1000 - 1;
    assert(cc_platform_monotonic_ms() == (UINT64_C(1) << 32) - 1);
    timer_us += 1;
    assert(cc_platform_monotonic_ms() == (UINT64_C(1) << 32));
    timer_us = INT64_MAX;
    assert(cc_platform_monotonic_ms() == (uint64_t)INT64_MAX / 1000);
    return 0;
}
'''
        run_c_test(prefix + function(source, "uint64_t cc_platform_monotonic_ms(") + tests)

    def test_random_buffer_arguments(self):
        """验证空缓冲参数、零长度和正常写入，并保留初始化可重复调用。"""
        source = (ROOT / "third_party/cclaw-sdk/cclaw/platforms/esp32/src/cc_esp32_platform.c").read_text(encoding="utf-8")
        prefix = r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
static int random_calls;
static void esp_fill_random(void *buffer, size_t size) {
    assert(buffer && size); random_calls++; memset(buffer, 0x5a, size);
}
'''
        tests = r'''
int main(void) {
    unsigned char buffer[4] = {0};
    cc_platform_init(); cc_platform_init();
    assert(cc_platform_random_bytes(NULL, 1) == -1);
    assert(cc_platform_random_bytes(NULL, 0) == 0);
    assert(cc_platform_random_bytes(buffer, 0) == 0 && random_calls == 0);
    assert(cc_platform_random_bytes(buffer, sizeof(buffer)) == 0 && random_calls == 1);
    for (size_t i = 0; i < sizeof(buffer); i++) assert(buffer[i] == 0x5a);
    return 0;
}
'''
        run_c_test(prefix + function(source, "void cc_platform_init(") +
                   function(source, "int cc_platform_random_bytes(") + tests)


if __name__ == "__main__":
    unittest.main()
