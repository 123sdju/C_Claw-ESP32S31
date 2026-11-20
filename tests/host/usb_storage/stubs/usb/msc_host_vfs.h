#pragma once
#include "usb/msc_host.h"
typedef void *msc_host_vfs_handle_t;
typedef struct { bool format_if_mount_failed; int max_files; size_t allocation_unit_size; } esp_vfs_fat_mount_config_t;
esp_err_t msc_host_vfs_register(msc_host_device_handle_t handle,const char *path,
                               const esp_vfs_fat_mount_config_t *config,msc_host_vfs_handle_t *vfs);
esp_err_t msc_host_vfs_unregister(msc_host_vfs_handle_t vfs);
