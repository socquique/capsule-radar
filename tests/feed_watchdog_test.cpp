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

// What every provider does when asked: no answer at all (a TLS handshake that cannot
// allocate fails at once), or HTTP 403.
enum Reply { SILENT, REFUSED };

struct Run {
    uint32_t restartAfterMs = 0;   // time from the start to RESTART; 0 = never
    int      stuckLogs = 0;
    int      skips = 0;
};

// `durationMs` of adsb_task with WiFi up and every provider giving `reply`.
static Run simulate(Reply reply, uint32_t largestBlock, uint32_t durationMs, uint32_t startMs) {
    AdsbPacer pacer;
    FeedWatchdog wd(startMs);
    Run r;
    uint32_t answeredMs = 0;       // AdsbClient::lastResponseMs(); 0 = nobody answered yet
    uint32_t lastPollMs = 0;
    bool polled = false;
    for (uint32_t t = 0; t < durationMs; t += TICK_MS) {
        const uint32_t now = startMs + t;
        if (answeredMs) wd.onAnswer(answeredMs);
        const FeedWatchdog::Action a = wd.check(true, now, [&] { return largestBlock; });
        if (a == FeedWatchdog::RESTART) { r.restartAfterMs = t; return r; }
        if (a == FeedWatchdog::LOG_STUCK) r.stuckLogs++;
        if (polled && now - lastPollMs < POLL_INTERVAL_MS) continue;
        polled = true;
        lastPollMs = now;
        bool asked = false;
        for (int i = 0; i < ADSB_PROVIDER_COUNT; ++i) {
            if (pacer.cooling(i, now)) continue;
            asked = true;
            pacer.onAttempt(i, now);
            if (reply == REFUSED) { pacer.onRefused(i, now); answeredMs = now; }
            else                  pacer.onTransportFail(i);
        }
        if (!asked) r.skips++;
        wd.onPoll(asked ? FeedWatchdog::FAILED : FeedWatchdog::SKIPPED, now);
    }
    return r;
}

static bool restartsOnSchedule(const Run &r) {
    return r.restartAfterMs > ADSB_STUCK_MS && r.restartAfterMs <= ADSB_STUCK_MS + TICK_MS;
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
        const Run r = simulate(SILENT, HEALTHY, 30 * 60000, 1000);
        assert(r.restartAfterMs == 0);
        assert(r.stuckLogs == 1);
    }

    // --- every provider refusing (403s, then 15-minute parks), healthy heap: stay up ---
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

    // --- WiFi down: never a restart, and the clock starts over when WiFi returns ---
    {
        FeedWatchdog wd(1000);
        auto fragmented = [] { return FRAGMENTED; };
        wd.onAnswer(1000);                         // the last answer before the drop
        for (uint32_t t = 1000; t <= 601000; t += TICK_MS)
            assert(wd.check(false, t, fragmented) == FeedWatchdog::NONE);
        // adsb_task passes the stale pre-drop stamp every loop: it must not drag the clock
        // back to before the outage and trigger a restart the moment WiFi returns.
        wd.onAnswer(1000);
        assert(wd.check(true, 601000 + ADSB_STUCK_MS, fragmented) == FeedWatchdog::NONE);
        wd.onAnswer(1000);
        assert(wd.check(true, 601000 + ADSB_STUCK_MS + 1, fragmented) == FeedWatchdog::RESTART);
    }
}
