// Host tests for the alert memory (src/alert_memory.h): the 15-minute "seen recently" window
// that stops re-pings when an aircraft drops out of the feed and comes back, the table
// reusing its oldest entry when full, and the ids built from `hex`.
#include "alert_memory.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void *alert_memory_calloc(size_t n, size_t size) { return calloc(n, size); }

static const uint32_t MIN = 60UL * 1000UL;

static uint32_t idFor(int i) {           // distinct ICAO-style ids for filling the table
    char hex[8];
    snprintf(hex, sizeof hex, "%06x", 0x100000 + i);
    return acId(hex);
}

int main() {
    // ---- ids ----
    assert(acId("a1b2c3") == (0xA1B2C3u | (1u << 25)));
    assert(acId("A1B2C3") == acId("a1b2c3"));
    assert(acId("~a1b2c3") == (0xA1B2C3u | (1u << 24) | (1u << 25)));
    assert(acId("~a1b2c3") != acId("a1b2c3"));       // non-ICAO and ICAO don't collide
    assert(acId("000000") != 0);                      // the marker bit: 0 stays "empty slot"

    // ---- the 15-minute window ----
    {
        AlertMemory m;
        assert(m.ready());
        const uint32_t id = acId("ae268e"), t0 = 1000;
        assert(!m.recent(id, t0, ALERT_FORGET_MS));    // never seen: new
        m.touch(id, t0);
        assert(m.recent(id, t0, ALERT_FORGET_MS));
        // drops out of the feed and is back after 14 minutes: still known, no ping
        assert(m.recent(id, t0 + 14 * MIN, ALERT_FORGET_MS));
        // the window's edge counts as recent; one millisecond later it's new again
        assert(m.recent(id, t0 + ALERT_FORGET_MS, ALERT_FORGET_MS));
        assert(!m.recent(id, t0 + ALERT_FORGET_MS + 1, ALERT_FORGET_MS));
        // still in range: each poll refreshes the time, so it never times out while present
        m.touch(id, t0 + 10 * MIN);
        assert(m.recent(id, t0 + 20 * MIN, ALERT_FORGET_MS));
        // another aircraft is unaffected
        assert(!m.recent(acId("ae017a"), t0 + 20 * MIN, ALERT_FORGET_MS));
        // a second memory (the proximity one) is independent
        AlertMemory prox;
        assert(prox.ready());
        assert(!prox.recent(id, t0 + 20 * MIN, ALERT_FORGET_MS));
    }

    // ---- millis() rollover ----
    {
        AlertMemory m;
        assert(m.ready());
        const uint32_t id = acId("abcdef"), t0 = UINT32_MAX - 1000;
        m.touch(id, t0);
        assert(m.recent(id, 5000, ALERT_FORGET_MS));                 // 6 s later, across the wrap
        assert(!m.recent(id, (uint32_t)(t0 + ALERT_FORGET_MS + 1), ALERT_FORGET_MS));
    }

    // ---- a full table reuses its oldest entry ----
    {
        AlertMemory m;
        assert(m.ready());
        for (int i = 0; i < ALERT_MEMORY_SLOTS; ++i) m.touch(idFor(i), 1000 + i);
        for (int i = 0; i < ALERT_MEMORY_SLOTS; ++i)
            assert(m.recent(idFor(i), 2000, ALERT_FORGET_MS));
        // refresh the first entry, so the oldest is now the second
        m.touch(idFor(0), 3000);
        const uint32_t extra = acId("fedcba");
        m.touch(extra, 3001);
        assert(m.recent(extra, 3001, ALERT_FORGET_MS));
        assert(!m.recent(idFor(1), 3001, ALERT_FORGET_MS));         // the oldest was replaced
        assert(m.recent(idFor(0), 3001, ALERT_FORGET_MS));          // the refreshed one kept
        for (int i = 2; i < ALERT_MEMORY_SLOTS; ++i)
            assert(m.recent(idFor(i), 3001, ALERT_FORGET_MS));      // and everything else
    }

    // ---- a table with room never drops anyone ----
    {
        AlertMemory m;
        assert(m.ready());
        m.touch(idFor(0), 1000);
        m.touch(idFor(1), 2000);
        m.touch(idFor(2), 50000);                                    // goes to an empty slot
        assert(m.recent(idFor(0), 50000, ALERT_FORGET_MS));
        assert(m.recent(idFor(1), 50000, ALERT_FORGET_MS));
        assert(m.recent(idFor(2), 50000, ALERT_FORGET_MS));
    }

    printf("  alert memory: all checks passed\n");
    return 0;
}
