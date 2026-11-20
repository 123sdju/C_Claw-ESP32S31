"""编译实际日志hook全文件，核对格式化失败、级别提交顺序及迟到静态回调。"""
from pathlib import Path
import unittest

from test_event_ring_ownership import run_c_test


ROOT = Path(__file__).resolve().parents[1]


def production_source():
    """仅移除生产文件的include，原样编译全部函数体，依赖由同步fixture提供。"""
    text = (ROOT / "main/services/events/app_event_log_hooks.c").read_text(encoding="utf-8")
    return "\n".join(line for line in text.splitlines() if not line.startswith("#include"))


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define APP_EVENT_TEXT_MAX 2048U
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_INTERNAL 2
#define MALLOC_CAP_8BIT 4
#define APP_EVENT_KIND_LOG 1
#define APP_EVENT_SOURCE_SYSTEM 1
#define APP_EVENT_SOURCE_RUNTIME 2
typedef enum { CC_LOG_TRACE,CC_LOG_DEBUG,CC_LOG_INFO,CC_LOG_WARN,CC_LOG_ERROR } cc_log_level_t;
typedef enum { ESP_LOG_NONE,ESP_LOG_ERROR,ESP_LOG_WARN,ESP_LOG_INFO,ESP_LOG_DEBUG } esp_log_level_t;
typedef int (*vprintf_like_t)(const char *,va_list);
typedef void (*sink_fn)(void *,cc_log_level_t,const char *,const char *);
static struct {
    bool maintenance_mode,serial_debug_enabled,serial_fallback_debug_enabled,usb_log_enabled,vprintf_installed;
    cc_log_level_t usb_log_level;
} g_app_event_bus;
static int bus_depth,pending_update,inject_update,global_level,level_calls;
static int format_calls,fail_format_at,oversized_second,heap_fail,heap_live,heap_attempts;
static int published,late_closed,events_open=1,source_seen;
static char captured[APP_EVENT_TEXT_MAX+1];
static vprintf_like_t installed_printf=vprintf;
static sink_fn installed_sink;
static void *installed_user;
static int printf_sets,sink_sets;

/** @brief 声明实际批次提交入口，供模型中的延后写线程调用。 @post 按生产实现设置日志级别。 */
void app_event_log_hooks_apply_esp_level(void);
/** @brief 模拟可递归状态锁。 @post 当前同步执行者取得一层锁，状态写入不得越过该层。 */
static void app_event_bus_lock(void) { bus_depth++; }
/** @brief 归还状态锁并执行此前被阻塞的状态更新。 @post 最外层归还后可提交新状态，模型不产生真实线程。 */
static void app_event_bus_unlock(void)
{
    assert(bus_depth>0);
    bus_depth--;
    if (!bus_depth && pending_update) {
        pending_update=0;
        g_app_event_bus.maintenance_mode=true;
        app_event_log_hooks_apply_esp_level();
    }
}
/**
 * @brief 模拟native阈值写入，并在第一次wildcard提交前安排一次维护模式切换。
 * @param[in] tag 同步借用标签。
 * @param[in] level 新阈值。
 * @post 无总线锁时新更新可先完成再被旧wildcard覆盖，持锁时更新排到整批提交之后。
 */
