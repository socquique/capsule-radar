// Host test for the idle clock animation model (src/clock_face.h).
#include "clock_face.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

static const float DEV_DPS = 360000.0f / SWEEP_COMP_PERIOD_MS;   // compositor sweep (device)
static const float SIM_DPS = 360000.0f / SWEEP_PERIOD_MS;        // LVGL sweep (simulator)

// Wall clock that starts at 10:09:s0 (+ ms0) when millis() == base.
struct Wall {
    uint32_t base; int s0, ms0;
    ClockHands at(uint32_t nowMs) const {
        const uint32_t ms = (uint32_t)(s0 * 1000 + ms0) + (nowMs - base);
        return clock_hands(10, 9 + (int)(ms / 60000), (int)(ms / 1000 % 60), (int)(ms % 1000));
    }
};

static bool near(float a, float b, float eps) { return fabsf(a - b) <= eps; }
static bool nearAngle(float a, float b, float eps) { return fabsf(clock_wrap180(a - b)) <= eps; }

// One entry, sampled every 10 ms: returns the total forward travel of the sweep line.
static float checkEntry(uint32_t t0, float radarDeg, float dps, const Wall &w) {
    ClockFace c;
    assert(c.enter(t0, radarDeg, dps, true, w.at(t0)));
    ClockFrame f = c.frame(t0, w.at(t0));
    assert(nearAngle(f.leadDeg, radarDeg, 0.01f));         // starts on the radar's sweep line
    assert(f.p == 0 && f.stage == 1);                        // labels go at once
    float travel = 0, prev = f.leadDeg;
    for (uint32_t t = 10; t <= (uint32_t)CLOCK_ENTER_MS; t += 10) {
        const ClockHands h = w.at(t0 + t);
        f = c.frame(t0 + t, h);
        const float step = clock_wrap180(f.leadDeg - prev);
        assert(step >= -1e-3f);                              // never runs backwards
        travel += step;
        prev = f.leadDeg;
        if (t >= (uint32_t)(CLOCK_LOCK_FRAC * CLOCK_ENTER_MS))
            assert(nearAngle(f.leadDeg, h.secDeg, 0.01f));   // locked onto the second
    }
    assert(c.phase() == ClockFace::ON && f.p == 1 && f.stage == 3);
    return travel;
}

