// Per-provider pacing policy (src/adsb_pacing.h). Guards the September 2026 outage
// behaviour: airplanes.live answers 403 to everyone without prior approval, and retrying
// it every poll doubled the request rate onto the providers that still worked.
#include "adsb_pacing.h"

#include <assert.h>
#include <stdint.h>

static const uint32_t STEP = 250;                          // adsb_task's loop tick

// Longest time each provider was held back between two attempts, and when it was last asked.
struct Gaps {
    uint32_t maxGap[ADSB_PROVIDER_COUNT] = {0};
    uint32_t last[ADSB_PROVIDER_COUNT]   = {0};
    bool     seen[ADSB_PROVIDER_COUNT]   = {false};
};

// Poll every tick, asking each provider that is not cooling (as AdsbClient::poll does).
// Slots in `silentMask` never answer; the others answer 200 at once.
static void drive(AdsbPacer &p, Gaps &g, unsigned silentMask, uint32_t start, uint32_t durMs) {
    for (uint32_t t = 0; t < durMs; t += STEP) {
        const uint32_t now = start + t;
        for (int s = 0; s < ADSB_PROVIDER_COUNT; ++s) {
            if (p.cooling(s, now)) continue;
            if (g.seen[s] && now - g.last[s] > g.maxGap[s]) g.maxGap[s] = now - g.last[s];
            g.seen[s] = true;
            g.last[s] = now;
            p.onAttempt(s, now);
            if (silentMask & (1u << s)) p.onTransportFail(s);
            else                        p.onOk(s);
        }
    }
}

