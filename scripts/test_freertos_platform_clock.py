"""编译实际 FreeRTOS 平台函数，核对 tick 回绕、高频换算及 BSP 随机转发。"""
from pathlib import Path
import unittest

from test_event_ring_ownership import run_c_test
from test_profile_task_lifetime import function

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "third_party/cclaw-sdk/cclaw/platforms/freertos/src/cc_freertos_platform.c"


def check_clock(bits, rate):
    """以指定 tick 位宽和频率编译真实函数，检查快照、回绕及饱和。"""
    source = SOURCE.read_text(encoding="utf-8")
    prefix = r'''
#include <assert.h>
#include <stdint.h>
typedef TICK_TYPE TickType_t;
typedef int32_t BaseType_t;
typedef uint32_t UBaseType_t;
typedef struct { BaseType_t xOverflowCount; TickType_t xTimeOnEntering; } TimeOut_t;
#define portMAX_DELAY ((TickType_t)~(TickType_t)0)
#define configTICK_RATE_HZ CLOCK_RATE
static TimeOut_t kernel_snapshot;
static unsigned snapshot_calls;
/** @brief 原子快照替身。 @param[out] out 接收测试内核状态。 @post 记录一次快照调用。 */
static void vTaskSetTimeOutState(TimeOut_t *out) { *out = kernel_snapshot; snapshot_calls++; }
'''.replace("TICK_TYPE", f"uint{bits}_t").replace("CLOCK_RATE", str(rate))
    if bits < 64:
        epoch_ticks = 8 * (1 << bits)
        before_ms = (epoch_ticks - 1) * 1000 // rate
        after_ms = epoch_ticks * 1000 // rate
        tests = f'''
/** @brief 检查回绕边界和快照调用。 @return 断言通过返回零。 */
int main(void) {{
    kernel_snapshot.xOverflowCount = 7;
    kernel_snapshot.xTimeOnEntering = portMAX_DELAY;
    assert(cc_platform_monotonic_ms() == UINT64_C({before_ms}));
    kernel_snapshot.xOverflowCount = 8;
    kernel_snapshot.xTimeOnEntering = 0;
    assert(cc_platform_monotonic_ms() == UINT64_C({after_ms}));
    kernel_snapshot.xOverflowCount = 0;
    kernel_snapshot.xTimeOnEntering = configTICK_RATE_HZ;
    assert(cc_platform_monotonic_ms() == 1000);
    assert(snapshot_calls == 3);
    return 0;
}}
'''
    else:
        maximum_ms = min(((1 << 64) - 1) * 1000 // rate, (1 << 64) - 1)
        tests = f'''
/** @brief 检查 64 位 tick、整数截断及饱和。 @return 断言通过返回零。 */
int main(void) {{
    kernel_snapshot.xTimeOnEntering = 1;
    assert(cc_platform_monotonic_ms() == UINT64_C({1000 // rate}));
    kernel_snapshot.xTimeOnEntering = UINT64_MAX;
    assert(cc_platform_monotonic_ms() == UINT64_C({maximum_ms}));
    assert(snapshot_calls == 2);
    return 0;
}}
'''
    run_c_test(prefix + function(source, "uint64_t cc_platform_monotonic_ms(") + tests)


class FreeRtosClockTests(unittest.TestCase):
    """参数由编译期替身提供，真实内核锁和 BSP 硬件行为单独验证。"""

    def test_16_bit_ticks(self):
        """核对 16 位 tick 在 100Hz 时跨回绕仍递增。"""
        check_clock(16, 100)

    def test_32_bit_ticks(self):
        """核对 32 位 1000Hz 快照和回绕。"""
        check_clock(32, 1000)

    def test_high_frequency_ticks(self):
        """核对 2000Hz 下毫秒仍推进，避免整数 tick 周期变零。"""
        check_clock(32, 2000)

    def test_64_bit_ticks(self):
        """核对 64 位 tick 不执行位宽外移位，并在低频时饱和。"""
        check_clock(64, 128)

    def test_default_random_source(self):
        """核对默认硬件随机入口明确不可用，初始化可重复。"""
        source = SOURCE.read_text(encoding="utf-8")
        prefix = "#include <assert.h>\n#include <stddef.h>\n"
        tests = r'''
/** @brief 检查默认随机源和初始化。 @return 断言通过返回零。 */
int main(void) {
    char data[2] = {1, 2};
    cc_platform_init(); cc_platform_init();
    assert(cc_freertos_hardware_random_bytes(data, sizeof(data)) == -1);
    assert(data[0] == 1 && data[1] == 2);
    assert(cc_freertos_hardware_random_bytes(NULL, 0) == -1);
    return 0;
}
'''
        run_c_test(prefix + function(source, "void cc_platform_init(") +
                   function(source, "int cc_freertos_hardware_random_bytes(") + tests)

    def test_random_source_forwarding(self):
        """核对 BSP 转发的参数、返回码和 NULL 非零长度拒绝。"""
        source = SOURCE.read_text(encoding="utf-8")
        prefix = r'''
#include <assert.h>
#include <stddef.h>
static void *expected_buffer;
static size_t expected_size;
static unsigned calls;
/** @brief 模拟 BSP 随机源。 @param[out] buffer 验证借用缓冲。 @param[in] size 验证字节数。 @return 合成失败码。 */
static int cc_freertos_hardware_random_bytes(void *buffer, size_t size) {
    assert(buffer == expected_buffer && size == expected_size); calls++; return -7;
}
'''
        tests = r'''
/** @brief 检查随机转发。 @return 断言通过返回零。 */
int main(void) {
    char data[2]; expected_buffer = data; expected_size = sizeof(data);
    assert(cc_platform_random_bytes(data, sizeof(data)) == -7 && calls == 1);
    assert(cc_platform_random_bytes(NULL, 1) == -1 && calls == 1);
    expected_buffer = NULL; expected_size = 0;
    assert(cc_platform_random_bytes(NULL, 0) == -7 && calls == 2);
    return 0;
}
'''
        run_c_test(prefix + function(source, "int cc_platform_random_bytes(") + tests)


if __name__ == "__main__":
    unittest.main()
