#pragma once
// Per-provider request pacing for the live ADS-B feed.
//
// Extracted from AdsbClient so it can be exercised on the host: every method takes the
// current millis() instead of reading the clock, so tests/adsb_pacing_test.cpp can drive
// a 15-minute park or a millis() rollover in a few microseconds. The 403 and 429 rules
// come from the inline version; the silence gap below came later.
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
//   no answer at all (connect refused, TLS failure, read timeout) -> often a blip that
//          clears itself, so the first failure is free. But a provider that stays silent
//          must not be re-asked every poll, each time paying its full connect+TLS
//          timeout while the radar goes stale: from the second consecutive silence it
//          waits a doubling gap (the same 4..60 s steps as a 429), and the first usable
//          response (onOk) clears that gap at once — silence, unlike a rate limit, ends
//          the moment the provider answers.
//
// All time comparisons are millis()-rollover-safe: cooldowns compare unsigned remaining
// time BOUNDED by the longest park we ever impose, and spacing uses unsigned elapsed
// time. A plain signed "(int32_t)(until - now) > 0" reads a never-set cooldown (0 from
// boot) as "parked until the far future" once millis() passes 2^31 at ~24.85 days of
// uptime — every provider silently parked, poll() asked nobody, and about three minutes
// later the caller's feed watchdog rebooted the device.

#include <stdint.h>
#include "config.h"

class AdsbPacer {
public:
    // Skip this provider for now? True while it is parked, or still inside its spacing window.
    bool cooling(int slot, uint32_t nowMs) const {
        // _cooldownUntil == 0 means "never parked". It cannot be treated as a plain
        // deadline: with nowMs just below the 2^32 wrap it would read as "parked until
        // right after the wrap", silencing every fresh provider for the last stretch
        // before rollover. (A real park stores 0 only if its deadline lands exactly on
        // the wrap; the cost there is one extra request, not a hang.)
        if (_cooldownUntil[slot] != 0) {
            // A park counts only while its remaining time fits inside the longest park
            // we ever impose. Retry-After is clamped to the same bound in onRateLimited,
            // so a live park always passes; a deadline from before the last rollover
            // reads as a huge remaining time and is ignored instead of parking the
            // provider (the old signed compare parked everyone at ~24.85 days uptime).
            const uint32_t remaining = _cooldownUntil[slot] - nowMs;
            if (remaining != 0 && remaining <= ADSB_COOLDOWN_403_MAX_MS) return true;
        }
        // Unsigned elapsed: a signed compare read an attempt older than 2^31 ms as
        // "just happened" after rollover and cooled the provider forever.
        const uint32_t gap = _spacingMs[slot] > _silenceMs[slot] ? _spacingMs[slot] : _silenceMs[slot];
        if (gap && nowMs - _lastAttemptMs[slot] < gap) return true;
        return false;
    }

    void onAttempt(int slot, uint32_t nowMs) {
        _lastAttemptMs[slot] = nowMs;
        // Forget a deadline that lapsed longer ago than the longest park: unsigned
        // wrap would otherwise read it as a live park again ~49.7 days later.
        if ((uint32_t)(_cooldownUntil[slot] - nowMs) > ADSB_COOLDOWN_403_MAX_MS)
            _cooldownUntil[slot] = 0;
    }

    // HTTP 403. Returns true only the first time since the last success, so the caller
    // announces the park once instead of on every poll.
    //
    // The park ESCALATES: 15 min, 30, 60 ... capped at 6 h. Since airplanes.live went
    // contributor-only (2026-09, access granted by feeder IP), a 403 means "this network is
    // not a contributor" — a state that never clears in minutes, so re-knocking every
    // 15 min forever is just noise on their door. Doubling the park makes a refused device
    // near-silent within a few hours, while someone who starts feeding gets the provider
    // back automatically the same day (or immediately on reboot). Any success resets the
    // ladder to 15 min.
    bool onRefused(int slot, uint32_t nowMs) {
        const uint32_t park = _park403Ms[slot] ? _park403Ms[slot] : ADSB_COOLDOWN_403_MS;
        _cooldownUntil[slot] = nowMs + park;
        _park403Ms[slot] = (park >= ADSB_COOLDOWN_403_MAX_MS / 2)
                             ? ADSB_COOLDOWN_403_MAX_MS : park * 2;
        if (_cooldownLogged[slot]) return false;
        _cooldownLogged[slot] = true;
        return true;
    }

