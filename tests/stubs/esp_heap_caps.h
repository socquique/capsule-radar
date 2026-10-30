// esp_heap_caps stub for host tests: plain heap.
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
inline void *heap_caps_malloc(size_t n, int) { return malloc(n); }
inline void *heap_caps_calloc(size_t n, size_t sz, int) { return calloc(n, sz); }
