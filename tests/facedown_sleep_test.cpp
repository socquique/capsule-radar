// Face-down sleep (src/facedown_sleep.h), driven with accelerometer Z readings measured on a
// 1.75 standing on its kickstand: resting about -1280, face-down about +15340, about
// +14000 while a hand lifts it with the screen still down. One update() is one IMU check
// (FACEDOWN_CHECK_MS apart). Guards the bug where a touch on the dark screen made the radar
// take face-down for its resting pose, so it stayed lit face-down afterwards.
#include "facedown_sleep.h"

#include <assert.h>
#include <stdint.h>

static const int16_t REST = -1280, DOWN = 15340, LIFTED = 14000;
static const uint32_t NO_TOUCH = 600000;           // ms since the last touch: none lately
static const uint32_t TOUCH = 50;                  // a touch since the last check

// `n` checks in one pose; true when the screen is dark after the last one.
static bool hold(FaceDownSleep &s, int16_t az, int n, uint32_t touchedMs = NO_TOUCH, bool enabled = true) {
    for (int i = 0; i < n; ++i) s.update(true, az, touchedMs, enabled);
    return s.asleep();
}

int main() {
    // --- face-down goes dark after FACEDOWN_COUNT checks, and standing up lights it ---
    {
        FaceDownSleep s;
        assert(!hold(s, REST, 20));
        assert(!hold(s, DOWN, FACEDOWN_COUNT - 1));
        assert(hold(s, DOWN, 1));
        assert(!hold(s, REST, 1));
    }

    // --- the reported bug: a touch on the dark screen while it is still face-down, then it
    //     lies face-down untouched again -> dark again, and every later face-down too ---
    {
        FaceDownSleep s;
        hold(s, REST, 20);
        assert(hold(s, DOWN, FACEDOWN_COUNT));
        assert(!hold(s, LIFTED, 1, TOUCH));           // the touch lights it at once
        assert(!hold(s, LIFTED, FACEDOWN_COUNT - 1)); // not dark again before the full count
        assert(hold(s, DOWN, 1));                     // dark again, still face-down
        assert(!hold(s, REST, 1));                    // stood up
        assert(hold(s, DOWN, FACEDOWN_COUNT));        // the next face-down works as before
    }

    // --- a finger on the glass while laying it down keeps it lit; dark once it lifts ---
    {
        FaceDownSleep s;
        hold(s, REST, 20);
        assert(!hold(s, DOWN, 3 * FACEDOWN_COUNT, TOUCH));
        assert(!hold(s, DOWN, FACEDOWN_COUNT - 1));
        assert(hold(s, DOWN, 1));
    }

    // --- a failed read neither counts nor resets; only failures never go dark ---
    {
        FaceDownSleep s;
        hold(s, REST, 20);
        hold(s, DOWN, FACEDOWN_COUNT - 1);
        for (int i = 0; i < 10; ++i) s.update(false, 0, NO_TOUCH, true);
        assert(!s.asleep());
        assert(hold(s, DOWN, 1));                     // the count kept its place
        FaceDownSleep t;
        for (int i = 0; i < 50; ++i) t.update(false, 0, NO_TOUCH, true);
        assert(!t.asleep());
    }

    // --- the web switch: off never goes dark; on while face-down goes dark at the next check ---
    {
        FaceDownSleep s;
        hold(s, REST, 20, NO_TOUCH, false);
        assert(!hold(s, DOWN, 3 * FACEDOWN_COUNT, NO_TOUCH, false));
        assert(hold(s, DOWN, 1, NO_TOUCH, true));
    }

    // --- orientation-agnostic: a unit whose Z has the other sign works the same way ---
    {
        FaceDownSleep s;
        assert(!hold(s, (int16_t)-REST, 20));
        assert(hold(s, (int16_t)-DOWN, FACEDOWN_COUNT));
        assert(!hold(s, (int16_t)-REST, 1));
    }

    // --- booted face-down: the baseline starts on the face-down pose, and standing upright
    //     long enough moves it back, so face-down works again ---
    {
        FaceDownSleep s;
        assert(!hold(s, DOWN, 2 * FACEDOWN_COUNT));   // nothing to compare with yet: stays lit
        hold(s, REST, 60);
        assert(hold(s, DOWN, FACEDOWN_COUNT));
    }
}
