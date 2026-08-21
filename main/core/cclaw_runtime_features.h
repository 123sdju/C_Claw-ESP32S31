#pragma once

#include "cc/app/cc_runtime_features.h"

/*
 * 获取 CClaw 应用层合并后的运行时 feature set。
 *
 * 返回: 指向静态存储的 cc_runtime_feature_set_t 指针，包含 SDK 内置工具
 *       （如 file_read、file_write、http.request、memory）与应用层工具
 *       （camera_capture、lua_run_code、screen_draw 等）的合并结果。
 *
 * 首次调用触发线程安全的懒初始化（双重检查锁定），后续调用直接返回缓存。
 * 返回指针在应用生命周期内有效。
 */
const cc_runtime_feature_set_t *cclaw_runtime_features(void);
