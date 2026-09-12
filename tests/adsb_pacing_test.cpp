// Per-provider pacing policy (src/adsb_pacing.h). Guards the September 2026 outage
// behaviour: airplanes.live answers 403 to everyone without prior approval, and retrying
// it every poll doubled the request rate onto the providers that still worked.
#include "adsb_pacing.h"

#include <assert.h>
#include <stdint.h>

int main() {
    // --- a fresh provider is asked immediately ---
    {
        AdsbPacer p;
        assert(!p.cooling(0, 1000));
        p.onAttempt(0, 1000);
        assert(!p.cooling(0, 1001));      // no spacing imposed yet: ask as often as we like
    }

    // --- 403 parks the provider for the full cooldown, and announces exactly once ---
    {
        AdsbPacer p;
        assert(p.onRefused(0, 1000));                       // first refusal: caller logs it
        assert(p.cooling(0, (uint32_t)(1000 + ADSB_COOLDOWN_403_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(1000 + ADSB_COOLDOWN_403_MS)));   // 15 min: try again

        // A second refusal re-parks from that moment (so a provider that keeps saying no is
        // never asked more than once per cooldown) and stays quiet on the serial log.
        assert(!p.onRefused(0, 3000));
        assert(p.cooling(0, (uint32_t)(3000 + ADSB_COOLDOWN_403_MS - 1)));
        assert(!p.cooling(0, (uint32_t)(3000 + ADSB_COOLDOWN_403_MS)));

        p.onOk(0);
        assert(p.onRefused(0, 999999));                     // recovered, so a new park is news
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
}
