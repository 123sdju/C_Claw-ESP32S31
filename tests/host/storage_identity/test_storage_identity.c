#include "storage_nvs_service.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
static esp_err_t init_error;
static unsigned init_calls, erase_calls, open_calls, set_calls, commit_calls;
static char identity[64]="existing-device-identity";
const char *esp_err_to_name(esp_err_t error) { (void)error; return "injected NVS error"; }
esp_err_t nvs_flash_init(void) { init_calls++; return init_error; }
esp_err_t nvs_flash_erase(void) { erase_calls++; identity[0]='\0'; return ESP_OK; }
esp_err_t nvs_open(const char *name,nvs_open_mode_t mode,nvs_handle_t *out)
{
    (void)name; open_calls++;
    if (mode==NVS_READONLY && !identity[0]) return ESP_ERR_NVS_NOT_FOUND;
    *out=1; return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle,const char *key,char *out,size_t *length)
{
    (void)handle; (void)key;
    if (!identity[0]) return ESP_ERR_NVS_NOT_FOUND;
    size_t needed=strlen(identity)+1U;
    if (out) { if (*length<needed) return ESP_FAIL; memcpy(out,identity,needed); }
    *length=needed; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle,const char *key,const char *value)
{ (void)handle; (void)key; set_calls++; snprintf(identity,sizeof(identity),"%s",value); return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; commit_calls++; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }

int main(void)
{
    const esp_err_t failures[]={ESP_ERR_NVS_NO_FREE_PAGES,ESP_ERR_NVS_NEW_VERSION_FOUND};
    for (size_t i=0;i<sizeof(failures)/sizeof(failures[0]);i++) {
        init_error=failures[i];
        cc_result_t rc=storage_nvs_service_check();
        CHECK(rc.code==CC_ERR_STORAGE &&
              !erase_calls && !open_calls && !set_calls && !commit_calls &&
              strcmp(identity,"existing-device-identity")==0);
        cc_result_free(&rc);
    }
    init_error=ESP_OK;
    cc_result_t rc=storage_nvs_service_check();
    CHECK(rc.code==CC_OK && init_calls==3 && !open_calls && !erase_calls &&
          !set_calls && !commit_calls && strcmp(identity,"existing-device-identity")==0);
    cc_result_free(&rc);
    puts("storage NVS preservation tests passed");
    return 0;
}
