#pragma once
// Per-provider request pacing for the live ADS-B feed.
//
// Extracted from AdsbClient so it can be exercised on the host: every method takes the
// current millis() instead of reading the clock, so tests/adsb_pacing_test.cpp can drive
// a 15-minute park or a millis() rollover in a few microseconds. The policy itself is
// unchanged from the inline version.
//
// Two mechanisms, because the failures differ:
//
//   403 -> a policy refusal (needs approval, or a User-Agent they reject). It will not
//          clear in seconds, so the provider is PARKED outright. Retrying it every poll
//          only burns a request and doubles the rate onto the surviving provider.
//
//   429 -> we are simply too fast. adsb.lol documents its limits as "dynamic based on
//          the environment load", so there is no fixed rate to hard-code. Instead keep
//          an adaptive minimum SPACING per provider: double it on every 429, and ease
//          it back down after a run of successes. That settles on the fastest rate the
//          provider will currently tolerate, instead of flapping between full speed and
//          a dead stop. An explicit Retry-After still wins and parks us for that long.
//
// All time comparisons are millis()-rollover-safe via signed subtraction.

#include <stdint.h>
#include "config.h"

class AdsbPacer {
public:
    // Skip this provider for now? True while it is parked, or still inside its spacing window.
    bool cooling(int slot, uint32_t nowMs) const {
        if ((int32_t)(_cooldownUntil[slot] - nowMs) > 0) return true;
        if (_spacingMs[slot] &&
            (int32_t)(nowMs - _lastAttemptMs[slot]) < (int32_t)_spacingMs[slot]) return true;
        return false;
    }

    void onAttempt(int slot, uint32_t nowMs) { _lastAttemptMs[slot] = nowMs; }

    // HTTP 403. Returns true only the first time since the last success, so the caller
    // announces the park once instead of on every poll.
    bool onRefused(int slot, uint32_t nowMs) {
        _cooldownUntil[slot] = nowMs + ADSB_COOLDOWN_403_MS;
        if (_cooldownLogged[slot]) return false;
        _cooldownLogged[slot] = true;
        return true;
    }

    // HTTP 429. `retryAfterS` <= 0 when the provider sent no usable Retry-After.
    // Returns the new spacing when it changed (for logging), 0 when it was already at the cap.
    uint32_t onRateLimited(int slot, uint32_t nowMs, long retryAfterS) {
        if (retryAfterS > 0) _cooldownUntil[slot] = nowMs + (uint32_t)retryAfterS * 1000UL;
        return backOff(slot);
    }

    // A 200 whose body we cannot use (no aircraft array). Not a refusal and not a rate
    // limit, but persistent — a provider that changes its payload shape would otherwise be
    // re-asked every poll forever. Same multiplicative backoff, same automatic recovery.
    uint32_t onUnusable(int slot) { return backOff(slot); }

    // A usable response. Returns true when the spacing was eased (for logging).
    bool onOk(int slot) {
        _cooldownLogged[slot] = false;      // healthy again: allow a future park to be announced
        // Ease the imposed gap back down after a sustained good run, so a one-off busy period
        // upstream does not slow us permanently. Additive decrease against the multiplicative
        // increase above: quick to back off, cautious to speed up.
        if (!_spacingMs[slot] || ++_okStreak[slot] < ADSB_SPACING_EASE_OKS) return false;
        _okStreak[slot] = 0;
        _spacingMs[slot] = (_spacingMs[slot] > ADSB_SPACING_STEP_MS)
                             ? _spacingMs[slot] - ADSB_SPACING_STEP_MS : 0;
        return true;
    }

    uint32_t spacingMs(int slot) const { return _spacingMs[slot]; }

private:
    uint32_t backOff(int slot) {
        _okStreak[slot] = 0;
        uint32_t sp = _spacingMs[slot] ? _spacingMs[slot] * 2 : ADSB_SPACING_STEP_MS;
        if (sp > ADSB_SPACING_MAX_MS) sp = ADSB_SPACING_MAX_MS;
        if (sp == _spacingMs[slot]) return 0;
        _spacingMs[slot] = sp;
        return sp;
    }

    uint32_t _cooldownUntil[ADSB_PROVIDER_COUNT]  = {0};
    uint32_t _spacingMs[ADSB_PROVIDER_COUNT]      = {0};
    uint32_t _lastAttemptMs[ADSB_PROVIDER_COUNT]  = {0};
    uint16_t _okStreak[ADSB_PROVIDER_COUNT]       = {0};
    bool     _cooldownLogged[ADSB_PROVIDER_COUNT] = {false};
};
