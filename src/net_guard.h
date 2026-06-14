#pragma once
#include "config.h"
#include "app_log.h"
#include <Arduino.h>
#include <esp_heap_caps.h>

static inline uint32_t tls_largest_internal_block(void) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
}

static inline bool tls_heap_ok(const char *tag) {
    const uint32_t biggest = tls_largest_internal_block();
    if (biggest >= TLS_INTERNAL_MIN_BYTES) return true;

    static uint32_t lastLog = 0;
    const uint32_t now = millis();
    if (now - lastLog > 10000UL) {
        lastLog = now;
        app_log_printf("[%s] low TLS memory, skipping HTTPS (largest internal block %u)\n",
                      tag ? tag : "net", (unsigned)biggest);
    }
    return false;
}