static void esp_log_level_set(const char *tag,esp_log_level_t level)
{
    if (!strcmp(tag,"*") && inject_update) {
        inject_update=0;
        if (bus_depth) pending_update=1;
        else {
            g_app_event_bus.maintenance_mode=true;
            app_event_log_hooks_apply_esp_level();
        }
    }
    if (!strcmp(tag,"*")) global_level=level;
    level_calls++;
}
/** @brief 模拟原子全局printf注册，无同步用户回调。 @param[in] callback 新函数。 @return 旧函数借用。 */
static vprintf_like_t esp_log_set_vprintf(vprintf_like_t callback)
{
    assert(bus_depth>0);
    vprintf_like_t previous=installed_printf;
    installed_printf=callback;
    printf_sets++;
    return previous;
}
/** @brief 模拟SDK sink锁内指针对替换，实际回调由测试在解锁后调用。 @param[in] sink 新回调或NULL。 @param[in,out] user 借用上下文。 @post 配对发布且没有回调重入。 */
static void cc_logger_set_sink(sink_fn sink,void *user)
{ assert(bus_depth>0);installed_sink=sink;installed_user=user;sink_sets++; }
/** @brief 模拟公开发布门，关闭后不访问动态总线对象。 @param[in] kind 种类。 @param[in] source 来源。 @param[in] level 级别。 @param[in] text 调用期正文借用。 @post 开门复制正文，关门仅累计迟到调用。 */
static void app_event_publish(int kind,int source,cc_log_level_t level,const char *text)
{
    (void)kind;(void)level;
    assert(!bus_depth);
    if (!events_open) {late_closed++;return;}
    published++;source_seen=source;
    assert(strlen(text)<=APP_EVENT_TEXT_MAX);
    strcpy(captured,text);
}
/** @brief 分配填毒堆缓冲或模拟两级分配失败。 @param[in] size 字节数。 @param[in] caps 忽略能力。 @return 独占内存或NULL。 */
static void *heap_caps_malloc(size_t size,int caps)
{ (void)caps;heap_attempts++;if(heap_fail)return NULL;void *ptr=malloc(size);assert(ptr);memset(ptr,0xa5,size);heap_live++;return ptr; }
/** @brief 消费本fixture的拥有堆块。 @param[in,out] ptr 可空分配。 @post 非空归还且计数减少。 */
static void heap_caps_free(void *ptr) {if(ptr){heap_live--;free(ptr);}}
/** @brief 注入任一次格式化负值或合法的容量截断。 @param[out] out 输出缓冲。 @param[in] size 含NUL容量。 @param[in] format 格式借用。 @param[in] args 可变参数借用。 @return 格式长度或负值。 */
static int hook_vsnprintf(char *out,size_t size,const char *format,va_list args)
{
    format_calls++;
    if (format_calls==fail_format_at) return -17;
    if (oversized_second && format_calls==2) {
        memset(out,'z',size-1);out[size-1]=0;return (int)size+100;
    }
    return vsnprintf(out,size,format,args);
}
#define vsnprintf hook_vsnprintf
'''


SUFFIX = r'''
#undef vsnprintf
/** @brief 用真实可变参数调用指定hook。 @param[in] hook 回调借用。 @param[in] format 格式借用。 @param[in] ... 匹配参数。 @return hook原结果，原游标在本层结束。 */
static int invoke(vprintf_like_t hook,const char *format,...)
{ va_list args;va_start(args,format);int result=hook(format,args);va_end(args);return result; }
'''


class LogHookContracts(unittest.TestCase):
    def test_formatting_and_filtering(self):
        """实际formatter两次失败均不发布，长文保留预算截断及返回长度。"""
        tests = r'''
