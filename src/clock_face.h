#pragma once
// Idle clock: the animation model. Pure (no LVGL, no Arduino), so tests/clock_face_test.cpp
// drives it on the host; radar_view.cpp (dial, dimmed scene, simulator hands) and the
// compositor in display.cpp (hands on the device) both draw from the same ClockFrame.
//
// The radar's sweep line becomes the second hand: on entry it spins FORWARD (at least
// CLOCK_MIN_SPIN_DEG) onto the current second and stays on it from CLOCK_LOCK_FRAC of the
// entry; on exit it speeds up from 6 deg/s to the radar sweep speed, so the radar sweep goes
// on from where the hand was. The hour and minute hands grow out of the centre with a small
// overshoot.
//
// Angles are degrees, 0 = 12 o'clock / north, clockwise. Times are ms; every elapsed time is
// an unsigned difference, so a millis() rollover mid-animation is harmless.
#include <math.h>
#include <stdint.h>
#include "config.h"

static inline float clock_smooth(float x) { return x <= 0 ? 0 : (x >= 1 ? 1 : x * x * (3 - 2 * x)); }
static inline float clock_seg(float p, float a, float b) { return clock_smooth((p - a) / (b - a)); }
// Ease-out-back: 0 -> 0, 1 -> 1, peaks ~1.1 just before the end (the hands' small overshoot).
static inline float clock_out_back(float x) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    const float s = 1.6f, u = x - 1;
    return 1 + (s + 1) * u * u * u + s * u * u;
}
static inline float clock_wrap360(float d) { d = fmodf(d, 360.0f); return d < 0 ? d + 360.0f : d; }
static inline float clock_wrap180(float d) { return clock_wrap360(d + 180.0f) - 180.0f; }

// Wall-clock hand angles. Smooth: the second hand includes the ms, the minute hand the
// seconds, the hour hand the minutes.
struct ClockHands { float hourDeg, minDeg, secDeg; };
static inline ClockHands clock_hands(int hour, int min, int sec, int ms) {
    const float s = (float)sec + (float)ms / 1000.0f;
    const float m = (float)min + s / 60.0f;
    const float h = (float)(hour % 12) + m / 60.0f;
    return { h * 30.0f, m * 6.0f, s * 6.0f };
}

struct ClockFrame {
    float   p;          // 0 = radar ... 1 = clock, linear in time
    int     stage;      // LVGL scene step: 0 radar, 1 labels gone, 2 dimmed + dial, 3 full dial
    float   leadDeg;    // sweep line / second hand
    float   leadLen;    // tip, fraction of RADAR_R_OUTER_PX
    float   leadTail;   // tail past the centre, fraction of R (0 on the radar)
    float   leadW;      // px: SWEEP_LEAD_W on the radar, one more on the clock
    uint8_t trailOpa;   // sweep trail multiplier: 255 radar, CLOCK_TRAIL_OPA clock, 0 without a sweep
    float   grow;       // hour/minute hand length factor: 0 ... ~1.1 ... 1
    float   hourDeg, minDeg;
    float   capR;       // centre cap radius, px (0 = none)
    float   capDot;     // opacity 0..1 of the lead-coloured dot on the cap
};

class ClockFace {
public:
    enum Phase { OFF, ENTERING, ON, EXITING };

    // Start the radar -> clock animation. radarDeg/radarDps: where the sweep line is now and
    // how fast it turns (whoever draws it). sweepShown: a sweep is visible to turn into the
    // second hand; without one (Orb, or the sweep switched off) the second hand grows in with
    // the others. Refused unless OFF: a clock still animating away finishes first.
    bool enter(uint32_t nowMs, float radarDeg, float radarDps, bool sweepShown, const ClockHands &wall) {
        if (_phase != OFF) return false;
        _phase = ENTERING;
        _t0 = nowMs;
        _sweep = sweepShown;
        _dps = radarDps;
        _r0 = clock_wrap360(radarDeg);
        _c0 = clock_wrap360(wall.secDeg);
        // Whole extra turns so the line travels at least CLOCK_MIN_SPIN_DEG, always forward.
        const float turns = ceilf((CLOCK_MIN_SPIN_DEG - (_c0 - _r0)) / 360.0f);
        _turns = turns > 0 ? turns : 0;
        return true;
    }

