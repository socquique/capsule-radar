#pragma once
// Self-heal watchdog for the ADS-B task (adsb_task in main.cpp). Pure logic, so
// tests/feed_watchdog_test.cpp can drive minutes of polls against the real AdsbPacer on
// the host.
//
// It targets one failure: the internal heap fragmenting until a TLS handshake can no longer
// allocate, which only a reboot recovers (settings persist). So it restarts only when BOTH
// hold:
//   - WiFi is up and no provider has answered for ADSB_STUCK_MS, and
//   - the largest free internal heap block is below ADSB_STUCK_MIN_LARGEST_BLOCK.
// A feed stuck with a healthy heap (every provider refusing us, or the internet down behind
// a working WiFi) logs once per episode and keeps running with the amber HUD warning; both
// used to reboot the device every three minutes forever.
//
// Proof of life is a provider ANSWERING: any HTTP status, a 403 included (onAnswer), or a
// good poll. A poll the pacer skipped is not — nothing was sent, so nothing was learned.
// Counting skips as alive would disable this watchdog: fast TLS failures from a fragmented
// heap open per-provider silence gaps (AdsbPacer::onTransportFail), the poll skips inside
// them every few seconds, and each skip would push the deadline out again, so the restart
// would never come. Providers parked by refusals are covered by the heap gate instead: with
// a healthy heap the device just stays up.
#include <stdint.h>
#include "config.h"

class FeedWatchdog {
public:
    enum Poll   { FETCHED, SKIPPED, FAILED };
    enum Action { NONE, LOG_STUCK, RESTART };

    explicit FeedWatchdog(uint32_t nowMs) : _aliveMs(nowMs) {}

    // A provider answered at `atMs` (AdsbClient::lastResponseMs()). A stamp older than the
    // current proof of life is ignored, so passing the same stamp every loop is fine.
    void onAnswer(uint32_t atMs) {
        if ((int32_t)(atMs - _aliveMs) > 0) _aliveMs = atMs;
    }

    // The outcome of one feed poll. Only FETCHED is proof of life (see the top of this
    // file); an error status that did come back already counted through onAnswer().
    void onPoll(Poll p, uint32_t nowMs) {
        if (p == FETCHED) onAnswer(nowMs);
    }

    // Once per task loop. `largestBlock()` is only called while the feed is stuck.
    template <typename LargestBlockFn>
    Action check(bool wifiUp, uint32_t nowMs, LargestBlockFn largestBlock) {
        if (!wifiUp) {                      // WiFi has its own recovery path, not this one
            _aliveMs = nowMs;
            _logged = false;
            return NONE;
        }
        if (nowMs - _aliveMs <= ADSB_STUCK_MS) { _logged = false; return NONE; }
        if (largestBlock() < ADSB_STUCK_MIN_LARGEST_BLOCK) return RESTART;
        if (_logged) return NONE;           // one log line per stuck episode, not one per loop
        _logged = true;
        return LOG_STUCK;
    }

private:
    uint32_t _aliveMs;
    bool     _logged = false;
};