/** @brief 核对解析、过滤、堆回退和负格式化结果。 @return 全部断言通过为零。 */
int main(void)
{
    assert(esp_line_level("\r\n\033[31mE (1) app: error")==CC_LOG_ERROR);
    assert(line_has_tag("\033[32mI (1) app: value","app"));
    assert(!strcmp(line_tag_payload("I (1) app: value","app"),"value"));
    assert(app_log_should_drop(CC_LOG_INFO,"I (1) wifi: diagnostic"));
    assert(invoke(app_log_vprintf,NULL)==0);
    const char *plain="I (1) app: value\r\n";
    assert(invoke(app_log_vprintf,"%s",plain)==(int)strlen(plain));
    assert(published==1&&!strcmp(captured,"I (1) app: value")&&source_seen==APP_EVENT_SOURCE_SYSTEM);
    invoke(app_log_vprintf,"%s","I (1) wifi: diagnostic");assert(published==1);
    assert(invoke(app_log_vprintf,"%c",0)==1&&published==1);
    assert(invoke(app_log_vprintf,"I (1) app: prefix%cignored",0)>0);
    assert(published==2&&!strcmp(captured,"I (1) app: prefix"));
    sdk_logger_sink(NULL,CC_LOG_WARN,"borrowed-name","SDK message");
    assert(published==3&&source_seen==APP_EVENT_SOURCE_RUNTIME&&!strcmp(captured,"SDK message"));
    char long_text[APP_EVENT_TEXT_MAX+101];memset(long_text,'x',sizeof(long_text)-1);long_text[sizeof(long_text)-1]=0;
    format_calls=0;assert(invoke(app_log_vprintf,"%s",long_text)==(int)strlen(long_text));
    assert(strlen(captured)==APP_EVENT_TEXT_MAX&&!heap_live);
    format_calls=0;heap_fail=1;heap_attempts=0;
    assert(invoke(app_log_vprintf,"%s",long_text)==(int)strlen(long_text));
    assert(strlen(captured)==APP_LOG_HOOK_STACK_BUF-1&&heap_attempts==2&&!heap_live);heap_fail=0;
    int before=published;
    format_calls=0;fail_format_at=1;assert(invoke(app_log_vprintf,"%s",long_text)==-17);
    assert(published==before&&!heap_live);
    format_calls=0;fail_format_at=2;assert(invoke(app_log_vprintf,"%s",long_text)==-17);
    assert(published==before&&!heap_live);fail_format_at=0;
    char medium[301];memset(medium,'m',300);medium[300]=0;
    format_calls=0;oversized_second=1;
    assert(invoke(app_log_vprintf,"%s",medium)==300&&strlen(captured)==300&&!heap_live);
    return 0;
}
'''
        run_c_test(PREFIX + production_source() + SUFFIX + tests)

    def test_level_order_and_callback_lifetime(self):
        """用确定性交错模拟旧提交晚写，核对整批状态锁和关闭后迟到静态回调。"""
        tests = r'''
/** @brief 验证最新级别最终生效、hook幂等安装及关闭后的静态回调。 @return 全部断言通过为零。 */
int main(void)
{
    g_app_event_bus.serial_debug_enabled=true;
    app_event_log_hooks_apply_esp_level();assert(global_level==ESP_LOG_DEBUG&&!bus_depth);
    g_app_event_bus.serial_debug_enabled=false;
    inject_update=1;app_event_log_hooks_apply_esp_level();
    assert(g_app_event_bus.maintenance_mode&&global_level==ESP_LOG_WARN&&!pending_update&&!bus_depth);
    assert(level_calls>10);
    app_event_log_hooks_install();app_event_log_hooks_install();
    assert(g_app_event_bus.vprintf_installed&&printf_sets==1&&sink_sets==2&&!bus_depth);
    assert(installed_printf==app_log_vprintf&&installed_sink==sdk_logger_sink&&!installed_user);
    vprintf_like_t saved_printf=installed_printf;sink_fn saved_sink=installed_sink;
    saved_sink(NULL,CC_LOG_INFO,"name","before close");assert(published==1);
    events_open=0;app_event_log_hooks_uninstall();
    assert(!g_app_event_bus.vprintf_installed&&installed_printf==vprintf&&!installed_sink&&!bus_depth);
    assert(invoke(saved_printf,"%s","I (1) app: late")==15);
    saved_sink(NULL,CC_LOG_INFO,"name","late SDK");
    assert(late_closed==2&&published==1&&!heap_live);
    app_event_log_hooks_uninstall();assert(printf_sets==2&&sink_sets==4&&!bus_depth);
    return 0;
}
'''
        run_c_test(PREFIX + production_source() + SUFFIX + tests)


if __name__ == "__main__":
    unittest.main()
