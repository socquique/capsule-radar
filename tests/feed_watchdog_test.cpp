// Host test for the feed-stuck watchdog (src/feed_watchdog.h), driven together with the
// real pacer the way adsb_task drives both: a 250 ms loop, a feed poll every
// POLL_INTERVAL_MS, and each poll asking every provider that is not cooling
// (AdsbClient::poll).
#include "feed_watchdog.h"
#include "adsb_pacing.h"

#include <assert.h>
#include <stdint.h>

static const uint32_t TICK_MS    = 250;                               // adsb_task's vTaskDelay
static const uint32_t FRAGMENTED = ADSB_STUCK_MIN_LARGEST_BLOCK - 1;
static const uint32_t HEALTHY    = ADSB_STUCK_MIN_LARGEST_BLOCK * 4;

// What the providers do when asked: no answer at all (a TLS handshake that cannot
// allocate fails at once), HTTP 403, or MIXED: provider 0 answers 403 while the rest are silent.
enum Reply { SILENT, REFUSED, MIXED };

// A 403 takes a real round trip. The latency also keeps the park deadlines off the loop's
// 250 ms grid and the poll's 2 s grid, as on the device; a simulation that lands every
// park end exactly on a tick hides the check-before-poll ordering in adsb_task.
static const uint32_t REFUSE_LATENCY_MS = 337;

struct Run {
    uint32_t restartAfterMs = 0;   // time from the start to the restart; 0 = never
    uint32_t sinceAnswerMs = 0;    // silence the watchdog had seen when it restarted
    bool     backstop = false;     // RESTART_LONG (30-minute backstop) rather than RESTART (fragmented heap)
    int      stuckLogs = 0;
    int      skips = 0;
};

// `durationMs` of adsb_task with WiFi up and the providers giving `reply`.
static Run simulate(Reply reply, uint32_t largestBlock, uint32_t durationMs, uint32_t startMs) {
    AdsbPacer pacer;
    FeedWatchdog wd(startMs);
    Run r;
    uint32_t answeredMs = 0;       // AdsbClient::lastResponseMs(); 0 = nobody answered yet
    uint32_t lastPollMs = 0;
    bool polled = false;
    uint32_t busyUntil = startMs;  // the loop is inside a request until then
    for (uint32_t t = 0; t < durationMs; t += TICK_MS) {
        const uint32_t now = startMs + t;
        if ((int32_t)(now - busyUntil) < 0) continue;   // still inside a request
        if (answeredMs) wd.onAnswer(answeredMs);
        const FeedWatchdog::Action a = wd.check(true, now, pacer.allParked(now), [&] { return largestBlock; });
        if (a == FeedWatchdog::RESTART || a == FeedWatchdog::RESTART_LONG) {
            r.restartAfterMs = t;
            r.sinceAnswerMs = now - (answeredMs ? answeredMs : startMs);
            r.backstop = (a == FeedWatchdog::RESTART_LONG);
            return r;
        }
        if (a == FeedWatchdog::LOG_STUCK) r.stuckLogs++;
        if (polled && now - lastPollMs < POLL_INTERVAL_MS) continue;
        polled = true;
        lastPollMs = now;
        bool asked = false;
        uint32_t clock = now;
        for (int i = 0; i < ADSB_PROVIDER_COUNT; ++i) {
            if (pacer.cooling(i, clock)) continue;
            asked = true;
            pacer.onAttempt(i, clock);
            if (reply == REFUSED || (reply == MIXED && i == 0)) {
                clock += REFUSE_LATENCY_MS;
                pacer.onRefused(i, clock);
                answeredMs = clock;
            } else {
                pacer.onTransportFail(i);
            }
        }
        busyUntil = clock;
        if (!asked) r.skips++;
        wd.onPoll(asked ? FeedWatchdog::FAILED : FeedWatchdog::SKIPPED, now);
    }
    return r;
}

// The heap-gated restart fired one loop tick after the silence passed ADSB_STUCK_MS.
static bool restartsOnSchedule(const Run &r) {
    return !r.backstop && r.sinceAnswerMs > ADSB_STUCK_MS && r.sinceAnswerMs <= ADSB_STUCK_MS + TICK_MS;
}