    // HTTP 429. `retryAfterS` <= 0 when the provider sent no usable Retry-After.
    // Returns the new spacing when it changed (for logging), 0 when it was already at the cap.
    uint32_t onRateLimited(int slot, uint32_t nowMs, long retryAfterS) {
        if (retryAfterS > 0) {
            // Clamp to the longest park cooling() honours, so a huge or hostile
            // Retry-After cannot store a deadline that outlives the bound. Clamp the
            // SECONDS first: the old code scaled first and kept the product in a uint32_t,
            // so above 4,294,967 s it wrapped to a tiny park that slipped under a clamp
            // applied afterwards.
            const uint32_t maxS = (uint32_t)(ADSB_COOLDOWN_403_MAX_MS / 1000u);
            const uint32_t park = ((unsigned long)retryAfterS >= maxS)
                                    ? (uint32_t)ADSB_COOLDOWN_403_MAX_MS
                                    : (uint32_t)retryAfterS * 1000u;
            _cooldownUntil[slot] = nowMs + park;
        }
        return backOff(slot);
    }

    // A 200 whose body we cannot use (no aircraft array). Not a refusal and not a rate
    // limit, but persistent — a provider that changes its payload shape would otherwise be
    // re-asked every poll forever. Same multiplicative backoff, same automatic recovery.
    uint32_t onUnusable(int slot) { return backOff(slot); }

    // Connect, TLS or read failure (HTTPClient code <= 0): the provider never answered.
    // Returns 0 (no gap to log) on the first consecutive failure, then doubles a silence
    // gap (ADSB_SPACING_STEP_MS up to ADSB_SPACING_MAX_MS) for every further one. The gap
    // is kept apart from the 429 spacing on purpose: silence ends the moment a provider
    // answers, so onOk() drops it at once, while a rate limit eases back only slowly.
    // Sharing the 429 ladder kept the feed at 20-60 s gaps for ~20 minutes after a short
    // internet outage.
    uint32_t onTransportFail(int slot) {
        if (_transportFails[slot] == 0) { _transportFails[slot] = 1; return 0; }
        uint32_t gap = _silenceMs[slot] ? _silenceMs[slot] * 2 : ADSB_SPACING_STEP_MS;
        if (gap > ADSB_SPACING_MAX_MS) gap = ADSB_SPACING_MAX_MS;
        if (gap == _silenceMs[slot]) return 0;
        _silenceMs[slot] = gap;
        return gap;
    }

    // A usable response. Returns true when the spacing was eased (for logging).
    bool onOk(int slot) {
        _cooldownLogged[slot] = false;      // healthy again: allow a future park to be announced
        _park403Ms[slot] = 0;               // and restart the 403 escalation ladder at 15 min
        _transportFails[slot] = 0;          // the silence streak is over too,
        _silenceMs[slot] = 0;               // and so is its gap (see onTransportFail)
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
    uint32_t _park403Ms[ADSB_PROVIDER_COUNT]      = {0};   // next 403 park length; 0 = base (15 min)
    uint32_t _spacingMs[ADSB_PROVIDER_COUNT]      = {0};
    uint32_t _lastAttemptMs[ADSB_PROVIDER_COUNT]  = {0};
    uint16_t _okStreak[ADSB_PROVIDER_COUNT]       = {0};
    uint8_t  _transportFails[ADSB_PROVIDER_COUNT] = {0};   // consecutive code<=0 fetches; 0 = last one answered
    uint32_t _silenceMs[ADSB_PROVIDER_COUNT]      = {0};   // gap for a provider that stays silent; 0 after any usable answer
    bool     _cooldownLogged[ADSB_PROVIDER_COUNT] = {false};
};