    // Start the clock -> radar animation, from wherever the entry got to. No-op unless
    // ENTERING or ON.
    void exit(uint32_t nowMs, const ClockHands &wall) {
        if (_phase != ENTERING && _phase != ON) return;
        const ClockFrame f = frame(nowMs, wall);
        _a0 = f.leadDeg;
        _p0 = f.p;
        _t0 = nowMs;
        _phase = _p0 > 0 ? EXITING : OFF;
    }

    Phase phase() const { return _phase; }
    bool requested() const { return _phase == ENTERING || _phase == ON; }

    // The frame at nowMs. Also finishes the animations: ENTERING -> ON, EXITING -> OFF (that
    // last call still returns the exit's final frame, so a renderer can hand the sweep back
    // at the hand's angle).
    ClockFrame frame(uint32_t nowMs, const ClockHands &wall) {
        const uint32_t el = nowMs - _t0;
        float p = 0, lead = wall.secDeg;
        switch (_phase) {
        case OFF:
            break;
        case ENTERING: {
            const float u = (float)el / (float)CLOCK_ENTER_MS;
            p = u >= 1 ? 1 : u;
            if (_sweep) {
                const float r = _r0 + _dps * ((float)el / 1000.0f);             // where the sweep would be
                const float c = _c0 + clock_wrap180(wall.secDeg - _c0);        // the second, unwrapped near c0
                const float e = clock_smooth(p / CLOCK_LOCK_FRAC);
                lead = r + e * (c + 360.0f * _turns - r);
            }
            if (u >= 1) _phase = ON;
            break;
        }
        case ON:
            p = 1;
            break;
        case EXITING: {
            const float durMs = _p0 * (float)CLOCK_EXIT_MS;
            const float elc = (float)el < durMs ? (float)el : durMs;
            const float v = elc / durMs;
            p = _p0 * (1 - v);
            if (_sweep) {
                const float dt = elc / 1000.0f;
                lead = _a0 + 6.0f * dt + clock_smooth(v) * (_dps - 6.0f) * dt;   // 6 deg/s -> radar speed
            }
            if (v >= 1) { p = 0; _phase = OFF; }
            break;
        }
        }

        ClockFrame f;
        f.p = p;
        f.stage = _phase == OFF ? 0 : (p >= STAGE3_P ? 3 : (p >= STAGE2_P ? 2 : 1));
        f.leadDeg = clock_wrap360(lead);
        const float k = clock_seg(p, 0.5f, 1.0f);                  // second-hand styling
        f.grow = clock_out_back(clock_seg(p, 0.45f, 1.0f));
        f.leadTail = CLOCK_SEC_TAIL * k;
        f.leadW = SWEEP_LEAD_W + k;
        if (_sweep) {
            f.leadLen = 1.0f - (1.0f - CLOCK_SEC_LEN) * clock_seg(p, 0.4f, 1.0f);
            f.trailOpa = (uint8_t)lroundf(255.0f - (255.0f - (float)CLOCK_TRAIL_OPA) * k);
        } else {
            f.leadLen = CLOCK_SEC_LEN * f.grow;
            f.trailOpa = 0;
        }
        f.hourDeg = wall.hourDeg;
        f.minDeg = wall.minDeg;
        f.capR = k > 0 ? (float)CLOCK_CAP_R * (0.57f + 0.43f * k) : 0;
        f.capDot = k;
        return f;
    }

    // Scene steps for the LVGL side (its repaints are slow, so the dim and the dial change in
    // a few steps; everything that moves is in the frame above).
    static constexpr float STAGE2_P = 0.45f;
    static constexpr float STAGE3_P = 0.80f;

private:
    Phase    _phase = OFF;
    uint32_t _t0 = 0;
    bool     _sweep = true;
    float    _dps = 0, _r0 = 0, _c0 = 0, _turns = 0;   // entry
    float    _a0 = 0, _p0 = 0;                          // exit
};
