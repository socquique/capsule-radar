#pragma once
// Which aircraft the alerts have seen, and when (checkAudioEvents() in main.cpp). Pure
// logic, so tests/alert_memory_test.cpp can drive it on the host.
//
// A fixed table of (id, last seen), allocated on first use through alert_memory_calloc():
// main.cpp puts it in PSRAM, so the alert memory never touches the internal heap TLS needs;
// the host test uses plain calloc. When the table is full the oldest entry is reused.

#include "config.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

// Zeroed storage for the table; defined by the caller (main.cpp: PSRAM first).
void *alert_memory_calloc(size_t n, size_t size);

// Ids are the 24-bit ICAO address from `hex`, plus a bit for '~' (non-ICAO) addresses and a
// marker bit so 0 can mean an empty slot.
inline uint32_t acId(const char *hex) {
    const bool tilde = (*hex == '~');
    const uint32_t a = (uint32_t)strtoul(tilde ? hex + 1 : hex, nullptr, 16) & 0xFFFFFFu;
    return a | (tilde ? (1u << 24) : 0u) | (1u << 25);
}

struct AlertMemory {
    struct Slot { uint32_t id, at; };
    Slot *s = nullptr;
    bool ready() {
        if (!s) s = (Slot *)alert_memory_calloc(ALERT_MEMORY_SLOTS, sizeof(Slot));
        return s != nullptr;
    }
    // seen within `windowMs` of `now`?
    bool recent(uint32_t id, uint32_t now, uint32_t windowMs) const {
        for (int i = 0; i < ALERT_MEMORY_SLOTS; ++i)
            if (s[i].id == id) return now - s[i].at <= windowMs;
        return false;
    }
    void touch(uint32_t id, uint32_t now) {
        int victim = 0;
        for (int i = 0; i < ALERT_MEMORY_SLOTS; ++i) {
            if (s[i].id == id) { s[i].at = now; return; }
            if (s[i].id == 0) { victim = i; break; }                 // first empty slot
            if (now - s[i].at > now - s[victim].at) victim = i;      // else the oldest
        }
        s[victim] = { id, now };
    }
};
