#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the service so the host test can inject driver callbacks. */
#include "../../../main/services/usb_storage_service.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
typedef struct fake_queue { int drop; usb_storage_event_t last; } fake_queue_t;
static fake_queue_t queue;
static int disconnect_during_install;
static int fail_unregister;
static int fail_uninstall;
static int fail_register;
static unsigned queue_creations;
static msc_host_device_handle_t device=(void *)0x1234;
/** @brief 记录队列创建次数并借出固定 fixture。
 * @param[in] count 预期的队列深度。
 * @param[in] size 预期的单事件字节数。
 * @return 固定队列借用，fixture 持续整个测试。
 */
QueueHandle_t xQueueCreate(UBaseType_t count,size_t size)
{ (void)count; (void)size; queue_creations++; return &queue; }
BaseType_t xQueueSend(QueueHandle_t q,const void *item,unsigned wait)
{ (void)wait; fake_queue_t *f=q; if (f->drop) return 0; f->last=*(const usb_storage_event_t *)item; return pdTRUE; }
BaseType_t xTaskCreate(TaskFunction_t fn,const char *name,uint32_t stack,void *arg,UBaseType_t priority,TaskHandle_t *handle)
{ (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; return pdPASS; }
void vTaskDelete(TaskHandle_t handle) { (void)handle; }
esp_err_t usb_host_install(const usb_host_config_t *c) { (void)c; return ESP_OK; }
esp_err_t usb_host_uninstall(void) { return ESP_OK; }
esp_err_t usb_host_lib_handle_events(uint32_t timeout,uint32_t *flags) { (void)timeout; *flags=0; return ESP_OK; }
esp_err_t usb_host_device_free_all(void) { return ESP_OK; }
esp_err_t msc_host_install(const msc_host_driver_config_t *c) { (void)c; return ESP_OK; }
esp_err_t msc_host_install_device(uint8_t address,msc_host_device_handle_t *out)
{
    (void)address; *out=device;
    if (disconnect_during_install) {
        msc_host_event_t event={.event=MSC_DEVICE_DISCONNECTED}; event.device.handle=device;
        msc_event_cb(&event,NULL);
    }
    return ESP_OK;
}
esp_err_t msc_host_uninstall_device(msc_host_device_handle_t handle) { (void)handle; return fail_uninstall ? ESP_FAIL : ESP_OK; }
/** @brief 模拟仅成功时发布 VFS 句柄的原生注册。
 * @param[in] handle 设备借用。
 * @param[in] path 挂载路径借用。
 * @param[in] config 原生挂载配置借用。
 * @param[out] out 成功写固定 VFS 身份，失败保持槽值。
 * @return 按 fail_register 返回 ESP_FAIL 或 ESP_OK。
 */
esp_err_t msc_host_vfs_register(msc_host_device_handle_t handle,const char *path,
                               const esp_vfs_fat_mount_config_t *config,msc_host_vfs_handle_t *out)
{ (void)handle; (void)path; (void)config; if(fail_register) return ESP_FAIL; *out=(void *)0x5678; return ESP_OK; }
esp_err_t msc_host_vfs_unregister(msc_host_vfs_handle_t handle) { (void)handle; return fail_unregister ? ESP_FAIL : ESP_OK; }

/** @brief 验证代际、补偿事件、重复初始化及挂载回滚清理责任。
 * @return 全部断言通过为零，失败为一，所有已安装 fixture 设备最终卸载。
 */
int main(void)
{
    QueueHandle_t events=NULL; CHECK(usb_storage_service_init(&events)==ESP_OK && events);
    QueueHandle_t same_events=NULL;
    CHECK(usb_storage_service_init(&same_events)==ESP_OK && same_events==events && queue_creations==1);
    CHECK(usb_storage_service_start_host(events)==ESP_OK);
    CHECK(usb_storage_service_start_host(events)==ESP_ERR_INVALID_STATE);
    queue.drop=1;
    msc_host_event_t connection={.event=MSC_DEVICE_CONNECTED}; connection.device.address=7;
    msc_event_cb(&connection,NULL);
    usb_storage_event_t recovered_connection={0};
    CHECK(usb_storage_service_take_pending_connect(&recovered_connection) &&
          recovered_connection.id==USB_STORAGE_DEVICE_CONNECTED && recovered_connection.data.address==7);
    CHECK(!usb_storage_service_take_pending_connect(&recovered_connection));
    queue.drop=0;
    CHECK(usb_storage_service_mount(1)==ESP_OK);
    uint64_t first=0; CHECK(usb_storage_service_current_generation(&first) && first>0);
    msc_host_event_t unrelated={.event=MSC_DEVICE_DISCONNECTED}; unrelated.device.handle=(void *)0x9999;
    msc_event_cb(&unrelated,NULL);
    uint64_t now=0; CHECK(usb_storage_service_current_generation(&now) && now==first);
    CHECK(!usb_storage_service_disconnect_is_current(queue.last.data.handle,queue.last.generation));
    msc_host_device_handle_t pending=NULL; uint64_t pending_generation=0;
    CHECK(!usb_storage_service_take_pending_disconnect(&pending,&pending_generation));

    /* 即使队列满，断开仍立即关闭准入，owner 可取补偿通知恢复清理。 */
    queue.drop=1; msc_host_event_t lost={.event=MSC_DEVICE_DISCONNECTED}; lost.device.handle=device;
    msc_event_cb(&lost,NULL);
    CHECK(!usb_storage_service_is_mounted());
    CHECK(usb_storage_service_take_pending_disconnect(&pending,&pending_generation));
    CHECK(pending==device && pending_generation>first && usb_storage_service_disconnect_is_current(pending,pending_generation));
    uint64_t old_disconnect=pending_generation;
    CHECK(usb_storage_service_begin_unmount(pending)==ESP_OK);
    fail_unregister=1; CHECK(usb_storage_service_unmount(pending)==ESP_FAIL && !usb_storage_service_is_mounted()); fail_unregister=0;
    fail_uninstall=1; CHECK(usb_storage_service_unmount(pending)==ESP_FAIL && !usb_storage_service_is_mounted()); fail_uninstall=0;
    CHECK(usb_storage_service_unmount(pending)==ESP_OK);
    queue.drop=0; CHECK(usb_storage_service_mount(2)==ESP_OK);
    CHECK(usb_storage_service_current_generation(&now) && now>old_disconnect);
    CHECK(!usb_storage_service_disconnect_is_current(device,old_disconnect));

    /* 安装期间断开使已成功注册 VFS 的盘失效，清理通知使用实际设备句柄。 */
    CHECK(usb_storage_service_begin_unmount(device)==ESP_OK && usb_storage_service_unmount(device)==ESP_OK);
    disconnect_during_install=1; CHECK(usb_storage_service_mount(3)==ESP_OK); disconnect_during_install=0;
    CHECK(!usb_storage_service_is_mounted());
    CHECK(usb_storage_service_take_pending_disconnect(&pending,&pending_generation) && pending==device &&
          usb_storage_service_disconnect_is_current(pending,pending_generation));
    CHECK(usb_storage_service_begin_unmount(pending)==ESP_OK && usb_storage_service_unmount(pending)==ESP_OK);

    /* VFS 错误后卸载也失败时，保留设备并补发可重试清理通知。 */
    fail_register=fail_uninstall=1;
    CHECK(usb_storage_service_mount(4)==ESP_FAIL && !usb_storage_service_is_mounted());
    CHECK(usb_storage_service_mount(5)==ESP_ERR_INVALID_STATE);
    CHECK(usb_storage_service_take_pending_disconnect(&pending,&pending_generation) && pending==device);
    CHECK(usb_storage_service_disconnect_is_current(pending,pending_generation));
    CHECK(usb_storage_service_begin_unmount(pending)==ESP_OK);
    CHECK(usb_storage_service_unmount(pending)==ESP_FAIL);
    fail_register=fail_uninstall=0;
    CHECK(usb_storage_service_unmount(pending)==ESP_OK);

    /* generation 耗尽保持关闭，不重新开放携带过期身份的 IO。 */
    s_usb_generation=UINT64_MAX;
    CHECK(usb_storage_service_mount(4)==ESP_OK && !usb_storage_service_is_mounted());
    CHECK(usb_storage_service_take_pending_disconnect(&pending,&pending_generation) && pending_generation==UINT64_MAX);
    CHECK(usb_storage_service_begin_unmount(pending)==ESP_OK && usb_storage_service_unmount(pending)==ESP_OK);
    return 0;
}