int main() {
    // --- a fresh provider is asked immediately ---
    {
        AdsbPacer p;
        assert(!p.cooling(0, 1000));
        p.onAttempt(0, 1000);
        assert(!p.cooling(0, 1001));      // no spacing imposed yet: ask as often as we like
    }

    // --- 403 parks the provider, escalates on repeat, and announces exactly once ---
    {
        AdsbPacer p;
        assert(p.onRefused(0, 1000));                       // first refusal: caller logs it
        assert(p.cooling(0, (uint32_t)(1000 + ADSB_COOLDOWN_403_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(1000 + ADSB_COOLDOWN_403_MS)));   // 15 min: try again

        // Since airplanes.live went contributor-only, a repeated "no" escalates: the second
        // refusal parks twice as long (and stays quiet on the serial log).
        assert(!p.onRefused(0, 3000));
        assert(p.cooling(0, (uint32_t)(3000 + 2 * ADSB_COOLDOWN_403_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(3000 + 2 * ADSB_COOLDOWN_403_MS)));

        // ...and caps at ADSB_COOLDOWN_403_MAX_MS no matter how many refusals pile up.
        const uint32_t t = 5000;
        for (int i = 0; i < 12; ++i) p.onRefused(0, t);
        assert(p.cooling(0, (uint32_t)(t + ADSB_COOLDOWN_403_MAX_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(t + ADSB_COOLDOWN_403_MAX_MS)));

        // A success resets the ladder to the 15 min base and makes the next park news again.
        p.onOk(0);
        assert(p.onRefused(0, 999999));
        assert(p.cooling(0, (uint32_t)(999999 + ADSB_COOLDOWN_403_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(999999 + ADSB_COOLDOWN_403_MS)));
    }

    // --- 429 backs off multiplicatively and caps ---
    {
        AdsbPacer p;
        assert(p.onRateLimited(0, 1000, 0) == ADSB_SPACING_STEP_MS);
        assert(p.onRateLimited(0, 2000, 0) == ADSB_SPACING_STEP_MS * 2);
        assert(p.onRateLimited(0, 3000, 0) == ADSB_SPACING_STEP_MS * 4);
        for (int i = 0; i < 10; ++i) p.onRateLimited(0, 4000, 0);
        assert(p.spacingMs(0) == ADSB_SPACING_MAX_MS);
        assert(p.onRateLimited(0, 5000, 0) == 0);           // already at the cap: nothing to log
    }

    // --- spacing actually suppresses requests, and only for that provider ---
    {
        AdsbPacer p;
        p.onAttempt(1, 1000);
        p.onRateLimited(1, 1000, 0);                        // spacing = STEP
        assert(p.cooling(1, 1000 + ADSB_SPACING_STEP_MS - 1));
        assert(!p.cooling(1, 1000 + ADSB_SPACING_STEP_MS));
        assert(!p.cooling(0, 1000));                        // provider 0 is untouched
    }

    // --- an explicit Retry-After wins over the spacing window ---
    {
        AdsbPacer p;
        p.onAttempt(0, 1000);
        p.onRateLimited(0, 1000, 120);                      // "come back in two minutes"
        assert(p.cooling(0, 1000 + ADSB_SPACING_STEP_MS));  // spacing alone would allow it
        assert(p.cooling(0, 1000 + 119000));
        assert(!p.cooling(0, 1000 + 120000));
    }

    // --- easing needs a run of successes, and is additive, not a reset ---
    {
        AdsbPacer p;
        p.onRateLimited(0, 1000, 0);
        p.onRateLimited(0, 2000, 0);                        // spacing = 2 * STEP
        for (int i = 1; i < ADSB_SPACING_EASE_OKS; ++i) assert(!p.onOk(0));
        assert(p.onOk(0));                                  // Nth success eases one step
        assert(p.spacingMs(0) == ADSB_SPACING_STEP_MS);
        for (int i = 1; i < ADSB_SPACING_EASE_OKS; ++i) assert(!p.onOk(0));
        assert(p.onOk(0));
        assert(p.spacingMs(0) == 0);                        // back to full speed
        assert(!p.onOk(0));                                 // nothing left to ease
    }

    // --- a 429 mid-run restarts the streak: no easing off a stale count ---
    {
        AdsbPacer p;
        p.onRateLimited(0, 1000, 0);
        for (int i = 1; i < ADSB_SPACING_EASE_OKS; ++i) p.onOk(0);
        p.onRateLimited(0, 2000, 0);                        // throttled again
        assert(!p.onOk(0));                                 // streak reset, not one-from-easing
        assert(p.spacingMs(0) == ADSB_SPACING_STEP_MS * 2);
    }

    // --- a 200 we cannot parse paces like a 429 instead of looping every poll ---
    {
        AdsbPacer p;
        p.onAttempt(2, 1000);
        assert(p.onUnusable(2) == ADSB_SPACING_STEP_MS);
        assert(p.cooling(2, 1000 + ADSB_SPACING_STEP_MS - 1));
        assert(p.onUnusable(2) == ADSB_SPACING_STEP_MS * 2);
        p.onAttempt(2, 9000);                               // and it recovers on its own
        for (int i = 0; i < ADSB_SPACING_EASE_OKS * 2; ++i) p.onOk(2);
        assert(p.spacingMs(2) == 0);
    }

    // --- signed comparison keeps both timers correct across millis() rollover ---
    {
        const uint32_t late = UINT32_MAX - 5;
        AdsbPacer p;
        p.onRefused(0, late);
        assert(p.cooling(0, (uint32_t)(late + ADSB_COOLDOWN_403_MS - 1)));   // wrapped past zero
        assert(!p.cooling(0, (uint32_t)(late + ADSB_COOLDOWN_403_MS)));

        AdsbPacer q;
        q.onAttempt(1, late);
        q.onRateLimited(1, late, 0);
        assert(q.cooling(1, (uint32_t)(late + ADSB_SPACING_STEP_MS - 1)));
        assert(!q.cooling(1, (uint32_t)(late + ADSB_SPACING_STEP_MS)));
    }

    // --- uptime past 2^31 ms (~24.85 days): a never-parked provider must not read as parked ---
    {
        // _cooldownUntil is 0 from boot and never cleared. The old signed check read
        // (int32_t)(0 - now) > 0 as TRUE once now passed 2^31, so poll() asked nobody and
        // the 180 s feed watchdog rebooted the device every three minutes.
        AdsbPacer p;
        assert(!p.cooling(0, 0x80000001u));   // just past 2^31
        assert(!p.cooling(1, 0xFFFFFF00u));   // just before the 2^32 wrap
        // Same class on the spacing side: an attempt made ~2^31 ms ago is not recent.
        p.onAttempt(2, 1000);
        p.onRateLimited(2, 1000, 0);          // impose a spacing window
        assert(!p.cooling(2, 0x80000001u));   // window expired long before rollover
    }

    // --- a real 403 park that straddles the 2^32 wrap still holds until it expires ---
    {
        AdsbPacer p;
        const uint32_t late = UINT32_MAX - 1000;
        p.onRefused(0, late);                 // 15 min park, deadline crosses zero
        assert(p.cooling(0, late + 5000));    // now has wrapped past 0: still parked
        assert(p.cooling(0, (uint32_t)(late + ADSB_COOLDOWN_403_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(late + ADSB_COOLDOWN_403_MS)));
    }

    // --- spacing still applies correctly across the 2^32 wrap ---
    {
        AdsbPacer p;
        const uint32_t late = UINT32_MAX - 100;
        p.onAttempt(0, late);
        p.onRateLimited(0, late, 0);          // spacing = STEP, window crosses zero
        assert(p.cooling(0, (uint32_t)(late + ADSB_SPACING_STEP_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(late + ADSB_SPACING_STEP_MS)));
    }

    // --- Retry-After is clamped to the longest park cooling() honours ---
    {
        AdsbPacer p;
        p.onAttempt(0, 1000);
        p.onRateLimited(0, 1000, 24L * 3600); // a full day: clamped, not obeyed
        assert(p.cooling(0, (uint32_t)(1000 + ADSB_COOLDOWN_403_MAX_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(1000 + ADSB_COOLDOWN_403_MAX_MS)));
    }
    // A Retry-After past the 32-bit millisecond range: 4,294,968 s * 1000 wraps 32 bits to
    // 704 ms, which slipped under the clamp and re-asked the provider at once. It must
    // park for the full cap like any other huge value.
    {
        AdsbPacer p;
        p.onAttempt(0, 1000);
        p.onRateLimited(0, 1000, 4294968L);
        assert(p.cooling(0, (uint32_t)(1000 + ADSB_COOLDOWN_403_MAX_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(1000 + ADSB_COOLDOWN_403_MAX_MS)));
    }

    // --- a silent provider (connect/TLS/read failure) backs off from the 2nd consecutive one ---
    {
        AdsbPacer p;
        // One silence is free: it is usually a blip, and one request proves nothing.
        assert(p.onTransportFail(0) == 0);
        assert(!p.cooling(0, 2000));                        // asked again next poll
        p.onAttempt(0, 2000);
        // Second in a row: a gap on the same 4..60 s steps as a 429, and it keeps escalating.
        assert(p.onTransportFail(0) == ADSB_SPACING_STEP_MS);
        assert(p.cooling(0, (uint32_t)(2000 + ADSB_SPACING_STEP_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(2000 + ADSB_SPACING_STEP_MS)));
        p.onAttempt(0, 2000 + ADSB_SPACING_STEP_MS);
        assert(p.onTransportFail(0) == ADSB_SPACING_STEP_MS * 2);
        assert(p.spacingMs(0) == 0);                        // the 429 spacing is untouched
        // The first answer ends the silence at once: no lingering gap. (Sharing the 429
        // ladder kept the feed slow for ~20 minutes after a short internet outage.)
        p.onOk(0);
        assert(!p.cooling(0, (uint32_t)(2000 + ADSB_SPACING_STEP_MS + 1)));
        // The streak is reset too: the next lone failure is free again.
        assert(p.onTransportFail(0) == 0);
        // And the silence ladder is per provider.
        assert(p.onTransportFail(1) == 0);
    }

    // --- a silence does not erase a 429 spacing that is still easing back ---
    {
        AdsbPacer p;
        p.onRateLimited(0, 1000, 0);                        // 429 -> spacing STEP
        p.onAttempt(0, 1000);
        p.onTransportFail(0);
        assert(p.onTransportFail(0) == ADSB_SPACING_STEP_MS);
        p.onOk(0);                                          // silence gone, 429 spacing stays
        assert(p.spacingMs(0) == ADSB_SPACING_STEP_MS);
        assert(p.cooling(0, (uint32_t)(1000 + ADSB_SPACING_STEP_MS - 1)));
    }

    // --- every provider silent at once: nobody is held back longer than the outage cap ---
    // Without it the doubling gap reached 60 s, so after a network-wide outage the next
    // poll could be a minute late.
    {
        AdsbPacer p;
        Gaps g;
        drive(p, g, 0x7, 1000, 10 * 60000);
        for (int s = 0; s < ADSB_PROVIDER_COUNT; ++s) {
            assert(g.seen[s]);
            assert(g.maxGap[s] == ADSB_SILENCE_OUTAGE_MAX_MS);   // doubled up to the cap, no further
        }
        // The boundary itself, from the last attempt of slot 0.
        assert(p.cooling(0, (uint32_t)(g.last[0] + ADSB_SILENCE_OUTAGE_MAX_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(g.last[0] + ADSB_SILENCE_OUTAGE_MAX_MS)));
        assert(p.spacingMs(0) == 0);                            // the 429 spacing is not involved
    }
    // ...and the same across the 2^32 millis() wrap.
    {
        AdsbPacer p;
        Gaps g;
        drive(p, g, 0x7, 0xFFFFFFFFu - 90000, 10 * 60000);
        for (int s = 0; s < ADSB_PROVIDER_COUNT; ++s) assert(g.maxGap[s] == ADSB_SILENCE_OUTAGE_MAX_MS);
    }

    // --- one provider silent while another answers: its full gap still applies ---
    {
        AdsbPacer p;
        Gaps g;
        drive(p, g, 0x5, 1000, 10 * 60000);                     // slots 0 and 2 silent, slot 1 answers
        assert(g.maxGap[0] == ADSB_SPACING_MAX_MS);
        assert(g.maxGap[2] == ADSB_SPACING_MAX_MS);
        assert(g.maxGap[1] == STEP);                            // the working one is asked every poll
    }

    // --- a 403-parked provider does not count as an answerer: two silent ones are capped ---
    {
        AdsbPacer p;
        Gaps g;
        p.onAttempt(0, 1000);
        p.onRefused(0, 1000);                                   // parked for 15 min
        drive(p, g, 0x6, 1000, 10 * 60000);                     // slots 1 and 2 silent for 10 min
        assert(!g.seen[0]);                                     // slot 0 stayed parked throughout
        assert(g.maxGap[1] == ADSB_SILENCE_OUTAGE_MAX_MS);
        assert(g.maxGap[2] == ADSB_SILENCE_OUTAGE_MAX_MS);
    }

    // --- recovery: the first answer restores the other providers' full silence gaps ---
    {
        AdsbPacer p;
        Gaps g;
        drive(p, g, 0x7, 1000, 4 * 60000);                      // outage: stored gaps are at the 60 s ceiling
        const uint32_t last1 = g.last[1], last2 = g.last[2];
        // Still in the outage the cap applies...
        assert(!p.cooling(1, (uint32_t)(last1 + ADSB_SILENCE_OUTAGE_MAX_MS)));
        assert(!p.cooling(2, (uint32_t)(last2 + ADSB_SILENCE_OUTAGE_MAX_MS)));
        // ...then slot 0 answers, and slots 1 and 2 wait their full gap again.
        p.onAttempt(0, (uint32_t)(4 * 60000 + 1000));
        p.onOk(0);
        assert(p.cooling(1, (uint32_t)(last1 + ADSB_SPACING_MAX_MS - 1)));
        assert(!p.cooling(1, (uint32_t)(last1 + ADSB_SPACING_MAX_MS)));
        assert(p.cooling(2, (uint32_t)(last2 + ADSB_SPACING_MAX_MS - 1)));
        assert(!p.cooling(2, (uint32_t)(last2 + ADSB_SPACING_MAX_MS)));
    }

    // --- the cap is only for silence: a 429 spacing keeps its length during an outage ---
    {
        AdsbPacer p;
        for (int i = 0; i < 6; ++i) p.onRateLimited(0, 1000, 0);   // spacing at the 60 s ceiling
        assert(p.spacingMs(0) == ADSB_SPACING_MAX_MS);
        for (int s = 0; s < ADSB_PROVIDER_COUNT; ++s) {
            p.onAttempt(s, 2000);
            p.onTransportFail(s);
            p.onTransportFail(s);
        }
        // Every provider is silent now, yet slot 0 is still spaced out by its 429 gap.
        assert(p.cooling(0, 2000 + ADSB_SPACING_MAX_MS - 1));
        assert(!p.cooling(0, 2000 + ADSB_SPACING_MAX_MS));
        // The others wait only the (capped) silence gap, which is 4 s after two failures.
        assert(p.cooling(1, 2000 + ADSB_SPACING_STEP_MS - 1));
        assert(!p.cooling(1, 2000 + ADSB_SPACING_STEP_MS));
    }

    // --- a provider that was never asked is a fallback, not a silent one: no cap yet ---
    {
        AdsbPacer p;
        for (int i = 0; i < 6; ++i) { p.onAttempt(0, 1000); p.onTransportFail(0); }   // slot 0 at 60 s
        // Slots 1 and 2 are untouched, so slot 0 keeps its full gap.
        assert(p.cooling(0, 1000 + ADSB_SPACING_MAX_MS - 1));
    }

    // --- allParked: true only while EVERY provider is inside a park ---
    {
        AdsbPacer p;
        assert(!p.allParked(1000));                         // fresh: nobody parked
        for (int s = 0; s < ADSB_PROVIDER_COUNT - 1; ++s) p.onRefused(s, 1000);
        assert(!p.allParked(1000));                         // the last one is still free
        p.onRefused(ADSB_PROVIDER_COUNT - 1, 1000);
        assert(p.allParked(1000));
        assert(p.allParked((uint32_t)(1000 + ADSB_COOLDOWN_403_MS - 1)));
        assert(!p.allParked((uint32_t)(1000 + ADSB_COOLDOWN_403_MS)));   // parks end together
        // A Retry-After park counts too, and silence or spacing alone does not.
        AdsbPacer q;
        q.onRateLimited(0, 1000, 120);
        q.onRefused(1, 1000);
        q.onTransportFail(2); q.onTransportFail(2);
        assert(!q.allParked(1000));
        q.onRateLimited(2, 1000, 120);
        assert(q.allParked(1000));
        // And across the 2^32 wrap.
        AdsbPacer w;
        const uint32_t late = 0xFFFFFFFFu - 1000;
        for (int s = 0; s < ADSB_PROVIDER_COUNT; ++s) w.onRefused(s, late);
        assert(w.allParked((uint32_t)(late + 2000)));
        assert(!w.allParked((uint32_t)(late + ADSB_COOLDOWN_403_MS)));
    }
}