// The 30-minute backstop fired, one loop tick after the silence passed ADSB_STUCK_HARD_MS.
static bool backstopOnSchedule(const Run &r) {
    return r.backstop && r.sinceAnswerMs > ADSB_STUCK_HARD_MS
                      && r.sinceAnswerMs <= ADSB_STUCK_HARD_MS + TICK_MS;
}

int main() {
    // --- fast TLS failures from a fragmented heap reboot on schedule ---
    // The pacer answers the failures with silence gaps and the poll skips inside them.
    // Counting those skips as alive would keep the restart from ever coming.
    {
        const Run r = simulate(SILENT, FRAGMENTED, 10 * 60000, 1000);
        assert(r.skips > 0);                       // the scenario really does skip
        assert(restartsOnSchedule(r));
    }
    // ...and across a millis() rollover.
    {
        const Run r = simulate(SILENT, FRAGMENTED, 10 * 60000, 0xFFFFFFFFu - 60000);
        assert(r.skips > 0);
        assert(restartsOnSchedule(r));
    }

    // --- internet down behind working WiFi, healthy heap: stay up, log once ---
    {
        // Shortened from 30 to 29 minutes: at 30 minutes the backstop below takes over.
        const Run r = simulate(SILENT, HEALTHY, 29 * 60000, 1000);
        assert(r.restartAfterMs == 0);
        assert(r.stuckLogs == 1);
    }
    // ...but not forever: 30 minutes without any answer restarts it whatever the heap
    // says, once, one loop tick after the 30th minute.
    {
        const Run r = simulate(SILENT, HEALTHY, 40 * 60000, 1000);
        assert(r.stuckLogs == 1);                  // one log for the 3..30 minute stretch
        assert(backstopOnSchedule(r));
        assert(r.restartAfterMs > ADSB_STUCK_HARD_MS && r.restartAfterMs <= ADSB_STUCK_HARD_MS + TICK_MS);
    }
    // ...across the millis() rollover, wrapping early in the stuck stretch and late in it.
    {
        const Run early = simulate(SILENT, HEALTHY, 40 * 60000, 0xFFFFFFFFu - 60000);
        assert(early.stuckLogs == 1);
        assert(backstopOnSchedule(early));
        const Run late = simulate(SILENT, HEALTHY, 40 * 60000, 0xFFFFFFFFu - ADSB_STUCK_HARD_MS + 60000);
        assert(late.stuckLogs == 1);
        assert(backstopOnSchedule(late));
    }
    // A fragmented heap is not held back by the backstop: it still restarts at ~180 s, as RESTART.
    {
        const Run r = simulate(SILENT, FRAGMENTED, 40 * 60000, 1000);
        assert(restartsOnSchedule(r));
        assert(!r.backstop);
    }

    // --- every provider refusing (403s, then escalating parks), healthy heap: stay up ---
    {
        const Run r = simulate(REFUSED, HEALTHY, 14 * 60000, 1000);
        assert(r.skips > 0);
        assert(r.restartAfterMs == 0);
        assert(r.stuckLogs == 1);
    }
    // Parks do not shield a fragmented heap: it still reboots, which clears both.
    {
        const Run r = simulate(REFUSED, FRAGMENTED, 14 * 60000, 1000);
        assert(restartsOnSchedule(r));
    }
    // The silence while every provider is parked is our own, so the 30-minute backstop
    // must not count it. The second park (2 x 15 min) lasts exactly as long as the
    // backstop; on a network that refuses everything, the first loop after the park saw
    // 30 min of silence and rebooted the device before it could knock again, about every
    // 45 minutes. Eight hours cover the 15, 30, 60, 120, 240 and 360 min parks. Start
    // offsets are off the tick and poll grids, and one run crosses the 2^32 wrap.
    {
        const uint32_t starts[] = { 1000, 1137, 0xFFFFFFFFu - 60000 - 137,
                                    0xFFFFFFFFu - 45 * 60000 - 137 };
        for (uint32_t s : starts) {
            const Run r = simulate(REFUSED, HEALTHY, 8 * 3600000u, s);
            assert(r.skips > 0);
            assert(r.restartAfterMs == 0);         // neither RESTART nor RESTART_LONG
        }
    }
    // A refusal on one provider does not shield a feed that is stuck otherwise: provider 0
    // answers 403 and is parked, 1 and 2 stay silent, so not every provider is parked and
    // the backstop runs on schedule (30 min after the last 403, which was at the third knock).
    {
        const uint32_t starts[] = { 1000, 1137, 0xFFFFFFFFu - 60000 - 137 };
        for (uint32_t s : starts) {
            const Run r = simulate(MIXED, HEALTHY, 3 * 3600000u, s);
            assert(r.skips > 0);
            assert(backstopOnSchedule(r));
        }
    }

    // --- WiFi down: never a restart, and the clock starts over when WiFi returns ---
    {
        FeedWatchdog wd(1000);
        auto fragmented = [] { return FRAGMENTED; };
        wd.onAnswer(1000);                         // the last answer before the drop
        for (uint32_t t = 1000; t <= 601000; t += TICK_MS)
            assert(wd.check(false, t, false, fragmented) == FeedWatchdog::NONE);
        // adsb_task passes the stale pre-drop stamp every loop: it must not drag the clock
        // back to before the outage and trigger a restart the moment WiFi returns.
        wd.onAnswer(1000);
        assert(wd.check(true, 601000 + ADSB_STUCK_MS, false, fragmented) == FeedWatchdog::NONE);
        wd.onAnswer(1000);
        assert(wd.check(true, 601000 + ADSB_STUCK_MS + 1, false, fragmented) == FeedWatchdog::RESTART);
    }

    // --- the backstop's own edges ---
    {
        auto healthy = [] { return HEALTHY; };
        FeedWatchdog wd(1000);
        assert(wd.check(true, 1000 + ADSB_STUCK_HARD_MS, false, healthy) == FeedWatchdog::LOG_STUCK);
        assert(wd.check(true, 1000 + ADSB_STUCK_HARD_MS + 1, false, healthy) == FeedWatchdog::RESTART_LONG);
        // An answer just short of the backstop starts the clock over.
        FeedWatchdog wd2(1000);
        wd2.onAnswer(1000 + ADSB_STUCK_HARD_MS - 1000);
        assert(wd2.check(true, 1000 + ADSB_STUCK_HARD_MS + 1, false, healthy) == FeedWatchdog::NONE);
        // The backstop needs WiFi: while it is down the clock is held at zero.
        FeedWatchdog wd3(1000);
        assert(wd3.check(false, 1000 + 2 * ADSB_STUCK_HARD_MS, false, healthy) == FeedWatchdog::NONE);
        assert(wd3.check(true, 1000 + 2 * ADSB_STUCK_HARD_MS + 1, false, healthy) == FeedWatchdog::NONE);
    }

    // --- the backstop's clock stands still while every provider is parked ---
    {
        auto healthy = [] { return HEALTHY; };
        FeedWatchdog wd(1000);
        // Two hours of parks: no backstop, and the heap-gated restart is not involved.
        for (uint32_t t = 1000; t <= 1000 + 2 * 3600000u; t += TICK_MS) {
            const FeedWatchdog::Action a = wd.check(true, t, true, healthy);
            assert(a == FeedWatchdog::NONE || a == FeedWatchdog::LOG_STUCK);
        }
        // The park ends and nobody has answered: the full 30 minutes start from the park
        // end (the last loop that saw it), not from the last answer long ago.
        const uint32_t end = 1000 + 2 * 3600000u;
        assert(wd.check(true, end + ADSB_STUCK_HARD_MS, false, healthy) == FeedWatchdog::NONE);
        assert(wd.check(true, end + ADSB_STUCK_HARD_MS + 1, false, healthy) == FeedWatchdog::RESTART_LONG);
        // The 180 s heap-gated restart is NOT paused by a park: a fragmented heap reboots.
        FeedWatchdog wd2(1000);
        auto fragmented = [] { return FRAGMENTED; };
        assert(wd2.check(true, 1000 + ADSB_STUCK_MS + 1, true, fragmented) == FeedWatchdog::RESTART);
    }
}
