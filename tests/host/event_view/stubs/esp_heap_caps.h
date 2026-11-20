#pragma once
#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1U
#define MALLOC_CAP_INTERNAL 2U
#define MALLOC_CAP_8BIT 4U
void *heap_caps_malloc(size_t bytes, unsigned caps);
void *heap_caps_calloc(size_t count, size_t bytes, unsigned caps);
void heap_caps_free(void *ptr);
