// Face-down sleep (src/facedown_sleep.h), driven with accelerometer Z readings measured on a
// 1.75 standing on its kickstand: resting about -1280, face-down about +15340, about
// +14000 while a hand lifts it with the screen still down. One update() is one IMU check.
// Guards the bug where a touch on the dark screen made the radar take face-down for its
// resting pose, so it stayed lit face-down afterwards, and the one where a quick tap on the
// dark screen didn't wake it because the checks run further apart than FACEDOWN_CHECK_MS.
#include "facedown_sleep.h"

#include <assert.h>
#include <stdint.h>

static const int16_t REST = -1280, DOWN = 15340, LIFTED = 14000;
static const bool NO_TOUCH = false;
static const bool TOUCH = true;                    // a touch since the last check

// `n` checks in one pose; true when the screen is dark after the last one.
static bool hold(FaceDownSleep &s, int16_t az, int n, bool touched = NO_TOUCH, bool enabled = true) {
    for (int i = 0; i < n; ++i) s.update(true, az, touched, enabled);
    return s.asleep();
}

// The loop's checks with real times, wired like main.cpp: LVGL's inactivity time comes from
// the last touch (or boot), and touchedSinceLastCheck() decides "a touch since the previous
// check" from it.
struct Checks {
    FaceDownSleep s;
    uint32_t now = 0, lastCheck = 0, lastTouch = 0;
    // The next check, gapMs after the previous one; true when the screen is dark after it.
    bool check(uint32_t gapMs, int16_t az) {
        now += gapMs;
        const uint32_t inactiveMs = now - lastTouch;
        s.update(true, az, touchedSinceLastCheck(inactiveMs, now - lastCheck), true);
        lastCheck = now;
        return s.asleep();
    }
    // A tap whose last pressed reading is afterMs past the previous check.
    void tapEndsIn(uint32_t afterMs) { lastTouch = lastCheck + afterMs; }
};

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

    // --- a quick tap on the dark screen wakes it at the next check, however far apart the
    //     checks run. On a busy 1.75 they ran 406-1182 ms apart (median 627); the old rule
    //     ("touched in the last FACEDOWN_CHECK_MS") found most taps already too old. The tap
    //     counts once: with no other touch, the screen goes dark again after the full count ---
    {
        static const uint32_t GAPS[] = {406, 512, 627, 701, 790, 955, 1100, 1182};
        for (uint32_t gap : GAPS) {
            const uint32_t tapEnds[] = {gap / 10, gap / 2, gap - 20};
            for (uint32_t tapEnd : tapEnds) {
                Checks c;
                for (int i = 0; i < 20; ++i) c.check(627, REST);
                for (int i = 0; i < FACEDOWN_COUNT; ++i) c.check(627, DOWN);
                assert(c.s.asleep());
                c.tapEndsIn(tapEnd);
                assert(!c.check(gap, DOWN));                  // the tap woke it
                for (int i = 0; i < FACEDOWN_COUNT - 1; ++i) assert(!c.check(627, DOWN));
                assert(c.check(627, DOWN));                   // dark again, untouched
            }
        }
    }

    // --- no touch at all: nothing wakes it, at any pace ---
    {
        Checks c;
        for (int i = 0; i < 20; ++i) c.check(627, REST);
        for (int i = 0; i < FACEDOWN_COUNT; ++i) c.check(406, DOWN);
        for (int i = 0; i < 30; ++i) assert(c.check(i % 2 ? 1182 : 406, DOWN));
    }
}
