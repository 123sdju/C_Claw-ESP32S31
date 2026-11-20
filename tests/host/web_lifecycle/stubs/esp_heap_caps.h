#pragma once
#include <stdlib.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
/* 主机模型的 PSRAM/内部缓冲均使用 libc 堆。 */
#define heap_caps_free free
