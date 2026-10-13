#pragma once
// Self-heal watchdog for the ADS-B task (adsb_task in main.cpp). Pure logic, so
// tests/feed_watchdog_test.cpp can drive minutes of polls against the real AdsbPacer on
// the host.
//
// Its main target is one failure: the internal heap fragmenting until a TLS handshake can
// no longer allocate, which only a reboot recovers (settings persist). That restart
// (RESTART) needs BOTH:
//   - WiFi is up and no provider has answered for ADSB_STUCK_MS, and
//   - the largest free internal heap block is below ADSB_STUCK_MIN_LARGEST_BLOCK.
// A feed stuck with a healthy heap (every provider refusing us, or the internet down behind
// a working WiFi) logs once per episode and keeps running with the amber HUD warning; both
// used to reboot the device every three minutes.
//
// Because a heap figure cannot show every way a feed gets stuck, there is also an
// unconditional backstop: WiFi up and no provider has answered for more than
// ADSB_STUCK_HARD_MS (30 min) restarts regardless of the heap (RESTART_LONG). An outage
// shorter than that never gets there, since the first answer resets the clock. The
// backstop has its own clock, which stands still while every provider is parked by a
// refusal or Retry-After (check()'s allParked): that silence is our own, and without the
// pause the second 403 park (30 min) would end just after the backstop and reboot a
// refused device before it could knock again.
//
// Proof of life is a provider ANSWERING: any HTTP status, a 403 included (onAnswer), or a
// good poll. A poll the pacer skipped is not — nothing was sent, so nothing was learned.
// Counting skips as alive would disable this watchdog: fast TLS failures from a fragmented
// heap open per-provider silence gaps (AdsbPacer::onTransportFail), the poll skips inside
// them every few seconds, and each skip would push the deadline out again, so the restart
// would never come. Providers parked by refusals are covered by the heap gate, and by the
// backstop once a park ends and the silence is no longer our own.
#include <stdint.h>
#include "config.h"

class FeedWatchdog {
public:
    enum Poll   { FETCHED, SKIPPED, FAILED };
    enum Action { NONE, LOG_STUCK, RESTART, RESTART_LONG };

    explicit FeedWatchdog(uint32_t nowMs) : _aliveMs(nowMs), _hardMs(nowMs) {}

    // A provider answered at `atMs` (AdsbClient::lastResponseMs()). A stamp older than the
    // current proof of life is ignored, so passing the same stamp every loop is fine.
    void onAnswer(uint32_t atMs) {
        if ((int32_t)(atMs - _aliveMs) > 0) _aliveMs = atMs;
        if ((int32_t)(atMs - _hardMs) > 0)  _hardMs = atMs;
    }

    // The outcome of one feed poll. Only FETCHED is proof of life (see the top of this
    // file); an error status that did come back already counted through onAnswer().
    void onPoll(Poll p, uint32_t nowMs) {
        if (p == FETCHED) onAnswer(nowMs);
    }

    // Once per task loop. `allParked` is AdsbClient::allParked(). `largestBlock()` is only
    // called while the feed is stuck.
    template <typename LargestBlockFn>
    Action check(bool wifiUp, uint32_t nowMs, bool allParked, LargestBlockFn largestBlock) {
        if (!wifiUp) {                      // WiFi has its own recovery path, not this one
            _aliveMs = _hardMs = nowMs;
            _logged = false;
            return NONE;
        }
        if (allParked) _hardMs = nowMs;     // nobody may be asked: not a wedge (see top)
        const uint32_t silentMs = nowMs - _aliveMs;
        if (silentMs <= ADSB_STUCK_MS) { _logged = false; return NONE; }
        if (nowMs - _hardMs > ADSB_STUCK_HARD_MS) return RESTART_LONG;   // backstop: heap or not
        if (largestBlock() < ADSB_STUCK_MIN_LARGEST_BLOCK) return RESTART;
        if (_logged) return NONE;           // one log line per stuck episode, not one per loop
        _logged = true;
        return LOG_STUCK;
    }

private:
    uint32_t _aliveMs;                      // last proof of life; drives the heap-gated restart
    uint32_t _hardMs;                       // like _aliveMs, but also held at "now" while all providers are parked
    bool     _logged = false;
};