int main() {
    // --- wall-clock hand angles (the classic 10:09:32.5 watch pose) ---
    {
        const ClockHands h = clock_hands(10, 9, 32, 500);
        assert(near(h.hourDeg, 304.770833f, 1e-3f));
        assert(near(h.minDeg, 57.25f, 1e-3f));
        assert(near(h.secDeg, 195.0f, 1e-3f));
        const ClockHands n = clock_hands(23, 15, 0, 0);       // 24 h input: 23:15 reads like 11:15
        assert(near(n.hourDeg, 337.5f, 1e-3f) && near(n.minDeg, 90.0f, 1e-3f));
    }

    // --- entry over every start angle x second, at both sweep speeds ---
    // The line must start on the radar sweep, only ever move forward, travel at least
    // CLOCK_MIN_SPIN_DEG (and at most one extra turn), and sit on the second from
    // CLOCK_LOCK_FRAC of the entry on. Unwrapped against the WRAPPED start angle, a start
    // just past the second used to aim backwards or spin a needless turn.
    const float speeds[] = { DEV_DPS, SIM_DPS };
    for (float dps : speeds)
        for (int r = 0; r < 360; r += 15)
            for (int s = 0; s < 60; s += 1) {
                const Wall w = { 5000, s, 250 };
                const float travel = checkEntry(5000, (float)r, dps, w);
                assert(travel >= CLOCK_MIN_SPIN_DEG - 0.01f);
                assert(travel < 360.0f + CLOCK_MIN_SPIN_DEG + 6.0f * CLOCK_ENTER_MS / 1000.0f + 0.01f);
            }

    // --- the clock itself, and the exit back to the radar ---
    {
        const Wall w = { 1000, 32, 500 };
        ClockFace c;
        c.enter(1000, 40.0f, DEV_DPS, true, w.at(1000));
        const uint32_t tOn = 1000 + CLOCK_ENTER_MS + 5000;
        ClockFrame f = c.frame(tOn, w.at(tOn));
        assert(c.phase() == ClockFace::ON && c.requested());
        assert(f.stage == 3 && f.p == 1 && f.grow == 1);
        assert(nearAngle(f.leadDeg, w.at(tOn).secDeg, 0.01f));
        assert(near(f.leadTail, CLOCK_SEC_TAIL, 1e-4f) && near(f.leadW, SWEEP_LEAD_W + 1, 1e-4f));
        assert(near(f.leadLen, CLOCK_SEC_LEN, 1e-4f) && f.trailOpa == CLOCK_TRAIL_OPA);
        assert(near(f.capR, CLOCK_CAP_R, 1e-3f));

        const float hand = f.leadDeg;
        c.exit(tOn, w.at(tOn));
        assert(c.phase() == ClockFace::EXITING && !c.requested());
        assert(!c.enter(tOn + 10, 0.0f, DEV_DPS, true, w.at(tOn + 10)));   // finishes leaving first
        f = c.frame(tOn, w.at(tOn));
        assert(nearAngle(f.leadDeg, hand, 0.01f) && f.p == 1);               // leaves from the hand
        float prevP = f.p, prev = f.leadDeg, lastSpeed = 0;
        for (uint32_t t = 10; t <= (uint32_t)CLOCK_EXIT_MS; t += 10) {
            f = c.frame(tOn + t, w.at(tOn + t));
            assert(f.p <= prevP);
            const float step = clock_wrap180(f.leadDeg - prev);
            assert(step >= 0);
            lastSpeed = step / 0.010f;
            prevP = f.p;
            prev = f.leadDeg;
        }
        assert(c.phase() == ClockFace::OFF && f.p == 0 && f.stage == 0);
        assert(near(lastSpeed, DEV_DPS, DEV_DPS * 0.03f));                    // hands over at sweep speed
        assert(f.leadTail == 0 && f.leadW == SWEEP_LEAD_W && f.grow == 0 && f.capR == 0);
        assert(f.trailOpa == 255 && f.leadLen == 1.0f);
        assert(c.enter(tOn + CLOCK_EXIT_MS + 10, 0.0f, DEV_DPS, true, w.at(tOn + CLOCK_EXIT_MS + 10)));
    }

    // --- a touch mid-entry: the way back starts where the entry was, without a jump ---
    {
        const Wall w = { 0, 7, 0 };
        ClockFace c;
        c.enter(0, 300.0f, DEV_DPS, true, w.at(0));
        const ClockFrame before = c.frame(790, w.at(790));
        c.exit(800, w.at(800));
        const ClockFrame after = c.frame(810, w.at(810));
        assert(before.p < 0.41f && after.p > 0.38f && after.p < before.p + 0.02f);
        assert(fabsf(clock_wrap180(after.leadDeg - before.leadDeg)) < 8.0f);
        const ClockFrame end = c.frame(800 + CLOCK_EXIT_MS, w.at(800 + CLOCK_EXIT_MS));
        assert(c.phase() == ClockFace::OFF && end.p == 0);
    }

    // --- no sweep to morph from (Orb, or the sweep switched off) ---
    {
        const Wall w = { 0, 50, 0 };
        ClockFace c;
        c.enter(0, 123.0f, DEV_DPS, false, w.at(0));
        float firstLen = -1, lastLen = -1;
        for (uint32_t t = 0; t <= (uint32_t)CLOCK_ENTER_MS; t += 50) {
            const ClockFrame f = c.frame(t, w.at(t));
            assert(nearAngle(f.leadDeg, w.at(t).secDeg, 0.01f));   // already the second hand
            assert(f.trailOpa == 0 && f.leadLen >= 0);
            if (firstLen < 0) firstLen = f.leadLen;
            lastLen = f.leadLen;
        }
        assert(firstLen == 0);                                       // grows in from nothing
        assert(near(lastLen, CLOCK_SEC_LEN, 1e-4f));
    }

    // --- millis() rolls over in the middle of the entry ---
    {
        const uint32_t t0 = 0xFFFFFFFFu - 700;
        const Wall w = { t0, 3, 0 };
        ClockFace c;
        c.enter(t0, 10.0f, DEV_DPS, true, w.at(t0));
        const ClockFrame mid = c.frame(t0 + 1000, w.at(t0 + 1000));
        assert(near(mid.p, 0.5f, 1e-3f) && c.phase() == ClockFace::ENTERING);
        c.frame(t0 + CLOCK_ENTER_MS, w.at(t0 + CLOCK_ENTER_MS));
        assert(c.phase() == ClockFace::ON);
    }

    // --- ease-out-back: starts and ends on its values, small overshoot ---
    {
        assert(clock_out_back(0) == 0 && clock_out_back(1) == 1);
        float peak = 0;
        for (int i = 0; i <= 1000; ++i) peak = fmaxf(peak, clock_out_back(i / 1000.0f));
        assert(peak > 1.0f && peak < 1.12f);
    }

    printf("clock_face: all checks passed\n");
    return 0;
}
